# Upstream Wine D3D9 tests on Switch

This runner attempts the pinned Wine D3D9 test inventory against the actual
D3D9→deko3d backend, without the game or its assets. The upstream files are
unaltered and retain their LGPL notices. Revision:
`840a939ee526137639665ff5f587224d6a36403f`.

There are 269 upstream entry points: 135 visual, 106 device, one state-management
chain, and 27 D3D9Ex tests. The native adapter can dispatch 220 original case
bodies. The other 49 are explicitly recorded as platform skips, including
Windows desktop/window-procedure/GDI/x86 behavior, D3D9On12, and D3D9Ex (which
this Switch backend does not expose). A runtime request for a Windows desktop or
GDI also records a platform skip. No unavailable test is counted as passing.
This is a full inventory attempt, not a claim that Windows-only tests execute
natively on Horizon. D3D9Ex source is retained but its module is not compiled
into the native dispatch path until that extension and its platform adapter exist.

The generator preserves all 220 dispatched case bodies exactly, instruments
upstream START_TEST calls, and replaces platform-dependent helpers with explicit
skip exits. It keeps upstream source line numbers. The native test framework
records passes, failures, TODO-tagged failures and capability skips separately.
Legacy `broken()` alternatives are disabled; canonical expectations are used.

## Run on Switch

Copy `sd/switch/d3d9-wine/` to the SD card. Use title takeover (hold R while
launching an installed game), then choose **D3D9 Wine Tests** in Homebrew Menu.
Do not launch DiRT 2 for this run. The game build and its settings are untouched.

The runner records one non-platform case per NRO load, then requests that the
homebrew loader launch it again. Restarts, flashes of test colors, and blank
screens between tests are expected. Known platform skips can be batched without
creating GPU resources. It returns to Homebrew Menu when finished and writes
`results/complete.txt`. Automatic next-load requires loader support; if unavailable,
launching the test NRO again resumes at the next case.

If a test crashes, reopen the runner: it imports the saved fault as a crash
and resumes after it. There is no need to stop for a fix after each crash.
If no valid fault record exists, it records the case as interrupted. A 90-second watchdog stops a hung test
by terminating the title; reopen title takeover to resume. Its interruption is
recorded as a timeout on the next launch. A kernel/system crash also requires
manual reopening. The watchdog includes GPU cleanup in its time limit.

Open FTP afterward. Results are under `sd:/switch/d3d9-wine/results/`:

- `results.jsonl`: one result per completed/interrupted case, first failed
  assertion, original file/line, assertion/skip/TODO counts.
- `suite.log`: detailed traces and backend diagnostics.
- `next.txt` / `pending.txt`: resumable progress and crash evidence.
- `complete.txt`: confirms every inventory entry has been processed.

Failed assertions stop that case immediately, protecting against subsequent
NULL dereferences after failed API calls. Its later assertions are **not run**.
The next case gets freshly loaded NRO state. This is the same Horizon process,
so the runner explicitly destroys the GPU queue, allocations and driver, then
quits SDL before requesting the next load. This also reclaims GPU allocations
abandoned by failed cases. A hung cleanup terminates the title instead of
reloading over live driver mappings.
This is first-failure coverage per case, not exhaustive assertion coverage.
To start a new run, preserve/rename the old results directory first.

## Build

Enable `DIRT2_BUILD_WINE_D3D9_TESTS=ON` in the existing Mac or Switch build.
Build target `d3d9_wine` on Mac, `d3d9_wine_nro` on Switch. The build prepares
native sources in its own generated directory; it does not modify upstream files.

```sh
python3 tools/scripts/build_wine_d3d9_shaders.py
python3 tools/scripts/build_deko_ffp_shaders.py
cmake --build build/switch --target d3d9_wine_nro -j 6
```

The generated fixed-function and depth-copy DKSH files must accompany the NRO;
the packaging script includes them automatically.

The standalone pack compiles the test fixtures through the normal MojoShader →
SPIRV-Cross → UAM pipeline. It is separate from the game's shader pack. Currently
145 of 150 candidates compile in the comprehensive repair build (149 literal
candidates and one additional runtime shader). All 145 emitted SPIR-V modules
also pass structural validation. The remaining list includes deliberately invalid
API-test declarations and an unsupported semantic; the report preserves every
rejection rather than treating it as a passing test. The shader
report must be consulted when attributing rendering failures; absence from the
pack does not establish a deko GPU-state bug. Six nonliteral DWORD arrays are
non-shader state/flag/pixel-data tables, recorded in the report.

## Reference and comparison

Mac reference command (each child gets GPU access):

```sh
python3 tools/scripts/run_wine_d3d9_tests.py \
  build/macos-arm64/tests/d3d9_wine/d3d9_wine --out build/wine-d3d9-results/dxvk
```

The first run completed all 269 entries: 144 pass, 66 fail, eight partial capability
skips, one native process crash and 50 platform skips (49 preclassified plus a
runtime desktop/GDI request). These are the native adapter's strict first-failure
results, not an upstream DXVK certification score. In particular, TODO failures
remain marked failures rather than silently passing.

```sh
python3 tools/scripts/summarize_wine_d3d9_tests.py \
  build/switch/tests/d3d9_wine/generated/inventory.json SWITCH_RESULTS.jsonl \
  --reference build/wine-d3d9-results/dxvk/results.jsonl --out comparison.json
```

Compare source lines for cases failing on both backends; a shared failure is not
proof of a shared cause. Prioritize cases passing DXVK and failing Switch. Missing
or interrupted entries remain distinct. The existing 41 DiRT-specific regressions
remain available and have already matched DXVK exactly on Switch.

Validation: Mac and Switch builds; the complete Mac inventory run; 220 case-body
integrity checks; CPU-only reporter tests for pass, failure, TODO, skip and
fail-fast control flow. The SM1/2/3 token sizing helper passes bounded ASAN/UBSAN
checks over all 897 game shader fixtures, 149 Wine candidates and malformed inputs.
The first Switch execution completed case 0 (device reference leak) and crashed
in NV transfer-memory initialization after reloading for case 1. The original
runner left driver mappings live across NRO reload. Explicit final GPU/SDL
teardown now precedes each reload; its order and complete allocation reclamation
pass an ASAN/UBSAN driver-mock test. Hardware retest of this correction is pending.

The corrected reload lifecycle was verified on Switch through 178 completed
entries. Case 178 then exposed unsigned overflow in the backend shader-constant
range checks (register UINT_MAX wrapped through reg+count and reached memcpy).
All six setters and both float getters now use subtraction-based bounds checks;
actual accessors pass boundary/overflow/null/state-preservation tests under
ASAN/UBSAN. The new runner retries that case while retaining earlier results.

The next Switch run reached case 186, whose upstream fixture deliberately
replaces a texture's public COM vtable before SetTexture. Binding formerly
called public AddRef/Release and crashed. Backend-owned textures now register
their implementation/core so binding, lookup and release use concrete methods
without dispatching through the public vtable. CPU tests exercise all three
texture kinds with invalid/null vtables and check lifetime and validation.
This fixes the crash path; it does not yet fix binding refcount conformance.

## Collect failures before repairing them

The Switch-only fault hook preopens a raw file for each case. On a CPU fault it
records the case, exact NRO build ID, exception/ESR, fault address, PC/LR, general
registers, and whether execution or final cleanup failed. It avoids allocator,
stdio and backend mutexes in the handler and leaves the fault unhandled for the
OS/Atmosphere path. It does not recover through a possibly corrupt heap or GPU
state. Recording is best-effort; pending progress and Atmosphere are fallbacks.
The watchdog uses the same raw mechanism before terminating a hung title.

Normal cases reload automatically. After a hard fault or timeout, reopen title
takeover and **D3D9 Wine Tests**; the next case is selected automatically. Once
it returns to Homebrew Menu after completion, open FTP to collect results. App
disappearance alone does not confirm completion: `complete.txt` does.

`fault-NNNN.bin` files are 336-byte versioned native fault records; blank records
from successful cases are ignored. The summary tool imports valid records even
before a manual restart, resolves PC/LR against the matching saved ELF with
`--symbols build/package/switch-wine/symbols`, and groups failure signatures for
batched diagnosis. Similar signatures suggest a batch; they do not establish
a shared cause. Tests passing DXVK but failing Switch receive priority.

The current run retains earlier results, records the observed case 186 crash,
and continues at 187. The texture-binding crash fix is also present in source,
but its corrected behavior still needs a later targeted hardware rerun.

## Mac-parity repair batch 1

The optional `reference-pass.txt` lists selected indices from the pinned native
Mac run. The current package selects exactly its 144 passing cases. Original
case bodies and assertions remain unchanged. Other inventory entries are
recorded as `not-selected`, never as passing. Remove this file to run the full
269-entry inventory again. `complete.txt` means the selected run has finished;
only each result's status establishes a pass. The summary includes a separate
`mac_passing_targets` total, passing count, and remaining indices.

The first repair batch separates application COM references from internal
bindings, keeps devices alive while public resources exist, removes implicit
surface cycles, and frees device allocations after submitting/waiting for their
GPU work. Bound resources, FVF caches and texture-level surfaces use private
ownership. It also removes the test adapter's inappropriate Vulkan window flag
on Switch; adds real state storage/readbacks for matrices, lights/materials,
texture stages, clip planes, palettes, scene pairing, FVF and shader constants;
implements volume-level wrappers; rejects invalid/foreign resources; aligns
linear pitches to four bytes; and accounts for live GPU allocations in the
available-memory estimate.

The first selected hardware run passed 42/144 targets. The second passed
52/144: 80 fail and 12 partially skip, with no crashes and no regression among
the previous passes. Skips remain failures of the parity goal. The game build
has not been replaced while these standalone repairs are validated.

## Third repair batch

State blocks now record selected state without changing current device state;
Capture and Apply follow the pixel, vertex and all-state selections. Constants
are saved per register so overlapping writes work. Saved resources use private
ownership and recorded NULL declarations follow the native Apply behavior.
MultiplyTransform and render targets bypass recording, as on native D3D9.
Actual state-block methods and setters pass ASAN/UBSAN CPU tests for masks,
recording isolation, capture, ownership, overlapping constants and parent life.
GPU rendering validation is still required.

Clear with a non-NULL rectangle pointer and zero count now changes no pixels.
ATI volume lock pitches/offsets follow the exposed D3D layout independently of
compressed GPU storage. Texture-to-offscreen StretchRect validation and legacy
CPU-pool lock behavior are corrected. The translator now applies sampler
swizzles after sampling and emits dimension-correct explicit gradients. The
standalone shader pack contains 119 literal fixtures; all 897 retail game
shaders also compile with the patched translator (compilation is not image proof).

Missing bytecode is saved in `results/shaders/*.d3dbc` by this test runner.
It is diagnostic evidence, not a runtime compiler or a replacement for shader
validation. `build_wine_d3d9_shaders.py --extra-shaders DIRECTORY` can compile
captured runtime variants into a following pack. Failed translations cannot
reuse stale generated GLSL or DKSH files.

Fixed-function drawing, multisampling, unsupported shader instructions and
further rendering differences remain open. The goal remains all 144 targets.
# Reload diagnostic

When `results/reload-probe.txt` contains `0`, the next launch performs three
isolated restarts: the loader alone, SDL initialization and cleanup, then GPU
initialization and cleanup. It records each boundary in `results/reload-probe.log`
and returns to the Homebrew Menu after creating `reload-probe-complete.txt`.
This does not execute Wine cases or alter their saved progress. If the console
shows an error screen, reopen FTP for inspection before launching again.

When `results/render-probe.txt` exists, the next launch compares simple shaders,
the fixed-function vertex stage, the fixed-function pixel stage, and both stages
together. Each pair gets a fresh GPU device; uploads, clears, and draws are
synchronized to identify the failing operation. Results go to `render-probe.log`,
separate from Wine results and progress. A completed run writes
`render-probe-complete.txt` and removes its trigger. If all four colors match
and no GPU faults occur, it automatically resumes the Wine suite. If it returns
to the Homebrew Menu or shows an error, reopen FTP before relaunching.
