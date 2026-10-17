#!/bin/sh
# Applies the project's patches to the pinned third-party submodules.
# Idempotent: patches already applied are skipped.
set -e
cd "$(dirname "$0")/../.."
for dir in third_party/patches/*/; do
    name=$(basename "$dir")
    for p in "$dir"*.patch; do
        [ -f "$p" ] || continue
        if git -C "third_party/$name" apply --reverse --check "$PWD/$p" 2>/dev/null; then
            echo "already applied: $p"
        else
            git -C "third_party/$name" apply "$PWD/$p"
            echo "applied: $p"
        fi
    done
done
