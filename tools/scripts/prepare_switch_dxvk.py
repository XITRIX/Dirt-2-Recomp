#!/usr/bin/env python3
"""Prepare/merge the separate native Switch DXVK build; never edit Apple sources."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/src/dxvk'
SWITCH2 = '--switch2' in sys.argv
DEST = ROOT / ('build/src/dxvk-switch2' if SWITCH2 else 'build/src/dxvk-switch')
BUILD = ROOT / ('build/dxvk/switch2' if SWITCH2 else 'build/dxvk/switch-dx11')
SDK = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))
DEPS = ROOT / ('build/deps/switch2' if SWITCH2 else 'build/deps/switch-nvk')
NX_ROOT = SDK / 'libounce' if SWITCH2 else ROOT / 'build/nvk-sdk/root/opt/devkitpro/libnx'
PATCHES = sorted((ROOT / 'third_party/patches/dxvk-switch').glob('*.patch'))
AR = str(SDK / 'devkitA64/bin/aarch64-none-elf-ar')


def main():
    if '--archive' in sys.argv:
        output = BUILD / ('libdxvk_switch2.a' if SWITCH2 else 'libdxvk_switch.a')
        libraries = sorted(p for p in BUILD.rglob('*.a') if p != output)
        if not libraries:
            raise SystemExit('Build the DXVK static libraries first')
        # GNU ar MRI preserves objects with equal basenames from input archives.
        def archive_path(path):
            if sys.platform in ('msys', 'cygwin'):
                return subprocess.check_output(['cygpath', '-m', str(path)], text=True).strip()
            return str(path)
        script = f'CREATE {archive_path(output)}\n' + ''.join(f'ADDLIB {archive_path(p)}\n' for p in libraries) + 'SAVE\nEND\n'
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
                result = subprocess.run([sys.executable, str(ROOT / 'tools/scripts/apply_source_patch.py'), str(DEST), str(patch), '--reverse-check'], capture_output=True)
                if result.returncode:
                    raise SystemExit('Switch DXVK copy differs: preserve edits and remove build/src/dxvk-switch before refreshing')
        else:
            shutil.copytree(BASE, DEST, ignore=shutil.ignore_patterns('.git'))
            subprocess.run(['git', 'init', '-q', str(DEST)], check=True)
            for patch in PATCHES:
                subprocess.run([sys.executable, str(ROOT / 'tools/scripts/apply_source_patch.py'), str(DEST), str(patch)], check=True)
        marker.write_text(stamp)
    flags = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-ftls-model=local-exec',
             '-ffunction-sections', '-fdata-sections', '-D__SWITCH__',
             '-I' + str(DEPS / 'include'), '-I' + str(NX_ROOT / 'include')]
    if SWITCH2:
        flags = ['-mcpu=cortex-a78c+crypto'] + flags[2:] + ['-D__SWITCH2__', '-I' + str(SDK / 'portlibs/switch2/include')]
    cross = """[binaries]
c = '""" + str(SDK / 'devkitA64/bin/aarch64-none-elf-gcc') + """'
cpp = '""" + str(SDK / 'devkitA64/bin/aarch64-none-elf-g++') + """'
ar = '""" + str(SDK / 'devkitA64/bin/aarch64-none-elf-ar') + """'
strip = '""" + str(SDK / 'devkitA64/bin/aarch64-none-elf-strip') + """'
pkg-config = 'pkg-config'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '""" + str(DEPS / 'lib/pkgconfig') + """'
[built-in options]
c_args = """ + repr(flags) + '\ncpp_args = ' + repr(flags) + """
[host_machine]
system = 'horizon'
cpu_family = 'aarch64'
cpu = '""" + ('cortex-a78c' if SWITCH2 else 'cortex-a57') + """'
endian = 'little'
"""
    target = ROOT / ('build/dxvk/switch2.cross' if SWITCH2 else 'build/dxvk/switch-dx11.cross')
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text() != cross:
        target.write_text(cross)


if __name__ == '__main__':
    main()
