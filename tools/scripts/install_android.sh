#!/bin/bash
# Install APK, optionally copy an existing PC game folder; never includes it in APK.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ -z "${ADB:-}" ]; then
 ADB=${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb
 [ -x "$ADB" ] || ADB=adb
fi
# Complete the APK copy before installation; subsequent local repackaging is safe.
"$ADB" install --no-incremental -r build/package/android/DiRT2-android-arm64-debug.apk
if [ $# -gt 0 ]; then
 [ -f "$1/dirt2_game.exe" ] || { echo 'Expected a game folder containing dirt2_game.exe' >&2; exit 1; }
 "$ADB" shell "mkdir -p '/sdcard/Android/data/com.daniilvinogradov.dirt2/files/Dirt 2'"
 "$ADB" push --sync -a -z zstd "$1/." '/sdcard/Android/data/com.daniilvinogradov.dirt2/files/Dirt 2'
fi
"$ADB" shell am start -n com.daniilvinogradov.dirt2/.Dirt2Activity
