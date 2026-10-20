#!/usr/bin/env python3
"""Build an isolated CPU experiment from a known Switch generated archive.

Recompile selected profiled translation units, preserving original compile
options plus RECOMP_INLINE_GUEST_MEMORY. The baseline archive is never modified.
A manifest pins the baseline, sources/header and output digests. Full source
builds can instead enable CMake's DIRT2_INLINE_GUEST_MEMORY option.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT/'build/switch/libdirt2_gen.a')
    ap.add_argument('--commands', type=Path, default=ROOT/'build/switch/compile_commands.json')
    ap.add_argument('--output', type=Path, default=ROOT/'build/switch-cpu-inline')
    ap.add_argument('--chunks', nargs='+', default=['gen_0237', 'gen_0450', 'gen_0453', 'gen_0456'])
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    archive = out/'libdirt2_gen.a'
    if archive == args.baseline.resolve(): raise ValueError('Output must not replace baseline')
    commands = json.loads(args.commands.read_text())
    selected = []
    for chunk in args.chunks:
        matches = [c for c in commands if Path(c['file']).stem == chunk]
        if len(matches) != 1: raise ValueError(f'Expected one compile command for {chunk}')
        cmd = matches[0]
        argv = shlex.split(cmd['command'])
        if '-DRECOMP_TSO=1' not in argv: raise ValueError('TSO baseline is required')
        obj = out/(chunk+'.o')
        argv[argv.index('-o')+1] = str(obj)
        argv.insert(1, '-DRECOMP_INLINE_GUEST_MEMORY=1')
        selected.append((cmd, argv, obj))
    def compile_one(entry):
        cmd, argv, obj = entry
        print('Compiling',obj.name,flush=True)
        with obj.with_suffix('.log').open('w') as log:
            subprocess.run(argv,cwd=cmd['directory'],stdout=log,stderr=log,check=True)
        return {'source':cmd['file'],'source_sha256':sha(Path(cmd['file'])), 'object':obj.name,'command':argv}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        sources = list(pool.map(compile_one, selected))
    compiler = Path(selected[0][1][0]); ar = compiler.with_name('aarch64-none-elf-ar')
    members = subprocess.check_output([str(ar),'t',str(args.baseline)],text=True).splitlines()
    for _, _, obj in selected:
        if members.count(obj.name) != 1:
            raise ValueError(f'Expected exactly one baseline member {obj.name}')
    shutil.copy2(args.baseline, archive)
    subprocess.run([str(ar),'r',str(archive),*[str(e[2]) for e in selected]],check=True)
    manifest = {'baseline':str(args.baseline.resolve()),'baseline_sha256':sha(args.baseline),
        'header_sha256':sha(ROOT/'runtime/include/recomp/x86.h'), 'tso':True,
        'inline_guest_memory':True, 'sources':sources,'archive_sha256':sha(archive)}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('COMPLETE',archive,flush=True)
if __name__ == '__main__': main()
