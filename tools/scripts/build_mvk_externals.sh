#!/bin/bash
# Builds MoltenVK's external libraries (SPIRV-Cross with our patch, SPIRV-Tools)
# for every Apple platform we target and assembles the multi-platform
# External/build/Release/*.xcframework that MoltenVK's project references by a
# fixed path. That xcframework is a read-only input to the per-platform MoltenVK
# builds (build_moltenvk.sh); it is rebuilt only when the patches change, under
# a lock, so concurrent platform builds never see it half-written.
#
# Xcode's derived data lives outside ~/Documents: when Documents is synced by
# iCloud Drive, Xcode's SQLite build database fails with "disk I/O error".
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
MVK=$ROOT/build/src/MoltenVK  # patched copy (prepare_sources.sh)
DD=${DIRT2_XCODE_DD:-$HOME/Library/Caches/Dirt2Recomp/xcode}/mvk-externals
OUT=$MVK/External/build/Release
STAMP=$OUT/.dirt2-stamp

LOCK=$ROOT/build/.mvk-externals.lock
mkdir -p "$ROOT/build"
until mkdir "$LOCK" 2>/dev/null; do sleep 2; done
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT

tools/scripts/prepare_sources.sh

want=$(cat third_party/patches/SPIRV-Cross/*.patch | shasum | cut -d' ' -f1)
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want" ]; then
    echo "MoltenVK externals up to date"
    exit 0
fi

for dest in macOS iOS "iOS Simulator"; do
    case $dest in
        macOS) scheme=ExternalDependencies-macOS ;;
        *) scheme=ExternalDependencies-iOS ;;
    esac
    echo "== MoltenVK externals: $dest"
    (cd "$MVK" && xcodebuild -project ExternalDependencies.xcodeproj -scheme "$scheme" \
        -destination "generic/platform=$dest" -configuration Release -derivedDataPath "$DD/$dest" -quiet build)
done

mkdir -p "$OUT"
for lib in SPIRVCross SPIRVTools; do
    args=()
    for a in "$DD"/*/Build/Products/Release*/lib$lib.a; do
        [ -f "$a" ] && args+=(-library "$a")
    done
    tmp=$OUT/.tmp/$lib.xcframework
    rm -rf "$tmp"
    xcodebuild -quiet -create-xcframework "${args[@]}" -output "$tmp"
    rm -rf "$OUT/$lib.xcframework" && mv "$tmp" "$OUT/$lib.xcframework"
done
ln -sfn Release "$MVK/External/build/Latest"
echo "$want" > "$STAMP"
plutil -extract AvailableLibraries json -o - "$OUT/SPIRVCross.xcframework/Info.plist" |
    grep -o '"LibraryIdentifier":"[^"]*"' | sed 's/.*:"/  slice: /; s/"$//'
