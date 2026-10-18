#!/bin/bash
# Builds DXMT (D3D11 -> Metal, utmapp fork) natively per platform from the
# patched copy in build/src/dxmt (prepare_sources.sh):
#   build/dxmt/<platform>/lib/libdxmt-native.dylib   one image (LGPL: shipped as an
#                                                     embedded framework, not linked static)
#   build/dxmt/<platform>/include/dxmt-native/dxmt_native.h
#   build/dxmt/<platform>/bin/airconv                 DXBC -> metallib tool (macOS only)
#   build/dxmt/<platform>/dxmt.framework              iOS: the image as an embeddable framework
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
    cat > "$CROSS.new" <<EOF
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
    # Replaced only on a change: a newer cross file makes meson reconfigure.
    cmp -s "$CROSS.new" "$CROSS" && rm "$CROSS.new" || mv "$CROSS.new" "$CROSS"
    echo "== DXMT for $PLATFORM"
    if [ ! -f "$WORK/build.ninja" ]; then
        "$MESON" setup "$WORK" build/src/dxmt --cross-file "$CROSS" --buildtype=release --prefix="$OUT" \
            -Dnative_llvm_path="$LLVM" > "$WORK.setup.log" 2>&1 || { tail -30 "$WORK.setup.log" >&2; exit 1; }
    fi
    "$MESON" compile -C "$WORK" > "$WORK.build.log" 2>&1 || { grep -m30 -E "error|FAILED" "$WORK.build.log" >&2; exit 1; }
    "$MESON" install -C "$WORK" > /dev/null
    [ -x "$WORK/src/airconv/darwin/airconv" ] && mkdir -p "$OUT/bin" && cp "$WORK/src/airconv/darwin/airconv" "$OUT/bin/"
    if [ "$PLATFORM" = macos ]; then
        # Found through the app's rpath (Contents/Frameworks) or the build tree's.
        install_name_tool -id @rpath/libdxmt-native.dylib "$OUT/lib/libdxmt-native.dylib"
    else
        # iOS apps embed dynamic code as frameworks: Frameworks/dxmt.framework/dxmt.
        FW=$OUT/dxmt.framework
        rm -rf "$FW" && mkdir -p "$FW"
        cp "$OUT/lib/libdxmt-native.dylib" "$FW/dxmt"
        install_name_tool -id @rpath/dxmt.framework/dxmt "$FW/dxmt"
        cat > "$FW/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>dxmt</string>
<key>CFBundleIdentifier</key><string>org.dxmt.native</string>
<key>CFBundleName</key><string>dxmt</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleSupportedPlatforms</key><array><string>$( [ "$PLATFORM" = iphoneos ] && echo iPhoneOS || echo iPhoneSimulator )</string></array>
<key>MinimumOSVersion</key><string>$MIN_IOS</string>
</dict></plist>
PLIST
    fi
    echo "DXMT ($PLATFORM): $OUT"
done
