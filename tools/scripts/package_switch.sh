#!/bin/bash
# Packages the Switch build as an SD card layout: switch/dirt2/dirt2.nro, zipped.
# Copy the zip's contents to the root of the SD card, then the game's "Dirt 2"
# folder (with dirt2_game.exe) into switch/dirt2/. Start it by title takeover:
# hold R while launching any installed game, then pick DiRT 2 in the Homebrew Menu.
# Usage: package_switch.sh [build dir (build/switch)] [output dir (build/package/switch)]
set -euo pipefail
cd "$(dirname "$0")/../.."
BUILD=${1:-build/switch}
OUT=${2:-build/package/switch}
NRO=$BUILD/dirt2.nro
[ -f "$NRO" ] || { echo "no $NRO: cmake --preset switch && cmake --build build/switch" >&2; exit 1; }
rm -rf "$OUT/sd" && mkdir -p "$OUT/sd/switch/dirt2"
cp "$NRO" "$OUT/sd/switch/dirt2/dirt2.nro"
# The renderer's shader pack (game-derived; tools/scripts/dx9_shader_census.py).
PACK=build/shaders/dx9/dirt2.dkshpack
if [ -f "$PACK" ]; then cp "$PACK" "$OUT/sd/switch/dirt2/"; else echo "warning: no $PACK (run dx9_shader_census.py): nothing will draw" >&2; fi
cat > "$OUT/sd/switch/dirt2/put the Dirt 2 folder here.txt" <<'EOF'
Copy the game's "Dirt 2" folder (the one with dirt2_game.exe, Steam v1.1) into
this folder, so that this file sits next to "Dirt 2". The log is dirt2.log here.
Start DiRT 2 by title takeover: hold R while launching any installed game, then
pick DiRT 2 in the Homebrew Menu (from the Album it has too little memory).
EOF
rm -f "$OUT/DiRT2-switch.zip"
(cd "$OUT/sd" && zip -qr ../DiRT2-switch.zip switch)
echo "$OUT/DiRT2-switch.zip ($(du -h "$NRO" | cut -f1) NRO)"
