#!/bin/bash
# ARM64 Android game build. Uses SDK/NDK and the same lifted sources as Apple/Switch.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
export ANDROID_NDK_HOME=${ANDROID_NDK_HOME:-$SDK/ndk/29.0.14206865}
export CCACHE_DISABLE=1
export PATH=/opt/homebrew/bin:/opt/local/bin:$PATH
JOBS=${DIRT2_BUILD_JOBS:-6}
bash tools/scripts/build_android_deps.sh
bash tools/scripts/prepare_sources.sh --dxvk-only
python3 tools/scripts/prepare_android_dxvk.py
meson setup build/dxvk/android-arm64 build/src/dxvk-android \
 --cross-file build/dxvk/android-arm64.cross --buildtype=release \
 -Denable_d3d9=true -Denable_dxgi=true -Denable_d3d11=true -Denable_d3d10=false -Denable_d3d8=false \
 -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled
ninja -C build/dxvk/android-arm64 -j"$JOBS"
python3 - <<'PY'
import os,subprocess
from pathlib import Path
build=Path('build/dxvk/android-arm64').resolve();out=build/'libdxvk_android.a'
ar=Path(os.environ['ANDROID_NDK_HOME'])/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar'
libs=sorted(p for p in build.rglob('*.a') if p!=out)
if not libs:raise SystemExit('Missing DXVK archives')
subprocess.run([str(ar),'-M'],input='CREATE '+str(out)+'\n'+''.join('ADDLIB '+str(p)+'\n' for p in libs)+'SAVE\nEND\n',text=True,check=True)
PY
cmake --preset android-arm64
cmake --build --preset android-arm64 -j"$JOBS"
python3 tools/scripts/package_android.py
