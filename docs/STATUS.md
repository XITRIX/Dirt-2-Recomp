# Status

Target executable: `dirt2_game.exe`, SHA-256 `49b1e00ea1d4bd02e633ceed63390b5cae07601333f673c4efd8d2f0eb54fe48`.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Skeleton, PE loader, analysis report | Done |
| 2 | Full lift to C, links on macOS arm64 with stub imports | Done, with Unicorn differential tests |
| 3 | Headless boot on macOS to the main loop | In progress: boots through CRT, GFWL and engine init to `Direct3DCreate9` |
| 4 | Frontend renders (DXVK d3d9 + MoltenVK), audio, input, Bink | In progress: 3D frontend renders with audio; open: video green, device lost after ~40 s, flicker |
| 5 | Playable race on macOS | |
| 6 | Playable race on iOS | |
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

- 68,910 live functions, 3.36M instructions, 230 C files (417 MB). Dead code is excluded by default (`--include-dead` to add it).
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
| dinput8 / xinput1_3 | Keyboard and mouse via SDL3; XInput on SDL3 gamepads, keyboard fallback pad |
| dsound | `GetDeviceID`, `DllGetClassObject` (hardware detection only) |
| binkw32 | Stubs: videos are skipped until the FFmpeg decoder lands |
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
- **Video:** `shims/video/bink.cpp` is a real Bink player on FFmpeg (`binkvideo` + `binkaudio`), with frame-buffer mode, frame-rate timing, seek, pause, volume and SDL audio. Audio plays with correct timing. Video is skipped by default: in-game the frames render green (see below). `DIRT2_PLAY_VIDEOS=1` enables playback.
- **Sync:** each waiter has its own condition variable, critical-section lookup is lock-free, and the handle table is reader/writer. The old shared `notify_all` caused lock convoys with the game's ~40 threads.
- **Bridge:** SYSTEMMEM resources keep persistent guest shadows. The locked pointer stays valid after unlock, as on Windows, and is flushed at unlock and before `UpdateTexture`/`UpdateSurface`.

### Open issues

1. **Video renders green.**
   - Verified correct at the API level, at the moment of the draw:
     - decoded YUV reaches the game's planes and the L8 dynamic textures (readback);
     - the textures are bound to stages 0–2, with clamp and linear sampling;
     - pixel-shader constants are correct (`vidSize` 1280×720, `bufferSize` 2048×1024);
     - vertex streams are correct (UV up to 0.625×0.703, white colour);
     - the vertex shader is a pass-through.
   - The same dumped shader, textures, constants and per-frame DISCARD pattern render correctly in `tests/dxvk_smoke/video_shader.cpp`.
   - Next step: compare the DXVK state (spec constants, sampler and descriptor bindings) for the in-game draw against the test, for example with `MVK_CONFIG_DEBUG` and a Metal frame capture.
2. **Device lost after about 40 s** (`kIOGPUCommandBufferCallbackErrorInvalidResource`): a resource is used after being freed. Suspects are the DXVK null-descriptor patch and the bridge's resource lifetimes.
3. **Flicker in the 3D frontend**, reported on screen and not yet investigated. It may share a cause with 2.

Debug switches:

| Switch | Effect |
|---|---|
| `DIRT2_LOG_FILES` | File opens and overlapped reads |
| `DIRT2_LOG_WAITS` | Waits ≥ 50 ms and sleeps |
| `DIRT2_LOG_LOCKS` | Resource locks, updates, texture bindings, pixel shader changes |
| `DIRT2_LOG_CTAB` | D3DX constant table lookups and uploads |
| `DIRT2_DUMP_SHADERS=<dir>` | Shader bytecode |
| `DIRT2_WATCH_PS_CONST=<name>` | Full state dump for draws with the pixel shader whose CTAB names `<name>` |
| `DIRT2_SCREENSHOT_DIR` | Back-buffer capture |

`tests/dxvk_smoke/` holds standalone DXVK checks: clear, texture sampling, and the game's video shader.

## Run

```bash
./build/macos-arm64/tools/recomp/recomp lift "<game dir>/dirt2_game.exe" --out generated --entries tools/recomp/config/entries.txt
cmake --preset macos-arm64 && cmake --build build/macos-arm64
./build/macos-arm64/dirt2 --game "<game dir>"
```

Graphics prerequisites: `tools/scripts/build_graphics.sh` (MoltenVK + DXVK) and `brew install sdl3 glslang`.
