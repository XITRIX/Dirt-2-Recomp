#!/bin/bash
# Packages the macOS build into a folder and zip:
#   DiRT2-macOS/dirt2                 the recompiled game; every third-party library
#                                     (DXVK, MoltenVK, SDL3, OpenAL Soft, FFmpeg, vorbis)
#                                     is linked in statically
#   DiRT2-macOS/gamecontrollerdb.txt  SDL gamepad mappings (third_party/SDL_GameControllerDB)
#   DiRT2-macOS/run.sh                launcher; takes the game folder (the one with dirt2_game.exe)
# Game data is not included. Usage: package_macos.sh [out_dir]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=${1:-$ROOT/build/package/macos}
PKG=$OUT/DiRT2-macOS

rm -rf "$PKG"
mkdir -p "$PKG"
cp "$ROOT/build/macos-arm64/dirt2" "$PKG/dirt2"
cp "$ROOT/third_party/SDL_GameControllerDB/gamecontrollerdb.txt" "$PKG/"

# Static build: the executable may only load system libraries and frameworks.
foreign=$(otool -L "$PKG/dirt2" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/)' || true)
if [ -n "$foreign" ]; then
    echo "error: dirt2 links non-system libraries:" >&2
    echo "$foreign" >&2
    exit 1
fi
codesign --force --sign - "$PKG/dirt2"

cat > "$PKG/run.sh" <<'RUN'
#!/bin/bash
# DiRT 2 (static recompilation) launcher.
# Usage: ./run.sh ["/path/to/Dirt 2"]   (the folder that contains dirt2_game.exe)
# Without an argument it uses $DIRT2_GAME_DIR, then "Dirt 2" next to this script,
# then asks for the folder. Saves and logs go to ~/Library/Application Support/Dirt2Recomp.
# Options: DIRT2_PLAY_VIDEOS=1 plays the intro videos, DIRT2_WINDOWED=1 runs in a window.
HERE=$(cd "$(dirname "$0")" && pwd)
GAME=${1:-${DIRT2_GAME_DIR:-}}
[ -z "$GAME" ] && [ -f "$HERE/Dirt 2/dirt2_game.exe" ] && GAME="$HERE/Dirt 2"
if [ -z "$GAME" ]; then
    GAME=$(osascript -e 'POSIX path of (choose folder with prompt "Select the DiRT 2 folder (with dirt2_game.exe)")') || exit 1
fi
if [ ! -f "$GAME/dirt2_game.exe" ]; then
    echo "dirt2_game.exe not found in: $GAME" >&2
    exit 1
fi
export MVK_CONFIG_LOG_LEVEL=${MVK_CONFIG_LOG_LEVEL:-1}
exec "$HERE/dirt2" --game "$GAME"
RUN
chmod +x "$PKG/run.sh"

xattr -cr "$PKG" 2>/dev/null || true
(cd "$OUT" && rm -f DiRT2-macOS.zip && ditto -c -k --keepParent DiRT2-macOS DiRT2-macOS.zip)
ls -la "$OUT/DiRT2-macOS.zip"
