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
| Race | Races start and finish on the iPhone; a post-race menu crash is still to be reproduced with the new logs (`dirt2.log`, `dirt2.prev.log`). |
| iOS | Races run on an iPhone 15 Pro Max and iPad Pro (M4), up to 120 Hz. Saves sync between devices through CloudKit (`platform/apple/cloud_saves.mm`): pulled at launch, uploaded after each save, Steam-style prompt on conflicts. |

Latest fixes:

- Shader/pipeline compile stutter: DXVK now records the pipelines the game uses and compiles them on background threads at the next start (`third_party/patches/dxvk/0003-pipeline-warmup.patch`, section below). On a cold Metal cache, 110 s of frontend went from 151 compiles at draw time (19.1 s in total, 137 of them over 16 ms) to 1, with 153 pipelines compiled in the background in 4.2 s before the title screen.
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
| `build/pipelines` | Shipped pipeline warm-up data (game-derived, never committed): see Pipeline warm-up |
| `~/Library/Caches/Dirt2Recomp/xcode/` | Xcode derived data (must not live in `~/Documents`, see Pitfalls) |

```bash
git clone --recurse-submodules <repo> && cd Dirt2   # DXVK has nested submodules
tools/scripts/build_deps.sh macos iphoneos iphonesimulator      # libraries
tools/scripts/build_graphics.sh macos iphoneos iphonesimulator  # MoltenVK + DXVK
tools/scripts/build_llvm.sh macos && tools/scripts/build_dxmt.sh macos  # DX11 path (in progress, see below)

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
- Patches: `tools/scripts/prepare_sources.sh` exports each submodule to `build/src/<name>` and applies `third_party/patches/<name>/` there (MoltenVK's fetched dependencies too).
- MoltenVK's externals (SPIRV-Cross, SPIRV-Tools) are one multi-platform xcframework at a path MoltenVK's project hard-codes. `build_mvk_externals.sh` builds it for all platforms at once, under a lock, and only when the SPIRV-Cross patch changes; per-platform MoltenVK builds only read it. MoltenVK's own `make` and `Package/` folder are not used. Do not rerun `fetchDependencies` on an existing checkout: it resets the External sources and drops the patch.
- Game data (11.5 GB) is never committed. Saves, config and DXVK logs go to `~/Library/Application Support/Dirt2Recomp` on macOS and the app container's Library on iOS (override with `DIRT2_DATA_DIR`).

## Pipeline warm-up

MoltenVK has no pipeline libraries, so DXVK compiles every new shader/state pipeline at its first draw: 100–400 ms each on a cold Metal cache, 2–3 ms on a warm one. `0003-pipeline-warmup.patch` adds recording and warm-up (details in `third_party/patches/README.md`); `platform/sdl/sdl_main.cpp` (`setup_pipeline_warmup`) wires it up:

- Every run records new pipelines to `<data>/dirt2.pipelines` (`DXVK_PIPELINE_RECORD`; set it empty to stop). DXVK's shader cache sits next to it as `dirt2.dxvk.bin/.lut` (`DXVK_SHADER_CACHE_NAME=dirt2`).
- At device creation, the pipelines in the shipped `dirt2.pipelines` (next to the executable, or in the iOS app bundle) and in `<data>/dirt2.pipelines` are compiled on DXVK's worker threads (`DXVK_PIPELINE_WARMUP`). So each device also learns from its own play, from the second launch on.
- The shipped shader cache is copied into the data directory once per app build (DXVK opens it read-write and exclusive); `<data>/dirt2.dxvk.stamp` records which build was copied.
- macOS sets `MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1`.
- `DXVK_PIPELINE_TIMING=1` logs each pipeline compiled at a draw: the stutter metric.

Measured on macOS (M1 Max, 10 cores; 110 s of scripted frontend, `DIRT2_INPUT_SCRIPT`):

| Run | Compiled at draw | Draw compile time | Over 16 ms | Warm-up |
|---|---|---|---|---|
| Cold Metal cache, no warm-up | 151 | 19.1 s | 137 | |
| Cold Metal cache, warm-up | 1 | 0.15 s | 1 | 153 in 4.2 s, done before the title screen |
| Warm Metal cache, no warm-up | 153 | 0.42 s | 0 | |
| Warm Metal cache, warm-up | 0 | 0 | 0 | 153 in 0.11 s |

Not measured yet: iOS (the same files are bundled in the app; expect a longer warm-up on 6 cores), memory held by warmed pipelines on the iPhone, and a race (only the frontend has been recorded so far).

Producing the shipped data:

1. Record on one machine with one data directory, so the shader cache covers every recorded pipeline: start with an empty `DIRT2_DATA_DIR`, play the menus and as many tracks and cars as possible (`DIRT2_INPUT_SCRIPT` makes menu runs repeatable). Recording accumulates across runs; quitting by killing the game is fine.
2. Merge into `build/pipelines` with that run's cache:
   ```bash
   mkdir -p build/pipelines && cp <data>/dirt2.dxvk.bin <data>/dirt2.dxvk.lut build/pipelines/
   tools/scripts/merge_pipelines.py -o build/pipelines/dirt2.pipelines --lut build/pipelines/dirt2.dxvk.lut <data>/dirt2.pipelines [more recordings]
   ```
   The script drops duplicates and torn entries, refuses files from another DXVK build, and reports how many pipelines the cache covers. Recordings from other devices (an iPhone's `Library/Application Support` data) merge in too, but only pipelines whose shaders are in the shipped cache are warmed up; the device records the rest itself.
3. Re-run CMake (the files are picked up at configure time), build, and package: CMake copies them next to the macOS executable and into the iOS bundle; `package_macos.sh` includes them.
4. Whenever a DXVK patch changes shader conversion or the pipeline state layout, bump `DXVK_VERSION` in `0003-pipeline-warmup.patch` (`meson.build`) and re-record: older caches and pipeline files are then ignored, and logged as such.

## DX11 renderer on Metal (DXMT), in progress

The plan is to run the game's own DX11 renderer through DXMT (utmapp/dxmt fork, LGPL-2.1, shipped as an embedded `libdxmt-native` framework), with DX9/DXVK kept as a fallback. Phases and gates are in the plan; status:

- **Phase 0 (feasibility): passed.**
  - `third_party/dxmt` is a pristine submodule, prepared into `build/src/dxmt`.
  - `build_llvm.sh` builds LLVM 15.0.7 static from pinned tarballs, with no backends (96 MB of libraries).
  - `build_dxmt.sh` builds `libdxmt-native.dylib` (29 MB with LLVM) and the `airconv` tool, using meson 1.6.1 from `build/tools/venv`.
  - **Shader census** (`extract_dxbc.py` → `build/dxbc_census.md`): 2,768 SM 5.0 blobs, 1,109 unique. 14 hull and 42 domain shaders, all tri domains with `fractional_odd` partitioning. No isoline domains, no geometry shaders, no stream output.
  - **Conversion** (`airconv_census.py` → `build/airconv_census.md`): all 1,109 unique blobs convert and load in Metal.
  - The one DXMT gap that applies: fractional tessellation factors are rounded to integers, so tessellated detail steps instead of morphing.
- **Phase 1 (bridge, frontend on macOS): working.** `DIRT2_RENDERER=dx11` runs the game's DX11 renderer through DXMT. The frontend, menus and the attract-mode race render correctly, at 110–250 fps in the menus on an M1 Max. The default stays `dx9` until the race parity checks in Phase 2.
  - `shims/com/bridge_core.h`: the COM bridge core, shared with the D3D9 bridge.
  - `shims/d3d11/bridge.cpp`: DXGI/D3D11 proxies and hand-written overrides. The swap chain's `HWND` becomes the window's `CAMetalLayer` (`dxmt/0001`). Initial data, input layouts and adapter/output descriptors are converted. `Map`/`Unmap` use guest shadows.
  - `shims/d3d11/reflect.cpp`: `D3DReflect` in the d3dcompiler_42 layout, built in guest memory from RDEF, ISGN/OSGN and STAT.
  - `shims/d3d11/compiler.cpp` with `tools/scripts/dxbc_immediate.py`: `D3DCompile` serves generated SM 5.0 DXBC for the engine's 8 immediate-mode shaders, the only HLSL the game compiles at run time.
  - **Known:** one 0×0 texture fails to create at start-up. That is the game's own request, and D3D11 rejects it on Windows too.
- **Phase 3 (iOS): running.** DX11 is the default renderer on iOS and macOS (`DIRT2_DEFAULT_DX11`; `DIRT2_RENDERER=dx9` for DXVK).
  - `dxmt.framework` (28.7 MB including LLVM) is embedded in `Frameworks/` and signed by `package_ios.sh`.
  - First launch on an iPhone 15 Pro Max: the device is created at feature level 11_0 and the frontend presents at a steady 120 fps.
  - DXMT's "headless" iOS build presents fine through the swap chain on our `CAMetalLayer`. It only lacks display enumeration.
  - Build for iOS: `build_llvm.sh macos iphoneos` (the cross builds compile only the libraries airconv links), then `build_dxmt.sh iphoneos`, then the usual `ios` preset.
- **Next:**
  - Races and parity checks on both platforms (Phase 2).
  - MetalFX on DX11 through DXMT's own scaler. On DX11 the METALFX row is not shown, because the ambient occlusion row it reuses is a real setting there.
  - The DX9/DXVK path stays the macOS default until parity.

### DX11 pipeline warm-up (DXMT)

The same scheme as DXVK's, through `dxmt/0004`:
- **Recording:** every run records new pipelines to `<data>/dirt2.dxmt-pipelines`. DXMT's converted-shader cache is `<data>/dxmt/shaders_<metal>.db`.
- **Warm-up:** at start-up the shipped `dirt2.dxmt-pipelines`, plus the device's own, are queued for background compilation as soon as their shaders exist.
- **Metric:** `DXMT_PIPELINE_TIMING=1` logs each draw that waited for a pipeline.

Measured on macOS, scripted frontend from a cold cache: 13 draw-time waits without warm-up, 0 with.

To produce the shipped data:
1. Copy the data folders from devices that played: `xcrun devicectl device copy from ... --source "Library/Application Support/Dirt2Recomp/<file>"` for `dirt2.dxmt-pipelines`, `dxmt/shaders_320.db` and its `-wal`.
2. Merge them: `tools/scripts/merge_dxmt_pipelines.py -o build/pipelines <dir>...`. It drops duplicate pipelines, takes the union of the cache entries, and checkpoints the WALs.
3. Rebuild: CMake copies the files into the app, and `package_macos.sh` includes them.

The shipped cache replaces a device's own cache once per app build. So merge in every device's data each time it is shipped.

## Render scale and MetalFX

- **Resolution menu:** the game's own Resolution row lists native ×0.5, ×0.75, ×1, ×1.25, ×1.5 and ×2. Sizes are multiples of 8 and each label shows its multiplier, e.g. "2704 x 1760 (0.75x)". The modes come from `platform::display_modes()`; the bridge serves them through `EnumAdapterModes`, and the label hook is at vtable slot 0x0131B6C0 in `bridge.cpp`.
- **MetalFX:** below ×1, `platform/apple/metalfx.mm` upscales each frame into the drawable with `MTLFXSpatialScaler`. It runs from a DXVK present hook (`dxvk/0002-present-hook.patch`).
- **MetalFX on DX11:** DXMT's own scaler does the upscaling (`dxmt/0005`). The bridge passes the window's drawable size and the setting to `dxmt_set_spatial_upscale`.
- **METALFX row in Graphics:** this is the game's Aspect Ratio row (item_5, `pc_gfx_aspect`), moved under Multisampling, with Off/On options, the same on both renderers.
  - Only Normal is right on these devices: our resolutions always have the window's shape, and the game corrects the projection only for an "A:B" value (`f_00BED570`).
  - So Off stores `aspect="normal"` and On stores `aspect="metalfx"`, which renders exactly as Normal. `sdl_main.cpp` reads and watches that value.
  - A saved fixed ratio is reset to `metalfx`.
  - `shims/frontend/menu_patch.cpp` serves the patched `system/states.bin`, `hardware_settings_options.xml` and `hardware_settings_config.xml` from `<data>/overlay` through VFS overrides; the game folder is never touched.
  - On devices without MetalFX the row is the game's own Aspect Ratio row.
- **Row title:** the game shows a string id missing from the language file as `<id>`. The lookup at 0x01161960 is therefore hooked (`tools/recomp/config/hooks.txt`, `recomp lift --hooks`) to answer `lng_metalfx_combo_title`.
- **Menu data:** `tools/scripts/states_bin.py dump <states.bin> id=rv_graphics_options` prints the compiled menu XML, and `roundtrip` checks the encoder. In `states.bin`, a row's place is its slot number (`glyph`, `IBScreenGenericData index`, `IBGlyphLink`). `itemflow` holds the pad order, one id per line, and the game splits it on line breaks.

## Layout

| Path | Contents |
|---|---|
| `tools/recomp` | Lifter (Zydis; C emitter, SSE/x87, SEH landing pads, `--entries`, `--hooks`, `--check-esp`) |
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
- Third-party changes live only as patches in `third_party/patches/` (see its README). The submodules stay pristine; `prepare_sources.sh` builds patched copies in `build/src`, and every build reads from there. Never edit or commit inside `third_party/`.
