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
