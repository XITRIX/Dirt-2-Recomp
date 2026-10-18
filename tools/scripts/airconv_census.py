#!/usr/bin/env python3
"""Converts every unique DX11 shader blob with DXMT's airconv and checks the result.

Input: blobs written by `extract_dxbc.py --out DIR` (DIR/<pack>/<index>_<stage>.dxbc).
- vertex, pixel, compute: airconv -A (metallib) on its own.
- domain: needs its hull shader (--hull-before-domain); hulls are tried in turn
  until one converts (pack order does not pair stages reliably).
- hull: DXMT fuses vertex+hull, so a hull counts as converted when some vertex
  shader converts with it (--hull-after-vertex); vertex shaders are tried in turn.
Every metallib is then loaded with a Metal check tool (newLibraryWithData plus
newFunctionWithName for each function).

Usage: airconv_census.py DIR --airconv build/dxmt/macos/bin/airconv --check <mtlcheck> [--out OUT] [-j N]
"""
import argparse
import collections
import concurrent.futures as cf
import hashlib
import os
import subprocess
import sys


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return p.returncode, (p.stderr or p.stdout).strip().splitlines()[-1:] or [""]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--airconv", required=True)
    ap.add_argument("--check", required=True)
    ap.add_argument("--out", default="build/airconv_out")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    blobs = {}  # sha -> (path, stage)
    for pack in sorted(os.listdir(a.dir)):
        for f in sorted(os.listdir(os.path.join(a.dir, pack))):
            path = os.path.join(a.dir, pack, f)
            stage = f.rsplit("_", 1)[1].split(".")[0]
            sha = hashlib.sha1(open(path, "rb").read()).hexdigest()[:12]
            blobs.setdefault(sha, (path, stage))
    by_stage = collections.defaultdict(list)
    for sha, (path, stage) in blobs.items():
        by_stage[stage].append((sha, path))

    results = {}  # sha -> (ok, note)

    def convert(sha, path, extra=()):
        out = os.path.join(a.out, sha + (extra[1].rsplit("/", 1)[1][:12] if extra else "") + ".metallib")
        rc, msg = run([a.airconv, "-A", path, "-o", out, *extra])
        return rc == 0 and os.path.exists(out), out, msg[0]

    # Plain stages
    jobs = {}
    with cf.ThreadPoolExecutor(a.j) as ex:
        for stage in ("vertex", "pixel", "compute"):
            for sha, path in by_stage[stage]:
                jobs[ex.submit(convert, sha, path)] = (sha, stage)
        outs = []
        for fut in cf.as_completed(jobs):
            sha, stage = jobs[fut]
            ok, out, msg = fut.result()
            results[sha] = (stage, ok, "" if ok else msg)
            if ok:
                outs.append((sha, out))

    hulls = [p for _, p in by_stage["hull"]]

    # Domain shaders: try each hull.
    def domain_job(sha, path):
        for h in hulls:
            ok, out, msg = convert(sha, path, ("--hull-before-domain", h))
            if ok:
                return sha, True, out, os.path.basename(h)
        return sha, False, None, msg

    # Hull shaders: try vertex shaders until one fuses.
    vertex_paths = [p for _, p in by_stage["vertex"]]

    def hull_job(sha, path):
        last = ""
        for v in vertex_paths:
            vsha = hashlib.sha1(open(v, "rb").read()).hexdigest()[:12]
            ok, out, msg = convert(vsha, v, ("--hull-after-vertex", path))
            if ok:
                return sha, True, out, os.path.basename(v)
            last = msg
        return sha, False, None, last

    with cf.ThreadPoolExecutor(a.j) as ex:
        futs = [ex.submit(domain_job, s, p) for s, p in by_stage["domain"]]
        futs += [ex.submit(hull_job, s, p) for s, p in by_stage["hull"]]
        for fut in cf.as_completed(futs):
            sha, ok, out, note = fut.result()
            stage = blobs[sha][1]
            results[sha] = (stage, ok, ("with " + note) if ok else note)
            if ok:
                outs.append((sha, out))

    # Metal load check
    bad_load = {}
    paths = [o for _, o in outs]
    for i in range(0, len(paths), 200):
        p = subprocess.run([a.check, *paths[i:i + 200]], capture_output=True, text=True)
        for line in p.stdout.splitlines():
            if line.startswith("FAIL"):
                bad_load[line.split()[1]] = line
    for sha, out in outs:
        if out in bad_load:
            stage, _, _ = results[sha]
            results[sha] = (stage, False, "metal load: " + bad_load[out][5:].split(" ", 1)[-1])

    tot = collections.Counter()
    okc = collections.Counter()
    for sha, (stage, ok, note) in results.items():
        tot[stage] += 1
        okc[stage] += ok
    print("| stage | converted + loads | unique |\n|---|---|---|")
    for st in ("vertex", "pixel", "compute", "hull", "domain"):
        print(f"| {st} | {okc[st]} | {tot[st]} |")
    print(f"| **total** | {sum(okc.values())} | {sum(tot.values())} |\n")
    fails = [(st, sha, note) for sha, (st, ok, note) in results.items() if not ok]
    if fails:
        print("Failures:\n")
        for st, sha, note in sorted(fails):
            print(f"- {st} {sha} ({blobs[sha][0]}): {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
