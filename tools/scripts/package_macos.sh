#!/bin/bash
# Packages the macOS build into a self-contained folder and zip:
#   DiRT2-macOS/dirt2        the recompiled game
#   DiRT2-macOS/lib/         every non-system dylib it needs (DXVK, MoltenVK, SDL3, FFmpeg, ...)
#   DiRT2-macOS/run.sh       launcher; takes the game folder (the one with dirt2_game.exe)
# Game data is not included. Usage: package_macos.sh [out_dir]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=${1:-$ROOT/build/package}
PKG=$OUT/DiRT2-macOS
MVK_DIR=$ROOT/third_party/MoltenVK/Package/Release/MoltenVK/dylib/macOS
SEARCH=("$ROOT/build/dxvk-macos/src/d3d9" "$MVK_DIR" /opt/homebrew/lib)

rm -rf "$PKG"
mkdir -p "$PKG/lib"
cp "$ROOT/build/macos-arm64/dirt2" "$PKG/dirt2"
cp "$MVK_DIR/libMoltenVK.dylib" "$PKG/lib/"   # dlopened by DXVK and SDL, not linked

# Resolves a load command to a real file: absolute paths as they are,
# @rpath/@loader_path names by basename in the referencing file's folder or SEARCH.
resolve() {
    local ref=$1 from_dir=$2 name
    case $ref in
        /usr/lib/* | /System/*) return 1 ;;
        /*) [ -f "$ref" ] && { echo "$ref"; return 0; } ;;
    esac
    name=$(basename "$ref")
    for d in "$from_dir" "${SEARCH[@]}"; do
        [ -f "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    echo "unresolved: $ref (from $from_dir)" >&2
    return 1
}

deps() { otool -L "$1" | tail -n +2 | awk '{print $1}'; }

# Breadth-first copy of the dependency closure. ORIGINS maps each packaged file
# to the folder its original came from (for @loader_path/@rpath lookups).
ORIGINS=$(mktemp)
trap 'rm -f "$ORIGINS"' EXIT
printf '%s\t%s\n' "$PKG/dirt2" "$ROOT/build/macos-arm64" "$PKG/lib/libMoltenVK.dylib" "$MVK_DIR" > "$ORIGINS"
origin_of() { awk -F'\t' -v f="$1" '$1 == f {print $2}' "$ORIGINS"; }
queue=("$PKG/dirt2" "$PKG/lib/libMoltenVK.dylib")
while [ ${#queue[@]} -gt 0 ]; do
    f=${queue[0]}
    queue=("${queue[@]:1}")
    from=$(origin_of "$f")
    for ref in $(deps "$f"); do
        src=$(resolve "$ref" "$from") || continue
        name=$(basename "$src")
        [ "$name" = "$(basename "$f")" ] && continue
        if [ ! -f "$PKG/lib/$name" ]; then
            cp "$src" "$PKG/lib/$name"
            chmod u+w "$PKG/lib/$name"
            printf '%s\t%s\n' "$PKG/lib/$name" "$(dirname "$(realpath "$src")")" >> "$ORIGINS"
            queue+=("$PKG/lib/$name")
        fi
    done
done

# Rewrite install names to the bundled copies and re-sign (arm64 needs a signature).
relink() {
    local f=$1 prefix=$2
    for ref in $(deps "$f"); do
        case $ref in /usr/lib/* | /System/*) continue ;; esac
        name=$(basename "$ref")
        [ -f "$PKG/lib/$name" ] && install_name_tool -change "$ref" "$prefix/$name" "$f" 2>/dev/null
    done
}
for lib in "$PKG"/lib/*.dylib; do
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null
    relink "$lib" "@loader_path"
done
relink "$PKG/dirt2" "@executable_path/lib"
for rp in $(otool -l "$PKG/dirt2" | awk '/LC_RPATH/{getline; getline; print $2}'); do
    install_name_tool -delete_rpath "$rp" "$PKG/dirt2"
done
install_name_tool -add_rpath "@executable_path/lib" "$PKG/dirt2"
for f in "$PKG"/lib/*.dylib "$PKG/dirt2"; do codesign --force --sign - "$f" 2>/dev/null; done

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
# The bundled MoltenVK, not any system or Homebrew copy.
export DYLD_LIBRARY_PATH="$HERE/lib"
export SDL_VULKAN_LIBRARY="$HERE/lib/libMoltenVK.dylib"
export MVK_CONFIG_LOG_LEVEL=${MVK_CONFIG_LOG_LEVEL:-1}
exec "$HERE/dirt2" --game "$GAME"
RUN
chmod +x "$PKG/run.sh"

xattr -cr "$PKG" 2>/dev/null || true
(cd "$OUT" && rm -f DiRT2-macOS.zip && ditto -c -k --keepParent DiRT2-macOS DiRT2-macOS.zip)
echo "$(ls "$PKG/lib" | wc -l | tr -d ' ') libraries bundled"
ls -la "$OUT/DiRT2-macOS.zip"
