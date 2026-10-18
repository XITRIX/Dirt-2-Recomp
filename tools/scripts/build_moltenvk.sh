#!/bin/bash
# Builds MoltenVK as a static library for one platform into
# build/moltenvk/<platform>/{lib/libMoltenVK.a,include}; it is linked into the
# executable, where DXVK and SDL find vkGetInstanceProcAddr in the process.
# Platforms: macos, iphoneos, iphonesimulator. Each platform has its own Xcode derived data, so platforms can
# build at the same time. MoltenVK's own Package/ folder is not used.
# Usage: build_moltenvk.sh <platform>
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
PLATFORM=${1:?platform: macos, iphoneos or iphonesimulator}
MVK=$ROOT/build/src/MoltenVK  # patched copy (prepare_sources.sh)
DD=${DIRT2_XCODE_DD:-$HOME/Library/Caches/Dirt2Recomp/xcode}/moltenvk-$PLATFORM
OUT=$ROOT/build/moltenvk/$PLATFORM

case $PLATFORM in
    macos) scheme=MoltenVK-macOS-static dest=macOS products=Release ;;
    iphoneos) scheme=MoltenVK-iOS-static dest=iOS products=Release-iphoneos ;;
    iphonesimulator) scheme=MoltenVK-iOS-static dest="iOS Simulator" products=Release-iphonesimulator ;;
    *) echo "unknown platform $PLATFORM" >&2; exit 1 ;;
esac

tools/scripts/prepare_sources.sh
tools/scripts/build_mvk_externals.sh
echo "== MoltenVK ($PLATFORM)"
(cd "$MVK" && xcodebuild -project MoltenVK/MoltenVK.xcodeproj -scheme "$scheme" \
    -destination "generic/platform=$dest" -configuration Release -derivedDataPath "$DD" -quiet build)

rm -rf "$OUT" && mkdir -p "$OUT/lib"
cp "$DD/Build/Products/$products/libMoltenVK.a" "$OUT/lib/"
cp -R "$MVK/MoltenVK/include" "$OUT/include"
echo "MoltenVK ($PLATFORM): $OUT/lib/libMoltenVK.a"
