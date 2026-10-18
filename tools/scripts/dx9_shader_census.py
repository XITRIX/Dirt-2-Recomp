#!/usr/bin/env python3
"""Census of the DX9 shaders through the Switch shader pipeline (docs/SWITCH.md).

Collects every unique Direct3D 9 shader the game can use: the SM2/SM3 blobs in its
dx9 PSSG packs and in dirt2_game.exe, plus the d3dx9 shim's built-in ones
(shims/d3dx9/builtin_shaders.inc).
It then runs each through
    dx9shader  (d3dbc -> GLSL 4.60, build/tools/dx9shader, build_shader_tools.sh)
    uam        (GLSL -> deko3d DKSH, devkitPro)
and reports how many make it, with the failures grouped by message.

Output in --out (default build/shaders/dx9), all game-derived and never committed:
    src/<sha1>.d3dbc   glsl/<sha1>.{vert,frag}.glsl + .json (+ .err)
    dksh/<sha1>.dksh   census.md   dirt2.dkshpack (all DKSH + interfaces, for the device)

    dx9_shader_census.py [--game DIR] [--out DIR] [--jobs N]
"""
import argparse
import collections
import concurrent.futures
import hashlib
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_GAME = os.path.join(ROOT, "Colin McRae DiRT 2 (v.1.1) [Steam-Rip]", "Dirt 2")
DX9SHADER = os.path.join(ROOT, "build", "tools", "dx9shader", "dx9shader")
UAM = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "tools", "bin", "uam")
END = 0x0000FFFF


def walk_sm3(data, s):
    """End offset of a vs/ps 2.x-3.0 token stream starting at s, or None if it is
    not one. Comments (0xFFFE) carry their length in the high word, instructions
    in bits 24-27, so a 0x0000FFFF inside comment data (the constant table) is
    skipped and a version-like word inside other data does not walk to an end."""
    e = s + 4
    while e + 4 <= len(data):
        tok = struct.unpack_from("<I", data, e)[0]
        if tok == END:
            return e + 4
        if tok & 0xFFFF == 0xFFFE:
            e += 4 + 4 * (tok >> 16)
            continue
        op = tok & 0xFFFF
        if tok & 0x80000000 or not (op <= 0x60 or op == 0xFFFD):  # parameter token / unknown opcode
            return None
        e += 4 + 4 * ((tok >> 24) & 0xF)
    return None


def pack_blobs(path):
    """d3dbc blobs in a PSSG pack: SM2/SM3 version tokens whose stream walks to the end token."""
    data = open(path, "rb").read()
    for m in re.finditer(rb"[\x00-\x03][\x02\x03][\xfe\xff]\xff", data):
        e = walk_sm3(data, m.start())
        if e:
            yield data[m.start():e]


def builtin_blobs(path):
    for body in re.findall(r"static const uint32_t kShader\d+\[\] = \{(.*?)\};", open(path).read(), re.S):
        words = [int(w, 16) for w in re.findall(r"0x([0-9A-Fa-f]{8})u", body)]
        yield struct.pack("<%dI" % len(words), *words)


def collect(game):
    shaders = {}  # sha1 -> (blob, origins)
    packs = os.path.join(game, "dx9", "shaderpack")
    for name in sorted(os.listdir(packs)):
        for blob in pack_blobs(os.path.join(packs, name)):
            shaders.setdefault(hashlib.sha1(blob).hexdigest(), [blob, set()])[1].add(name)
    # Shaders compiled into the exe (Bink video's YUV conversion, ...).
    for blob in pack_blobs(os.path.join(game, "dirt2_game.exe")):
        shaders.setdefault(hashlib.sha1(blob).hexdigest(), [blob, set()])[1].add("dirt2_game.exe")
    for blob in builtin_blobs(os.path.join(ROOT, "shims", "d3dx9", "builtin_shaders.inc")):
        shaders.setdefault(hashlib.sha1(blob).hexdigest(), [blob, set()])[1].add("d3dx9 built-in")
    return shaders


def run_uam(glsl, dksh):
    stage = "vert" if glsl.endswith(".vert.glsl") else "frag"
    p = subprocess.run([UAM, "-s", stage, "-o", dksh, glsl], capture_output=True, text=True)
    msg = (p.stdout + p.stderr).strip()
    return p.returncode == 0 and os.path.exists(dksh), msg


def normalise(msg):
    """Groups similar failures: numbers and identifiers replaced."""
    line = next((l for l in msg.splitlines() if "error" in l.lower()), msg.splitlines()[0] if msg else "?")
    line = re.sub(r"^.*?(error)", r"\1", line, flags=re.I)
    line = re.sub(r"\b\d+\b", "N", line)
    line = re.sub(r"'[^']*'", "'X'", line)
    return line[:160]


USAGES = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
          "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]  # = D3DDECLUSAGE values
SAMPLER_TYPES = {"2d": 0, "cube": 1, "volume": 2}
UNIFORM_TYPES = {"float4": 0, "int4": 1, "bool": 2}


def shader_meta(j):
    """Binary interface record for the deko3d device (shims/d3d9/deko/shaders.cpp, PackMeta):
    u8 stage (0 vertex, 1 pixel), u8 inputs, u8 samplers, u8 uniforms, u32 uniform block bytes;
    inputs {u8 usage, u8 index, u8 register, u8 0}; samplers {u8 register, u8 type, u16 0};
    uniforms {u8 type, u8 0, u16 register, u16 count, u16 0}, in the block's order."""
    ins = []
    for a in j["inputs"]:
        reg = int(re.sub(r"\D", "", a["name"]) or 0)
        ins.append(struct.pack("<4B", USAGES.index(a["usage"]) if a["usage"] in USAGES else 0xFF, a["index"], reg, 0))
    smp = [struct.pack("<BBH", s["register"], SAMPLER_TYPES.get(s["type"], 0), 0) for s in j["samplers"]]
    uni = [struct.pack("<BBHHH", UNIFORM_TYPES[u["type"]], 0, u["register"], max(u["count"], 1), 0) for u in j["uniforms"]]
    block = 16 * sum(max(u["count"], 1) for u in j["uniforms"])
    head = struct.pack("<4BI", 0 if j["stage"] == "vertex" else 1, len(ins), len(smp), len(uni), block)
    return head + b"".join(ins + smp + uni)


def write_pack(path, entries):
    """dirt2.dkshpack: "DKSP", u32 version 1, u32 count; then count entries sorted by key
    {sha256 of the d3dbc[32], u32 dksh offset, u32 dksh size, u32 meta offset, u32 meta size};
    then the data. The device looks shaders up by the SHA-256 of what the game passes."""
    entries = sorted(entries)
    head = 12 + len(entries) * 48
    table, data = b"", b""
    for key, dksh, meta in entries:
        d_off = head + len(data)
        data += dksh
        m_off = head + len(data)
        data += meta
        data += b"\0" * (-len(data) % 4)
        table += key + struct.pack("<4I", d_off, len(dksh), m_off, len(meta))
    open(path, "wb").write(b"DKSP" + struct.pack("<II", 1, len(entries)) + table + data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "shaders", "dx9"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    for d in ("src", "glsl", "dksh"):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)

    shaders = collect(a.game)
    srcs = []
    for sha, (blob, _) in shaders.items():
        p = os.path.join(a.out, "src", sha + ".d3dbc")
        if not os.path.exists(p):
            open(p, "wb").write(blob)
        srcs.append(p)
    print(f"{len(shaders)} unique shaders")

    # Stage 1: translation (one process, all files).
    glsl_dir = os.path.join(a.out, "glsl")
    for f in os.listdir(glsl_dir):
        os.remove(os.path.join(glsl_dir, f))
    subprocess.run([DX9SHADER, "--out", glsl_dir] + srcs, capture_output=True)

    stage_of = {sha: ("vs" if blob[2] == 0xFE else "ps") for sha, (blob, _) in shaders.items()}
    results = {}  # sha -> (step, ok, msg)
    jobs = []
    for sha in shaders:
        err = os.path.join(glsl_dir, sha + ".err")
        if os.path.exists(err):
            results[sha] = ("translate", False, open(err).read().strip())
            continue
        ext = ".vert.glsl" if stage_of[sha] == "vs" else ".frag.glsl"
        jobs.append((sha, os.path.join(glsl_dir, sha + ext), os.path.join(a.out, "dksh", sha + ".dksh")))

    # Stage 2: uam, in parallel.
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for (sha, _, _), (ok, msg) in zip(jobs, ex.map(lambda j: run_uam(j[1], j[2]), jobs)):
            results[sha] = ("uam", ok, msg)

    total = collections.Counter()
    passed = collections.Counter()
    failures = collections.defaultdict(list)
    for sha, (step, ok, msg) in results.items():
        st = stage_of[sha]
        total[st] += 1
        if ok:
            passed[st] += 1
        else:
            failures[(step, normalise(msg))].append(sha)

    lines = ["# DX9 shader census (Switch: d3dbc -> GLSL -> DKSH)", "",
             "| Stage | Unique | Translated and compiled |", "|---|---|---|"]
    for st in ("vs", "ps"):
        lines.append(f"| {st} | {total[st]} | {passed[st]} |")
    lines.append(f"| all | {sum(total.values())} | {sum(passed.values())} |")
    lines += ["", "## Failures", ""]
    for (step, msg), shas in sorted(failures.items(), key=lambda kv: -len(kv[1])):
        lines.append(f"- **{len(shas)}** at {step}: `{msg}` (e.g. {shas[0]})")
    report = os.path.join(a.out, "census.md")
    open(report, "w").write("\n".join(lines) + "\n")

    import json
    pack = []
    for sha, (blob, _) in shaders.items():
        if not results[sha][1]:
            continue
        meta = shader_meta(json.load(open(os.path.join(glsl_dir, sha + ".json"))))
        dksh = open(os.path.join(a.out, "dksh", sha + ".dksh"), "rb").read()
        pack.append((hashlib.sha256(blob).digest(), dksh, meta))
    pack_path = os.path.join(a.out, "dirt2.dkshpack")
    write_pack(pack_path, pack)
    print(f"{pack_path}: {len(pack)} shaders, {os.path.getsize(pack_path) // 1024} KB")
    print("\n".join(lines[2:7]))
    print(f"{sum(len(v) for v in failures.values())} failures in {len(failures)} groups: {report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
