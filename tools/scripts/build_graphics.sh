#!/bin/bash
# Builds the graphics stack per platform from the patched copies in build/src
# (prepare_sources.sh; the submodules stay as upstream):
#   MoltenVK (static)          -> build/moltenvk/<platform>   (build_moltenvk.sh)
#   DXVK native d3d9 (static)  -> build/dxvk/<platform>/libdxvk_d3d9.a, one archive
#                                 with DXVK's internal libraries merged in
# Platforms: macos, iphoneos, iphonesimulator. Each has its own output and work
# folders, so several can build at the same time.
# Needs build_deps.sh first: DXVK compiles against SDL3's headers from build/deps.
# Usage: build_graphics.sh [platform ...]   (default: macos)
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
MIN_IOS=16.0

tools/scripts/prepare_sources.sh

for PLATFORM in ${*:-macos}; do
    tools/scripts/build_moltenvk.sh "$PLATFORM"

    BLD=$ROOT/build/dxvk/$PLATFORM
    MESON_ARGS=(--buildtype=release -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false
        -Denable_d3d11=false -Denable_d3d9=true -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled)
    case $PLATFORM in
        macos | iphoneos | iphonesimulator)
            case $PLATFORM in
                macos) TRIPLE=arm64-apple-macos12.0 XSDK=macosx ;;
                iphoneos) TRIPLE=arm64-apple-ios$MIN_IOS XSDK=iphoneos ;;
                *) TRIPLE=arm64-apple-ios$MIN_IOS-simulator XSDK=iphonesimulator ;;
            esac
            SYSROOT=$(xcrun --sdk "$XSDK" --show-sdk-path)
            CROSS=$ROOT/build/dxvk/$PLATFORM.cross
            mkdir -p "$ROOT/build/dxvk"
            cat > "$CROSS" <<EOF
[binaries]
c = ['xcrun', '--sdk', '$XSDK', 'clang', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
cpp = ['xcrun', '--sdk', '$XSDK', 'clang++', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
objc = ['xcrun', '--sdk', '$XSDK', 'clang', '-target', '$TRIPLE', '-isysroot', '$SYSROOT']
ar = ['xcrun', '--sdk', '$XSDK', 'ar']
strip = ['xcrun', '--sdk', '$XSDK', 'strip']
pkg-config = 'pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$ROOT/build/deps/$PLATFORM/lib/pkgconfig'

[host_machine]
system = 'darwin'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF
            MESON_ARGS+=(--cross-file "$CROSS")
            ;;
        *) echo "unknown platform $PLATFORM" >&2; exit 1 ;;
    esac
    [ -d "$BLD" ] || (cd build/src/dxvk && meson setup "$BLD" "${MESON_ARGS[@]}")
    ninja -C "$BLD"
    # One archive for the app: d3d9 plus every internal static library it uses.
    find "$BLD" -name '*.a' ! -path "$BLD/libdxvk_d3d9.a" -print0 | xargs -0 libtool -static -no_warning_for_no_symbols \
        -o "$BLD/libdxvk_d3d9.a" 2> >(grep -v "same member name" >&2)
    echo "DXVK ($PLATFORM): $BLD/libdxvk_d3d9.a"
done
