# Status

Target executable: `dirt2_game.exe`, SHA-256 `49b1e00ea1d4bd02e633ceed63390b5cae07601333f673c4efd8d2f0eb54fe48`.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Skeleton, PE loader, analysis report | Done |
| 2 | Full lift to C, links on macOS arm64 with stub imports | Done, with Unicorn differential tests |
| 3 | Headless boot on macOS to the main loop | In progress: boots through CRT, GFWL and engine init to `Direct3DCreate9` |
| 4 | Frontend renders (DXVK d3d9 + MoltenVK), audio, input, Bink | In progress: 3D frontend and Bink videos render in correct colour with audio; open: flicker, ~15 fps in 3D scenes |
| 5 | Playable race on macOS | |
| 6 | Playable race on iOS | In progress: runs on an iPhone 15 Pro Max (A17 Pro) to the 3D title screen at 35-60 fps; data copied into Documents. The simulator GPU lacks descriptor indexing and subgroup control, so DXVK stops at adapter selection there |
| 7 | tvOS, visionOS, performance | |

## Analysis results (milestone 1)

**Functions:** 160,860 in total.
- 68,910 are referenced, found by recursive descent from the entry point, direct calls and relocation targets.
- 91,950 are unreferenced dead code, found by the gap scan. Lifting them costs only compile time; they also stay in the dispatch table in case a computed address reaches one.

**Coverage:** 99.89% of `.text` is classified as code, jump-table data or padding. 16.7 KB remains unclaimed.

**Jump tables:** 780 resolved, with 6,117 entries. Bounds come from the relocation table plus `cmp/ja` guards, and byte-index tables are handled.

**Indirect jumps:** 1,101 unresolved `jmp reg` sites. The sampled ones are virtual tail calls (`mov eax,[ecx]; mov eax,[eax+N]; jmp eax`), so they are lifted as dispatch tail calls.

**Instruction mix:** 4.44M unique instructions, using 314 distinct mnemonics.

| Extension | Instructions |
|---|---|
| Base | 3.87M |
| SSE | 460k |
| SSE2 | 64k |
| x87 | 44k |
| CMOV | 1.1k |
| MMX | 3 |

## Lifting notes

- The guest stack lives in guest memory and every guest register is in the context struct. Splitting a function at a jump target and calling the remainder as a tail call is therefore always semantically safe, so wrong function boundaries cost performance, not correctness.
- No-return analysis is a greatest fixed point over "has a reachable `ret`, import tail jump, indirect jump or edge to a returning function". `RaiseException` is deliberately not treated as no-return, because MSVC's SetThreadName trick continues after it.

## Lift results (milestone 2)

- 68,910 live functions, 3.36M instructions, 230 C files (417 MB). Dead code is excluded by default (`--include-dead` to add it). The game build uses `--include-dead`: every run-time `unknown target` miss so far (`00A4D820`, `009A6490`, ...) was a gap-scan function reached through a computed pointer. With it, 160,840 functions and 5.4M instructions go into 537 files, and the build takes about 5 min.
- 35 unimplemented instruction sites, all garbage opcodes (`in`, `arpl`, `das`, ...) or rare FPU state saves (`fnsave`, `frstor`, `fnstenv`, `fldenv`): a handful of false functions still to prune.
- 28 functions read flags on entry; they load them from the spilled `eflags`. (13 before shifts by CL were treated as passing flags through when the count is 0.)
- `cmp`/`test`/`sub`/logic ops are fused with their `jcc`/`setcc`/`cmovcc` into direct C comparisons; other flags are computed only when live.
- Full build (lift + compile + link, `-O2`) takes about 3 minutes on a 10-core M-series Mac. Binary: 61 MB.

## Lifter differential tests

`tests/lifter` checks the lifter's per-instruction C against Unicorn 2.1.1 (`UC_MODE_32`). Each of the 1,346 cases in `cases.txt` is lifted by the real `FnLifter` twice: once with every flag live at `ret` (the `flags_live_out` test switch spills them to `eflags`) and once with default liveness, so fused and unfused `cmp`/`jcc` paths and dead-flag elimination are both covered. Both variants and Unicorn run from the same 256 random states per case (biased towards 8/16/32-bit edge values), and the GPRs, flags, DF, 64 KB of scratch memory, XMM registers and x87 TOP are compared. Flags that Zydis reports as undefined are not compared.

Coverage: the ALU, unary, shift/rotate (counts 0, 1, n, CL), `shld`/`shrd`, `mul`/`imul`/`div`/`idiv`, moves and conversions, stack ops, all 16 conditions as `setcc`/`cmovcc`/`jcc` after each fusible writer and unfused, `bt*`/`bsf`/`bsr`, string ops with and without `rep` in both directions, the SSE/SSE2 set the game uses and x87 arithmetic, compares and stores.

Fixed through these tests:

- Shifts with a zero count (immediate 0, or CL masked to 0 at run time) now pass the previous flags through instead of zeroing them.
- 16-bit `shld`/`shrd` were computed as 32-bit.
- 16-bit `bt`/`bts`/`btr`/`btc m16, r16` treated the bit offset as unsigned.
- `fprem`/`fprem1` now set the C0/C3/C1 quotient bits.

Known, accepted differences:

- x87 is modelled as double, so x87 results are compared with a tolerance. Unicorn runs with the 53-bit precision control word MSVC uses.
- Float lanes that are NaN compare equal whatever the payload: ARM's default NaN is positive, x86's is negative.
- `fprem` is a complete reduction (C2 always clear), so it is only compared through the usual `fprem; fnstsw; sahf; jp` loop. `fprem1` is not compared, because Unicorn's (QEMU 5) result is wrong when |y|/2 < |x| < |y|.

```bash
cmake --preset macos-tests && cmake --build --preset macos-tests && ctest --preset macos-tests -j8
```

After editing `cases.txt`, run `tests/lifter/assemble.py` to regenerate `cases.bin.txt` (needs a clang that targets i386). `lifter_difftest <substring>` runs a subset.

## Shim layer (milestone 3)

| Area | Coverage |
|---|---|
| kernel32 | All 186 imports plus the CRT's dynamically resolved APIs (Fls*, EncodePointer, ...): waitables and threads, critical sections, TLS, heaps and virtual memory, files with overlapped I/O, paths, locale and code pages, time, SEH (`RaiseException`, `RtlUnwind`) |
| user32 / gdi32 | Headless window table, message queue, synchronous `WM_NCCREATE`/`WM_CREATE`, display modes |
| advapi32, shell32, shlwapi, ole32, oleaut32, version, imagehlp, crypt32, wintrust, winmm | Complete for the imported set |
| xlive (GFWL) | All 134 ordinals: offline, one local profile, async calls complete through XOVERLAPPED |
| ws2_32 | Network reported as down (online services are gone) |
| d3d9 | Bridge to DXVK 3.1.1 native on MoltenVK 1.4.1 (see Graphics below) |
| d3dx9 | Constant tables from CTAB, built-in run-time shaders, input semantics, mip filter/fill |
| dinput8 / xinput1_3 | Keyboard and mouse via SDL3; XInput on SDL3 gamepads (HID + `gamecontrollerdb.txt`, MFi driver off), keyboard fallback pad |
| dsound | `GetDeviceID`, `DllGetClassObject` (hardware detection only) |
| binkw32 | Bink player on FFmpeg (`shims/video`); start-up logo stings skipped (`DIRT2_PLAY_VIDEOS=1` plays them); the trailer TV, tutorials and attract play |
| OpenAL, ddraw, d3dcompiler | Not yet |

Notable fixes found by booting:

- Immediates are masked to the operation width (`push imm8` was truncated).
- Jump-table targets are always decoded in the owning function. Tables with displaced bases (MSVC memcpy's `LeadUpVec`) are resolved, and table data that was misread as functions is purged.
- SEH: functions that install `fs:[0]` frames get `setjmp` landing pads (1,990 functions). Catch and `__except` continuations resume through `recomp_resume_at`.
- The SecuROM launcher handshake (`-=[SMS_dirt2_game.exe_SMS]=-` file mapping) is emulated, so `dirt2.exe` is not needed.

Debugging aids:

- `RECOMP_TRACE=1` logs every shim call with arguments, result and guest return address.
- A crash handler prints guest registers and a host backtrace whose frames are named `f_<guest address>`.
- `tools/scripts/relift.sh [seconds]` re-lifts, rebuilds and runs headless with a time limit.
- Runtime misses go to `recomp_discovered.txt`; merge them into `tools/recomp/config/entries.txt`.

## Graphics (milestone 4)

**Stack:** game → guest COM bridge (`shims/d3d9`) → DXVK 3.1.1 d3d9 (native, SDL3 WSI) → MoltenVK 1.4.1 → Metal.

- DXVK and MoltenVK are pinned, pristine submodules. The Apple changes are in `third_party/patches`, applied to copies in `build/src` by `tools/scripts/prepare_sources.sh`; `tools/scripts/build_graphics.sh` builds both.
- DXVK patches:
  - Darwin build fixes.
  - Vulkan and SDL resolved from the process, so static linking on iOS works.
  - MoltenVK gaps treated as optional: geometry shaders, cull distance, depth clip, robustness2, null descriptors.
  - Null descriptors backed by zero-filled dummy images (one per view type) and a dummy buffer.
  - SDL window calls and surface creation marshalled to the main thread.
- `tests/dxvk_smoke` checks SDL3 → DXVK → MoltenVK → Metal with a pixel readback.
- **Bridge:**
  - Guest proxies with generated vtables (`tools/scripts/gen_d3d9_vtables.py`, 17 interfaces).
  - Arguments converted by C++ signature; pointer-free structs are passed zero-copy.
  - `D3DPRESENT_PARAMETERS` and HWNDs are marshalled.
  - Objects are wrapped by their dynamic type.
  - Every Lock/Unlock goes through guest-memory staging; vertex and index buffers keep a persistent shadow.
  - Exclusive fullscreen becomes borderless desktop fullscreen (`DIRT2_WINDOWED=1` for a window).
- **Run-time HLSL:** the game compiles one fixed file of 8 immediate-mode shaders. `tools/scripts/sm3asm.py` assembles them to SM3 bytecode with CTAB, and there is no HLSL compiler dependency.
- The lifted code builds with `RECOMP_TSO` (acquire loads, release stores) by default. Memory `xchg` is atomic.

**Boot now reaches:**
- launcher check, GFWL, hardware detection, DirectInput/XInput;
- window creation and `CreateDevice` (1024×768 → `D3D_OK`);
- the built-in shaders and the D3D pipeline cache;
- the Bink and TrackIR probes.

**Current blocker:** the engine's update thread (thread 264) calls a virtual function through a member that holds a list node instead of an object (`f_0085FFB0`, `mov esi,[ecx+0xC]`, called from `f_00864970`).
- It is timing-dependent: it disappears with `RECOMP_TRACE=1`.
- It is not fixed by TSO emulation, atomic `xchg`, header-stored heap free lists, or Windows-style pending overlapped I/O.
- Next step: a guest write-watch on that member to find the writer.

## Frontend (milestone 4, continued)

- With intros skipped, the game now reaches the 3D frontend: the magazine loading scene, then the first-race introduction in the X Games paddock with Travis Pastrana.
- **Audio:** OpenAL Soft passthrough. Streamed music and speech use the native libvorbisfile bridge.
- **Video:** `shims/video/bink.cpp` is a real Bink player on FFmpeg (`binkvideo` + `binkaudio`), with frame-buffer mode, frame-rate timing, seek, pause, volume and SDL audio. Audio plays with correct timing. Decoding lives in the host-only `bink::Decoder` (`shims/video/bink_decoder.*`), which the tests use too. Intros are skipped by default to reach the game faster; `DIRT2_PLAY_VIDEOS=1` plays them.
- **Sync:** each waiter has its own condition variable, critical-section lookup is lock-free, and the handle table is reader/writer. The old shared `notify_all` caused lock convoys with the game's ~40 threads.
- **Bridge:** SYSTEMMEM resources keep persistent guest shadows. The locked pointer stays valid after unlock, as on Windows, and is flushed at unlock and before `UpdateTexture`/`UpdateSurface`.

### Open issues

1. ~~Video renders green, 3D scene sepia and overexposed~~: fixed in SPIRV-Cross (MoltenVK's shader translator).
   - **Isolated layer by layer** (frame 120 of `AMD_sting.bik`):

     | Layer | Harness | Result |
     |---|---|---|
     | FFmpeg | `ffmpeg -vf select` | reference |
     | Our decoder | `tests/video/bink_decode_test.cpp` | 46 dB vs reference |
     | Raw Vulkan on MoltenVK | `tests/video/mvk_yuv_test.cpp` | bit-exact upload, correct image |
     | DXVK on MoltenVK, game shaders | `tests/dxvk_smoke/video_shader.cpp` (`PLANES=`) | correct with L8, **green with A8** |

   - The game's video planes are `D3DFMT_A8`, not L8. DXVK maps A8 to R8 with a (0,0,0,R) view swizzle, and MoltenVK creates that swizzled Metal view correctly.
   - **Root cause:** DXVK 3.x emits one image variable per D3D9 sampler and uses it both for plain sampling and, behind a runtime branch, for depth comparison. SPIRV-Cross saw the comparison use and declared the texture `depth2d<float>`. Sampling a `depth2d` returns only the red channel splatted to all four, so alpha was red on every texture: A8 read 0 (green video), L8 and X8R8G8B8 lost their forced alpha of 1, and A8R8G8B8 alpha came from red. That broke the 3D scene's blending and grading.
   - **Fix** (`third_party/patches/SPIRV-Cross/0001-msl-mixed-compare-images.patch`): an image declared non-depth that is sampled both ways stays `texture2d`, and its comparisons are emulated (gather plus bilinear PCF, LESS_EQUAL as in D3D9; point compare for cube and 3D). All other shaders translate exactly as before.
   - Pitfall found on the way: `/usr/bin/perl` (SIP) strips `DYLD_LIBRARY_PATH`, so a test started through `perl -e alarm` loads Homebrew's MoltenVK 1.2.11 and DXVK finds no Vulkan 1.3 adapter. Set the variable inside perl (`$ENV{DYLD_LIBRARY_PATH}=...`).
2. ~~Device lost after about 40 s~~: fixed by forcing `D3DCREATE_MULTITHREADED`.
3. ~~Flicker: alternate black frames on some launches~~: fixed in MoltenVK.
   - Measured, not guessed: a screen recording analysed per frame showed one presented image in three black on affected launches (2 of 5 launches), while every back buffer read just before Present was intact. One presenter, one thread, one back buffer: the loss was after Present.
   - Root cause: DXVK blits into the swapchain image through an sRGB view (`BGRA8Unorm_sRGB` on a `BGRA8Unorm` image). MoltenVK therefore makes a Metal texture view of the image's texture, which for a swapchain image is the drawable current at first use, and caches it forever. CAMetalLayer hands out other drawable textures later, so frames for that image index went into a texture nobody presented.
   - Fix (`third_party/patches/MoltenVK/0003-swapchain-view-follows-drawable.patch`): a cached view of a swapchain image is rebuilt when the drawable texture changed. 10 of 10 launches clean afterwards. Forcing vsync, synchronous submits or presenting without a command buffer did not help, which ruled out pacing.
4. ~~Device lost again (`kIOGPUCommandBufferCallbackErrorInvalidResource`), and fullscreen launches that could take down macOS~~: fixed in MoltenVK.
   - `MTL_DEBUG_LAYER=1` asserted `notifyExternalReferencesNonZeroOnDealloc` within about 4 s: a Metal buffer or texture was freed while a submitted command buffer still used it. Under lldb, the backtrace was `DxvkSubmissionQueue::finishCmdLists` → `~DxvkBuffer` → `freeEmptyChunksInPool` → `vkDestroyBuffer` → `MVKBuffer::destroy`.
   - **Root cause:** on macOS 15+, MoltenVK keeps resources resident through one `MTLResidencySet` attached to the queue, and submits command buffers with unretained references (`needsRetain` is false when a residency set exists). `MVKDevice::removeResidency` removed the allocation and committed the set immediately, and the caller released the Metal object straight away. DXVK frees memory once *its* command list completes, while other command buffers still in flight rely on the residency set. Once the GPU touches a non-resident or freed allocation, `InvalidResource` follows, then device lost.
   - **Fix** (`third_party/patches/MoltenVK/0002-defer-residency-removal.patch`): `removeResidency` retains the allocation and commits an empty command buffer on every queue. The allocation leaves the set, and is released, only after all of those complete, which is after all earlier work on every queue.
   - Verified: 222 s windowed under Metal validation with no errors and up to about 84 fps, where it previously failed at about 4 s. Fullscreen has not been retested here, because it previously crashed the whole system.
   - The same run then hit a lifter miss at `0x00A4D820`, now in `tools/recomp/config/entries.txt`.
5. ~~Gamepad not responding~~ (Backbone One PlayStation Edition, VID `358a`, PID `0203`).
   - The shim worked: the game polls `XInputGetState(0)` and the pad opened. But SDL's GameController (MFi) driver never delivered any button or axis input for it, even inside a signed `.app` bundle with background events allowed. The same pad over IOKit HID works.
   - **Fix:** set `SDL_HINT_JOYSTICK_MFI=0`. Xbox, PlayStation and Switch pads still use SDL's HIDAPI drivers. Other pads use IOKit HID with the community mappings in `third_party/SDL_GameControllerDB/gamecontrollerdb.txt`, which is copied next to the executable and loaded at startup. That file maps this pad, and the mapping matches a manual calibration.
   - Optional user mappings go in `<data dir>/gamecontrollerdb.txt`. `DIRT2_JOYSTICK_MFI=1` restores the MFi driver.

Debug switches:

| Switch | Effect |
|---|---|
| `DIRT2_LOG_FILES` | File opens, reads and writes (sync and overlapped) |
| `DIRT2_LOG_REG` | Registry opens, queries and sets (registry is in-memory, per run) |
| `DIRT2_LOG_WAITS` | Waits ≥ 50 ms and sleeps |
| `DIRT2_LOG_LOCKS` | Resource locks, updates, texture bindings, pixel shader changes |
| `DIRT2_LOG_CTAB` | D3DX constant table lookups and uploads |
| `DIRT2_DUMP_SHADERS=<dir>` | Shader bytecode |
| `DIRT2_WATCH_PS_CONST=<name>` | Full state dump for draws with the pixel shader whose CTAB names `<name>` |
| `DIRT2_SCREENSHOT_DIR` | Back-buffer capture |
| `DIRT2_INPUT_TRACE` | First XInput call per pad/result and every pad state change |
| `DIRT2_JOYSTICK_MFI=1` | Re-enable SDL's GameController (MFi) joystick driver |
| `DIRT2_LOG_CS` | Log each InitializeCriticalSection with its caller |
| `DIRT2_PLAY_VIDEOS=1` / `=0` | Also play the start-up logo stings / skip every video (default: logos skipped, the rest plays) |
| `DIRT2_LOG_MAP` | DX11: reads of small mapped resources (the luminance readback) with their values, every 30th |
| `DIRT2_RENDERER=dx11` | Run the game's DX11 renderer through DXMT (default `dx9`, DXVK) |
| `DIRT2_DUMP_HLSL=<dir>` | DX11 path: save the HLSL of run-time `D3DCompile` calls not served by the immediate-mode shaders |
| `DXMT_LOG_LEVEL`, `DXMT_LOG_PATH` | DXMT's own log |
| `DXVK_PIPELINE_TIMING=1` | Log each pipeline compiled at a draw, with its time (stutter) |
| `DXVK_PIPELINE_RECORD=` / `DXVK_PIPELINE_WARMUP=` | Empty: stop recording pipelines / skip the warm-up (HANDOVER.md, Pipeline warm-up) |

`tests/dxvk_smoke/` holds standalone DXVK checks: clear, texture sampling, and the game's video shader (`FMT=` texture format, `PLANES=` real decoded frames, `OUT=` frame dump). `tests/video/` holds the decoder and raw-MoltenVK harnesses.

## Run

```bash
./build/macos-arm64/tools/recomp/recomp lift "<game dir>/dirt2_game.exe" --out generated --entries tools/recomp/config/entries.txt --hooks tools/recomp/config/hooks.txt
cmake --preset macos-arm64 && cmake --build build/macos-arm64
./build/macos-arm64/dirt2 --game "<game dir>"
```

Graphics prerequisites: `tools/scripts/build_graphics.sh` (MoltenVK + DXVK) and `brew install sdl3 glslang`.

## Build system: one static recipe for every platform

All third-party code is built from pinned sources as static libraries and linked into one executable, on macOS as on iOS: SDL3 3.4.18, OpenAL Soft 1.25.2, libogg 1.3.5, libvorbis 1.3.7, FFmpeg 7.1.1 (Bink decoders and demuxer only), MoltenVK 1.4.1 and DXVK 3.1.1 (d3d9 built static on Apple). Nothing is linked from Homebrew or `/usr/local`; the macOS binary loads only system libraries and frameworks (`package_macos.sh` checks this).

Outputs are per platform (`build/<component>/<platform>`, platforms `macos`, `iphoneos`, `iphonesimulator`), so platforms build in parallel. DXVK and SDL resolve `vkGetInstanceProcAddr` and SDL's functions in the process; the link keeps them with `-u`.

iOS specifics:

- The app reads game data from its Documents folder (Finder or Files app); `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` are set.
- Device entitlements: `extended-virtual-addressing` (the 4 GB guest space) and `increased-memory-limit`. The simulator refuses ad-hoc apps carrying them, so the simulator build is signed without.
- The iOS simulator's GPU lacks `descriptorIndexing`, `shaderSampledImageArrayDynamicIndexing`, `samplerMirrorClampToEdge`, `occlusionQueryPrecise`, `computeFullSubgroups` and `subgroupSizeControl`. DXVK 3's binding model needs the first ones, so rendering can only be tested on a device; the simulator runs boot up to adapter selection. `imageCubeArray` (also missing there) is tolerated on Apple: D3D9 has no cube arrays.



## Switch 2 Windows build (2026-11-13)

Built the game NRO and standalone DX11 diagnostic with devkitA64 GCC 16.1.0
and ounce-sdk 20261113 (libounce/OunceVK). The isolated `switch2` preset and
Windows helper are documented in docs/SWITCH2.md. Switch 1 NVK/deko3d remain
separate. Windows fixes cover streamed tar extraction, CRLF patches, SDL
precompiled headers, native GNU ar MRI paths, and the DX11 shim's SDL3 dependency.
Upstream submodules were not edited.

The executable matches the CI-pinned SHA-256 and lifted to 160,840 functions,
5,401,170 instructions and 99.89% coverage. Both NROs linked; the game ELF is
AArch64 PIE with no undefined symbols. Three host regressions and script syntax
checks passed. Build ID: 6fbd82a5decfda955f20dcfcbc962af5a64d4aaa.

Copied 6,302 assets (11,481,382,586 bytes) to `sdmc:/switch/dirt2/Dirt 2`, with
size verification. Executable readback matched the build input. Uploaded
`dirt2-switch2.nro` and `dirt2-switch2-gpu-test.nro` to `sdmc:/switch/dirt2`,
verifying both by SHA-256 readback. Settings/logs stay in `sdmc:/switch/dirt2`;
the existing savegame tree below it is retained. No saves or settings were deleted.

Neither NRO was launched. Next: GPU diagnostic, guest-memory permissions,
menus, one race and shutdown on hardware. Compilation does not establish
DXVK/OunceVK feature coverage or gameplay. Detailed evidence is retained in
`build/package/switch2/validation.json`.


### Switch 2 GPU diagnostic device result (2026-11-13)

The deployed diagnostic recorded `DX11 preflight: final result PASS` at 2.534 s.
Native C11/thread checks, audio, RGBA8/RGBA16F clear/copy/readback (zero
mismatches), Vulkan swapchain creation and presentation passed. The user
reported the app closed; FTP was subsequently available at 192.168.1.182:5000.
The log also contains `GPU retirement after the last instance: status 0x102`.
Do not mark teardown fully qualified from the PASS marker alone. Game execution
and gameplay remain untested. The original logs are saved with the validation
record under device-gpu-test-20261113-171026 in the CivSwitch runs directory.


### Switch 2 game startup failure and diagnostic (2026-11-13)

The first game attempt ended the entire host application. Captured dirt2.log
contains only successful native C11 checks and the shared save-path message.
It has no CPU exception address, executable-loaded marker or game GPU-device
creation. The existing DXVK log and dirt2.prev.log are from the passing probe.
The current HEN nro-cleanup.txt describes hbmenu, not the failed game, and
there is no crash-report directory in the inspected SD roots. The exact cause
is therefore still unconfirmed; the next code is guest-memory setup/image load.

A diagnostic NRO was built and deployed at the same game filename on
192.168.1.182:5000, retaining the previous binary under a backup suffix.
It leaves mapping behavior unchanged and flushes durable checkpoints to
`sdmc:/switch/dirt2/startup-stage.log` around reservation, image reads/hashing
and the first four native mappings. The previous trace rotates to
startup-stage.prev.log. The new build linked and its upload passed SHA-256
readback; a fresh device run is required to identify the failing call.


### Switch 2 forbidden mapping syscall and heap backend (2026-11-13)

The diagnostic device trace stopped at memory.MapProcessCodeMemory.begin,
after the PC executable passed its hash check. The downloaded staged native
title policy (/ouncehen/bin/game/main.npdm) forbids SVC 0x77 and the remaining
cross-process mapping calls in the original backend. SVC 0x02 is allowed.
Evidence is retained under device-startup-trace-20261113-172711 in the CivSwitch
port run directory, including the trace, exact NPDM and decoded syscall mask.

Switch 2 now owns one aligned 4 GiB heap allocation, initially inaccessible,
and commits ranges with ordinary RW page permissions. Low 64 KiB guard and
overflow-safe bounds are retained; Switch 1 mapping behavior is unchanged.
The host guard/bounds/failure-ownership regression passed. The full game build
linked with zero undefined ELF symbols; memory.o references only the allowed
svcSetMemoryPermission for memory operations. ELF Build ID:
1bc966fb2ed5f24fff66d1f4a53fe3f8ba7a17ed.

The replacement NRO was uploaded to 192.168.1.182:5000 at the existing filename,
with staged and activated SHA-256 readback and the previous NRO retained under
a backup suffix. All game assets, settings and sdmc:/switch/dirt2 saves remain.
This backend holds 4 GiB of heap: memory capacity, permission behavior and game
startup still need a fresh device run. No menus, race or gameplay are verified.
Deployment hashes and backup path are in validation.json guestMemoryBackendFix.


### Switch 2 heap alignment correction (2026-11-13)

The next device run returned to HBMenu. startup-stage.log recorded total heap
0x223DC0000, free heap 0x220C20000, and a null 4 GiB allocation; the permission
call was never reached. dirt2.log confirmed mem_init allocation_failed (stage 1).
Capture: device-heap-window-20261113-174401 in the CivSwitch port run directory.

Disassembly of the exact libounce_malloc.a shows mi_arenas_page_alloc_fresh
compares alignment against 0x10000. Larger alignment bypasses the premanaged
arena and requests backing from _heapPagesAllocate outside it. _mallocInitialize
preallocates nearly all heap into the arena, leaving only a small external
reserve. Our 2 MiB alignment therefore made the 4 GiB request ineligible for
the arena despite ample reported total free space. No syscall had failed yet.

The allocation now requests 64 KiB alignment, enough for all page permission
changes, while keeping the 4 GiB address window and low 64 KiB guard. Allocation
alignment, returned pointer and errno are flushed into the trace. The regression
uses a 64 KiB-aligned pointer that is not 2 MiB aligned and an allocator that
rejects requests above 64 KiB. Guard/bounds/failure ownership tests passed, as
did the incremental native game build. The replacement was uploaded with staged
and activated SHA-256 readback, retaining the old NRO backup. Save/data root
remains sdmc:/switch/dirt2. A fresh console run must still qualify allocation,
permissions and startup. Menus/gameplay remain unverified.


### Switch 2 first gameplay and performance baseline (2026-11-13)

The user confirms menus and races now run with the 64 KiB-aligned heap backend,
about 50 FPS in menus and 20 FPS in races with noticeable input lag in handheld
mode. The captured trace confirms the 4 GiB allocation and all recorded page
permission calls succeeded. The game log reached ExitProcess(0) after 528.8 s.
This is initial gameplay bring-up, not completion of a full-race or teardown
qualification.

Evidence: performance-20261113-180036 in the CivSwitch port run directory.
The captured settings are 1280x720, MSAA off, vsync 0, medium graphics detail,
workerMap4Core, shadows/particles/mirrors/AO enabled. Present averages include
17.7-19.6 FPS intervals; scene boundaries were not recorded, so do not assign
the entire log to one race. The driver selected MAILBOX presentation and one
shader compiler thread. These observations alone do not distinguish guest CPU,
driver, GPU, or shader compilation as the sustained performance bottleneck.

Existing low-frequency counters were enabled through /switch/dirt2/env.txt:
DIRT2_PROFILE=1 and DIRT2_DX11_PERF=1; indirect and native instruction sampling
are disabled for this first pass. Readback verified. No binary, graphics setting,
clock, affinity, game asset or save changes were made for this profiling pass.
CPU accounting reports registered guest threads every 10 s; DXVK reports frame
means, waits, draw/map/copy and compiler counters every 5 s. Native driver/audio
CPU is excluded, wait counters overlap, and queue idle is not GPU occupancy.

Next device run: same scene/settings, 30 s menu, 2 min race, restart the same race
and drive another minute, then quit normally and open FTP. Compare compilation
activity and warm-race CPU/wait samples before selecting an optimization. The
read-only capture_performance.py and analyze_performance.py utilities and config
backup record reside in the port run directory. Analyzer checks passed for
weighted FPS, contained time-window filters, wait/compiler parsing and CPU data.
No Switch 2 performance improvement has yet been measured.


### Switch 2 measured CPU limit (2026-11-13)

The first profiling run is captured at performance-20261113-180838 in the
CivSwitch port run directory. Same handheld 720p medium/no-MSAA settings. In
the late 210-270 s window, fully contained DXVK reports average 20.646 FPS;
median five-second mean frame time is 49.415 ms. Guest worker264, entry01161370,
uses median94.4% of one core. Main260 uses58.7%, worker34034.8%, worker32017.5%.
All queried guest affinities are preferred-1, mask3F, result0: six cores allowed.
Present median0.029ms/frame, native Map0.482ms/frame, GPU/CS waits both0.
Compiler activity is mostly idle in that late window. Queue-idle is not shader
occupancy, but the saturated worker plus low waits strongly supports a guest
CPU critical path, not sustained shader compilation or a resolution-only limit.
No input-to-display latency measurement was made; input lag is user feedback.

Targeted PC/LR sampling is enabled for tid264 on next launch (20Hz). The existing
profiler gates kernel access on advertised0x32/0x33 syscalls, resumes before
allocation/logging, and falls back to cooperative indirect wall contexts if the
loader hints are absent. The capture guard regression passes failed-pause/read/
resume cases. The baseline environment is preserved on device under
env.txt.backup-20261113-181024 and locally; readback verified. Same executable,
graphics, clocks, affinity and saves. Next: drive same race60-90s, quit/openFTP,
resolve samples against exact ELFfb0c7638fb312da749e8ffd70795a65cea78341a, then
select a CPU/codegen optimization. Existing Switch1 compact job-map experiment
previously stalled race loading and must not be enabled as an unverified fix.
No performance gain has been claimed or measured.


### Switch 2 profile-directed memory-helper inlining (2026-11-13)

PC capture: performance-20261113-182115 in the CivSwitch port run directory.
The exact baseline ELF is retained at build/switch2-cpu-hot-inline/baseline/
dirt2.elf (Build ID fb0c7638fb312da749e8ffd70795a65cea78341a). Profiles resolve
relative to main0xAA0E0. In steady-race180-225s, worker264 uses median94.2% of
one core, 22.87 FPS across fully contained windows. PC samples identify guest
memory helpers rd32/wr32 and f_009A4510, f_01013D00, f_0108B850, f_010B5520,
f_00C009B0, f_00C30AD0 and related routines. Wall snapshots are not CPU-weighted
self samples. SVC wait0x8B73268 and signal0x8B73270 were resolved separately;
sleeping-thread samples are not treated as hot CPU work.

New explicit build target: tools/scripts/switch2_windows.ps1 -Target cpu-hot-inline.
It creates a separate source/command/header-hashed GCC archive and relinks the
game. Selected chunks:0453,0446,0455,0232,0298,0300,0110,0241,0448,0450,0449,0273.
Only RECOMP_INLINE_GUEST_MEMORY=1 is added to those12 translation units. TSO,
unaligned-access fences, compiler architecture/optimization flags and FP policy
remain the baseline. No game logic, physics, worker topology, affinity, clocks,
resolution, assets or saves are changed. Normal -Target game returns to source
builds; the experiment is opt-in and is recorded/hash-checked in the ZIP manifest.

Measured ARM call instructions (static, not dynamic execution counts or FPS):
f_009A4510:231->4; f_01013D00:187->7; f_010B5520:263->4;
f_00C30AD0:753->165; f_00C009B0:1160->329. Calls to the small rd/wr helpers are
removed from these bodies, with acquire/release operations emitted in place.
The already-inlined f_0108B850 is unchanged. ELF text159706524->161996708 bytes
(+2290184, about1.4%). Larger code can affect instruction-cache behavior.

Validation: one million baseline/inline aligned and unaligned memory operations
produce identical outputs and surrounding bytes; four Switch2 tool/packaging
regressions pass, including stale CPU archive rejection. Shell syntax and diff
checks pass. Full game links with zero undefined symbols. New Build ID:
a6d12fb1a4350070580d9752de51be7797f5613a. NRO SHA256:
2d38d3a7f7bc63725ea30562f8a68a84980aa597c9e803df260d53a186e58120.

Uploaded to the same /switch/dirt2/dirt2-switch2.nro on192.168.1.182:5000;
staged and activated readback match. The previous working NRO is retained at
/switch/dirt2/dirt2-switch2.nro.backup-7729b9fa6031-20261113-182731.
Instruction/cooperative sampling is disabled for the comparison; low-frequency
CPU/DXVK counters remain enabled. No on-device gain or gameplay qualification
has been established for this NRO. Next: same handheld race/settings60-90s,
quit/openFTP, compare warm windows and correctness with the preserved baseline.


### Switch 2 brightness oscillation and fresh-exposure configuration (2026-11-13)

The user reports subjective improvement from the CPU variant but continued poor
performance, and severe continuous brightness oscillation also seen previously
on iPhone in one scene. The recording was downloaded read-only from Nintendo/
Album/2026/11/13/2026111319321500-1BF2326480D88558B801604C80173858L.mp4.
Evidence: performance-20261113-183544 in the CivSwitch port run directory.
SHA256301347620360ebcad394e4023014daa0c0e5204cddcaa5b9d5c988e7377922a9,
42996904bytes,29.87s,1920x1080 capture at30.03fps. Saved game settings remain
1280x720. The capture frame rate/resolution are not game rendering measurements.

Frame decoding/contact sheet confirms scene exposure swings from dark to nearly
white while HUD remains readable. Central-scene luma spans13.4-243.0/255, but
camera/content changes are included, so this is not an exposure-only metric or
proof of cause. Recent game intervals are roughly18-27FPS, with worker264 near
94% of one core. There is no controlled before/after speedup measurement yet.

The new Switch2 environment lacked the existing fresh-exposure compatibility
flags. The prior Switch1 investigation documented hundreds of milliseconds of
old-ring exposure readback delay and user-reported stable brightness with the
fresh path. This is a strong candidate, not a confirmed diagnosis of this run.
Enabled DIRT2_DX11_FRESH_EXPOSURE=1, ASYNC_EXPOSURE=0, LOG_EXPOSURE=1 and a600s
trace. The strict mode reads the newest queued matching exposure pixel at the
known DiRT2 Map READ caller; generic maps and exposure values are unchanged.
It may wait for that pixel; map_ms and original/actual copy ages will measure
the cost and whether selection actually takes effect. No pixel clamping or
physics/render-quality change. Optimized NRO2d38d3a7... remains installed.

Environment backup: /switch/dirt2/env.txt.backup-20261113-183825; current file
SHA256fc40d6f9f8d03726411ceb417686a16cf9cc7ad2fdaddec0ed8054109157b186
passed readback. CPU/DXVK low-frequency counters retained, instruction sampling
off. Next: same scene30-60s, verify brightness visually, then quit/openFTP to
fetch exposure.csv and compare ages/Map costs. Only after stable brightness is
qualified consider the bounded-two-frame asynchronous variant. No reboot or
NRO rebuild is needed for this configuration test. Saves remain sdmc:/switch/dirt2.


### Switch 2 brightness confirmed; Clang hot-code candidate (2026-11-13)

The user confirms brightness is fixed with strict fresh exposure. Captured
performance-20261113-184642 includes13513 exposure rows. In trace100000-220000ms
(2463 race readbacks), median original sample age708.429ms versus actual31.497ms,
100% rerouted. Median Map0.008ms; summed Map0.007501ms per observed present.
This strongly supports stale exposure feedback as the visible issue and shows
its corrected race readback is inexpensive. Keep fresh=1, async=0. Worker264
still typically uses94% of one core; CPU work remains the performance target.

Clang23.1.5 was installed in the private MSYS2 UCRT64 tool environment. The new
explicit -Target cpu-hot-clang recompiles the same12 profiled chunks, leaving
the rest of the generated archive and all native C++/SDL/DXVK/OunceVK on GCC.
It keeps Cortex-A78C targeting, PIC, O2, no-strict-aliasing, TSO and forced memory
helper inlining. The existing Clang builder selects aarch64-none-elf/newlib,
removes GCC soft-TLS options from these C objects (no native TLS in generated C),
uses -mno-outline-atomics -fno-math-errno -ffp-contract=off, and no fast-math.
Floating contraction/errno options: https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior

Static GCC-inline -> Clang-inline comparisons:
f_009A4510 instructions5275->4614;
f_01013D00 instructions5509->5039, vector arithmetic38->68;
f_010B5520 instructions8388->7330;
f_00C30AD0 calls165->56, vector arithmetic213->317;
f_00C009B0 calls329->205, vector arithmetic529->722;
f_0108B850 instructions539->446.
ELF text161996708->160400292 bytes (-1596416). Counts include static bodies,
not dynamic execution frequencies or measured FPS gains.

Validation: new optional AArch64 fixture/Unicorn test compares GCC and Clang
execution from identical CPU/memory states.16384 cases pass over all rounding
modes, FTZ/default-NaN combinations, special floating values, SSE min/max/compare,
unaligned guest loads/stores and exact surrounding memory. NaN payloads alone
are canonicalized in floating output lanes; integer masks and memory are exact.
X86/XMM/RecompFrame/newlib jmp_buf sizes, alignments and key offsets match.
This is compiler differential coverage, not the complete x86 lifter oracle or
gameplay qualification. The harness enables FP and uses Unicorn's MAX CPU to
support the SDK's RCpc LDAPR instructions; default A57 model cannot execute them.
Build fixture ELFs with tests/runtime/build_arm_codegen.sh and run
tests/runtime/test_arm_codegen.py with both ELFs (unicorn/pyelftools required).

The full game links with zero undefined symbols. Build ID:
b5d3200e4e6b0f30ead8be79fcd7722f1880baf4. The previous GCC working ELF/NRO is
retained under build/switch2-cpu-hot-clang/baseline. Deployment record in
validation.json contains SHA256/readback and on-device rollback backup path.
Strict fresh exposure remains enabled, exposure CSV tracing is removed, and
low-frequency CPU/DXVK counters remain. LOG_EXPOSURE is presence-based, so
the exact key was removed rather than set to0. No game assets, saved graphics
settings, physics, worker maps, clocks, affinity or saves changed.

Next: same handheld race/settings60-90s, then quit/openFTP and compare CPU,
frame timing and visual/physics correctness. Clang candidate performance is
unverified; do not claim a speedup from smaller code or more SIMD alone.


### Next build filename (2026-11-13)

The user requests Dirt2.nro (exact case) starting with the next build. The
Switch2 game packager now emits build/package/switch2/Dirt2.nro and includes
switch/dirt2/Dirt2.nro in the ZIP. The FTP deployment helper stages this name
and preserves/removes the legacy launcher entry by renaming the old NRO to a
backup on the next upload. The currently installed test build has not changed.


### Switch2 original parallel job-graph candidate (2026-11-13)

User reports the hot-Clang build is much more playable but far from stable60FPS.
Capture performance-20261113-190808: late180-240s fully contained windows average
18.69FPS, median window frame52.649ms, worker264 median94.6% of one core;
Present0.029ms, native Map0.510ms, GPU/CS waits0. Different race moments prevent
a controlled before/after speedup claim. The CPU critical path remains.

The selected workerMap4Core.xml leaves collision, handling, AI, cloth, pfx and
generic worker sets empty. The shipped workerMap8Core.xml populates them and
keeps solver/softbody disjoint. Its full16-thread/23-group graph was downloaded
and verified identical to the PC copy, SHA256
f5e5c5ad973068ce0d67b9a8d17b1f9cd3ee78a32f74bba7637ae110295a4d89.
This is the complete original graph, not the previous compact5-worker map that
stalled Switch1 loading. No workers are merged or renamed.

DIRT2_SWITCH_STOCK_8CORE_JOBS=1 opts in on Switch2. Startup first queries the
current allowed thread core mask and requires at least six bits. It validates
the7705-byte original map by exact hash, then uses the existing read-only VFS
override to serve that original file when the game requests workerMap4Core.xml.
Missing/unrecognized input or insufficient cores logs a refusal and retains
the normal map. There is no data-file rewrite, guest CPU-count spoof, host
affinity/clock/priority change or physics algorithm change. Six allowed cores
schedule the16 workers; scheduling/runtime correctness still needs hardware
qualification. Marker: original8-core worker graph enabled;16workers.

Validation: exact-map guard accepts the real file and rejects same-size worker
reassignment, truncation and missing data. XML references and disjoint groups
checked against the actual device file. Real VFS regression confirms normalized
worker-file aliases redirect while saves/settings retain their paths. Four
packaging checks pass; native game links with zero undefined symbols.

Build ID b43c8c14a2cd493612e5c39db9dd88dbf5602fa3, SHA256
34028cad961e58d0eeedf2dc50f5ef3a9bf8e9cf6cf191d5778f7ec8f59449c9.
The package and console entry are now Dirt2.nro, as requested. Legacy filename
was renamed to a backup, not left as a duplicate launcher entry. Staged and
activated readback passed. Clang hot-code and confirmed strict fresh exposure
are retained; compact job map is explicitly0, exposure/instruction tracing off,
lightweight counters on. No assets, saved graphics settings or saves changed.
Rollback configuration: stock8 flag0 (brightness-fixed helper mode).
Next: verify race loads and drive the same event90s, then quit/openFTP. A runtime
FPS gain, race-loading success and stable60FPS have not yet been established.


### Stock job-map test did not activate: process-mask guard repair (2026-11-13)

Captured performance-20261113-223054. Startup says "stock parallel map requires
six allowed cores (mask1, result00000000)"; no enabled marker appears. The guard
incorrectly queried the startup thread's affinity, which is pinned to core0.
Registered guest workers later report mask3F. The stock graph was not loaded,
so this run cannot establish its performance impact. In170-225s contained race
windows:19.97FPS, median window mean50.4005ms, worker264 median94.6% of one core,
Present0.028ms, nativeMap0.490ms, GPU/CS waits0. Scene/draw counts vary versus the
prior run; do not attribute the difference from18.25FPS to the inactive map.
The captured exposure.csv is an unchanged earlier trace, not data from this run.

Corrected eligibility to svcGetInfo(InfoType_CoreMask,CUR_PROCESS_HANDLE,0),
which queries process core permissions. The existing SDK documents info type0
as the allowed core mask; upstream kernel GetInfoImpl returns process->GetCoreMask:
https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_info.cpp
Startup thread affinity is now diagnostic only; the enabled marker reports both
process and startup masks. No affinity is changed. Regression includes process3F
with startup1, noncontiguous six-core masks, insufficient masks and failed queries,
plus the existing exact-map/reassignment/truncation checks. Host regression,
native game build and source formatting checks pass; ELF has0 undefined symbols.

Corrected Build ID a6a748c0570fe8898833278936576b69b201d016. Dirt2.nro uploaded
with staged/activated SHA256 readback and previous build retained. Environment
already enables the stock graph and is unchanged. Brightness, Clang and saves
are retained. Next run must show the enabled marker and16 original workers;
then verify race loading and compare90s at the same event/settings. No job-map
speedup or stable60FPS is verified yet. Prior guard failure was our implementation
error, not evidence that the console only exposes one CPU core.


### Original worker graph activated; no demonstrated FPS gain (2026-11-13)

Capture performance-20261113-224158 confirms startup marker:
original8-core worker graph enabled;16workers, process core mask3F; startup
thread mask1, result0. Guest registry grows31->38/39threads, confirming the game
created the additional workers. The process-mask guard repair took effect.

For fully contained five-second reports in log100-140s: current20.731FPS versus
previous22.285FPS, median window frame47.803ms versus46.672ms. Current draws
683.1/frame versus608.9 previously. Race moments differ, so neither speedup nor
regression can be established by that difference. Both remain roughly20FPS.
Update worker264 CPU median94.1%->59.2% of one core; main/render guest260 now
69.5%. Parallel workers324-348 each about27-29%. Thus work distribution changed
substantially without a demonstrated frame-rate improvement. GPU/CS waits still0;
Present0.029ms, nativeMap0.564ms. No driver/GPU occupancy claim is made.

Only logs/settings were read; the NRO and environment were not changed. The
confirmed brightness fix remains. Next profiling target is main/render work and
job-coordination overhead to locate the remaining CPU critical path. Do not
assume more workers or smaller compiled code implies progress toward60FPS.
Exposure.csv in this capture is the unchanged earlier trace and is not evidence
of new exposure measurements with tracing disabled.


### Main/render-thread profiling and corrected copy counters (2026-11-14)

User requested continued optimization. Main/render guest260 is now selected for
20Hz PC/LR sampling using the existing deployed binary. Low-frequency CPU/DXVK
counters, full stock worker graph and strict fresh exposure remain; no new NRO
has been deployed in this step. Environment readback verified and backed up at
/switch/dirt2/env.txt.backup-20261113-234428. User has been asked to drive the
same race60-90s and reopenFTP. At the last check FTP was refused (game session
likely still open); targeted capture has not yet been retrieved.

IMPORTANT: the exact profiling ELF/NRO is preserved under
build/switch2-render-profile-baseline/, Build ID
a6a748c0570fe8898833278936576b69b201d016, NRO SHA
7b1ddee816f0119cddd61d92893fc0f0b40139378e5e3d738a791e9bb681ee93.
Resolve new PC samples against THAT ELF, not build/switch2/dirt2.elf, since a
local diagnostic-only counter build has subsequently linked at the latter path.
The generated-function index in the CivSwitch port run folder maps160838 guest
definitions across537chunks for selecting measured render-code candidates.

Source review found the reported write-copy volume counted shadow capacity even
when NO_OVERWRITE wrote few bytes. bridge.cpp now counts the actual returned
dirty-byte writes (and actual mapped bytes for regular writes), and separately
reports NO_OVERWRITE scan volume. Actual copy/write behavior is unchanged. The
existing exact-dirty-byte/protected-page regression passed and the full Clang-hot
Switch2 build linked. This counter-only build is LOCAL ONLY and is not a claimed
performance optimization. The next runtime change should follow the main-thread
PC trace, particularly COM/proxy lookup, shadow scans/copies or lifted render
code, rather than assume the bottleneck from the previous physics-worker trace.


### Main/render profile retrieved; render Clang and COM-cache candidate (2026-11-14)

Capture performance-20261113-235238 was decoded against the preserved
switch2-render-profile-baseline ELF (a6a748c0570fe8898833278936576b69b201d016).
Do not symbolize this capture with the new ELF. Fully contained race reports
in log90-120s average20.873FPS; median five-second mean47.740ms. Whole-log
averages include fast menus and are not race FPS. Main/render guest260 has
790 PC/LR wall samples after90s, zero capture errors.
Blocked mutex/condition waits dominate many snapshots; these are not selfCPU
costs. Active samples show rd32/wr32, lifted render routines, memcpy (mainly
ctx_unmap), and COM registry unordered_map lookup. PC/LR is not a full stack.

New target cpu-render-clang retains the12 physics chunks and adds12 measured
main/render chunks:0436,0495,0435,0432,0302,0354,0421,0022,0424,0109,0020,0119.
Reproduce with tools/scripts/switch2_windows.ps1 -Target cpu-render-clang.
Commands/source/header/archive hashes are in build/switch2-cpu-render-clang.
Same Clang23.1.5/newlib ABI and TSO/memory-order options as the prior candidate;
no fast-math. Static calls in f_00D64540 fall312->33 and f_00C400F0 fall156->12;
selected bodies grow, so fewer calls alone do not establish an FPS improvement.
ELF text160403080->161370264 bytes. All ordered-memory semantics retained.

The existing DIRT2_COM_CACHE=1 lookup cache is now enabled; its100000
retirement/collision/reuse checks pass. The protected-page/exact dirty-write
regression passes. Shadow-copy algorithms and CRT hooks are unchanged.
Corrected native copy diagnostics are included: actual dirty bytes written
are counted separately from NO_OVERWRITE scan volume. The earlier local-only
counter note above is superseded by this deployment. Prior16384 GCC/Clang ARM
fixture cases cover compiler memory/FP/ABI behavior, not whole-game correctness.
Full native game links with zero undefined symbols; shell/source checks pass.

Dirt2.nro Build ID87feaea915e36cb731d0d6fcc217b74b708694eb; SHA256
3d1adf071510b32e485e6ffe68fdb8f5e20be1382320734de5a2b09830ab38bd,
164903109bytes. FTP staged and activated SHA256 readbacks passed at
192.168.1.182:5000. Previous NRO retained on device as
/switch/dirt2/Dirt2.nro.backup-7b1ddee816f0-20261114-000051.
Environment readback verified, backup env.txt.backup-20261114-000329;
PC/indirect sampling off, lightweight counters on, COM cache1, full stock jobs1,
compact jobs0, strict fresh exposure1/async0. Exposure tracing key removed.
Original assets, graphics settings, clocks, affinity, physics and saves unchanged.

Hardware execution/performance of this new candidate is still pending. Next:
same handheld race/settings60-90s, normal quit, openFTP, compare contained race
windows and check visuals/physics. No FPS gain or stable60FPS claim yet.
Deployment/configuration/evidence records are in the CivSwitch port run folder:
render-clang-deployment.json, render-cache-configuration.json, main-symbols.json,
render-chunk-selection.json and race-90-120.json (last three under capture).


### Outdoor menu below60FPS; focused bridge diagnostics (2026-11-14)

User identifies outdoor/main-menu graphics-settings counter around40FPS and
reports almost120FPS in the same area on iPhone15ProMax. The latter is user
feedback, not a captured controlled device comparison. Capture
performance-20261114-000935 confirms the deployed render-Clang/COM-cache candidate:
contained menu60-90s windows38.231FPS, median window mean26.189ms;
race120-150s22.106FPS, median44.977ms. Changing scene/settings prevent attributing
the difference from the prior20.873FPS race to an optimization. Low-detail saved
fields coexist with graphics_detail="medium", mirrors/shadows/AO enabled; do not
silently rewrite settings or call this a verified all-low preset.

Menu Present median0.036ms, nativeMap0.305ms, GPU/CS waits0; shader compilation
completed. These counters support investigating CPU/runtime/bridge costs but
do not measure GPU occupancy or every driver operation. Race writes10.7MiB/frame;
menu around3.6-4MiB/frame. Bytes alone do not establish copy duration. Both
platforms default to TSO; Switch generated/native code is optimizedO2/NDEBUG,
not an accidental Debug build. Apple uses DXMT/Metal, Switch DXVK/OunceVK, so
identical settings do not imply identical driver/CPU costs.

Added gated aggregate wall timers to D3D11 Map/Unmap: Map-total, Map-shadow,
Unmap-total, write-copy, native-Unmap, map-lock-wait. These are inclusive/nested,
not additive CPU phases. Lock-wait covers the Map-shadow/Unmap g_mu acquisitions,
not every COM lookup. Existing copy/synchronization behavior is unchanged.
Timing parsing/contained-window regression and four packaging tests pass; full
native link has0 undefined symbols. New build is diagnostic, not a speedup claim.

Uploaded Dirt2.nro Build IDfe62b1cd1477dfa3dfef89a0fbe3290a58ebb8d2,
SHA25602bb5192f90e4ff17b7eeaa77bd7d8cf8bba58b3869ece621a5449e698869ccb;
164903109bytes. Staged/activated FTP readback verified; previous NRO retained at
/switch/dirt2/Dirt2.nro.backup-3d1adf071510-20261114-001419.
Main/render260 PC/LR sampling enabled, lightweight counters retained. Environment
backup env.txt.backup-20261114-001510. Full stock jobs, strict fresh exposure,
COM cache and user graphics settings remain. Exact new ELF/NRO preserved at
build/switch2-menu-profile-baseline; use THAT ELF for the next capture.
Records: menu-timing-deployment.json, menu-profile-configuration.json in port dir.
Next user action: launch, stay in the same slow outdoor graphics-settings menu
for60-90s, quit normally and reopenFTP. No race needed for this targeted pass.
Hardware timings and PC samples from this diagnostic are still pending.


### Outdoor menu timing result and COM-cache collision repair (2026-11-14)

Retrieved performance-20261114-090927; symbolized the exact preserved
switch2-menu-profile-baseline ELF fe62b1cd1477dfa3dfef89a0fbe3290a58ebb8d2.
Contained menu75-115s:34.732FPS, median window mean28.889ms; Map-total1.422ms,
Unmap-total1.525ms, nested write-copy0.867ms, map-lock-wait0.098ms, nativeMap0.523ms,
Present0.024ms. These are medians of window means and inclusive wall counters,
not additive exclusive CPU phases. Copy/these bridge locks are a small part of
frame time. Transient59-69s reports include compilation and GPU/Map stalls;
steady75-115s GPU/CS waits are0 and compilation is idle. Do not infer occupancy.
The user changed graphics settings: saved config remains medium with envmap6,
postprocess1, crowd/cloth/groundcover on. Draw counts differ versus the prior run;
34.7 versus38.2 is not a controlled regression measurement.

Main/render75-115s has789 wallPC samples, including104 kernel mutex waits and
103 condition waits. Active samples include COM registry find23, memcpy27,
rd32 19, f_011D5F30 17, f_00FC9C60 17, f_00498B20 15, lookup13. Wait snapshots
are not selfCPU costs and LR is not a full stack. Native COM lookup remains a
measured target even though the cache was enabled; this capture has no hit-rate
counter, so the actual reason for its misses is not yet established.

Found a concrete cache weakness in proxy_lookup_cache.h: its low-bit direct
index makes allocator-aligned addresses128/1024/4096bytes apart collide.
Changed to high-product indexing and four-way sets with the same64 entries.
Generation checks/registry mutex ownership/COM lifetimes remain unchanged.
The100000-case authoritative retirement/collision/reuse regression passes.
Five10000-lookup16-proxy stride tests:32/64byte strides retain9984 hits;128,
1024,4096byte strides improve0->9984 hits. These crafted host hit-rate results
are not an FPS benchmark or proof of the device's exact address distribution.

The cache now records per-owner-thread hits/misses without shared atomic
increments. DX11 low-frequency counters report present-thread lookup rate,
hit rate, raw hit/miss counts and bridge-wide proxy retirements/window. Other
threads' lookups are not included. Global generation invalidations still retire
all cached entries; the next trace can distinguish frequent retirements from
address collisions. No larger cache, stale-proxy acceptance or lock elision.
Native build de8903bc17b29f6975d0cf1b2d7a6e0064e426a3 links with0 undefined
symbols; four packaging tests, cache tests and timing parser checks pass.

PC/indirect sampling is disabled for the comparison, low-frequency counters on.
Strict fresh exposure, COM cache, full stock jobs, graphics settings and saves
are preserved. Deployment result is recorded separately after FTP readback.
Next test: identical outdoor menu/view/settings60-90s, then normal quit/openFTP;
compare FPS and cache hit/miss/retirement counts. This incremental fix does not
establish60FPS or explain the entire Switch/iPhone performance gap.


COM-cache candidate deployment completed with staged/activated readback:
/switch/dirt2/Dirt2.nro, SHA256
57e9e0ec7537596628802b83f04c5f1eb1ac73ad620320a7257ea42e2eeac2d6,
164907205bytes. Previous diagnostic retained as
/switch/dirt2/Dirt2.nro.backup-02bb5192f90e-20261114-091427.
Exact candidate ELF/NRO preserved in build/switch2-com-cache-baseline.
Environment readback passed; backup env.txt.backup-20261114-091505.
Device execution, hit-rate benefit and FPS change of this candidate are pending.


### COM cache run still slow; native call-chain profiling (2026-11-14)

User reports unchanged menu around40FPS. Retrieved performance-20261114-091959.
Contained50-100s reports:36.649FPS, median window mean27.289ms, Map-total0.948ms,
Unmap-total1.116ms, write-copy0.644ms, map-lock-wait0.0595ms. Main26062.5%core,
update26448.8%. COM cache active,83.4% hit over about11052 lookups/frame and0
median proxy retirements/window. Thus steady misses are not caused by repeated
global invalidation. The cache repair did not demonstrate the desired frame-rate
gain. Previous saved settings hash is identical, but draw counts564vs855/frame
show different view/workload;36.65vs34.73 is not a controlled speedup claim.

Isolated compiler experiment: add __restrict to context parameters in copied
300-function gen_0020 and gen_0436 chunks. f_00498B20 and f_00FC9C60 retain EXACT
instruction/call/load/store/ordering counts (1417/6/449/266/137 and
1253/4/376/164/194). It gives no static evidence of benefit, so was rejected.
No generated source, build recipe or deployed game code adopts the annotation.
Experiment script/context-comparison.json remain in the CivSwitch port folder;
compiled scratch is build/switch2-context-experiment.

Earlier main PC/LR capture had substantial mutex/condition blocking but cannot
identify the full engine caller chain. Added opt-in DIRT2_PROFILE_STACK=1 to
PC profiling. The registering target queries its current readable stack mapping
with svcQueryMemory. A successful pause/context read may follow at most12
AArch64 previousFP/savedLR records. Every read requires16-byte alignment,
FP>=SP and the entire record inside the saved readable mapping; links must be
nonzero/aligned and frame addresses increase. Invalid/absent bounds/frames stop
without reading. No allocation/logging/locks occur in the paused read callback;
the existing capture guard resumes even on read error, and failed resume remains
fatal. No arbitrary stack scan or guest-memory inspection. Frame omission and
tail calls can truncate chains. Results are inclusive wall samples, not selfCPU.

After resuming, aggregate chains and log top48 per10s window. The decoder
symbolizes savedLR-4, retains raw offsets and deduplicates inclusive function
counts per sample. Added boundary/cycle/depth/failed-read regression and retained
pause/read/resume error tests; both pass. Four packaging checks, parser attribution,
configuration guard and diff checks pass. Native link has0 undefined symbols.
Build IDba14fddcfff772615a6624fe79581eaf5661bbe8. Deployment confirmation follows
separately. Brightness/stock jobs/cache/settings/saves retained; the new build is
diagnostic and is not a claimed optimization.

Next test: unchanged outdoor view/graphics-settings menu60-90s, quit normally,
openFTP. Target main260, PC+stack sampling20Hz. Look for readable-stack marker,
nonzero prof-stack frame-chain coverage, failures/capture cost, then native chains
behind mutex/condition waits and active render work. Decode with the exact saved
switch2-native-stack-baseline ELF once deployment is verified.


Native-stack diagnostic deployment completed: Dirt2.nro,164911301bytes,
SHA2565df9bf258f98d228d1fad8677e2533430c9e8bca266970b1c51a22d65e1d2862.
Staged and activated readback verified; previous NRO retained as
/switch/dirt2/Dirt2.nro.backup-57e9e0ec7537-20261114-092645.
Exact ELF/NRO is preserved in build/switch2-native-stack-baseline.
Configuration readback verified, backup env.txt.backup-20261114-092738;
DIRT2_PROFILE_STACK=1, PC=1, PC_TID260. Native stack coverage and hardware
execution remain pending the requested same-view menu run.
