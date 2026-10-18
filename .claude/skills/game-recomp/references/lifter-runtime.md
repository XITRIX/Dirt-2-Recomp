# Lifter and runtime

## Choosing the approach

- Static recompilation lifts each guest function to one C function that operates on a CPU context struct (`X86* c`). Indirect calls and jumps go through a sorted dispatch table.
- Prior art worth reading: **recomp-kit** (behind populous-recomp: dispatch table, discovery log, SEH chain walker, `{dll, name, argc, fn}` shim table, SDL3 host) and **pcrecomp** (packed SSE and 80-bit x87 semantics). Neither ran a large game as-is (Python and Ghidra pipeline, a 256 MB guest, a single-runner thread model, no SSE), so we wrote our own C++ lifter and copied the design.
- Lifter language: C++ with Zydis (FetchContent). The output is C (`gen_NNNN.c`, about 300 functions per file), compiled at `-O2`.
- Reverse-engineering aids, not part of the build: `rea` (Hopper backend; names subsystems, finds the main loop, renderer and threads), plus the lifter's own `recomp disasm <exe> <addr> [n]`. For quick RE, grep the generated C: every instruction is a `/* ADDR: asm */` comment.

## PE analysis

- Load sections, relocations, imports and TLS; skip relocation (the image loads at its preferred base, here 0x400000).
- **Discovery inputs:**
  - recursive descent from the entry point and the TLS/CRT init tables;
  - every relocation target inside `.text` (vtables, callbacks);
  - prologue and padding scans (the gap scan);
  - a run-time discovery log (`recomp_discovered.txt`) merged into `config/entries.txt`.
- **Jump tables:** take bounds from the relocation table plus `cmp/ja` guards. Handle byte-index tables, and displaced bases such as MSVC memcpy's `LeadUpVec`. Always decode a table's targets inside the owning function, and purge table data that was misread as functions.
- **Unresolved `jmp reg`** sites were virtual tail calls (`mov eax,[ecx]; mov eax,[eax+N]; jmp eax`): lift them as dispatch tail calls.
- **No-return analysis:** a greatest fixed point over "has a reachable `ret`, an import tail jump, an indirect jump or an edge to a returning function". Do not mark `RaiseException` no-return: MSVC's SetThreadName trick continues after it.
- **Wrong boundaries are safe.** The guest stack is in guest memory and every register is in the context, so splitting a function at a jump target and tail-calling the rest is always correct; a wrong boundary costs only speed.
- **Numbers for scale (DiRT 2):** 160,860 functions, of which 68,910 are referenced and 91,950 come from the gap scan. 99.89% of `.text` classified, 780 jump tables, 4.44M instructions, 314 mnemonics (460k SSE, 64k SSE2, 44k x87). The full lift with dead code: 537 files, 5.4M instructions, about 5 min to build on 10 cores, a 61 MB binary.

## Code generation

- **Memory:** reserve 4 GB of host VA anywhere (`mmap(MAP_NORESERVE)`). A guest address is `base + u32`, so there is no fixed low mapping and it works under iOS `__PAGEZERO`. Make the first 64 KB `PROT_NONE` so null dereferences fault. iOS needs the `extended-virtual-addressing` entitlement for this.
- **Calls:** a direct `call` becomes a C call: `push32(c, ret); f_XXXX(c);`. Indirect calls go through `recomp_call(c, target)` (binary search of the sorted table), and an unknown target is logged to the discovery file and aborts. A callee's `ret N` adjusts `esp` itself.
- **Flags** are kept as split fields. `cmp/test/sub/logic` are fused with the following `jcc/setcc/cmovcc` into direct C comparisons; other flags are computed only when live. Functions that read flags on entry load them from the spilled `eflags`.
- **SSE/SSE2** maps to NEON through SIMDe or hand-written intrinsics. MXCSR FTZ/DAZ maps to FPCR.
- **x87** is modelled as `double`, with exact f80 load/store and the FILD/FISTP integer paths. NaN payload differences between ARM and x86 are expected.
- **SEH:** functions that write `fs:[0]`, or call a small, widely used helper that does (`__SEH_prolog4`, `_EH_prolog`), get a `setjmp` landing pad (`RecompFrame` with the function's sorted block labels). `RaiseException` walks the guest chain and calls the lifted `__CxxFrameHandler`. Catch and `__except` continuations resume through `recomp_resume_at(label)`, which longjmps into the owning frame. About 2,000 functions needed this.
- **Immediates** must be masked to the operation width: a sign-extended `push imm8` was once truncated.
- **Threads:** real host threads, each with its own context, guest stack (with guard pages) and TEB/TLS. `Interlocked*` and the sync objects are implemented on host atomics and condition variables.
- **Memory ordering (TSO):** x86 is TSO and ARM64 is weak. A `RECOMP_TSO` build makes every guest load an acquire and every store a release (`ldar`/`stlr`; unaligned accesses use plain copies with fences), and memory `xchg` is atomic. It is on by default; the cost was acceptable, and it rules out a whole class of races.

## Function hooks (host replaces a lifted function)

Needed when a behaviour must change inside the game's code and there is no vtable slot to patch. For example: answering a missing localisation id.

- Keep a `config/hooks.txt` (one hex address per line) and pass `recomp lift --hooks <file>`.
- For each listed address, the lifter emits the body as `f_X_orig` and declares that name only in its own file. `funcs.h` and every caller stay unchanged, so only one generated file recompiles.
- The host defines `extern "C" void f_X(X86* c)`. It reads its arguments from `c->esp + 4...`, does its work and returns with `c->esp += 4 * (1 + nargs)` for stdcall or thiscall, or calls `f_X_orig(c)`.
- Write generated files only when their content changed, so a re-lift stays incremental.
- **vtable hooks need no lifter change.** Check that the slot holds the expected function (for example `0x005704E0` at `0x0131B6C0`), then write a host thunk there (`rt::alloc_thunk`). The hook can call the original with `rt::invoke(c, addr, {args}, thiscall)`.

## Differential tests (Unicorn)

- For each instruction case, lift it with the real `FnLifter` twice: once with all flags live at `ret`, and once with default liveness. This covers both the fused and the unfused paths.
- Run both variants and Unicorn (`UC_MODE_32`) from the same 256 random states, biased towards 8, 16 and 32-bit edge values. Compare the GPRs, flags, DF, 64 KB of memory, the XMM registers and x87 TOP. Skip flags that Zydis reports as undefined.
- **Bugs these tests caught:**
  - a zero shift count (immediate, or CL masked to 0) must pass the old flags through;
  - 16-bit `shld`/`shrd`;
  - a signed bit offset in 16-bit `bt*` with a memory operand;
  - the `fprem` C0/C3/C1 quotient bits.
- **Accepted differences:** x87 results within a tolerance (Unicorn runs with MSVC's 53-bit precision control); NaN lanes compare equal whatever the payload; Unicorn's `fprem1` is wrong when |y|/2 < |x| < |y|.
- Cases are assembled from text by a clang that targets i386 (`assemble.py`).

## Run-time aids built into the runtime

- `RECOMP_TRACE=1` logs every shim call with its arguments, result and guest return address. It changes timing, so a race can disappear under it.
- **Crash handler:** guest registers, then a host backtrace whose frames are named `f_<guest address>`. Map a frame back to guest code by finding the instruction comment in that function, or by reading what the function pushes and calls.
- `RECOMP_WATCH=addr,len` (a watch build) traps guest stores to a range, to find who corrupts a field.
- `--check-esp` lifts with a check that every `ret` sees the entry `esp`, to find stack imbalances.
- `rt::fatal` sleeps briefly before `abort()`, so the stderr tee writes its last lines to the log file.
