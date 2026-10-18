# Handover

State of the DiRT 2 static recompilation as of 2026-10-18. Detailed findings and per-milestone notes live in [docs/STATUS.md](docs/STATUS.md); this file is the short version for whoever picks the work up next.

## Goal

Run Colin McRae DiRT 2 (Steam v1.1, `dirt2_game.exe`) natively on Apple platforms by static recompilation: x86 PE32 lifted to C, compiled for arm64, every Windows/DirectX/GFWL import replaced. First target milestone is a playable race on iOS. Graphics go through DXVK (d3d9) on MoltenVK behind our own d3d9 bridge.

## Where it stands

| Area | State |
|---|---|
| Lifter and runtime | Done. 68,910 live functions lift with Unicorn differential tests passing. |
| Boot | Boots through CRT, GFWL (offline) and engine init on macOS. |
| Frontend | 3D frontend renders in correct colour with audio. Bink intros play correctly (skipped by default). |
| Race | Not reached yet. |
| iOS | Runs on an iPhone 15 Pro Max (A17 Pro): boots, plays the intros, renders the 3D title screen at 35-60 fps. Same sources and scripts as macOS (see Build). |

Latest fixes:

- `a300843`: green video and wrong alpha everywhere. SPIRV-Cross typed DXVK's textures as `depth2d` because DXVK samples each D3D9 sampler both plainly and with depth compare; patched in `third_party/patches/SPIRV-Cross/`. Full write-up in STATUS.md, open issue 1.
- GPU device lost (`kIOGPUCommandBufferCallbackErrorInvalidResource`): MoltenVK removed freed allocations from its queue-wide `MTLResidencySet` while unretained command buffers still used them. Removal is now deferred until every queue drains: `third_party/patches/MoltenVK/0002-defer-residency-removal.patch`, STATUS.md open issue 4.
- Gamepad (Backbone One) silent: SDL's MFi driver delivered no input. It is now disabled, and pads use HIDAPI/IOKit with the bundled `gamecontrollerdb.txt`. STATUS.md open issue 5.
- `f93c43c`: `tools/scripts/package_macos.sh` builds a self-contained, playable `build/package/DiRT2-macOS.zip` (no game data).

## Open issues, in priority order

1. **Fullscreen not retested** since the device-lost fix below; it previously crashed all of macOS. Test it windowed first, then fullscreen.
2. ~~Flicker (black frames on some launches)~~: fixed in MoltenVK, `0003-swapchain-view-follows-drawable.patch` (STATUS.md, open issue 3).
3. **Performance:** about 15 fps in 3D scenes. Not profiled yet.
4. Then: drive the game into a race and fix what breaks. On iOS: a touch overlay, the memory budget on a real device, and backgrounding.

## Build and run

Every third-party library is built from pinned sources as a **static** library, the same way for every platform, and linked into one executable. Nothing is taken from Homebrew or the system except Apple's frameworks; Homebrew provides build tools only (cmake, meson, ninja, glslang, pkg-config).

Per-platform outputs never overlap, so platforms can build at the same time. Platforms are `macos`, `iphoneos` and `iphonesimulator`:

| Path | Contents |
|---|---|
| `build/deps/<platform>` | SDL3, OpenAL Soft, ogg, vorbis, FFmpeg (Bink only): `build_deps.sh` |
| `build/moltenvk/<platform>` | `libMoltenVK.a` with the SPIRV-Cross patch: `build_moltenvk.sh` |
| `build/dxvk/<platform>` | `libdxvk_d3d9.a`, DXVK's internal libraries merged in: `build_graphics.sh` |
| `build/macos-arm64`, `build/ios`, `build/ios-sim` | Game builds (CMake presets) |
| `build/package/<platform>` | Packaged builds |
| `~/Library/Caches/Dirt2Recomp/xcode/` | Xcode derived data (must not live in `~/Documents`, see Pitfalls) |

```bash
tools/scripts/build_deps.sh macos iphoneos iphonesimulator      # libraries
tools/scripts/build_graphics.sh macos iphoneos iphonesimulator  # MoltenVK + DXVK

cmake --preset macos-arm64 && cmake --build build/macos-arm64
./build/macos-arm64/dirt2 --game "<folder with dirt2_game.exe>"
tools/scripts/package_macos.sh               # build/package/macos/DiRT2-macOS.zip

cmake --preset ios-sim && cmake --build build/ios-sim
tools/scripts/package_ios.sh sim             # ad-hoc signs build/ios-sim/dirt2.app
cmake --preset ios && cmake --build build/ios
tools/scripts/package_ios.sh device          # signs, writes build/package/iphoneos/DiRT2-iOS.ipa
```

- The game on iOS reads its data from the app's Documents folder (copy the `Dirt 2` folder in with Finder or the Files app). In the simulator, `SIMCTL_CHILD_DIRT2_GAME_DIR=<mac path>` points it at the Mac's copy.
- Device signing uses team `D59DHVRS87` and bundle id `com.daniilvinogradov.dirt2recomp` (override with `DIRT2_TEAM`, `DIRT2_BUNDLE_ID`). The profile must carry `extended-virtual-addressing` (the 4 GB guest space) and `increased-memory-limit`; if none is installed, `package_ios.sh` has Xcode create one through a stub project.
- Re-lift after lifter changes: `tools/scripts/relift.sh`. It lifts with `--include-dead`; without it, races crash with `unknown target` on gap-scan functions.
- Patches: `tools/scripts/apply_patches.sh` applies `third_party/patches/<name>/` to `third_party/<name>` or MoltenVK's `External/<name>`. It is idempotent.
- MoltenVK's externals (SPIRV-Cross, SPIRV-Tools) are one multi-platform xcframework at a path MoltenVK's project hard-codes. `build_mvk_externals.sh` builds it for all platforms at once, under a lock, and only when the SPIRV-Cross patch changes; per-platform MoltenVK builds only read it. MoltenVK's own `make` and `Package/` folder are not used. Do not rerun `fetchDependencies` on an existing checkout: it resets the External sources and drops the patch.
- Game data (11.5 GB) is never committed. Saves, config and DXVK logs go to `~/Library/Application Support/Dirt2Recomp` on macOS and the app container's Library on iOS (override with `DIRT2_DATA_DIR`).

## Layout

| Path | Contents |
|---|---|
| `tools/recomp` | Lifter (Zydis; C emitter, SSE/x87, SEH landing pads, `--entries`, `--check-esp`) |
| `runtime/` | 4 GB guest address space, threads/TEB/TLS, dispatch, SEH, `RECOMP_TSO` |
| `shims/` | One folder per DLL. Key files: `d3d9/bridge.cpp` (guest COM proxies to DXVK), `d3dx9/`, `kernel32/sync.cpp`, `video/bink*.cpp` (FFmpeg) |
| `platform/sdl` | SDL3 host: window and events on the main thread |
| `third_party/` | DXVK 3.1.1, MoltenVK 1.4.1 (+ SPIRV-Cross) with patches in `third_party/patches/`; SDL_GameControllerDB mappings |
| `tests/lifter` | Unicorn differential tests |
| `tests/dxvk_smoke`, `tests/video` | Standalone graphics and video harnesses, built by hand with clang++ |

## Debugging toolkit

- **Isolate by layer first.** The video bug took three days of guessing and one hour of layering: FFmpeg, then our decoder, then raw MoltenVK, then DXVK standalone, then the game. The harnesses for each layer are in `tests/video` and `tests/dxvk_smoke`.
  - `video_shader.cpp` replays the game's dumped shaders with `FMT=`, `PLANES=`, `OUT=`, `RS=`/`SS=`.
  - A one-instruction pixel shader built with `tools/scripts/sm3asm.py` helpers probes the raw texel values DXVK returns.
- **Game logging switches** are in STATUS.md (`DIRT2_LOG_*`, `DIRT2_DUMP_SHADERS`, `DIRT2_SCREENSHOT_*`, `DIRT2_WATCH_PS_CONST`, `DIRT2_PLAY_VIDEOS`, `DIRT2_WINDOWED`).
- **Runtime tools:** `RECOMP_TRACE`, the `--check-esp` lift for stack imbalances, and `RECOMP_WATCH=addr,len` (`DIRT2_WATCH` build) for guest write watches.
- **Shader inspection:**
  - `DXVK_SHADER_DUMP_PATH=<dir>` dumps DXVK's SPIR-V.
  - `MVK_CONFIG_DEBUG=1 MVK_CONFIG_LOG_LEVEL=3` prints the generated Metal source.
  - `build/spirv-cross/spirv-cross --msl --msl-version 30000 --msl-argument-buffers --msl-argument-buffer-tier 1 --msl-device-argument-buffer 0 --msl-device-argument-buffer 1 --msl-device-argument-buffer 2 <file.spv>` reproduces MoltenVK's translation offline.

## Pitfalls

- Xcode's SQLite build database fails with `disk I/O error` inside `~/Documents` when iCloud Drive syncs it: keep derived data in `~/Library/Caches` (the scripts do).
- Killing a build script does not kill its `xcodebuild`/`ninja` children; a leftover one corrupts the next build in the same folder ("database is locked", wrong platform objects). Check `pgrep -fl xcodebuild` before rebuilding.
- FFmpeg's configure autodetects host libraries (it found Homebrew's X11); `build_deps.sh` passes `--disable-autodetect` and limits pkg-config to the platform's own prefix. After a dependency rebuild, delete the game build's `CMakeCache.txt`: pkg-config results are cached.
- The standalone test harnesses in `tests/` still link DXVK and MoltenVK from the old dylib builds: `/usr/bin/perl` and `/bin/sh` are SIP-protected and strip `DYLD_*` variables, so a harness started through `perl -e 'alarm ...'` loads Homebrew's MoltenVK 1.2.11. Set `$ENV{DYLD_LIBRARY_PATH}` inside perl before `exec`.
- `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` hangs DXVK.
- Rebuilding MoltenVK after a patch: `tools/scripts/build_moltenvk.sh <platform>` (SPIRV-Cross patches: the externals rebuild by themselves).
- Game data paths with brackets break lldb's `run`. Symlink the game folder to a plain path, and use Xcode's lldb, not a toolchain one.
- xlive ordinals are stdcall: a wrong argument count corrupts the guest stack far from the call site (that is how `XUserCheckPrivilege` cost a day).
- The game's video planes are `D3DFMT_A8`, not L8.

## Conventions

- Linear history on `main`: no merge commits. Rebase or recommit, and amend your own unpushed commits.
- Commit milestone by milestone with the findings in the message, and keep STATUS.md current.
- Nothing is pushed yet. The submodules show as modified because the patches are applied in place; the patch files are the source of truth.
