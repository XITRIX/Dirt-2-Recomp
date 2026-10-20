#!/bin/bash
# Install APK, optionally copy an existing PC game folder; never includes it in APK.
set -euo pipefail
cd "$(dirname "$0")/../.."
ADB=${ADB:-adb}
"$ADB" install -r build/package/android/DiRT2-android-arm64-debug.apk
if [ $# -gt 0 ]; then
 [ -f "$1/dirt2_game.exe" ] || { echo 'Expected a game folder containing dirt2_game.exe' >&2; exit 1; }
 "$ADB" shell mkdir -p '/sdcard/Android/data/com.daniilvinogradov.dirt2/files/Dirt 2'
 "$ADB" push "$1/." '/sdcard/Android/data/com.daniilvinogradov.dirt2/files/Dirt 2'
fi
"$ADB" shell am start -n com.daniilvinogradov.dirt2/.Dirt2Activity
