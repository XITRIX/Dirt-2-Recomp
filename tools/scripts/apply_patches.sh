#!/bin/sh
# Applies the project's patches to the pinned third-party sources.
# third_party/patches/<name>/ targets third_party/<name>, or MoltenVK's fetched
# dependency third_party/MoltenVK/External/<name> (e.g. SPIRV-Cross).
# Idempotent: patches already applied are skipped.
set -e
cd "$(dirname "$0")/../.."
for dir in third_party/patches/*/; do
    name=$(basename "$dir")
    target="third_party/$name"
    [ -d "$target" ] || target="third_party/MoltenVK/External/$name"
    if [ ! -d "$target" ]; then
        echo "skipped (not fetched yet): $dir"
        continue
    fi
    for p in "$dir"*.patch; do
        [ -f "$p" ] || continue
        if git -C "$target" apply --reverse --check "$PWD/$p" 2>/dev/null; then
            echo "already applied: $p"
        else
            git -C "$target" apply "$PWD/$p"
            echo "applied: $p"
        fi
    done
done
