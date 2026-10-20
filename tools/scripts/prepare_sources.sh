#!/bin/bash
# Prepares patched copies of the third-party sources in build/src, leaving the
# submodules exactly as upstream:
#   build/src/dxvk                       third_party/dxvk + patches/dxvk
#   build/src/MoltenVK                   third_party/MoltenVK + patches/MoltenVK
#   build/src/MoltenVK/External/<name>   MoltenVK's fetched dependencies + patches/<name>
#   build/src/dxmt                       third_party/dxmt + patches/dxmt
#
# Each copy is its own small git repo: a commit "upstream <sha>" (tag upstream),
# then one commit per patch named after it (the last one tagged patched). To
# change the last patch, edit the copy, then
#   git -C build/src/<name> diff patched > /dev/null   # review
#   git -C build/src/<name> diff patched~1 > third_party/patches/<name>/<patch>
# (with a single patch, patched~1 is upstream).
# A copy is rebuilt when its submodule commit or its patches change; one with
# uncommitted edits is never overwritten.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
SRC=$ROOT/build/src
mkdir -p "$SRC"

patches_hash() { cat "$ROOT/third_party/patches/$1"/*.patch 2>/dev/null | shasum | cut -d' ' -f1; }

# Exports a submodule's committed tree (and its nested submodules) to dst:
# never its working tree, so local edits in third_party never leak in.
export_tree() {
    local src=$1 dst=$2
    mkdir -p "$dst"
    git -C "$src" archive HEAD | tar -x -C "$dst"
    DST=$dst git -C "$src" submodule foreach --recursive --quiet \
        'mkdir -p "$DST/$displaypath" && git archive HEAD | tar -x -C "$DST/$displaypath"'
}

# Refuses to touch a copy whose working tree has edits not yet in a patch.
guard_clean() {
    local dir=$1
    if [ -d "$dir/.git" ] && [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]; then
        echo "error: $dir has edits that are not in a patch yet:" >&2
        git -C "$dir" status --short --untracked-files=no >&2
        echo "Save them into third_party/patches first (see this script's header)." >&2
        exit 1
    fi
}

# commit_and_patch <dir> <patch set> <upstream label>: records the copy as the
# upstream commit, applies the patch set and records that too.
commit_and_patch() {
    local dir=$1 set=$2 label=$3
    (cd "$dir" && git init -q && git add -A -f && git -c user.name=prepare -c user.email=prepare@local \
        commit -q -m "upstream $label" && git tag -f upstream > /dev/null)
    # One commit per patch, so a later patch can be regenerated against the
    # ones before it: git diff <its parent commit>.
    for p in "$ROOT/third_party/patches/$set"/*.patch; do
        [ -f "$p" ] || continue
        git -C "$dir" apply "$p" || { echo "error: $(basename "$p") does not apply to $set $label" >&2; exit 1; }
        (cd "$dir" && git add -A -f && git -c user.name=prepare -c user.email=prepare@local commit -q --allow-empty \
            -m "$(basename "$p")")
    done
    git -C "$dir" tag -f patched > /dev/null
}

# ---- DXVK (with its nested submodules: headers, dxbc-spirv, libdisplay-info) ----
prepare_dxvk() {
    local dst=$SRC/dxvk sha
    sha=$(git -C third_party/dxvk rev-parse HEAD)
    local stamp="$sha $(patches_hash dxvk)"
    [ -f "$dst/.dirt2-stamp" ] && [ "$(cat "$dst/.dirt2-stamp")" = "$stamp" ] && return
    guard_clean "$dst"
    echo "== preparing build/src/dxvk ($sha)"
    rm -rf "$dst"
    export_tree third_party/dxvk "$dst"
    commit_and_patch "$dst" dxvk "$sha"
    echo "$stamp" > "$dst/.dirt2-stamp"
}

# ---- DXMT (D3D11 -> Metal; with its nested submodules: directx headers, nvapi) ----
prepare_dxmt() {
    local dst=$SRC/dxmt sha
    sha=$(git -C third_party/dxmt rev-parse HEAD)
    local stamp="$sha $(patches_hash dxmt)"
    [ -f "$dst/.dirt2-stamp" ] && [ "$(cat "$dst/.dirt2-stamp")" = "$stamp" ] && return
    guard_clean "$dst"
    echo "== preparing build/src/dxmt ($sha)"
    rm -rf "$dst"
    export_tree third_party/dxmt "$dst"
    commit_and_patch "$dst" dxmt "$sha"
    echo "$stamp" > "$dst/.dirt2-stamp"
}

# ---- MoltenVK and its fetched dependencies ----
prepare_moltenvk() {
    local dst=$SRC/MoltenVK sha
    sha=$(git -C third_party/MoltenVK rev-parse HEAD)
    local stamp="$sha $(patches_hash MoltenVK) $(patches_hash SPIRV-Cross)"
    [ -f "$dst/.dirt2-stamp" ] && [ "$(cat "$dst/.dirt2-stamp")" = "$stamp" ] && return
    guard_clean "$dst"
    [ -d "$dst/External/SPIRV-Cross" ] && guard_clean "$dst/External/SPIRV-Cross"
    echo "== preparing build/src/MoltenVK ($sha)"
    # Keep the fetched dependencies (and their built libraries) across refreshes.
    if [ -d "$dst/External" ]; then mv "$dst/External" "$SRC/.MoltenVK-External"; fi
    rm -rf "$dst"
    export_tree third_party/MoltenVK "$dst"
    commit_and_patch "$dst" MoltenVK "$sha"
    if [ -d "$SRC/.MoltenVK-External" ]; then
        rm -rf "$dst/External" && mv "$SRC/.MoltenVK-External" "$dst/External"
    else
        (cd "$dst" && ./fetchDependencies --none)
    fi
    # Dependencies with patches: back to their pinned revision, then patched.
    for set in "$ROOT"/third_party/patches/*/; do
        local name
        name=$(basename "$set")
        local dep=$dst/External/$name
        [ -d "$dep/.git" ] || continue
        rev=$(cat "$dst/ExternalRevisions/${name}_repo_revision")
        git -C "$dep" checkout -q --force "$rev"
        git -C "$dep" clean -fdq
        git -C "$dep" tag -f upstream > /dev/null
        for p in "$set"*.patch; do
            git -C "$dep" apply "$p" || { echo "error: $(basename "$p") does not apply to $name $rev" >&2; exit 1; }
        done
        # Recorded as a local commit in the copy, like the other trees.
        (cd "$dep" && git add -A && git -c user.name=prepare -c user.email=prepare@local commit -q -m patched &&
            git tag -f patched > /dev/null)
    done
    echo "$stamp" > "$dst/.dirt2-stamp"
}

prepare_dxvk
# Native Android needs DXVK, without preparing Apple-only graphics libraries.
if [ "${1:-}" = "--dxvk-only" ]; then exit 0; fi
prepare_moltenvk
prepare_dxmt
