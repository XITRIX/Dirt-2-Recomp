#!/bin/bash
# Primary Switch D3D11 -> DXVK -> NVK renderer. Requires the usual Switch
# dependencies/lifted archive, Python >=3.11, Meson, Ninja and glslang.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
MODE=${1:-all}
case "$MODE" in all|libraries|game) ;; *) echo "Usage: $0 [all|libraries|game]" >&2; exit 2 ;; esac
if [ "$MODE" != game ]; then
python3 tools/scripts/fetch_switch_nvk_sdk.py
python3 tools/scripts/prepare_switch_nvk.py
fi
export DEVKITPRO=$ROOT/build/nvk-sdk/root/opt/devkitpro
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/opt/local/bin:$PATH
SDK=$DEVKITPRO
PREFIX=$ROOT/build/deps/switch-nvk
WORK=$ROOT/build/deps/switch-nvk-work
mkdir -p "$WORK" "$ROOT/build/dxvk"
# Reuse a TSO-enabled Switch archive when present, otherwise build lifted code
# with the primary SDK. A D3D9 fallback build is not a prerequisite.
GENERATED_ARGS=()
if [ -n "${DIRT2_SWITCH_CPU_ARCHIVE:-}" ]; then
    [ -f "$DIRT2_SWITCH_CPU_ARCHIVE" ] || { echo "Missing CPU experiment archive" >&2; exit 1; }
    GENERATED_ARGS=(-DDIRT2_SWITCH_GENERATED_ARCHIVE="$DIRT2_SWITCH_CPU_ARCHIVE")
elif [ -f build/switch/libdirt2_gen.a ]; then
    GENERATED_ARGS=(-DDIRT2_SWITCH_GENERATED_ARCHIVE="$ROOT/build/switch/libdirt2_gen.a")
fi
if [ "$MODE" != game ]; then
bash tools/scripts/build_deps.sh switch
bash tools/scripts/prepare_sources.sh --dxvk-only
# All unchanged audio/video dependencies stay the same as the normal build.
[ -d "$PREFIX" ] || cp -R build/deps/switch "$PREFIX"
SDL_SRC=$WORK/SDL3-src
if [ ! -d "$SDL_SRC" ]; then
    cp -R build/deps/src/SDL3-3.4.18 "$SDL_SRC"
    for patchfile in third_party/patches/sdl3/*.patch; do patch -s -p1 -d "$SDL_SRC" < "$patchfile"; done
fi
# A previous manual prototype used the shared prepared SDL source. Only discard
# this prototype's generated CMake files when moving to the isolated source.
if [ -f "$WORK/sdl3/CMakeCache.txt" ] && ! rg -Fq "CMAKE_HOME_DIRECTORY:INTERNAL=$SDL_SRC" "$WORK/sdl3/CMakeCache.txt"; then
    rm -rf "$WORK/sdl3"
fi
cmake -S "$SDL_SRC" -B "$WORK/sdl3" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SDK/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_VULKAN=ON -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER_GPU=OFF -DSDL_GPU=OFF
cmake --build "$WORK/sdl3" --target install -j6
python3 tools/scripts/prepare_switch_dxvk.py
meson setup "$ROOT/build/dxvk/switch-dx11" "$ROOT/build/src/dxvk-switch" \
    --cross-file "$ROOT/build/dxvk/switch-dx11.cross" --buildtype=release \
    -Denable_d3d9=true -Denable_dxgi=true -Denable_d3d11=true -Denable_d3d10=false -Denable_d3d8=false \
    -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled
ninja -C build/dxvk/switch-dx11 -j6
python3 tools/scripts/prepare_switch_dxvk.py --archive
fi
[ "$MODE" != libraries ] || exit 0
cmake -S . -B build/switch-dxvk -G Ninja -DCMAKE_TOOLCHAIN_FILE="$SDK/cmake/Switch.cmake" \
    -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDIRT2_BUILD_TOOLS=OFF \
    -DDIRT2_SWITCH_DXVK=ON -DDIRT2_DEPS_DIR="$PREFIX" \
    "${GENERATED_ARGS[@]}"
if [ "${DIRT2_ENABLE_CCACHE:-0}" != 1 ]; then export CCACHE_DISABLE=1; fi
cmake --build build/switch-dxvk --target dirt2_nro -j6
bash tools/scripts/package_switch.sh
