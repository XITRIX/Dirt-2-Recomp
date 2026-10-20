# Switch performance investigation

The visual correctness checkpoint is `41d7774`. DX11/DXVK/NVK is the primary
renderer; `switch-deko` retains D3D9/deko3d. Hardware feedback reports clean
images, but about20 FPS in menus and11–13 FPS in races at0.75x/low settings.
The user additionally reports that reducing resolution does not change FPS and
GPU load is around30–40%. This strongly supports a CPU-side limit; distinguishing
engine CPU work, graphics submission and synchronization still needs the trace.
The target is30 FPS. It has **not** been reached or demonstrated by these changes.

## Established evidence

- The physical Vulkan output is1280×720. Guest rendering was960×544 in the
  retrieved settings, with MSAA off. The0.5x mode is640×360; a separate XML
  restriction forced it to640×480 until the private Switch override was added. The user confirmed640×360 works.
- With bounded fresh exposure readback, native exposure Map costs a median
  **0.014 ms** in the race trace (95th percentile0.016–0.018 ms). This no longer
  explains60–90 ms frames. Copy-to-read age is a different measurement.
- The clean-session logs/settings/CSV and analysis are preserved under
  `build/package/switch-dx11/session-20261020-195601/`.
- A75-second Mac menu/intro run using an isolated data directory produced
  sampling data under `build/performance/mac-menu/`. Native backend: DXMT on an
  M1 Max. These results identify shared CPU candidates, not Switch bottlenecks.
- In five10-second windows, object-proxy conversion (`to_host`) accounted for
  at least4.558% of reported non-kernel self samples. It previously took a mutex
  and hash lookup for every graphics object argument. Small interface-array
  conversion also allocated temporary vectors for binding/getter calls.

## First CPU changes

`PointerArray` keeps up to16 interface pointers on the call stack. Larger arrays
still use dynamic storage. Getter arrays are zeroed and output wrapping is
unchanged. Host ASAN/UBSAN regression checks cover zero/boundary/large arrays,
input/getter reuse, and moving the storage.

`DIRT2_COM_CACHE=1` opts into a64-slot per-thread proxy lookup cache. Proxy
retirement or host-object reuse increments an atomic generation. Misses still
use the authoritative locked registry, and null lookups are never cached. COM
reference ownership and guest proxy identity are unchanged. Retirement/reuse and
collision regressions run100,000 randomized operations under ASAN/UBSAN. The
cache stays **off by default** until Switch comparison data supports enabling it.

Four bounded Mac runs used the same binary/settings with cache off/on/on/off.
Median FPS in the15–30 second menu window was214.55/208.05/202.35/203.85.
Reported proxy-lookup self-sample shares were4.95%/2.71%/2.63%/3.47%. No unknown
proxy errors occurred. This suggests reduced lookup CPU work but establishes
**no FPS gain**; menu animation, warmup and other workloads introduce variance.
The comparison isolates the cache, not the stack-array change. Reproducible
results are in `build/performance/mac-ab/analysis.json`.

## Measure the Switch directly

The opt-in profile uses no thread suspension, stack inspection, or extra GPU
waits. It records:

- Guest-thread scheduled CPU ticks every10 seconds, using Horizon's current or
  legacy ThreadTickCount info type. Each report includes the guest entry/id and
  queried core mask/result. Native driver/compiler/audio threads are excluded.
- Native DXVK counters and Present wall time every5 seconds: draw/dispatch/pass/
  barrier/submission counts, GPU and command-stream wait time, compiler work,
  queue idle and command-stream idle.
- Native Map call time and bytes copied between guest shadows and mapped native
  resources. Copy volume helps identify memory bandwidth costs; it is not a
  measurement of memcpy execution time.

GPU queue idle is **not hardware shader occupancy**, and concurrent wait totals
must not be added or subtracted as exclusive frame phases. Five-second snapshots
can blur short stalls. Timing/counting adds overhead, so final gains need a
profile-off comparison at the same settings and scene.

```sh
bash tools/scripts/package_switch_dx11.sh --primary \
  --output-dir build/package/switch-performance-current \
  --startup-watchdog --no-disk-shader-cache \
  --fresh-exposure --async-exposure --performance-trace
```

Capture at least30 seconds in the menu and60 seconds in the same race, with
resolution/settings held fixed. Retrieve `dirt2.log`, the DXVK log, and hardware
settings after closing the game. Compare CPU saturation, compiler activity,
command-stream waits, native Map/copy volume and queue inactivity before changing
synchronization, shader options, or scheduling. Keep the brightness repair.

The linked libnx `__syscall_thread_create` was also checked: pthread creation
already sets the thread affinity to the process's allowed core mask. It would
be incorrect to assume every pthread is pinned to the default core merely
because the lower-level `threadCreate` initially receives the default-core flag.
No affinity or clock changes have been made.


The first full profiling build was installed and byte-verified on hardware as
`8d28b243d48a3b802312548bfa9dd2d9a810a857` (NRO SHA256
`abd8c14e55ea649500dd5fa6bb1bd9ccf72db78dbbefdf5a8f3eb4f74ce4d6e7`).
Its baseline leaves the lookup cache disabled; enable it later through env.txt
for comparison with the same executable. Actual Switch profiling results are
pending the next user run. The640×360 fix was tested separately and confirmed.

## First hardware profile and quit repair (2026-10-20)

Evidence: `build/performance/switch-20261020-203708/`. In the185–306 second
race window, median frame time was86.185ms (11.6FPS). Guest worker264, entry
`01161370`, consumed93.3% of one core; main260 used39.45% and worker340 used
18.5%. Allowed affinity mask was7. Native Map calls totaled0.533ms/frame;
Present0.057ms/frame; GPU and command-stream wait counters were zero. Compiler
progress was complete at1361/1361. These identify a saturated game worker,
without identifying the task executed inside its generic worker wrapper.

`--performance-trace --indirect-profile` enables100Hz cooperative sampling of
active indirect-call target/return address. Each guest thread publishes atomic
TLS context; registry locking protects TLS lifetime. Reports rank inclusive wall
contexts for threads above20% CPU. These are neither instruction PC samples nor
self CPU time. Nested calls restore context on return/C++ exceptions; a guest
longjmp can bypass that restoration, so context remains best-effort. The mode
adds atomic stores per indirect call and is diagnostic, not an FPS optimization.

The matching quit crash ends at SDL touch polling after libnx service cleanup
started from guest ExitProcess. Switch ExitProcess/TerminateProcess now terminate
all threads through svcExitProcess rather than invoking concurrent SDK cleanup.
The build passes; hardware quit validation is pending.

The uploaded start-grid recording shows wheel motion at14–15FPS. Low frame rate
is a candidate explanation, not an established physics diagnosis. An isolated
Mac12FPS intro ran, but the attempted app-controlled race comparison produced a
black screen; no matching race reference or physics fix has been validated.

## Quit follow-up

The438df7fa console test still showed a system error after Quit. Its log reaches
ExitProcess(0); FTP lists no newer Atmosphere report than the preceding build's
HID teardown report. Direct svcExitProcess avoided SDK teardown but omitted AM's
normal application-exit notification. A raw process exit is therefore unsuitable
for the normal Quit path.

Switch shutdown now dispatches to SDL's main thread and uses libnx applet exit
mode1 and its installed application-exit handler. This sends AM's SelfExit and
waits for Horizon to terminate the title, preserving GPU memory until process
termination. It neither closes HID beneath SDL nor returns to hbloader over live
native threads. Guest-entry return uses the same path and keeps the main event
pump running until the exit callback executes. Normal termination closes the
title to HOME; it does not chainload hbmenu. The user confirmed this closes cleanly.

SDK disassembly confirms the bundled _appletCleanup supports mode1 and installs
the application-exit callback, matching the upstream implementation:
https://github.com/switchbrew/libnx/blob/master/nx/source/services/applet.c

The new330-second run and worker context ranking are preserved under
build/performance/switch-20261020-214628. Worker264 has multiple substantial
active contexts in the race, including009EB920,010D3A20 and010716F0; generic
job queue00BF1750 also includes waiting. Inclusive wall contexts do not assign
self CPU cost to these functions. No physics or FPS fix has been validated from
this trace yet. The next packaged build turns off the indirect sampler now that
the trace is collected, retaining low-frequency CPU/driver counters.

## CPU comparison: dispatch cache and hot memory helpers

The shared dispatcher cache had a publication race. Two writers colliding on a
slot can interleave `fn` and `addr` stores so a reader sees A's address with B's
function even after checking the address twice. The real former dispatcher
fails the new eight-thread collision regression. The replacement uses4096 slots
per thread and an acquire/release generation for thunk re-registration. The
slots allocate lazily on the host heap; static TLS holds only their owner so
libnx's small supplied watchdog/profiler stacks remain usable. This is a proven
cache correctness repair, not a demonstrated explanation for wheel motion.

Switch GCC also outlines small guest-memory helpers in profiled engine/math
code. An opt-in `RECOMP_INLINE_GUEST_MEMORY` attribute prevents that outlining
without changing loads, stores, unaligned fallback fences, x87 or SSE formulas.
A limited experiment recompiles gen_0237,gen_0450,gen_0453 andgen_0456 from the
existing TSO-enabled compile commands. Other generated objects and the original
archive are retained. The source build equivalent is
`-DDIRT2_INLINE_GUEST_MEMORY=ON`, off by default pending hardware evidence.

In the static body of f_010716F0, calls drop476→63; f_01079D00 drops88→9.
Instructions grow4607→8055 and867→1512 respectively, excluding the separately
called helper bodies in the baseline. Those figures are compiler evidence,
not executed-call counts or an FPS prediction. More code can hurt instruction
cache behavior, which is why this starts with four translation units.

Reproduce the isolated archive:

```sh
python3 tools/scripts/build_switch_cpu_variant.py
DIRT2_SWITCH_CPU_ARCHIVE="$PWD/build/switch-cpu-inline/libdirt2_gen.a" \
  bash tools/scripts/build_switch_dx11.sh
```

The builder validates TSO and one matching archive member per chunk, leaves the
baseline archive untouched, and records its hash, source/header hashes and exact
compile commands in build/switch-cpu-inline/manifest.json. Packaging verifies the
archive hash and records the variant in the game manifest. Baseline NRO/ELF and
new NRO/ELF remain archived separately. No new GPU waits, affinity changes,
resolution reductions or relaxed memory ordering are part of this comparison.

Validation: real dispatcher passes4million colliding lookups plus cross-thread
re-registration/unknown-target checks, including ASAN/UBSAN. One million memory
operations compare baseline and inline variants across aligned/unaligned1/2/4/
8/16-byte accesses, checking outputs and surrounding memory under ASAN/UBSAN.
Both runtime tests are wired into CTest with assertions enabled in release.
Existing eight Unicorn differential shards pass when run outside the sandbox
(JIT execution fails with SIGILL inside it). The complete Switch build passes.
Hardware FPS and image/physics results remain pending. Detailed100Hz indirect
sampling is off; the same low-frequency CPU/driver counters are retained.

## Broader CPU build after the first hardware comparison

User confirms Quit now works. The314-second6fb23422 run is preserved under
build/performance/switch-20261020-223332. The145–270 second race window has median
11.7FPS,85.566ms/frame,Present0.0575ms. Worker264 remains about93% of a core.
Earlier race median was11.6FPS. These are different race moments/settings (the
new settings are1280x720), so they establish no controlled FPS improvement.

The next combined experiment has three parts:

1. Recompile **all540 generated objects** with Apple Clang21 targeting bare-metal
   AArch64/Cortex-A57 against the same devkitA64 newlib headers. Preserve the
   original TSO, PIC, architecture and aliasing options; no forced memory
   inlining. Disable FP contraction and host math errno, with fast-math off.
   Generated C has no native TLS, so GCC's soft-TLS flag is removed for these C
   objects only; native C++/SDL/DXVK/NVK remain on the original SDK compiler.
2. Opt into a private five-worker version of the shipped8-core job graph.
   Four-core XML leaves collision/handling/AI/cloth/pfx/solver job groups empty.
   The new map collapses existing parallel workers onto simulation roles and
   two grass workers. Solver and soft-body workers remain disjoint, as required
   by the shipped graph. The role mapping is tested against that graph exactly.
   No host affinity, priority, physics timestep, damage or visual options change.
3. Enable the previously tested COM proxy lookup cache in this package. The
   previous Mac comparison showed less lookup CPU work but no isolated FPS gain.

For f_010716F0 the baseline GCC static body has476 calls/64 vector arithmetic
instructions; Clang has39 calls/109 vector arithmetic instructions. These are
static counts, not dynamic CPU time. Generated archive text grows from136841053
bytes to161301576 bytes, so instruction-cache behavior is an explicit tradeoff.
The pool has five workers versus eight in the4-core map (each guest host stack
is16MiB on Switch), reducing stack reservations. Runtime effects remain untested.

The CPU archive builder now supports resumable, input-hashed full builds:

```sh
python3 tools/scripts/build_switch_cpu_variant.py --all --compiler clang \
  --no-force-inline --jobs 6 --output build/switch-cpu-clang
```

Clang uses `--target=aarch64-none-elf`, the original compiler's sysroot,
`-mno-outline-atomics -fno-math-errno -ffp-contract=off`. The floating-point
contract/errno options are documented in the compiler manual:
https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior

Validation:1346 Unicorn differential instruction cases pass with the selected
math/TSO options; GCC/Clang emit identical CPU/SIMD/SEH layouts; all three runtime/topology tests pass, and the optional proxy
cache passes its sanitizer regression. All540 Switch objects compile and the
full native game links. The benchmark command-line hook is opt-in through
DIRT2_BENCHMARK_FILE; both A/W getters use the same string. An isolated Mac run
using the shipped example stayed in attract mode, so it did **not** provide a
race CPU reference. It was stopped; real saves/assets were untouched. The generic
explicit-archive option was used only to avoid rebuilding unchanged Mac guest
code for that attempt, and the Mac cache was reset to normal source builds.

The combined package uses --parallel-jobs --com-cache and retains the lightweight
CPU/driver counters, fresh/async exposure and disk-cache disable. Its normal
launch does not enable the unfinished automatic benchmark path. No FPS gain,
physics improvement or30FPS result has been established for this build yet.
