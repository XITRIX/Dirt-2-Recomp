#!/bin/bash
# Isolated Switch 2 build: libounce + OunceVK. No Switch 1 NVK SDK download.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
MODE=${1:-all}
case "$MODE" in all|libraries|game|cpu-hot-inline|cpu-hot-clang|cpu-render-clang|probe) ;; *) echo "Usage: $0 [all|libraries|game|cpu-hot-inline|cpu-hot-clang|cpu-render-clang|probe]" >&2; exit 2 ;; esac
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
TOOLCHAIN="$DEVKITPRO/libounce/cmake/Switch2.cmake"
[ -f "$TOOLCHAIN" ] || { echo "Install the ounce SDK in DEVKITPRO first" >&2; exit 1; }
JOBS=${DIRT2_BUILD_JOBS:-4}
if [ "$MODE" = all ] || [ "$MODE" = libraries ]; then
    bash tools/scripts/build_deps.sh switch2
    bash tools/scripts/prepare_sources.sh --dxvk-only
    python3 tools/scripts/prepare_switch_dxvk.py --switch2
    if [ -f build/dxvk/switch2/meson-private/coredata.dat ]; then RECONFIGURE=(--reconfigure); else RECONFIGURE=(); fi
    meson setup "${RECONFIGURE[@]}" build/dxvk/switch2 build/src/dxvk-switch2         --cross-file build/dxvk/switch2.cross --buildtype=release         -Denable_d3d9=true -Denable_dxgi=true -Denable_d3d11=true -Denable_d3d10=false -Denable_d3d8=false         -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled
    ninja -C build/dxvk/switch2 -j"$JOBS"
    python3 tools/scripts/prepare_switch_dxvk.py --switch2 --archive
fi
[ "$MODE" != libraries ] || exit 0
# Source builds are the default; an experimental archive is selected explicitly.
cmake --preset switch2 -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" -DDIRT2_SWITCH_GENERATED_ARCHIVE=
if [ "$MODE" = cpu-hot-inline ] || [ "$MODE" = cpu-hot-clang ] || [ "$MODE" = cpu-render-clang ]; then
    CPU_COMPILER=gcc
    CPU_OUTPUT=build/switch2-cpu-hot-inline
    if [ "$MODE" = cpu-hot-clang ]; then
        CPU_COMPILER=clang
        CPU_OUTPUT=build/switch2-cpu-hot-clang
    fi
    if [ "$MODE" = cpu-render-clang ]; then
        CPU_COMPILER=clang
        CPU_OUTPUT=build/switch2-cpu-render-clang
    fi
    RENDER_CHUNKS=()
    if [ "$MODE" = cpu-render-clang ]; then
        # Guest260 PC samples: render routines and their rd/wr helper callers.
        RENDER_CHUNKS=(gen_0436 gen_0495 gen_0435 gen_0432 gen_0302 gen_0354
                       gen_0421 gen_0022 gen_0424 gen_0109 gen_0020 gen_0119)
    fi
    cmake --build build/switch2 --target dirt2_gen -j"$JOBS"
    # Switch 2 worker264 PC profile, steady race180-225s (2026-11-13).
    # Preserve all original flags, especially TSO and floating-point behavior.
    python3 tools/scripts/build_switch_cpu_variant.py \
        --baseline build/switch2/libdirt2_gen.a --commands build/switch2/compile_commands.json \
        --output "$CPU_OUTPUT" --compiler "$CPU_COMPILER" --jobs "$JOBS" \
        --chunks gen_0453 gen_0446 gen_0455 gen_0232 gen_0298 gen_0300 \
                 gen_0110 gen_0241 gen_0448 gen_0450 gen_0449 gen_0273 "${RENDER_CHUNKS[@]}"
    cmake --preset switch2 -DDIRT2_SWITCH_GENERATED_ARCHIVE="$ROOT/$CPU_OUTPUT/libdirt2_gen.a"
fi
if [ "$MODE" = probe ]; then
    cmake --build build/switch2 --target dirt2_dx11_probe_nro -j"$JOBS"
    python3 tools/scripts/package_switch2.py --probe --devkitpro "$DEVKITPRO"
else
    [ -f generated/sources.txt ] || { echo "Lift your DiRT 2 v1.1 executable into generated/ first; use probe mode without game data" >&2; exit 1; }
    cmake --build --preset switch2 -j"$JOBS"
    python3 tools/scripts/package_switch2.py --devkitpro "$DEVKITPRO"
fi
