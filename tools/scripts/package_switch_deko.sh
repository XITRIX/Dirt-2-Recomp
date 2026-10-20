#!/bin/bash
# Packages the Switch build as an SD card layout: switch/dirt2/dirt2.nro, zipped.
# Copy the zip's contents to the root of the SD card, then the game's "Dirt 2"
# folder (with dirt2_game.exe) into switch/dirt2/. Start it by title takeover:
# hold R while launching any installed game, then pick DiRT 2 in the Homebrew Menu.
# Usage: package_switch.sh [build dir (build/switch)] [output dir (build/package/switch)] [shader pack (build/shaders/dx9/dirt2.dkshpack)]
set -euo pipefail
cd "$(dirname "$0")/../.."
BUILD=${1:-build/switch-deko}
OUT=${2:-build/package/switch-deko}
NRO=$BUILD/dirt2.nro
[ -f "$NRO" ] || { echo "no $NRO: cmake --preset switch && cmake --build build/switch" >&2; exit 1; }
rm -rf "$OUT/sd" && mkdir -p "$OUT/sd/switch/dirt2"
cp "$NRO" "$OUT/sd/switch/dirt2/dirt2.nro"
# The renderer's shader pack (game-derived; tools/scripts/dx9_shader_census.py).
PACK=${3:-build/shaders/dx9/dirt2.dkshpack}
if [ -f "$PACK" ]; then cp "$PACK" "$OUT/sd/switch/dirt2/"; else echo "warning: no $PACK (run dx9_shader_census.py): nothing will draw" >&2; fi
python3 tools/scripts/build_deko_ffp_shaders.py
cp build/shaders/deko-ffp/*.dksh "$OUT/sd/switch/dirt2/"
cp third_party/deko_ffp/LICENSE "$OUT/sd/switch/dirt2/DXVK-FFP-LICENSE"
# Overlay installs must also replace an old diagnostic env.txt on the SD card.
cat > "$OUT/sd/switch/dirt2/env.txt" <<'EOF'
# Normal play: diagnostics disabled; renderer fixes use their built-in defaults.
EOF
cat > "$OUT/sd/switch/dirt2/put the Dirt 2 folder here.txt" <<'EOF'
Copy the game's "Dirt 2" folder (the one with dirt2_game.exe, Steam v1.1) into
this folder, so that this file sits next to "Dirt 2". The log is dirt2.log here.
Start DiRT 2 by title takeover: hold R while launching any installed game, then
pick DiRT 2 in the Homebrew Menu (from the Album it has too little memory).
EOF
rm -f "$OUT/DiRT2-switch.zip"
(cd "$OUT/sd" && zip -qr ../DiRT2-switch.zip switch)
echo "$OUT/DiRT2-switch.zip ($(du -h "$NRO" | cut -f1) NRO)"
