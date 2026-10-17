# Status

Target executable: `dirt2_game.exe`, SHA-256 `49b1e00ea1d4bd02e633ceed63390b5cae07601333f673c4efd8d2f0eb54fe48`.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Skeleton, PE loader, analysis report | Done |
| 2 | Full lift to C, links on macOS arm64 with stub imports | Done (Unicorn differential tests still to do) |
| 3 | Headless boot on macOS to the main loop | In progress: boots through CRT, GFWL and engine init to `Direct3DCreate9` |
| 4 | Frontend renders (DXVK d3d9 + MoltenVK), audio, input, Bink | |
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
- 13 functions read flags on entry; they load them from the spilled `eflags`.
- `cmp`/`test`/`sub`/logic ops are fused with their `jcc`/`setcc`/`cmovcc` into direct C comparisons; other flags are computed only when live.
- Full build (lift + compile + link, `-O2`) takes about 3 minutes on a 10-core M-series Mac. Binary: 61 MB.

## Shim layer (milestone 3)

| Area | Coverage |
|---|---|
| kernel32 | All 186 imports plus the CRT's dynamically resolved APIs (Fls*, EncodePointer, ...): waitables and threads, critical sections, TLS, heaps and virtual memory, files with overlapped I/O, paths, locale and code pages, time, SEH (`RaiseException`, `RtlUnwind`) |
| user32 / gdi32 | Headless window table, message queue, synchronous `WM_NCCREATE`/`WM_CREATE`, display modes |
| advapi32, shell32, shlwapi, ole32, oleaut32, version, imagehlp, crypt32, wintrust, winmm | Complete for the imported set |
| xlive (GFWL) | All 134 ordinals: offline, one local profile, async calls complete through XOVERLAPPED |
| ws2_32 | Network reported as down (online services are gone) |
| d3d9, d3dx9, d3dcompiler, dinput8, xinput, OpenAL, binkw32, dsound, ddraw | Not yet: milestone 4 |

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

## Run

```bash
./build/macos-arm64/tools/recomp/recomp lift "<game dir>/dirt2_game.exe" --out generated --entries tools/recomp/config/entries.txt
cmake --preset macos-arm64 && cmake --build build/macos-arm64
./build/macos-arm64/dirt2 --game "<game dir>"
```

Current stop point: `d3d9!Direct3DCreate9` from 0x00EC5F11, after CRT init, the launcher check, GFWL sign-in, settings load and the start of 17 engine threads. Next step is the D3D9 bridge.
