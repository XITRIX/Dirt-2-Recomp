# Build system: one static recipe, pristine third-party code

## Layout

| Path | Contents |
|---|---|
| `third_party/<name>` | Pristine git submodules at pinned commits (with nested submodules: clone with `--recurse-submodules`) |
| `third_party/patches/<name>/NNNN-*.patch` | Every change to third-party code, plus a README table: patch → what it fixes |
| `build/src/<name>` | Patched copies the builds read from (`prepare_sources.sh`) |
| `build/deps/<platform>` | SDL3, OpenAL Soft, ogg, vorbis, FFmpeg: static, per platform |
| `build/moltenvk/<platform>`, `build/dxvk/<platform>` | Graphics stack, static |
| `build/<preset>` | Game builds (`macos-arm64`, `ios`, `ios-sim`) |
| `build/package/<platform>` | Packaged builds (zip, ipa) |
| `generated/` | Lifter output (gitignored) |

Platforms are `macos`, `iphoneos` and `iphonesimulator`. Outputs never overlap, so platforms build in parallel.

## Patched sources without dirty submodules

`prepare_sources.sh`:

- **Export:** `git archive HEAD` of each submodule, plus each nested submodule extracted separately into its path. It never reads the working tree, so local edits can't leak in. Concatenated tar streams didn't work; extract per submodule.
- **Commit the copy:** make it its own git repo, commit `upstream <sha>`, then **one commit per patch**, the last tagged `patched`. To regenerate the last patch: `git -C build/src/<name> diff patched~1 > third_party/patches/<name>/<patch>`. Diffing against the wrong base once lost half a patch.
- **Stamp:** submodule SHA plus a hash of the patches decides when to refresh.
- **Guard:** refuse to overwrite a copy that has uncommitted edits ("save them into a patch first").
- **MoltenVK's fetched externals** (SPIRV-Cross and others) live inside the copy. Keep them across refreshes; to patch them, check out the pinned revision and apply. Do not rerun `fetchDependencies` on an existing checkout: it resets the sources and drops the patch.
- **The rule:** never edit or commit inside `third_party/`, and don't use `ignore = dirty`.

## Static, portable dependencies

- **Build every library from source as static,** with the same recipe per platform. Nothing links from Homebrew or `/usr/local`; Homebrew provides build tools only (cmake, meson, ninja, glslang, pkg-config). Check the macOS binary loads only system libraries (`otool -L`; the package script asserts this).
- **pkg-config:** limit it to the platform's prefix with `PKG_CONFIG_LIBDIR`. Join `-framework X` pairs that pkg-config splits into two list items.
- **CMake:** the pkg-config results are cached, so delete `CMakeCache.txt` after rebuilding a dependency.
- **DXVK:** merge its internal static archives into one `libdxvk_d3d9.a` with `libtool -static`.
- **MoltenVK headers:** copy with `cp -RL` (the headers are symlinks).
- **MoltenVK externals:** build them as one multi-platform xcframework, under a lock, only when their patch changes. Don't use MoltenVK's own `make` or `Package/`.
- **Executable sources:** the platform `.mm` files must be listed in every `add_executable`. A single-replace edit once added `metalfx.mm` to one target only.
- **Frameworks:** `-framework CloudKit`, and `-weak_framework MetalFX` for optional OS features.

## Xcode and process pitfalls

- Xcode's SQLite build database fails with `disk I/O error` inside an iCloud-synced `~/Documents`. Keep derived data in `~/Library/Caches/<App>/xcode`.
- Killing a build script doesn't kill its `xcodebuild` or `ninja` children, and a leftover one corrupts the next build ("database is locked", wrong-platform objects). Run `pgrep -fl xcodebuild` first.
- **Make build scripts stop on the first compile error** (`set -euo pipefail`, and check `PIPESTATUS` when piping through `grep`). A compile error that the script carried on past cost a long rebuild that looked like slowness.
- `/usr/bin/perl` and `/bin/sh` are SIP-protected and strip `DYLD_*`. A harness started through `perl -e 'alarm ...'` loaded Homebrew's MoltenVK; set the variable inside perl before `exec`.
- In zsh, an unmatched glob aborts the command (and a script that `rm`s with a glob may leave stale outputs behind). Quote globs or use `setopt null_glob`.
- Game data paths with brackets break lldb's `run`. Symlink the folder to a plain path, and use Xcode's lldb.

## Packaging

- **macOS:** one static binary plus `gamecontrollerdb.txt`, the shipped pipelines and the icon, in a zip with a launcher. No game data.
- **iOS:** sign (see ios.md) and write an `.ipa`.
- **Game-derived files** (pipeline recordings, shader caches) are produced locally, copied in at configure time, and never committed.

## Lift and build loop

```bash
cmake --build build/macos-arm64 --target recomp
./build/macos-arm64/tools/recomp/recomp lift "<game>/game.exe" --out generated \
    --entries tools/recomp/config/entries.txt --hooks tools/recomp/config/hooks.txt --include-dead
cmake --build build/macos-arm64       # about 3–5 min full; incremental after a hook change
```

The lifter writes only changed files, so changing one hook recompiles one generated file.
