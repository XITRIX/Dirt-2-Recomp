#!/usr/bin/env python3
"""Prepare a private native Android DXVK tree without changing Apple/Switch copies."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
base = root / 'build/src/dxvk'
dest = root / 'build/src/dxvk-android'
build = root / 'build/dxvk/android-arm64'
sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Library/Android/sdk'))
ndk = Path(os.environ.get('ANDROID_NDK_HOME', sdk / 'ndk/29.0.14206865'))
tools = ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin'
patches = sorted((root / 'third_party/patches/dxvk-switch').glob('*.patch'))
patches += sorted((root / 'third_party/patches/dxvk-android').glob('*.patch'))
if not (base / '.dirt2-stamp').exists():
    raise SystemExit('Run tools/scripts/prepare_sources.sh first')
# Bump this version when source transformations below change. Cross flags may
# change independently without replacing the prepared source tree.
stamp = hashlib.sha256(b'android-native-v1' + (base / '.dirt2-stamp').read_bytes() + b''.join(p.read_bytes() for p in patches)).hexdigest()
marker = dest / '.android-native-stamp'
if dest.exists() and (not marker.exists() or marker.read_text() != stamp):
    raise SystemExit('Prepared Android source differs. Preserve local edits and remove build/src/dxvk-android before refreshing.')
if not dest.exists():
    shutil.copytree(base, dest, ignore=shutil.ignore_patterns('.git'))
    for patch in patches:
        subprocess.run(['git', 'apply', '--directory=' + str(dest.relative_to(root)), str(patch)], cwd=root, check=True)
    # Share the tested static D3D11/DXGI integration. Horizon-only hooks stay guarded.
    for file in ['src/d3d9/meson.build', 'src/d3d11/meson.build']:
        p = dest / file
        p.write_text(p.read_text().replace("platform == 'horizon'", "platform == 'horizon' or platform == 'android'"))
    p = dest / 'src/wsi/sdl3/wsi_platform_sdl3.cpp'
    p.write_text(p.read_text().replace('defined(__APPLE__) || defined(__SWITCH__)', 'defined(__APPLE__) || defined(__SWITCH__) || defined(__ANDROID__)'))
    p = dest / 'src/util/log/log.cpp'
    p.write_text(p.read_text().replace('dxvk-switch.log', 'dxvk-android.log'))
    marker.write_text(stamp)
prefix = root / 'build/deps/android-arm64'
cross = root / 'build/dxvk/android-arm64.cross'
cross.parent.mkdir(parents=True, exist_ok=True)
cross.write_text(f"""[binaries]
c = '{tools}/aarch64-linux-android28-clang'
cpp = '{tools}/aarch64-linux-android28-clang++'
ar = '{tools}/llvm-ar'
strip = '{tools}/llvm-strip'
pkg-config = 'pkg-config'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '{prefix}/lib/pkgconfig'
[built-in options]
b_staticpic = true
c_args = ['-I{prefix}/include']
cpp_args = ['-stdlib=libc++', '-I{prefix}/include']
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
""")
