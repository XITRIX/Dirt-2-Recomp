#!/bin/bash
# Pinned UAM with the deko-specific derivative and host build repairs.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SOURCE="$ROOT/build/src/uam-runtime"
REVISION=5a5afc2bae8b55409ab36ba45be63fcb73f68993
if [ ! -d "$SOURCE/.git" ]; then
    git clone https://github.com/devkitPro/uam.git "$SOURCE"
    git -C "$SOURCE" checkout --detach "$REVISION"
fi
[ "$(git -C "$SOURCE" rev-parse HEAD)" = "$REVISION" ] || { echo 'Unexpected UAM revision' >&2; exit 1; }
for patch_file in "$ROOT"/third_party/patches/uam/*.patch; do
    if git -C "$SOURCE" apply --reverse --check "$patch_file" 2>/dev/null; then continue; fi
    git -C "$SOURCE" apply --check "$patch_file"
    git -C "$SOURCE" apply "$patch_file"
done
export PATH="$ROOT/build/tools/venv/bin:$PATH"
python3 -c 'import mako' || { echo 'Install Mako in build/tools/venv first' >&2; exit 1; }
export CC=clang CXX=clang++ CCACHE_DISABLE=1
if [ ! -f "$ROOT/build/tools/uam/build.ninja" ]; then
    meson setup "$ROOT/build/tools/uam" "$SOURCE"
fi
meson compile -C "$ROOT/build/tools/uam" -j 6
