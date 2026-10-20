#!/usr/bin/env python3
"""Prepare/merge the separate native Switch DXVK build; never edit Apple sources."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/src/dxvk'
DEST = ROOT / 'build/src/dxvk-switch'
BUILD = ROOT / 'build/dxvk/switch-dx11'
PATCHES = sorted((ROOT / 'third_party/patches/dxvk-switch').glob('*.patch'))
AR = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar'


def main():
    if '--archive' in sys.argv:
        output = BUILD / 'libdxvk_switch.a'
        libraries = sorted(p for p in BUILD.rglob('*.a') if p != output)
        if not libraries:
            raise SystemExit('Build the DXVK static libraries first')
        # GNU ar MRI preserves objects with equal basenames from input archives.
        script = f'CREATE {output}\n' + ''.join(f'ADDLIB {p}\n' for p in libraries) + 'SAVE\nEND\n'
        subprocess.run([AR, '-M'], input=script, text=True, check=True)
        print('Merged native D3D9/D3D11/DXGI and internal libraries:', output)
        return
    if not (BASE / '.dirt2-stamp').exists():
        raise SystemExit('Run tools/scripts/prepare_sources.sh first')
    stamp = hashlib.sha256(b''.join(p.name.encode() + p.read_bytes() for p in PATCHES) + (BASE / '.dirt2-stamp').read_bytes()).hexdigest()
    marker = DEST / '.switch-native-stamp'
    if not marker.exists() or marker.read_text() != stamp:
        if DEST.exists():
            # Accept an already applied, matching prototype patch. Never erase
            # potentially useful local changes to a third-party source copy.
            for patch in PATCHES:
                result = subprocess.run(['git', '-C', str(DEST), 'apply', '--reverse', '--check', str(patch)], capture_output=True)
                if result.returncode:
                    raise SystemExit('Switch DXVK copy differs: preserve edits and remove build/src/dxvk-switch before refreshing')
        else:
            shutil.copytree(BASE, DEST, ignore=shutil.ignore_patterns('.git'))
            subprocess.run(['git', 'init', '-q', str(DEST)], check=True)
            for patch in PATCHES:
                subprocess.run(['git', '-C', str(DEST), 'apply', str(patch)], check=True)
        marker.write_text(stamp)
    flags = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-ftls-model=local-exec',
             '-ffunction-sections', '-fdata-sections', '-D__SWITCH__',
             '-I' + str(ROOT / 'build/deps/switch-nvk/include'),
             '-I' + str(ROOT / 'build/nvk-sdk/root/opt/devkitpro/libnx/include')]
    cross = """[binaries]
c = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc'
cpp = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++'
ar = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar'
strip = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-strip'
pkg-config = 'pkg-config'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '""" + str(ROOT / 'build/deps/switch-nvk/lib/pkgconfig') + """'
[built-in options]
c_args = """ + repr(flags) + '\ncpp_args = ' + repr(flags) + """
[host_machine]
system = 'horizon'
cpu_family = 'aarch64'
cpu = 'cortex-a57'
endian = 'little'
"""
    target = ROOT / 'build/dxvk/switch-dx11.cross'
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text() != cross:
        target.write_text(cross)


if __name__ == '__main__':
    main()
