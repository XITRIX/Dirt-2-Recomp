# Switch 2 native NRO build

The `switch2` target uses libounce and OunceVK from ounce-sdk 20261113. It keeps
DX11/DXVK and the existing SDL3 Switch backend. Guest memory uses an owned
4 GiB heap window with ordinary page-permission changes.
Switch 1 NVK and deko3d are separate targets and are not linked into this build.

## Windows environment

The local setup uses MSYS2 at `%USERPROFILE%/.dirt2-tools/msys64` and devkitPro at
`../devkitPro` beside this repository. The PowerShell wrapper accepts `-MsysRoot`
and `-DevkitProDirectory` for other installations. It sets environment variables
only for the build process; no system PATH changes are required.

Install the supplied ounce SDK into the devkitPro root. Required host tools are
MSYS2 CMake, Ninja, Meson, GCC, Git, Python, make, patch, pkgconf and the UCRT64
glslang package. The ARM compiler and NRO tools come from devkitPro's Switch kit.
Use MSYS2 CMake rather than native Windows CMake, as required by devkitPro.

From a PowerShell prompt in this repository:

```powershell
# Build third-party dependencies and native DXVK once.
./tools/scripts/switch2_windows.ps1 -Target libraries
# Build the standalone audio/DX11 graphics diagnostic (no game data needed).
./tools/scripts/switch2_windows.ps1 -Target probe
# Build the host lifter; later lift your own PC v1.1 executable.
./tools/scripts/switch2_windows.ps1 -Target host
./tools/scripts/switch2_windows.ps1 -Target lift -GameDirectory 'C:/Games/Dirt 2'
# Build and package the actual game once generated/sources.txt exists.
./tools/scripts/switch2_windows.ps1 -Target game
```

Linux/MSYS2 users can export DEVKITPRO and invoke
`bash tools/scripts/build_switch2.sh libraries`, followed by `probe` or `game`.
The `switch2` CMake preset consumes `$DEVKITPRO/libounce/cmake/Switch2.cmake`.
Outputs stay under `build/deps/switch2`, `build/dxvk/switch2` and `build/switch2`.
Native Windows GNU ar requires Windows paths inside its MRI input, unlike shell
arguments; the archive merger converts those paths under MSYS2.

## Files on the console

- Game assets: `sdmc:/switch/dirt2/Dirt 2`, including `dirt2_game.exe` v1.1.
- Settings and logs: **`sdmc:/switch/dirt2`**.
- Saves: `sdmc:/switch/dirt2/C/Users/Player/Documents/My Games/DiRT2/savegame`.
- Switch 2 NRO names: `Dirt2.nro` and `dirt2-switch2-gpu-test.nro`.

Packages are ZIPs under `build/package/switch2`. They contain neither game data
nor env.txt, saves or existing user settings. Copy the NRO into `/switch/dirt2`
and launch through the working HEN/HBMenu title-takeover session.

The diagnostic logs to `sdmc:/switch/dirt2/dirt2.log`, retaining the previous log
as `dirt2.prev.log`. It checks native C11/thread support, audio, DX11 RGBA8 and
RGBA16F clear/copy/readback, presentation and teardown. A passing graphics test
shows a blue frame briefly and returns to HBMenu. OunceVK owns GPU teardown;
Switch 1 NVK shutdown and transfer-memory wrappers are excluded.

## Validation boundaries

Compilation does not establish that OunceVK implements every Vulkan feature
needed by DXVK 3.1.1. Run the diagnostic before testing the game. The guest-memory heap allocation and page-permission path also need
validation on hardware; compilation alone does not qualify them.
Menus, one complete race, clean shutdown and measured performance remain device
milestones. Initial Windows validation built the host recompiler and the standalone DX11
probe. The supplied executable matches the CI-pinned SHA-256 and lifted to 160,840 functions and 5,401,170
instructions, with 99.89% code coverage. The game build consumes that generated
code; its assets are copied separately and are never included in the NRO ZIP.

## Guest memory and startup tracing

The first game startup trace ended inside `svcMapProcessCodeMemory` (SVC 0x77).
The staged native title's `main.npdm` forbids that call, as well as the other
cross-process mapping calls used by the original Switch backend. The Switch 2
build instead allocates one 64 KiB-aligned, owned 4 GiB heap slice, sets it inaccessible,
and enables RW access for committed guest ranges with `svcSetMemoryPermission`
(SVC 0x02, allowed by the captured policy). The low 64 KiB remain inaccessible.
Switch 1 keeps its existing backend. No title policy or HEN binary is patched.

This bring-up implementation holds 4 GiB of the preallocated heap even when
the guest has committed less. `committed_bytes()` counts ranges made accessible,
not the complete physical allocation. Heap exhaustion or a permission failure
is logged and stops startup. Reducing that footprint is a later optimization.
Boundary/guard and failed-acquisition ownership checks run on the host; the new
backend still requires a device run. Menus and gameplay have not been qualified.

Startup checkpoints are flushed to `sdmc:/switch/dirt2/startup-stage.log`;
the previous trace becomes `startup-stage.prev.log`. They include heap capacity,
allocation/protection results, image validation and the first four commits.

The supplied SDK uses a premanaged mimalloc arena. Requests aligned above 64 KiB
bypass that arena; a 2 MiB-aligned 4 GiB allocation failed on hardware even with
8.5 GiB reported free. The backend uses 64 KiB alignment to retain arena
eligibility. This threshold was verified against the shipped allocator object;
it is independent of the 2 MiB granularity used to group guest commits.


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


## Profile-directed CPU build

`./tools/scripts/switch2_windows.ps1 -Target cpu-hot-inline` builds an opt-in
GCC variant for12 translation units selected from Switch2 race PC samples.
It forces inlining of the existing guest-memory access helpers while retaining
acquire/release ordering and unaligned-access fences. No physics or job-map
changes are included. `-Target game` returns to the normal generated source build.
The working baseline and separate variant archive are under
`build/switch2-cpu-hot-inline`; the package records and verifies the archive hash.
Static helper-call removal is confirmed; hardware performance and correctness
must be compared in the same race/settings. Do not infer an FPS gain from code
size or static instruction counts. See HANDOVER.md for the exact profile/build.


## Brightness compatibility test

The original game uses a multi-frame exposure readback ring. If brightness pumps
severely at low frame rates, the existing compatibility experiment can be enabled
in sdmc:/switch/dirt2/env.txt with DIRT2_DX11_FRESH_EXPOSURE=1. Start with
DIRT2_DX11_ASYNC_EXPOSURE=0 to consume the newest queued exposure pixel. This
can add a GPU wait. DIRT2_LOG_EXPOSURE=1 and DIRT2_EXPOSURE_TRACE_SECONDS=600
record original/actual copy ages and native Map time in exposure.csv. General
resource mapping and exposure values are not changed. The bounded asynchronous
mode is a separate performance comparison after brightness is qualified. The
Switch2 configuration was enabled after capturing the severe-oscillation video;
its on-device result remains pending.


`./tools/scripts/switch2_windows.ps1 -Target cpu-hot-clang` builds the second
profile-directed candidate with Clang for the same12 generated chunks. The
native libraries/runtime stay on the SDK GCC toolchain. Install MSYS2 package
mingw-w64-ucrt-x86_64-clang in the private environment first. The archive/package
hashes and exact compiler commands are retained in build/switch2-cpu-hot-clang.
The optional GCC/Clang AArch64 regression uses tests/runtime/build_arm_codegen.sh
plus test_arm_codegen.py (Unicorn/pyelftools). See HANDOVER.md for test scope.

Strict fresh exposure was confirmed to fix brightness on Switch2; keep
DIRT2_DX11_FRESH_EXPOSURE=1 and DIRT2_DX11_ASYNC_EXPOSURE=0. Recorded race
Map cost is about0.008ms, so it is not the sustained CPU performance limit.
Remove DIRT2_LOG_EXPOSURE after diagnosis: its presence enables tracing even
when the value is0. Current comparison keeps only low-frequency CPU/DXVK logs.


## Original parallel job-graph test

DIRT2_SWITCH_STOCK_8CORE_JOBS=1 opts into the game's complete shipped8-core
worker map on Switch2. It redirects workerMap4Core.xml through VFS to the
unmodified workerMap8Core.xml after exact hash verification, and requires at
least six process-allowed host cores (queried with GetInfo CoreMask).
Startup-thread affinity can be one core and is logged separately. All16 original workers and dependencies remain;
Horizon schedules them without affinity changes. DIRT2_SWITCH_PARALLEL_JOBS=0
keeps the older compact-map experiment off. Set STOCK_8CORE_JOBS to0 to return
to the normal map. No asset or saved-settings files are patched. This candidate
requires race-loading, correctness and performance tests; original-graph validity
does not establish that it improves frame time on this six-core runtime.


The first stock-graph test was refused because the initial implementation checked
the startup thread's affinity instead of process permissions. This guard is now
corrected; validate the "original8-core worker graph enabled" startup marker
before comparing performance. A configured environment flag alone is not proof
that the graph was installed.


The corrected stock-worker guard has now activated on hardware: process mask3F,
startup thread mask1,38/39tracked guests versus31previously. Work spreads across
the new workers, but sampled race FPS remains near20; no controlled improvement
has been demonstrated. Keep activation/correctness distinct from performance.


## Main/render optimization candidate (2026-11-14)

`tools/scripts/switch2_windows.ps1 -Target cpu-render-clang` builds the previous
12 physics chunks plus12 profile-selected render chunks with Clang and forced
inline guest memory access. It preserves memory ordering and strict FP options.
`build/switch2-cpu-render-clang` retains commands/hashes/static comparisons.
`-Target game` restores the ordinary GCC source archive. Package name remains
`Dirt2.nro`. The main/render trace must be symbolized against the preserved
`build/switch2-render-profile-baseline/dirt2.elf`, not the new candidate ELF.

The candidate is uploaded (Build ID87feaea915e36cb731d0d6fcc217b74b708694eb).
Existing `DIRT2_COM_CACHE=1` is enabled; PC/indirect sampling is disabled for the
comparison, and low-frequency counters remain. Corrected copy diagnostics report
actual dirty bytes written separately from NO_OVERWRITE scan volume. Strict fresh
exposure and the activated full stock job graph remain enabled. Baseline race
reports at90-120s average20.873FPS. Candidate FPS and gameplay correctness still
need the same handheld race60-90s followed by normal quit and FTP capture.
See HANDOVER.md for tests, exact hashes, backups and profiling limitations.


The latest diagnostic (Build IDfe62b1cd1477dfa3dfef89a0fbe3290a58ebb8d2)
adds inclusive Map/Unmap/copy/lock wall timers, gated by DIRT2_DX11_PERF=1.
Main260 PC sampling is currently enabled for the slow outdoor menu. Stay there
60-90s, then quit/openFTP; no race is needed. Symbolize this next capture with
build/switch2-menu-profile-baseline/dirt2.elf. Do not add nested bridge wall times
as if they were independent CPU phases. The previous capture confirms38.2FPS
in the menu and22.1FPS in race windows; a render-optimization gain is unproven.


The focused menu profile was retrieved: steady window34.7FPS, Map/Unmap about
3ms inclusive, write-copy0.87ms and measured bridge-lock wait0.10ms. It does not
support buffer copying as the main explanation for the gap. A COM-cache candidate
now uses high-bit/four-way indexing to avoid aligned-address thrashing while
retaining generation invalidation. New perf logs expose present-thread cache hit
rate and global retirement counts. Instruction sampling is off for comparison.
Check the same outdoor view/settings60-90s; a device FPS gain is not yet known.


`DIRT2_PROFILE_STACK=1` extends the20Hz PC profiler with at most12 bounded
AArch64 saved-frame links from the selected thread's readable stack mapping.
It requires DIRT2_PROFILE_PC=1; failed bounds/frame validation gives no chain.
The target resumes before aggregation/logging. `prof-stack` records top48 chains
per window; decoding saved returns uses LR-4. Chains can omit frames/tail calls
and measure inclusive wall contexts. This diagnostic is currently enabled for
main260 because the83.4%-hit COM-cache run still averaged36.6FPS. Keep the same
outdoor view/settings60-90s and reopenFTP. The exact native-stack-baseline ELF
is required; the earlier PC-only ELF addresses have changed.
