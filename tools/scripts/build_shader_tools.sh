#!/bin/bash
# Builds the host shader tools for the Switch renderer (docs/SWITCH.md):
#   build/tools/dx9shader/dx9shader   D3D9 bytecode -> GLSL 4.60 (tools/dx9shader)
# from MojoShader at a pinned commit (checksummed tarball, plus
# third_party/patches/mojoshader) and the SPIRV-Cross
# copy MoltenVK pins (build/src/MoltenVK/External/SPIRV-Cross, prepared by
# build_graphics.sh). uam (GLSL -> deko3d DKSH) comes with devkitPro.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SRC=$ROOT/build/src
MOJO_COMMIT=ad5dff84830c2863c841f4b1f4e3df78c705b383  # 2026-09-03
MOJO_SHA256=728ff9098115059c4012895b7a85065688c857fdadf9fbc04484fec8c7be3081
MOJO=$SRC/mojoshader-$MOJO_COMMIT
SPIRV_CROSS=$SRC/MoltenVK/External/SPIRV-Cross
[ -f "$SPIRV_CROSS/spirv_glsl.cpp" ] || { echo "no $SPIRV_CROSS: run tools/scripts/build_graphics.sh macos first" >&2; exit 1; }

# Patched copy (third_party/patches/mojoshader), prepared again when the patches change.
hash=$(cat "$ROOT"/third_party/patches/mojoshader/*.patch | shasum | cut -d' ' -f1)
[ "$(cat "$MOJO.patched" 2>/dev/null)" = "$hash" ] || rm -rf "$MOJO" "$MOJO.patched"
if [ ! -d "$MOJO" ]; then
    mkdir -p "$SRC"
    tmp=$(mktemp -d)
    curl -fsSL "https://github.com/icculus/mojoshader/archive/$MOJO_COMMIT.tar.gz" -o "$tmp/mojo.tar.gz"
    echo "$MOJO_SHA256  $tmp/mojo.tar.gz" | shasum -a 256 -c --quiet || { echo "error: MojoShader checksum mismatch" >&2; exit 1; }
    tar -xzf "$tmp/mojo.tar.gz" -C "$tmp"
    for p in "$ROOT"/third_party/patches/mojoshader/*.patch; do patch -s -p1 -d "$tmp/mojoshader-$MOJO_COMMIT" < "$p"; done
    mv "$tmp/mojoshader-$MOJO_COMMIT" "$MOJO"
    rm -rf "$tmp"
    echo "$hash" > "$MOJO.patched"
fi

cmake -S "$ROOT/tools/dx9shader" -B "$ROOT/build/tools/dx9shader" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMOJOSHADER_DIR="$MOJO" -DSPIRV_CROSS_DIR="$SPIRV_CROSS" > /dev/null
cmake --build "$ROOT/build/tools/dx9shader"
echo "build/tools/dx9shader/dx9shader"
