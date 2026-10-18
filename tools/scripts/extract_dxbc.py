#!/usr/bin/env python3
"""Census of the DXBC shader blobs embedded in DiRT 2's DX11 PSSG packs.

Scans each pack for the "DXBC" magic, sizes the blob from the DXBC header,
decodes the SHEX/SHDR program header and declarations, and prints a markdown
report. With --out DIR each blob is written to DIR/<pack>/<index>_<stage>.dxbc.

    extract_dxbc.py [--packs DIR] [--out DIR] [--report FILE]
"""
import argparse
import hashlib
import os
import struct
import sys
from collections import Counter, defaultdict

DEFAULT_PACKS = ("Colin McRae DiRT 2 (v.1.1) [Steam-Rip]/Dirt 2/dx11/shaderpack")
PACKS = ("shaderpack", "postprocess", "ui")

STAGES = {0: "pixel", 1: "vertex", 2: "geometry", 3: "hull", 4: "domain", 5: "compute"}

# D3D10_SB / D3D11_SB opcode numbers (d3d11TokenizedProgramFormat.hpp)
OP_EMIT = 63
OP_CUT = 64
OP_CUSTOMDATA = 53
OP_DCL_GS_OUTPUT_TOPOLOGY = 92
OP_DCL_GS_INPUT_PRIMITIVE = 93
OP_DCL_MAX_OUTPUT_VERTEX_COUNT = 94
OP_HS_DECLS = 113
OP_HS_CONTROL_POINT_PHASE = 114
OP_HS_FORK_PHASE = 115
OP_HS_JOIN_PHASE = 116
OP_EMIT_STREAM = 117
OP_CUT_STREAM = 118
OP_EMITTHENCUT_STREAM = 119
OP_DCL_STREAM = 143
OP_DCL_INPUT_CONTROL_POINT_COUNT = 147
OP_DCL_OUTPUT_CONTROL_POINT_COUNT = 148
OP_DCL_TESS_DOMAIN = 149
OP_DCL_TESS_PARTITIONING = 150
OP_DCL_TESS_OUTPUT_PRIMITIVE = 151
OP_DCL_HS_MAX_TESSFACTOR = 152
OP_DCL_HS_FORK_PHASE_INSTANCE_COUNT = 153
OP_DCL_HS_JOIN_PHASE_INSTANCE_COUNT = 154
OP_DCL_THREAD_GROUP = 155
OP_DCL_GS_INSTANCE_COUNT = 206

DOMAIN = {1: "isoline", 2: "tri", 3: "quad"}
PARTITIONING = {1: "integer", 2: "pow2", 3: "fractional_odd", 4: "fractional_even"}
TESS_OUT = {1: "point", 2: "line", 3: "triangle_cw", 4: "triangle_ccw"}
GS_IN = {1: "point", 2: "line", 3: "triangle", 6: "line_adj", 7: "triangle_adj"}
GS_OUT = {1: "pointlist", 3: "linestrip", 5: "trianglestrip"}


def find_blobs(data):
    """Yield (offset, size) for every DXBC blob whose header is self-consistent."""
    pos = 0
    while True:
        o = data.find(b"DXBC", pos)
        if o < 0:
            return
        pos = o + 4
        if o + 32 > len(data):
            yield o, None
            continue
        total, = struct.unpack_from("<I", data, o + 24)
        nchunks, = struct.unpack_from("<I", data, o + 28)
        if 32 + 4 * nchunks <= total and o + total <= len(data) and nchunks < 64:
            yield o, total
            pos = o + total
        else:
            yield o, None


def chunks(blob):
    nchunks, = struct.unpack_from("<I", blob, 28)
    out = {}
    for i in range(nchunks):
        off, = struct.unpack_from("<I", blob, 32 + 4 * i)
        tag = blob[off:off + 4].decode("latin1")
        size, = struct.unpack_from("<I", blob, off + 4)
        if off + 8 + size > len(blob):
            raise ValueError("chunk %s overruns blob" % tag)
        out[tag] = blob[off + 8:off + 8 + size]
    return out


def parse_shader(blob):
    ch = chunks(blob)
    code = ch.get("SHEX") or ch.get("SHDR")
    if code is None:
        raise ValueError("no SHEX/SHDR chunk")
    ver, nwords = struct.unpack_from("<II", code, 0)
    stage = STAGES.get(ver >> 16)
    if stage is None:
        raise ValueError("unknown program type %d" % (ver >> 16))
    info = {"stage": stage, "model": "%d.%d" % ((ver >> 4) & 15, ver & 15),
            "chunks": sorted(ch), "emit_stream": 0, "emit": 0, "dcl_stream": 0,
            "phases": Counter()}
    if nwords * 4 > len(code):
        raise ValueError("SHEX length exceeds chunk")
    words = struct.unpack_from("<%dI" % (nwords - 2), code, 8)
    i = 0
    while i < len(words):
        tok = words[i]
        op = tok & 0x7FF
        n = (tok >> 24) & 0x7F
        if op == OP_CUSTOMDATA:
            n = words[i + 1] if i + 1 < len(words) else 0
        if n == 0:
            raise ValueError("zero-length instruction at word %d" % i)
        arg = (tok >> 11)
        nxt = words[i + 1] if i + 1 < len(words) else 0
        if op == OP_DCL_TESS_DOMAIN:
            info["domain"] = DOMAIN.get(arg & 7, "?%d" % (arg & 7))
        elif op == OP_DCL_TESS_PARTITIONING:
            info["partitioning"] = PARTITIONING.get(arg & 7, "?%d" % (arg & 7))
        elif op == OP_DCL_TESS_OUTPUT_PRIMITIVE:
            info["tess_out"] = TESS_OUT.get(arg & 7, "?%d" % (arg & 7))
        elif op == OP_DCL_HS_MAX_TESSFACTOR:
            info["max_tess"] = struct.unpack("<f", struct.pack("<I", nxt))[0]
        elif op == OP_DCL_INPUT_CONTROL_POINT_COUNT:
            info["in_cp"] = arg & 0x3F
        elif op == OP_DCL_OUTPUT_CONTROL_POINT_COUNT:
            info["out_cp"] = arg & 0x3F
        elif op == OP_DCL_THREAD_GROUP:
            info["group"] = tuple(words[i + 1:i + 4])
        elif op == OP_DCL_GS_INPUT_PRIMITIVE:
            info["gs_in"] = GS_IN.get(arg & 0x3F, "?%d" % (arg & 0x3F))
        elif op == OP_DCL_GS_OUTPUT_TOPOLOGY:
            info["gs_out"] = GS_OUT.get(arg & 0x3F, "?%d" % (arg & 0x3F))
        elif op == OP_DCL_MAX_OUTPUT_VERTEX_COUNT:
            info["gs_max_verts"] = nxt
        elif op == OP_DCL_GS_INSTANCE_COUNT:
            info["gs_instances"] = nxt
        elif op == OP_DCL_STREAM:
            info["dcl_stream"] += 1
        elif op in (OP_EMIT_STREAM, OP_CUT_STREAM, OP_EMITTHENCUT_STREAM):
            info["emit_stream"] += 1
        elif op in (OP_EMIT, OP_CUT):
            info["emit"] += 1
        elif op in (OP_HS_CONTROL_POINT_PHASE, OP_HS_FORK_PHASE, OP_HS_JOIN_PHASE):
            info["phases"][op] += 1
        i += n
    info["stream_out"] = bool(info["dcl_stream"] or info["emit_stream"])
    # SM4 streams also show up as a stream-output signature chunk
    info["sig_streams"] = "OSG5" in ch
    return info


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--packs", default=os.path.join(root, DEFAULT_PACKS))
    ap.add_argument("--out", help="write each blob to DIR/<pack>/<index>_<stage>.dxbc")
    ap.add_argument("--report", default=os.path.join(root, "build", "dxbc_census.md"))
    a = ap.parse_args()

    blobs = []  # dicts
    failures = []
    bad_headers = []
    for pack in PACKS:
        path = os.path.join(a.packs, pack + ".pssg")
        data = open(path, "rb").read()
        idx = 0
        for off, size in find_blobs(data):
            if size is None:
                bad_headers.append((pack, off))
                continue
            blob = data[off:off + size]
            rec = {"pack": pack, "index": idx, "offset": off, "size": size,
                   "sha1": hashlib.sha1(blob).hexdigest()}
            idx += 1
            try:
                rec.update(parse_shader(blob))
            except Exception as e:  # noqa: BLE001 - report every parse failure
                rec["stage"] = "unparsed"
                failures.append((rec, str(e)))
            blobs.append(rec)
            if a.out:
                d = os.path.join(a.out, pack)
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, "%04d_%s.dxbc" % (rec["index"], rec["stage"])), "wb") as f:
                    f.write(blob)

    stages = ["vertex", "pixel", "geometry", "hull", "domain", "compute", "unparsed"]
    cnt = Counter((b["pack"], b["stage"]) for b in blobs)
    uniq = {p: len({b["sha1"] for b in blobs if b["pack"] == p}) for p in PACKS}
    L = ["# DiRT 2 DX11 shader census", ""]

    L += ["## Blobs per pack and stage", ""]
    rows = []
    for p in PACKS:
        rows.append([p] + [cnt[(p, s)] for s in stages] +
                    [sum(cnt[(p, s)] for s in stages), uniq[p]])
    rows.append(["**total**"] + [sum(cnt[(p, s)] for p in PACKS) for s in stages] +
                [len(blobs), len({b["sha1"] for b in blobs})])
    L += [md_table(["pack"] + stages + ["blobs", "unique"], rows), ""]

    sm = Counter((b["stage"], b["model"]) for b in blobs if "model" in b)
    L += ["## Shader model per stage", "",
          md_table(["stage", "model", "count"], [(s, m, n) for (s, m), n in sorted(sm.items())]), ""]

    hulls = [b for b in blobs if b["stage"] == "hull"]
    domains = [b for b in blobs if b["stage"] == "domain"]
    L += ["## Hull shaders: domain x partitioning x output primitive", ""]
    hk = Counter((b.get("domain", "-"), b.get("partitioning", "-"), b.get("tess_out", "-")) for b in hulls)
    L += [md_table(["domain", "partitioning", "output primitive", "count"],
                   [(*k, n) for k, n in sorted(hk.items())]) if hk else "No hull shaders.", ""]
    if hulls:
        L += [md_table(["in CPs", "out CPs", "max tess factor", "count"],
                       [(*k, n) for k, n in sorted(Counter(
                           (b.get("in_cp", "-"), b.get("out_cp", "-"), b.get("max_tess", "-")) for b in hulls).items(),
                           key=str)]), ""]
    L += ["## Domain shaders: domain", ""]
    dk = Counter(b.get("domain", "-") for b in domains)
    L += [md_table(["domain", "count"], sorted(dk.items())) if dk else "No domain shaders.", ""]

    geoms = [b for b in blobs if b["stage"] == "geometry"]
    L += ["## Geometry shaders", ""]
    if geoms:
        gk = Counter((b.get("gs_in", "-"), b.get("gs_out", "-"), b.get("gs_max_verts", "-"),
                      "yes" if b["stream_out"] else "no", "yes" if b["sig_streams"] else "no") for b in geoms)
        L += [md_table(["input prim", "output topology", "max verts", "stream output (dcl/emit_stream)",
                        "OSG5 chunk"], [(*k, n) for k, n in sorted(gk.items(), key=str)]), ""]
    else:
        L += ["No geometry shaders.", ""]

    comps = [b for b in blobs if b["stage"] == "compute"]
    L += ["## Compute thread groups", ""]
    L += [md_table(["group size", "count"], [("x".join(map(str, k)), n) for k, n in sorted(
        Counter(b.get("group") for b in comps).items(), key=str)]) if comps else "No compute shaders.", ""]

    L += ["## Parse failures", ""]
    if failures or bad_headers:
        L += [md_table(["pack", "index", "offset", "size", "error"],
                       [(r["pack"], r["index"], r["offset"], r["size"], e) for r, e in failures])
              if failures else "", ""]
        if bad_headers:
            L += ["Inconsistent DXBC headers (not counted as blobs): " +
                  ", ".join("%s@%d" % x for x in bad_headers), ""]
    else:
        L += ["None.", ""]

    iso = [b for b in hulls + domains if b.get("domain") == "isoline"]
    frac = [b for b in hulls if b.get("partitioning", "").startswith("fractional")]
    so = [b for b in geoms if b["stream_out"]]
    total = sum(cnt.values())
    ok = total == len(blobs)
    L += ["## Gate 1 facts", "",
          "- Isoline domain: **%s** (%d hull/domain shaders)" % ("YES" if iso else "NO", len(iso)),
          "- Fractional partitioning (odd/even): **%s** (%d hull shaders; pow2: %d, integer: %d)" % (
              "YES" if frac else "NO", len(frac),
              sum(b.get("partitioning") == "pow2" for b in hulls),
              sum(b.get("partitioning") == "integer" for b in hulls)),
          "- Geometry-shader stream output: **%s** (%d of %d GS)" % ("YES" if so else "NO", len(so), len(geoms)),
          "- Totals: %d blobs found, %d stage-counted, %d parse failures, %d unique by SHA-1" % (
              len(blobs), total, len(failures), len({b['sha1'] for b in blobs})),
          ""]
    L += ["## Blob index", "", "Offsets are into the .pssg file; sizes in bytes.", "",
          md_table(["pack", "#", "stage", "model", "offset", "size", "sha1"],
                   [(b["pack"], b["index"], b["stage"], b.get("model", "-"), b["offset"], b["size"],
                     b["sha1"][:12]) for b in blobs]), ""]

    text = "\n".join(L)
    os.makedirs(os.path.dirname(a.report), exist_ok=True)
    with open(a.report, "w") as f:
        f.write(text)
    print(text[:text.index("## Blob index")])
    print("report written to", a.report, file=sys.stderr)
    return 0 if ok and not failures else 1


if __name__ == "__main__":
    sys.exit(main())
