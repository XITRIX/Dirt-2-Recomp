#!/bin/bash
# Compiles the game's own icon (dirt2.ico from the game folder; game data stays
# out of the repo) into the app bundle's asset catalog as AppIcon.
# Usage: ios_app_icon.sh <dirt2.ico> <app bundle> <iphoneos|iphonesimulator> <work dir>
set -euo pipefail
ICO=$1 APP=$2 SDK=$3 WORK=$4
if [ ! -f "$ICO" ]; then
    echo "ios_app_icon: $ICO not found, app keeps the default icon" >&2
    exit 0
fi
SET=$WORK/Assets.xcassets/AppIcon.appiconset
rm -rf "$WORK/Assets.xcassets" && mkdir -p "$SET"
# Largest frame of the .ico, scaled to the single 1024 px source iOS wants
# (opaque: home screen icons cannot have alpha).
sips -s format png "$ICO" --out "$WORK/icon_src.png" > /dev/null
sips -z 1024 1024 "$WORK/icon_src.png" --out "$WORK/icon_1024.png" > /dev/null
sips -s format jpeg "$WORK/icon_1024.png" --out "$WORK/icon_1024.jpg" > /dev/null
sips -s format png "$WORK/icon_1024.jpg" --out "$SET/icon_1024.png" > /dev/null
cat > "$SET/Contents.json" <<'JSON'
{
  "images": [{ "filename": "icon_1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" }],
  "info": { "author": "xcode", "version": 1 }
}
JSON
cat > "$WORK/Assets.xcassets/Contents.json" <<'JSON'
{ "info": { "author": "xcode", "version": 1 } }
JSON
xcrun actool "$WORK/Assets.xcassets" --compile "$APP" --platform "$SDK" --minimum-deployment-target 16.0 \
    --app-icon AppIcon --target-device iphone --target-device ipad \
    --output-partial-info-plist "$WORK/assets-partial.plist" --output-format human-readable-text > "$WORK/actool.log"
