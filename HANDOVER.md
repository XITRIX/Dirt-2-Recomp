# Handover

State of the DiRT 2 static recompilation as of 2026-10-17 (`main` at `f93c43c`). Detailed findings and per-milestone notes live in [docs/STATUS.md](docs/STATUS.md); this file is the short version for whoever picks the work up next.

## Goal

Run Colin McRae DiRT 2 (Steam v1.1, `dirt2_game.exe`) natively on Apple platforms by static recompilation: x86 PE32 lifted to C, compiled for arm64, every Windows/DirectX/GFWL import replaced. First target milestone is a playable race on iOS. Graphics go through DXVK (d3d9) on MoltenVK behind our own d3d9 bridge.

## Where it stands

| Area | State |
|---|---|
| Lifter and runtime | Done. 68,910 live functions lift with Unicorn differential tests passing. |
| Boot | Boots through CRT, GFWL (offline) and engine init on macOS. |
| Frontend | 3D frontend renders in correct colour with audio. Bink intros play correctly (skipped by default). |
| Race | Not reached yet. |
| iOS | Not started. |

Latest fixes:

- `a300843`: green video and wrong alpha everywhere. SPIRV-Cross typed DXVK's textures as `depth2d` because DXVK samples each D3D9 sampler both plainly and with depth compare; patched in `third_party/patches/SPIRV-Cross/`. Full write-up in STATUS.md, open issue 1.
- `f93c43c`: `tools/scripts/package_macos.sh` builds a self-contained, playable `build/package/DiRT2-macOS.zip` (no game data).

## Open issues, in priority order

1. **GPU device lost again, reported from the packaged build.** `Lost VkDevice ... Invalid Resource (kIOGPUCommandBufferCallbackErrorInvalidResource)`, then `VK_ERROR_DEVICE_LOST` on every submit.
   - The user's run: fresh data folder (no save game, so the first-time player flow), swapchain recreated from 3600×2260 to 1600×1200 on a Retina display, loss after about 3 minutes. Log: `~/Library/Application Support/Dirt2Recomp/dirt2_d3d9.log`.
   - The same error was fixed once by forcing `D3DCREATE_MULTITHREADED` (`e4e3376`); this is a new trigger.
   - Repro attempt with a copy of that data folder: the game called `ExitProcess(0)` after 54 s with no device loss, so inconclusive. Needs a longer run with input, ideally driven into the first-race flow.
   - Suspects: the new compare emulation (a gather through a comparison sampler on a depth texture bound as `texture2d`), and swapchain or render-target recreation on resize.
   - Next step: run with `MTL_DEBUG_LAYER=1` (Metal API validation) and `MVK_CONFIG_LOG_LEVEL=3` to name the invalid resource, or take a GPU capture of the failing frame.
2. **Flicker in the 3D frontend** (alternate black frames on some launches). Not rechecked since the alpha fix, which may have cured it.
3. **Performance:** about 15 fps in 3D scenes. Not profiled yet.
4. Then: drive the game into a race, fix what breaks, then the iOS preset (entitlements, data import, touch overlay, memory budget).

## Build and run

```bash
tools/scripts/build_graphics.sh            # MoltenVK (with SPIRV-Cross patch) + DXVK, once
cmake --preset macos-arm64 && cmake --build build/macos-arm64
./build/macos-arm64/dirt2 --game "<folder with dirt2_game.exe>"
tools/scripts/package_macos.sh             # playable zip in build/package/
```

- Re-lift after lifter changes: `tools/scripts/relift.sh`.
- Patches: `tools/scripts/apply_patches.sh` applies `third_party/patches/<name>/` to `third_party/<name>` or MoltenVK's `External/<name>`. It is idempotent.
- Rebuilding SPIRV-Cross alone: MoltenVK links a prebuilt `External/build/Release/SPIRVCross.xcframework`. A plain `make macos` does **not** pick up SPIRV-Cross edits. Rebuild it with the `xcodebuild ... ExternalDependencies-macOS` and `create_ext_lib_xcframeworks.sh` steps in `build_graphics.sh`, then `make macos`. Do not rerun `fetchDependencies`: it resets the External checkouts and drops the patch.
- Game data (11.5 GB) is never committed. Saves, config and DXVK logs go to `~/Library/Application Support/Dirt2Recomp` (override with `DIRT2_DATA_DIR`).

## Layout

| Path | Contents |
|---|---|
| `tools/recomp` | Lifter (Zydis; C emitter, SSE/x87, SEH landing pads, `--entries`, `--check-esp`) |
| `runtime/` | 4 GB guest address space, threads/TEB/TLS, dispatch, SEH, `RECOMP_TSO` |
| `shims/` | One folder per DLL. Key files: `d3d9/bridge.cpp` (guest COM proxies to DXVK), `d3dx9/`, `kernel32/sync.cpp`, `video/bink*.cpp` (FFmpeg) |
| `platform/sdl` | SDL3 host: window and events on the main thread |
| `third_party/` | DXVK 3.1.1, MoltenVK 1.4.1 (+ SPIRV-Cross) with patches in `third_party/patches/` |
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

- `/usr/bin/perl` and `/bin/sh` are SIP-protected and strip `DYLD_*` variables. A test started through `perl -e 'alarm ...'` silently loads Homebrew's MoltenVK 1.2.11 from `/opt/homebrew/lib`, and DXVK then reports no Vulkan 1.3 adapter. Set `$ENV{DYLD_LIBRARY_PATH}` inside perl before `exec`.
- `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` hangs DXVK.
- xlive ordinals are stdcall: a wrong argument count corrupts the guest stack far from the call site (that is how `XUserCheckPrivilege` cost a day).
- The game's video planes are `D3DFMT_A8`, not L8.

## Conventions

- Linear history on `main`: no merge commits. Rebase or recommit, and amend your own unpushed commits.
- Commit milestone by milestone with the findings in the message, and keep STATUS.md current.
- Nothing is pushed yet. The submodules show as modified because the patches are applied in place; the patch files are the source of truth.
