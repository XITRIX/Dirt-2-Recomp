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
| binkw32 | Bink player on FFmpeg (`shims/video`); intros skipped unless `DIRT2_PLAY_VIDEOS=1` |
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

- DXVK and MoltenVK are pinned submodules. The Apple changes are in `third_party/patches` and applied by `tools/scripts/apply_patches.sh`; `tools/scripts/build_graphics.sh` builds both.
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

`tests/dxvk_smoke/` holds standalone DXVK checks: clear, texture sampling, and the game's video shader (`FMT=` texture format, `PLANES=` real decoded frames, `OUT=` frame dump). `tests/video/` holds the decoder and raw-MoltenVK harnesses.

## Run

```bash
./build/macos-arm64/tools/recomp/recomp lift "<game dir>/dirt2_game.exe" --out generated --entries tools/recomp/config/entries.txt
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

