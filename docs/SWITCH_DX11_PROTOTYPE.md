# Switch DX11 / DXVK / NVK

This is the primary Switch renderer as of2026-10-20. The user reports clean
menu/race graphics. Performance at0.75x low settings is about20 FPS in menus
and11–13 FPS racing; reaching30 FPS is the next objective. The filename of this
document and the `dirt2-dx11` installation directory are retained for continuity.
D3D9/deko3d is preserved as the `switch-deko` build and package fallback.

## Install and test

The primary package is `build/package/switch/DiRT2-switch.zip`; launch **DiRT 2**
through title takeover. Its executable remains at
`sdmc:/switch/dirt2-dx11/dirt2-dx11.nro`. Game assets remain at
`sdmc:/switch/dirt2/Dirt 2`. Logs/settings stay in `dirt2-dx11`, while the savegame
tree is shared with the original `dirt2` version. Disk shader caches remain
disabled because concurrent SD cache writes stalled startup. Exposure uses the
verified fresh-readback correction with the bounded two-frame ready-pixel policy.
Shader-library startup warmup remains opt-in, not enabled by the primary package.

The0.5x resolution option is640×360. Two independent game restrictions applied:
a minimum-height check in lifted mode-list code (already patched for Switch),
and `system/hardware_settings_restrictions.xml`, which forced heights below480
to640×480. Startup now validates and creates a private VFS override of those
three XML attributes: minimum height360 and fallback640×360. It preserves the
original game asset and does not change Apple. Host ASAN/UBSAN checks verify
exact roundtrip edits, fail-closed handling and absence of unrelated changes.
The source XML supplied decisive evidence; REA/Hopper startup timed out, so no
claim here relies on successful decompilation.

The first0.5x repair was fully installed/readback verified before renderer
promotion: build48d03d71a782b605abc683bf4a4ba50dbbd0431d,
SHA1db7fb4440b2ff691a6e3ba8efb29a8415733c369013d0784c953eac6dcab7a4.
The user must still confirm the option in the running game.

The sections below retain the implementation and bring-up history. Earlier
prototype/unverified labels describe those historical builds.

## Build

Prerequisites are the normal Switch dependencies, prepared DXVK sources and
`build/switch/libdirt2_gen.a`, plus Python 3.11+, Meson, Ninja and glslang. Reusing
the existing lifted-code archive avoids recompiling 2.5 GB of unchanged game code.
It must come from the same generated sources and normal TSO-enabled Switch build.

```sh
bash tools/scripts/build_switch_dx11.sh
```

Outputs are in `build/switch-dxvk`, `build/dxvk/switch-dx11`,
`build/deps/switch-nvk` and `build/package/switch-dx11`. The script prepares a
separate SDL source/build, applies the Switch patches to a separate DXVK copy,
and leaves the system SDK, normal NRO and Apple graphics libraries intact.

`fetch_switch_nvk_sdk.py` extracts selected SDK files from the public
[autorunhq/switch-dev](https://github.com/autorunhq/switch-dev) image, pinned to
arm64 image digest
`sha256:c130418a889d8f9afc3d80c5bda699dfcc83741a22d2f15f800ca5afba17fca1`.
Every downloaded layer is SHA256-checked. It does not run the container or install
Docker. The isolated SDK uses:

- Mesa/NVK `0ff52079ca9e9e837ed27eb95a8d239f439a1864` (Mesa 26.2.4).
- libnx `146c3d1446d35546943c7b6cfcd479bc1f5e9b67`.
- The existing host devkitA64 compiler and tools.

Upstream driver source is [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch).
The image's metadata also lists other components; LSFG, Wine, FEX and VKD3D-Proton
are not linked into this prototype.

## Integration and checks

- `DIRT2_SWITCH_DXVK=ON` selects the alternative graphics stack; default is OFF.
- The DX11 bridge passes an SDL window to DXVK and excludes the MetalFX hook.
- SDL uses Vulkan-only window support and NVK's static `vkGetInstanceProcAddr`.
- DXVK's native loader/SDL bindings are static. D3D9/D3D11/DXGI share one logger;
  a colliding internal D3D9 format helper has a distinct name in this build.
- The game's minimal FFmpeg/audio libraries are linked by absolute path, avoiding
  similarly named full-feature libraries shipped in the NVK SDK.
- Native DXGI WCHAR arrays are converted to guest UTF-16, including surrogate
  pairs, rather than copying native 32-bit wide characters into guest arrays.
- The linked executable contains D3D11CreateDevice, CreateDXGIFactory,
  vkGetInstanceProcAddr and nvk_CreateDevice; its link excludes deko3d and EGL.
- Component builds and NRO link passed. UTF-16 conversion passed ASAN/UBSAN.
  The SDL window source also passes syntax checking with the original EGL-enabled
  configuration. Both SDL patches reproduce the compiled sources from a clean
  SDL tree; the Switch DXVK patch applies to the existing prepared base.

The package manifest records the NRO hash/build ID and SDK revisions. Matching
ELF/symbols are preserved before installation. A compiled NRO is not a hardware
rendering or performance result.

## First hardware run and TLS repair

The first build (6f4484f0) initialized NVK and successfully created a feature-level
11.0 device/swap chain, but never logged a completed first Present. OpenAL then
reported `al::tss::tss()` allocation errors. The user experienced a black screen
with HOME unresponsive and recovered by holding POWER for 20 seconds. No new
Atmosphere crash report was produced; logs are preserved in
`build/package/switch-dx11/hang-20261020/`.

The prototype now multiplexes newlib pthread/C11 TLS keys through one native
libnx slot (libnx provides only 12). It supports 256 keys, per-thread values,
generation checks on key reuse and four destructor passes with reentrant
callbacks. libnx service TLS and the normal deko build remain unchanged.
Host ASAN/UBSAN checks cover slot exhaustion, thread isolation, key deletion/reuse
and destructor callbacks. This repairs the observed allocation failure; whether
it resolves the full system hang still requires a hardware test. Audio context
results and entry into the first Present are now explicitly logged.

The next run (789128ad) crashed earlier while opening the cache left by the forced
shutdown. The initiating thread threw `std::bad_alloc` in `readShaderLutKey`;
its abort ran libnx cleanup while the main thread was polling touch, causing the
reported secondary `LibnxError_NotInitialized`. The captured LUT has 728 valid
entries and a damaged final XFB count of 1,201,970,788 (a resize needing tens of GB).
Evidence: `build/package/switch-dx11/crash-tls-20261020/`.

Switch DXVK patch 0002 bounds cache reads/counts and binary offsets, catches cache
parse exceptions, clears partially parsed entries before truncation, and retains
StlFile access flags for correct size queries. A host ASAN/UBSAN regression uses
the actual patched cache-key reader and captured LUT; it rejects the damaged tail
after 728 entries. It also checks overflow bounds and write-only file size/append.
Run `python3 tools/scripts/test_switch_dxvk_cache.py` after preparing DXVK.
The damaged console cache is deliberately retained for the repaired reader to
reject and rebuild. The TLS repair's audio effect remains unverified because
this cache failure occurred before audio initialization.

## C11 ABI collision found after the cache repair

Build c2e5fbbc rejected/recreated the damaged LUT and reached the first Present,
but OpenAL's `al::tss::tss()` exception remained. Inspection of its actual compiled
headers reveals `thrd_success = 4`; Mesa's bundled `threads_posix.c.o` implements
C11 success as 0 and had interposed its 25 global C11 functions into the whole
program. The prior allocation-exhaustion diagnosis was incorrect: a successful
Mesa TLS creation was being interpreted as failure by native OpenAL. The dynamic
TLS multiplexer has been removed completely.

`prepare_switch_nvk.py` now creates a private copy of the NVK archive, renaming
all its C11 definitions AND internal references to `dirt2_mesa_*`, plus its
`__xpg_strerror_r` compatibility stub. The original extracted SDK is unchanged.
Other native components use the toolchain's C11 ABI; Mesa retains its own ABI.
A prototype-only startup check validates native and Mesa TLS, native mutex,
condition variable and thread creation/join before SDL/GPU initialization. Failure
returns without starting the game. Additional first-swapchain call logs distinguish
entry into the driver from the preceding DXVK log formatting/output.
Evidence for the freeze is in
`build/package/switch-dx11/freeze-cachefix-20261020/`; no new crash report was
created. The freeze's GPU cause and rendering remain unconfirmed until retested.

SDL's static interface also requested `-lvulkan`, which could silently reintroduce
the original archive after the private copy. CMake now aliases SDL's `vulkan`
dependency to the same private NVK target. Final link and ELF disassembly confirm
no original `-lvulkan`, native `tss_create` success=4, private Mesa success=0, and
no TLS syscall wrappers. The archive preparation verifies all 26 renamed symbol
definitions and references. Hardware self-checks still need a console run.

## Isolated graphics preflight

Build 62a6 passed all native/Mesa C11 checks on the console. The next freeze
occurred after creating a 1x1 floating-point target, before audio or Present was
logged; no new Atmosphere report. Evidence: `c11-freeze-20261020/` in the package
folder. The C11 conflict is repaired but the GPU freeze remains unresolved.

`DIRT2_DX11_PREFLIGHT_ONLY=1` selects a native test before loading the guest/game
or reserving its memory. It checks an actual OpenAL context, creates the DX11
device, clears/copies/readbacks 16x16 RGBA8 and RGBA16F textures, then presents a
blue frame for 1.5 seconds and returns to homebrew. It compares every pixel.
Game pipeline warm-up/replay and HUD are disabled and it uses its own DXVK cache
name. Stage logs bracket GPU work; event polling requests an Atmosphere report
after 10 seconds without GPU completion. A separate 30-second watchdog requests
a report if an API blocks. A full kernel/GPU hang may prevent this watchdog from
running, so automatic recovery is not guaranteed. Without this opt-in env flag,
the same executable starts the game normally. Hardware preflight pending.

First isolated run cfeac returned normally: all C11 checks PASS, actual OpenAL
context PASS, DX11 device creation PASS, first RGBA8 clear/copy/event/Map completed
but pixel comparison FAILED. It returned before testing float/presentation. Logs:
`build/package/switch-dx11/preflight-result-20261020/`. This is not a full GPU pass.
Next revision logs actual first pixel, pitch, mismatch count and saves tightly
packed 16x16 readbacks as `preflight-format-28.bin` and `preflight-format-10.bin`.
It continues to RGBA16F and presentation despite a pixel mismatch, so color
conversion differences can be distinguished from corrupt output. Exact comparisons
remain unchanged. Blue presentation is possible even when a readback comparison
fails; the final log result is authoritative for the automated checks.

The expanded run 45d542 crashed BEFORE reaching readback tests. The runtime log
has exception 257, PC main+144767896, LR main+143574456, FAR 0xb6bcf1000. Add ELF
main address 0xa8330 to these offsets: PC 0x8ab7ec8 is memset+0x88 (first 16-byte
store), LR 0x89948e8 is tmemCreate+0x48. Thus clearing a transfer-memory allocation
faulted at its initial address; the current evidence does not identify allocation
size, caller or ownership. No new Atmosphere report because the guest runtime's
exception handler exits the process. Evidence: `expanded-result-20261020/`.

A separate target `dirt2_dx11_probe_nro` links only SDL/OpenAL/DXVK/NVK plus the
preflight and C11 checks. It excludes guest code/runtime, COM bridge and FFmpeg
and uses normal libnx exception handling. Size ~22 MB, compared with ~159 MB for
the game executable. Transfer-memory calls log size and caller relative to main;
native aligned allocation logs query the returned memory's address, permissions,
type and attributes, without changing allocation behavior. This distinguishes
bad allocation/ownership from a backend image issue on the next run.

Build/package the standalone probe with:

```sh
CCACHE_DISABLE=1 cmake --build build/switch-dxvk --target dirt2_dx11_probe_nro -j4
bash tools/scripts/package_switch_dx11.sh --standalone-preflight
```

It installs in the same separate prototype folder/entry, always runs the test,
and needs no game assets. The full game prototype remains available in the build
directory; default packaging still chooses that full game executable.

## Standalone result and launcher-return repair

The first standalone run 917238 passed audio and first presentation, and displayed
a frame for 1.5 seconds. RGBA8 readback was uniformly (64,127,191,255), which is
valid for the requested color under D3D's 0.6 ULP FLOAT->UNORM tolerance; the old
strict comparison was a test defect. RGBA16F Map returned 0x887A000A (still drawing),
which the old test treated as failure instead of retrying. Most importantly that
early error leaked the texture/view/readback objects, retaining the GPU device.

The resulting crash report 01792507283 names **sphaira**, not the probe. Sphaira
faulted while clearing the heap at the exact 0xdebb3000 address the probe logged
for NVK's 8 MB transfer allocation. Thus the immediate crash is during launcher
reload with GPU-owned pages still present. Evidence: `native-result-20261020/`.
The unrelated full-game hard freeze remains unproven to have the same cause.

Test resources now have scoped COM/window owners on every return path; busy Map
is retried up to 10 seconds and pixel checks apply the documented tolerance (not
a blanket +/-1 allowance). ASAN/UBSAN CPU checks accept all captured pixels and
reject invalid neighbouring quantization values and damaged alpha. The pinned
backend source is preserved under `build/nvk-sdk/source-audit/`.

At standalone shutdown, after COM owners/SDL/workers are torn down, the backend's
process-shutdown hook closes retained mappings. Tracked native nvInitialize
references are closed too: the backend deliberately leaves that base session for
process death, whereas NRO return reuses the same process. The entire former 8 MB
transfer range is queried before return and must have RW permission and default
attributes. Otherwise the probe requests a controlled report instead of allowing
Sphaira to overwrite protected pages. The watchdog covers this shutdown stage.
Hardware verification of the repair passed on build 92915: both pixel formats,
presentation, audio and transfer-memory recovery passed. No new crash report was
created; cleanup had zero outstanding native NV references. Evidence is in
`cleanup-result-20261020/`.


## Full-game startup watchdog

Package the real game with `bash tools/scripts/package_switch_dx11.sh --startup-watchdog`.
This removes the preflight-only flag and uses a fresh `game-startup` shader cache
with pipeline warmup/record disabled. An opt-in watchdog requests an Atmosphere
report after 120 seconds without a successful native Present, and disarms at the
first successful Present. This detects CPU startup stalls; a full system hang can
prevent it from running. Compilation taking more than two minutes can also trip it.
The full game's guest return exits the HOS process, since other guest threads may
still own GPU resources; it does not attempt the standalone probe's quiescent cleanup.
Standalone success does not resolve the original full-game startup freeze.


The first game watchdog revision a9cdd4 crashed before guest entry because this
SDK's `pthread_detach` always returns ENOSYS (88). `std::thread::detach` threw,
termination returned to hbl and unmapped code beneath the live watchdog thread.
The report's instruction abort at svcSleepThread is secondary; the main thread
contains detach/throw/abort/unload. Evidence: `startup-result-20261020-171011/`.
The watchdog now uses checked libnx threadCreate/threadStart with a static aligned
stack and process-lifetime Thread object, avoiding unsupported POSIX detach.


Native watchdog run 603231 reached game startup, device creation and shader work.
At 120 seconds it requested the intended report (2345-0049). The game's thread
was in fsFileWrite while logging CreateTexture2D; DXVK's cache writer was also in
fsFileWrite, and GPU workers were idle. Evidence: `startup-result-20261020-171916/`.
This does not by itself establish a filesystem deadlock. The next diagnostic uses
the same executable with empty DXVK_SHADER_CACHE_PATH (no HOME/XDG fallback on
Horizon) and MESA_SHADER_CACHE_DISABLE=true to isolate background cache writes.
Disk cache files are preserved. Check the next log confirms cache-disabled state.

Reproduce this package with `bash tools/scripts/package_switch_dx11.sh --startup-watchdog --no-disk-shader-cache`. Configuration installed and read back; the executable remains 603231.


The user confirms the cache-disabled 603231 game reaches the main menu. Keep disk
shader caches disabled while investigating the exact I/O contention cause.

The full game prototype now maps only the guest savegame directory to the original
Switch version's `sdmc:/switch/dirt2/C/Users/Player/Documents/My Games/DiRT2/savegame`.
Both versions read/write the same saves. Graphics settings and logs remain in the
prototype folder; pre-existing prototype-only saves are left in place. VFS host
ASAN/UBSAN regression: `tests/vfs/test_save_mapping.cpp`.


## Virtual render resolution and exposure diagnostics

The game changes resolution through ResizeTarget followed by ResizeBuffers(0,0).
On Switch the physical window stays 1280x720, so resolving zero dimensions from
that window discarded a virtual render-size request. The bridge now retains the
successful target size and uses it for zero buffer dimensions on Switch only.
Explicit ResizeBuffers dimensions still take precedence. Guest DXGI mode listing
and matching use the same six 720p-based scales as the original port; desktop
coordinates also match its 720p base. Hardware validation remains pending.

The old exposure guard assumed any16-byte READ was a luminance pixel. It now
checks staging texture metadata (1x1 RGBA32F) and accepts padded row pitches, so
unrelated16-byte buffers are unaffected. An optional --exposure-trace package flag
records existing1x1 float readbacks, raw bits, formats, pitches, status and frame/
time in exposure.csv for120s. Writes batch128 samples or5s; busy maps are limited
to one failure perresource/frame. No extra GPU reads/waits are introduced. The
brightness oscillation is not yet diagnosed; don't claim the invalid-pixel guard
or low FPS is its proven cause. Keep disk caches disabled with this diagnostic.


## Confirmed 256x256 presentation surface

Screenshots in lowres-result-20261020-175505 show the native DXVK HUD degraded
along with the game. The log requests a1280x720 D3D backbuffer but reports an
actual256x256 Vulkan swapchain. The first guest loading window is256x256 and all
later guest windows reuse its SDL/NWindow. SDL's Switch Vulkan resize handler
never updates native dimensions (its resizing code was EGL-only), so fullscreen
metadata changes left the underlying surface at splash size. DXVK honors the
surface's currentExtent; the whole frame was reduced there, then screen-stretched.

The prototype now creates the shared physical window at1280x720, ignores helper
HWND sizes for physical output, and keeps ResizeTarget's physical size at720p
while retaining its virtual render-size request for ResizeBuffers. Hardware must
confirm actual Presenter Buffer size1280x720. This fixes a distinct initialoutput
issue; the earlier virtual-mode changes alone could not repair it.

The exposure trace has2751 valid RGBA16F readbacks (pitch8),16 resources and no
negative/nonfinite channels. Therefore an invalid-pixel theory is unsupported.
The brightness oscillation still needs validation/investigation after this fix.


## Opt-in native shader-library warm-up

`python3 tools/scripts/package_dx11_shaders.py` builds a4653520-byte read-only
bundle of1117 unique DX11 shaders (2768 extracted game blobs plus8 immediate UI
blobs before deduplication). Original DXBC is translated and compiled with the
actualSwitch device options by native D3D11. DXVK registers/retains shader pipeline
libraries; startup waits for the compiler queue to finish. Progress presents show
the native HUD, and successful shader/task progress extends the startup watchdog.
The warm-up does not enable either diskshadercache. It recompiles at eachlaunch;
full state-specific pipelines or generatedvariants can still compile during play.

Build/package with --shader-preload and --no-disk-shader-cache. Separate prepared
package: build/package/switch-dx11-preload/. Local parser tests verify the full
bundle and reject truncated, overflowing and mismatchedstage records under
ASAN/UBSAN. Native build passes; hardware behavior/memory/cost remain unverified.

## Fresh-exposure latency experiment

The user reports unplayable oscillations at lowFPS. The existingtrace alone does
not prove delay; sixteenreadbackresources are present. --fresh-exposure enables
an opt-in compatibility experiment for the game's known1x1 RGBA16F exposure Map
READ atFAF55C. It reads the mostrecentlyqueued matchingstagingpixel instead of an
olderringentry. Normalmaps remain unchanged; nativeUnmap follows theactualmapped
resource. No values are clamped. GPUoverlap can decrease because a newer pixel
may require waiting. --exposure-trace now records original/actual copyframe and
age to test thelatencyhypothesis directly. Shaderwarmup is disabled in this
comparisonpackage to avoid confounding the brightness test. Hardwaretestpending.


The baseline requested before fresh-exposure installation is preserved in
exposure-latency-20261020/baseline-20261020-182303/ with sizes/SHA checksums and
brightness-analysis.json. It confirms physical720p and2964 valid RGBA16F reads,
but the first120-second trace excludes later portions of the500+second session.
Ring reuse medians are16frames/474ms current and16frames/687ms prior, which is not
proof of actual readback age. The comparison package now records600seconds;
DIRT2_EXPOSURE_TRACE_SECONDS is bounded to3600. Shaderwarmup remainsdisabled.


### Exposure performance follow-up (2026-10-20)

The user reports no oscillation in the fresh-readback session, but about 15 FPS
in a race. This is encouraging for brightness, but does not establish that the
newest-pixel wait is the performance bottleneck. The opt-in
`DIRT2_DX11_ASYNC_EXPOSURE=1` comparison (requires fresh exposure) polls matching
readbacks newest first with DO_NOT_WAIT. It accepts only copies from the same
immediate context within two presented frames. If no such pixel has completed,
it waits for the newest pixel as before. General D3D11 mapping remains unchanged;
this is a game-specific latency/performance tradeoff requiring hardware testing.
The trace gains `map_ms`, covering selection plus Map, to measure readback cost.
No additional readbacks or pixel clamping are introduced. Shader preload stays
disabled during this comparison so its startup behavior is tested separately.


Performance baseline was preserved in
`build/package/switch-dx11/performance-20261020-191921/` before installation.
There are 7,628 successful traced readbacks; 99.79% consume the rerouted resource.
Median original ring-copy age is 406.76 ms versus 7.203 ms for the fresh pixel.
In the slow race window (frames 6000–6600), fresh copy age reaches a median
59.478 ms, while logged 300-present intervals include 13.5 and 13.0 FPS. Later
intervals stabilize near 21 FPS. These copy ages do not isolate GPU waiting;
the next build's map_ms instrumentation is needed for that. Output remains720p,
with saved internal rendering960x544 and antialiasing off. The comparison build
is8dc98898d8bbc46637f5ca528304d97263431f4a in the separate switch-dx11-async
package; hardware performance and brightness confirmation are pending.
