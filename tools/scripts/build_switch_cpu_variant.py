#!/usr/bin/env python3
"""Build an isolated CPU experiment from a known Switch generated archive.

Recompile selected or all generated translation units. GCC mode preserves the
original options; Clang mode uses the same AArch64 SDK headers and strict math.
Memory inlining is optional. The baseline archive is never modified. Per-object
input hashes permit resuming an interrupted full build. A manifest records the
baseline, compiler, source/header hashes and output archive digest.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import threading

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
    ap.add_argument('--all', action='store_true', help='Recompile every generated object')
    ap.add_argument('--compiler', choices=['gcc','clang'], default='gcc')
    ap.add_argument('--no-force-inline', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    archive = out/'libdirt2_gen.a'
    if archive == args.baseline.resolve(): raise ValueError('Output must not replace baseline')
    commands = json.loads(args.commands.read_text())
    selected = []
    chunks = [Path(c['file']).stem for c in commands if '/dirt2_gen.dir/' in c.get('output','')] if args.all else args.chunks
    header_hashes = {str(p.relative_to(ROOT)):sha(p) for p in (ROOT/'runtime/include/recomp').glob('*.h')}
    header_hashes['generated/src/funcs.h'] = sha(ROOT/'generated/src/funcs.h')
    clang = shutil.which('clang') if args.compiler == 'clang' else None
    if args.compiler == 'clang' and not clang: raise ValueError('Clang is not installed')
    ar = None
    for chunk in chunks:
        matches = [c for c in commands if Path(c['file']).stem == chunk and '/dirt2_gen.dir/' in c.get('output','')]
        if len(matches) != 1: raise ValueError(f'Expected one compile command for {chunk}')
        cmd = matches[0]
        argv = shlex.split(cmd['command'])
        if '-DRECOMP_TSO=1' not in argv: raise ValueError('TSO baseline is required')
        gcc = Path(argv[0])
        ar = gcc.with_name('aarch64-none-elf-ar')
        obj = out/Path(cmd['output']).name
        argv[argv.index('-o')+1] = str(obj)
        if not args.no_force_inline: argv.insert(1, '-DRECOMP_INLINE_GUEST_MEMORY=1')
        if clang:
            sysroot = subprocess.check_output([str(gcc),'-print-sysroot'],text=True).strip()
            argv[0] = clang
            argv = [v for v in argv if v not in ['-mtp=soft','-ftls-model=local-exec']]
            argv[1:1] = ['--target=aarch64-none-elf','--sysroot='+sysroot,
                         '-mno-outline-atomics','-fno-math-errno','-ffp-contract=off']
        selected.append((cmd, argv, obj))
    done = 0
    progress_lock = threading.Lock()
    def compile_one(entry):
        nonlocal done
        cmd, argv, obj = entry
        record = {'source':cmd['file'],'source_sha256':sha(Path(cmd['file'])), 'object':obj.name,'command':argv}
        stamp = obj.with_suffix('.json')
        inputs = {'record':record,'headers':header_hashes}
        if not obj.exists() or not stamp.exists() or json.loads(stamp.read_text()) != inputs:
            with obj.with_suffix('.log').open('w') as log:
                subprocess.run(argv,cwd=cmd['directory'],stdout=log,stderr=log,check=True)
            stamp.write_text(json.dumps(inputs,sort_keys=True))
        with progress_lock:
            done += 1
            if done <= 4 or done % 25 == 0 or done == len(selected):
                print(f'Compiled{done}/{len(selected)}: {obj.name}',flush=True)
        return record
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        sources = list(pool.map(compile_one, selected))
    members = subprocess.check_output([str(ar),'t',str(args.baseline)],text=True).splitlines()
    for _, _, obj in selected:
        if members.count(obj.name) != 1:
            raise ValueError(f'Expected exactly one baseline member {obj.name}')
    shutil.copy2(args.baseline, archive)
    subprocess.run([str(ar),'r',str(archive),*[str(e[2]) for e in selected]],check=True)
    manifest = {'baseline':str(args.baseline.resolve()),'baseline_sha256':sha(args.baseline),
        'header_sha256':sha(ROOT/'runtime/include/recomp/x86.h'), 'tso':True,
        'inline_guest_memory':not args.no_force_inline, 'compiler':args.compiler, 'full_generated':args.all,
        'compiler_version':subprocess.check_output([selected[0][1][0],'--version'],text=True).splitlines()[0],
        'headers':header_hashes, 'sources':sources,'archive_sha256':sha(archive)}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('COMPLETE',archive,flush=True)
if __name__ == '__main__': main()
