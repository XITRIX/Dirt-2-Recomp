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
# Up to date when the output was built from the current patched copy (its
# stamp: submodule commit and patches); the Xcode derived data may be gone (CI).
STAMP=$OUT/.dirt2-stamp
want="$(cat "$MVK/.dirt2-stamp") $(xcodebuild -version | tr '\n' ' ')"
if [ -f "$OUT/lib/libMoltenVK.a" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
    echo "== MoltenVK ($PLATFORM): up to date"
    exit 0
fi
echo "== MoltenVK ($PLATFORM)"
(cd "$MVK" && xcodebuild -project MoltenVK/MoltenVK.xcodeproj -scheme "$scheme" \
    -destination "generic/platform=$dest" -configuration Release -derivedDataPath "$DD" -quiet build)

rm -rf "$OUT" && mkdir -p "$OUT/lib"
cp "$DD/Build/Products/$products/libMoltenVK.a" "$OUT/lib/"
cp -RL "$MVK/MoltenVK/include" "$OUT/include"  # dereference: the links are relative to the MoltenVK tree
echo "$want" > "$STAMP"
echo "MoltenVK ($PLATFORM): $OUT/lib/libMoltenVK.a"
