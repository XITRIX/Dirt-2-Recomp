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
UAM = os.environ.get("DIRT2_UAM", os.path.join(ROOT, "build", "tools", "uam", "uam"))
if not os.path.isfile(UAM):
    raise RuntimeError("Patched UAM is missing: run tools/scripts/build_shader_tools.sh")
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


def fix_depth_mrt_output(sha, source):
    """Work around the captured DiRT 2 depth-pass shaders' broken Maxwell output.

    Hardware captures show the original vec4(1,0,0,0)*depth output writing
    zeros over valid R32F depth. Replicating depth fixes it while retaining
    motion vectors, alpha discard, and depth testing. The precise UAM codegen
    defect is not established. These game shaders use R32F for MRT1, so only
    red is stored; this must NOT be applied to arbitrary RGBA outputs.
    Match bytecode identities and expected source, failing if translation changes.
    """
    outputs = {
        "d491208d1c040d1d76741352b3b5dcabcca55a49": (
            "vec4(0.0, 1.0, 0.5, 0.0).yxxx * ps_v0.zzzz", "ps_v0.z"),
        "982fa4f7d93a58985e5b3e06bd7e62dc7485dd14": (
            "vec4(-0.4000000059604644775390625, 0.0, 1.0, 0.5).zyyy * ps_v3.zzzz", "ps_v3.z"),
        "b54d7409f49fe5db26ec292201b01c6d6a2eae1c": (
            "vec4(-0.5, 0.0, 1.0, 0.5).zyyy * ps_v0.zzzz", "ps_v0.z"),
        "cd02b1bc995caefdc3576bfac6b5abd89d413b1c": (
            "vec4(-0.4000000059604644775390625, 0.0, 1.0, 0.5).zyyy * ps_v1.zzzz", "ps_v1.z"),
        "9a4107bdcd78912caf8b7c45bcde012836214ddb": (
            "vec4(-0.4000000059604644775390625, 0.0, 1.0, 0.5).zyyy * ps_v3.zzzz", "ps_v3.z"),
    }
    if sha not in outputs:
        return source
    expression, depth = outputs[sha]
    old = "ps_oC1 = " + expression + ";"
    if source.count(old) != 1:
        raise ValueError(f"depth MRT workaround: unexpected translated output in {sha}")
    return source.replace(old, f"ps_oC1 = vec4({depth});")


def fix_color_lut_output(sha, source):
    """Express the race's LUT transform without masked register reconstruction.

    Capture 7529's original shader matches a CPU evaluation in red/green to
    within 0.001, but blue clips to one instead of averaging 0.423. Keep all
    three LUT samples, the matrix, bloom and vignette; use explicit RGB values
    to avoid the faulty compiled blue path. Scope to this exact bytecode and
    translator output, rather than changing color processing generally.
    """
    if sha != "3319a986c1ccd891e116ec8160ef9feffc621f01":
        return source
    if hashlib.sha256(source.encode()).hexdigest() not in ("8fb5074095a86d147984a53be5f962c0f7bf180673121ccd6b1680df6ad51f67", "66aac8eee7d47649df6a0f7babd872da1b87fec2c8c2dfe9da3a63590587f93d", "b0d857b27bd9eb3eb54e59215b103c7406fd515703dcbfe4d8db58c3a8a17b3c", "847a65f89951d87ad87b9df04a828445cd0871460e644af165a2128aabdb0df2"):
        raise ValueError(f"color LUT workaround: unexpected translated source in {sha}")
    # Keep the generic D3D9 alpha/fog epilogue when the translator wraps main.
    function = "void d3d9_original_main()" if "void d3d9_original_main()" in source else "void main()"
    start=source.index(function);opening=source.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[:start] + function + """
{
    vec2 uv = ps_PointCoordOrTexCoord0.xy;
    vec3 scene = texture(ps_s0, uv).rgb * 0.9375 + vec3(0.03125);
    vec2 rg = texture(ps_s2, scene.xy).rg;
    float b = texture(ps_s2, vec2(scene.z, 0.0)).b;
    vec3 graded = ps_uniforms_1._m0[0].rgb * rg.r
                + ps_uniforms_1._m0[1].rgb * rg.g
                + ps_uniforms_1._m0[2].rgb * b
                + ps_uniforms_1._m0[3].rgb;
    vec4 bloom = texture(ps_s1, uv);
    vec3 color = vec3(1.0) - graded * bloom.rgb;
    ps_oC0 = vec4(color + bloom.a * (ps_uniforms_1._m0[4].rgb - color), 1.0);

}
""" + source[end:]


BARRIER_SOURCE_HASHES = {
    "809bc0ee1608fd0ea00aec8542e940d0ddf3e186af0ed5b230063b2d46995155",
    "755908c6604a2793ae594990bef1e3bb9472f4513a89e6f4b8184143ab472e89",
}


def fix_barrier_lighting(sha, source):
    """Avoid masked-register reconstruction in the captured roadside material.

    The base-texture-only hardware probe removes the car/screen artifacts.
    This equivalent full material keeps the lighting, shadow, fog, specular
    and tone-map calculations. Native validation is still required; the
    underlying compiler defect has not been isolated.
    """
    if sha != "d5ba8ef2993994824ce136a7ad5348067cab3869":
        return source
    # Both supported translations have the same material interface. Fail on
    # translator changes rather than silently using stale register mappings.
    allowed = BARRIER_SOURCE_HASHES
    if hashlib.sha256(source.encode()).hexdigest() not in allowed:
        raise ValueError(f"barrier lighting workaround: unexpected translated source in {sha}")
    function = "void d3d9_original_main()" if "void d3d9_original_main()" in source else "void main()"
    start = source.index(function)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    path = os.path.join(ROOT, "tools", "dx9shader", "workarounds", "barrier_lighting.glsl")
    with open(path) as f:
        material = f.read().replace("void main()", function)
    # Preserve the current translator's native depth-comparison behavior as
    # well as its alpha/fog epilogue. Old packs have ordinary color samplers.
    if "uniform sampler2DShadow d3d9Shadow0;" in source:
        calls = [(0, "ps_v2.xy", "ps_v2.z"), (1, "ps_v2.xy", "ps_v2.z"),
                 (2, "screenUV", "1.0"), (3, "screenUV", "1.0")]
        for stage, uv, reference in calls:
            ordinary = f"texture(ps_s{stage},{uv})"
            comparison = f"vec4(texture(d3d9Shadow{stage},vec3({uv},clamp({reference},0.0,1.0))))"
            material = material.replace(ordinary,
                f"(((d3d9Control.w >> {16+stage}u) & 1u) != 0u ? {comparison} : {ordinary})")
    return source[:start] + material + source[end:]


def run_uam(glsl, dksh):
    stage = "vert" if glsl.endswith(".vert.glsl") else "frag"
    p = subprocess.run([UAM, "-s", stage, "-o", dksh, glsl], capture_output=True, text=True)
    msg = (p.stdout + p.stderr).strip()
    if p.returncode or not os.path.exists(dksh):
        return False, msg
    # D3DRS_SHADEMODE changes interpolation of the two diffuse/specular
    # varyings only. Preserve perspective interpolation for texture coordinates.
    flat_source = re.sub(r'(layout\(location = (?:16|17)\)) in ', r'\1 flat in ', open(glsl).read()) if stage == 'frag' else None
    if flat_source is not None and flat_source != open(glsl).read():
        flat_glsl = glsl + '.flat.glsl'
        open(flat_glsl, 'w').write(flat_source)
        flat = subprocess.run([UAM, '-s', stage, '-o', dksh + '.flat', flat_glsl], capture_output=True, text=True)
        if flat.returncode:
            return False, 'flat interpolation: ' + flat.stdout + flat.stderr
    return True, msg


def normalise(msg):
    """Groups similar failures: numbers and identifiers replaced."""
    line = next((l for l in msg.splitlines() if "error" in l.lower()), msg.splitlines()[0] if msg else "?")
    line = re.sub(r"^.*?(error)", r"\1", line, flags=re.I)
    line = re.sub(r"\b\d+\b", "N", line)
    line = re.sub(r"'[^']*'", "'X'", line)
    return line[:160]


USAGES = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
          "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]  # = D3DDECLUSAGE values
SAMPLER_TYPES = {"2d": 0, "cube": 1, "volume": 2, "legacy": 3}
UNIFORM_TYPES = {"float4": 0, "int4": 1, "bool": 2}


def shader_meta(j, flat_binary=b''):
    """Binary interface record for the deko3d device (shims/d3d9/deko/shaders.cpp, PackMeta):
    u8 stage (0 vertex, 1 pixel), u8 inputs, u8 samplers, u8 uniforms, u32 uniform block bytes;
    inputs {u8 usage, u8 index, u8 register, u8 0}; samplers {u8 register, u8 type, u16 0};
    uniforms {u8 type, u8 0, u16 register, u16 count, u16 0}, in the block's order (the
    uniform list without the shader's own constant arrays, which MojoShader embeds);
    then u32 colour outputs written (bit n = oCn; pixel shaders)."""
    ins = []
    for a in j["inputs"]:
        reg = int(re.sub(r"\D", "", a["name"]) or 0)
        ins.append(struct.pack("<4B", USAGES.index(a["usage"]) if a["usage"] in USAGES else 0xFF, a["index"], reg, 0))
    smp = [struct.pack("<BBH", s["register"], SAMPLER_TYPES.get(s["type"], 0), 0) for s in j["samplers"]]
    # Constant arrays the shader defines itself (def) are embedded in its code,
    # not in the uniform block: leaving them in shifted every real constant.
    live = [u for u in j["uniforms"] if not u.get("constant")]
    uni = [struct.pack("<BBHHH", UNIFORM_TYPES[u["type"]], 0, u["register"], max(u["count"], 1), 0) for u in live]
    block = 16 * sum(max(u["count"], 1) for u in live)
    head = struct.pack("<4BI", 0 if j["stage"] == "vertex" else 1, len(ins), len(smp), len(uni), block)
    # Pixel shaders: the colour targets written (bit per oCn). On Maxwell an
    # unwritten output writes garbage, so the device masks the other targets.
    colors = 0
    if j["stage"] == "pixel":
        for o in j["outputs"]:
            if o["usage"] == "COLOR" and o["index"] < 8:
                colors |= 1 << o["index"]
    return head + b"".join(ins + smp + uni) + struct.pack("<I", colors) + struct.pack("<I", len(flat_binary)) + flat_binary


def write_pack(path, entries):
    """dirt2.dkshpack: "DKSP", u32 version 2, u32 count; then count entries sorted by key
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
    open(path, "wb").write(b"DKSP" + struct.pack("<II", 2, len(entries)) + table + data)


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
        glsl_path = os.path.join(glsl_dir, sha + ext)
        with open(glsl_path) as f:
            source = f.read()
        fixed = fix_barrier_lighting(sha, fix_color_lut_output(sha, fix_depth_mrt_output(sha, source)))
        if fixed != source:
            with open(glsl_path, "w") as f:
                f.write(fixed)
        jobs.append((sha, glsl_path, os.path.join(a.out, "dksh", sha + ".dksh")))

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
        flat_path = os.path.join(a.out, "dksh", sha + ".dksh.flat")
        meta = shader_meta(json.load(open(os.path.join(glsl_dir, sha + ".json"))),
                           open(flat_path, 'rb').read() if os.path.exists(flat_path) else b'')
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
