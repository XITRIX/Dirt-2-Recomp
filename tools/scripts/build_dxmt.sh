#!/bin/bash
# Builds DXMT (D3D11 -> Metal, utmapp fork) natively per platform from the
# patched copy in build/src/dxmt (prepare_sources.sh):
#   build/dxmt/<platform>/lib/libdxmt-native.dylib   one image (LGPL: shipped as an
#                                                     embedded framework, not linked static)
#   build/dxmt/<platform>/include/dxmt-native/dxmt_native.h
#   build/dxmt/<platform>/bin/airconv                 DXBC -> metallib tool (macOS only)
# Needs build_llvm.sh <platform> first. Platforms: macos, iphoneos, iphonesimulator.
# Usage: build_dxmt.sh [platform ...]   (default: macos)
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
MIN_IOS=16.0
MIN_MACOS=13.0  # DXMT's floor (bindless argument buffers, mesh shaders, MetalFX)

tools/scripts/prepare_sources.sh

# DXMT needs meson >= 1.3 (the system one may be older): a pinned copy in a venv.
MESON=$ROOT/build/tools/venv/bin/meson
if [ ! -x "$MESON" ]; then
    python3 -m venv "$ROOT/build/tools/venv"
    "$ROOT/build/tools/venv/bin/pip" install -q meson==1.6.1
fi

for PLATFORM in ${*:-macos}; do
    LLVM=$ROOT/build/llvm/$PLATFORM
    [ -f "$LLVM/lib/libLLVMCore.a" ] || { echo "error: run tools/scripts/build_llvm.sh $PLATFORM first" >&2; exit 1; }
    case $PLATFORM in
        macos) TRIPLE=arm64-apple-macos$MIN_MACOS XSDK=macosx SUBSYSTEM=macos ;;
        iphoneos) TRIPLE=arm64-apple-ios$MIN_IOS XSDK=iphoneos SUBSYSTEM=ios ;;
        iphonesimulator) TRIPLE=arm64-apple-ios$MIN_IOS-simulator XSDK=iphonesimulator SUBSYSTEM=ios-simulator ;;
        *) echo "error: unknown platform $PLATFORM" >&2; exit 1 ;;
    esac
    SYSROOT=$(xcrun --sdk "$XSDK" --show-sdk-path)
    OUT=$ROOT/build/dxmt/$PLATFORM
    WORK=$ROOT/build/dxmt/work-$PLATFORM
    CROSS=$ROOT/build/dxmt/$PLATFORM.cross
    mkdir -p "$ROOT/build/dxmt"
    cat > "$CROSS" <<EOF
[binaries]
c = ['xcrun', '--sdk', '$XSDK', 'clang', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
cpp = ['xcrun', '--sdk', '$XSDK', 'clang++', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
objc = ['xcrun', '--sdk', '$XSDK', 'clang', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
objcpp = ['xcrun', '--sdk', '$XSDK', 'clang++', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
ar = ['xcrun', '--sdk', '$XSDK', 'ar']
strip = ['xcrun', '--sdk', '$XSDK', 'strip']

[host_machine]
system = 'darwin'
subsystem = '$SUBSYSTEM'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF
    echo "== DXMT for $PLATFORM"
    if [ ! -f "$WORK/build.ninja" ]; then
        "$MESON" setup "$WORK" build/src/dxmt --cross-file "$CROSS" --buildtype=release --prefix="$OUT" \
            -Dnative_llvm_path="$LLVM" > "$WORK.setup.log" 2>&1 || { tail -30 "$WORK.setup.log" >&2; exit 1; }
    fi
    "$MESON" compile -C "$WORK" > "$WORK.build.log" 2>&1 || { grep -m30 -E "error|FAILED" "$WORK.build.log" >&2; exit 1; }
    "$MESON" install -C "$WORK" > /dev/null
    [ -x "$WORK/src/airconv/darwin/airconv" ] && mkdir -p "$OUT/bin" && cp "$WORK/src/airconv/darwin/airconv" "$OUT/bin/"
    echo "DXMT ($PLATFORM): $OUT"
done
