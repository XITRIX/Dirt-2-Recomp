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
 # Resume the network copy in shell-owned staging, then unpack as the app UID.
 # Android scoped storage may deny files created by shell in Android/data.
 "$ADB" shell mkdir -p /data/local/tmp/dirt2-install-assets
 "$ADB" push --sync -a -z zstd "$1/." /data/local/tmp/dirt2-install-assets
 "$ADB" shell "set -o pipefail; toybox tar -cf - -C /data/local/tmp/dirt2-install-assets . | run-as com.daniilvinogradov.dirt2 sh -c \"mkdir -p 'files/Dirt 2' && toybox tar -xof - -C 'files/Dirt 2'\""
 "$ADB" shell rm -rf /data/local/tmp/dirt2-install-assets
fi
"$ADB" shell am start -n com.daniilvinogradov.dirt2/.Dirt2Activity
