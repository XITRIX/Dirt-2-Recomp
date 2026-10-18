#!/bin/bash
# Packages the macOS build as an app and zip (ad hoc signed, no developer identity):
#   DiRT2.app/Contents/MacOS/dirt2                    the recompiled game; every third-party
#                                                     library but DXMT is linked in statically
#   DiRT2.app/Contents/Frameworks/libdxmt-native.dylib  DXMT (DX11 -> Metal), LGPL, kept separate
#   DiRT2.app/Contents/Resources/gamecontrollerdb.txt   SDL gamepad mappings
#   DiRT2.app/Contents/Resources/dirt2.{dxvk.*,pipelines,dxmt-pipelines}, dxmt/
#                                                     pipeline warm-up data, if build/pipelines has it
#   DiRT2.app/Contents/Resources/dirt2.icns           from the game's dirt2.ico, if found
# Game data is not included: the app looks for a "Dirt 2" folder next to itself,
# then the folder chosen last time, then asks (platform/sdl/sdl_main.cpp).
# Usage: package_macos.sh [out_dir] [game folder (for the icon)]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=${1:-$ROOT/build/package/macos}
GAME=${2:-${DIRT2_GAME_DIR:-$ROOT/Colin McRae DiRT 2 (v.1.1) [Steam-Rip]/Dirt 2}}
APP=$OUT/DiRT2.app
BIN=$ROOT/build/macos-arm64/dirt2
DXMT=$ROOT/build/dxmt/macos/lib/libdxmt-native.dylib

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/dirt2"

# Only system libraries and the embedded DXMT may be loaded.
foreign=$(otool -L "$APP/Contents/MacOS/dirt2" | tail -n +2 | awk '{print $1}' |
    grep -vE '^(/usr/lib/|/System/|@rpath/libdxmt-native\.dylib)' || true)
if [ -n "$foreign" ]; then
    echo "error: dirt2 links non-system libraries:" >&2
    echo "$foreign" >&2
    exit 1
fi
if otool -L "$APP/Contents/MacOS/dirt2" | grep -q libdxmt-native; then
    [ -f "$DXMT" ] || { echo "error: $DXMT missing (tools/scripts/build_dxmt.sh macos)" >&2; exit 1; }
    cp "$DXMT" "$APP/Contents/Frameworks/"
fi

cp "$ROOT/third_party/SDL_GameControllerDB/gamecontrollerdb.txt" "$APP/Contents/Resources/"
for f in dirt2.dxvk.bin dirt2.dxvk.lut dirt2.pipelines dirt2.dxmt-pipelines; do
    if [ -f "$ROOT/build/pipelines/$f" ]; then cp "$ROOT/build/pipelines/$f" "$APP/Contents/Resources/"; fi
done
if [ -d "$ROOT/build/pipelines/dxmt" ]; then cp -R "$ROOT/build/pipelines/dxmt" "$APP/Contents/Resources/"; fi

ICON_KEY=
if [ -f "$GAME/dirt2.ico" ]; then
    WORK=$(mktemp -d)
    if sips -s format png "$GAME/dirt2.ico" --out "$WORK/icon.png" >/dev/null 2>&1; then
        mkdir -p "$WORK/dirt2.iconset"
        for sz in 16 32 128 256 512; do
            sips -z $sz $sz "$WORK/icon.png" --out "$WORK/dirt2.iconset/icon_${sz}x${sz}.png" >/dev/null
            sips -z $((sz * 2)) $((sz * 2)) "$WORK/icon.png" --out "$WORK/dirt2.iconset/icon_${sz}x${sz}@2x.png" >/dev/null
        done
        iconutil -c icns "$WORK/dirt2.iconset" -o "$APP/Contents/Resources/dirt2.icns" && ICON_KEY="<key>CFBundleIconFile</key><string>dirt2</string>"
    fi
    rm -rf "$WORK"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>dirt2</string>
<key>CFBundleIdentifier</key><string>${DIRT2_BUNDLE_ID:-com.daniilvinogradov.dirt2recomp}</string>
<key>CFBundleName</key><string>DiRT 2</string>
<key>CFBundleDisplayName</key><string>DiRT 2</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>LSApplicationCategoryType</key><string>public.app-category.racing-games</string>
<key>LSSupportsGameMode</key><true/>
<key>NSHighResolutionCapable</key><true/>
$ICON_KEY
</dict></plist>
PLIST

xattr -cr "$APP" 2>/dev/null || true
for lib in "$APP"/Contents/Frameworks/*.dylib; do [ -f "$lib" ] && codesign --force --sign - "$lib"; done
codesign --force --sign - "$APP"
(cd "$OUT" && rm -f DiRT2-macOS.zip && ditto -c -k --keepParent DiRT2.app DiRT2-macOS.zip)
ls -la "$OUT/DiRT2-macOS.zip"
