# D3D9 rendering regression suite

`shims/d3d9/conformance.cpp` runs the same ordinary D3D9 calls against the
Switch deko3d backend and the Mac DXVK backend. It creates its own device before
the game device, renders offscreen, reads pixels through `GetRenderTargetData`,
checks expected colors, and writes `results.json` plus tightly packed 64×64 BGRA
images. Tests do not change the game device's state. The three SM3 shaders are
existing D3DX built-ins already present in the shader pack; neither backend gets
a special test shader implementation.

The initial 41 cases cover:

- Clear and alpha preservation, ColorFill rectangles, StretchRect subrectangles.
- Shader drawing, scissor, source-alpha blending, alpha rejection, depth ordering,
  and disabled depth testing with depth writes enabled.
- RGBA8, BC1/DXT1, BC2/DXT3 and BC3/DXT5: non-square block patterns with a short
  row pitch, five selected mip levels, and SetLOD after binding/drawing.
- BC2/BC3 alpha, resource recreation/reuse, and three worker threads uploading
  textures while the rendering thread draws and reads back results.

Five additional cases keep 64 tile draws in flight until one final readback:
`batched_constants`, `batched_alpha_state`, `batched_texture_lod`,
`batched_resource_retirement`, and `batched_render_to_texture`. They alternate
constants, alpha rejection, mip descriptors, texture lifetimes, and dependent
render/sample operations. No intermediate pixel readback is allowed to hide
ordering or stale-state failures. The expanded 46-case suite passed on Mac DXVK
on 2026-10-20. Switch then passed all 46 cases, with all images matching the
Mac reference within the comparator tolerance. These use ordinary
D3D9 calls and the existing built-in shaders, so they do not yet reproduce the
full roadside material shader's arithmetic.

These are original targeted regressions, not the full Wine conformance suite.
Known-color assertions give an independent expectation; DXVK provides an
additional image reference, rather than being treated as the specification.
Pixel checks avoid primitive edges and allow two byte values of channel rounding.
The comparison tool compares full images and can therefore also report edge
differences that need separate interpretation.

## Mac reference

Configure the existing Mac preset/build with `DIRT2_BUILD_D3D9_TESTS=ON`, then:

```sh
cmake --build build/macos-arm64 --target d3d9_conformance -j 6
build/macos-arm64/d3d9_conformance build/d3d9-tests/dxvk
```

The standalone runner needs the local built SDL, DXVK and MoltenVK libraries,
but does not launch the game or require its assets. It uses a hidden SDL window
and returns nonzero on a test/setup/report error. Normal desktop GPU access is
required. Each output directory is replaced on a rerun; use a new directory to
retain evidence. Initial validation on Apple M1 Max / DXVK 3.1.1-dirt2.1:
41/41 pass, with a second run matching every pixel exactly.

## Switch

The normal build includes the suite but leaves it disabled. To run it once per
game process, add to `sd:/switch/dirt2/env.txt`:

```text
DIRT2_D3D9_TESTS=sdmc:/switch/dirt2/d3d9-tests/v1
```

Launch the game normally. The suite runs before the game's first D3D9 device is
created and then normal startup continues, even after test failures. Individual
results are also logged as `d3d9-tests: ... PASS/FAIL`. Close the game and retrieve
the output directory over FTP; remove the environment line for normal play.
Use a new output path to preserve different runs. Manual Minus+Plus captures
continue to work independently. A GPU hang/crash can prevent the final report;
in that case use the last logged case and the Atmosphere crash report.

## Compare

```sh
python3 tools/scripts/compare_d3d9_tests.py \
  build/d3d9-tests/dxvk build/d3d9-tests/switch \
  --out build/d3d9-tests/comparison
```

The dependency-free comparator returns nonzero on missing cases/images,
expectation/API errors, an invalid reference, or image differences beyond the
default per-channel tolerance of 2. It writes `comparison.json` and candidate /
amplified RGB difference PNGs for mismatches. Raw BGRA artifacts retain alpha
differences too. Negative checks verify detection of a changed pixel, a missing
image, and a missing case. Reference self-comparison and repeat-run comparison
both pass. A passing suite only establishes the behavior covered by these cases.

## Expanding coverage

[Wine's D3D9 tests](https://github.com/wine-mirror/wine/tree/master/dlls/d3d9/tests)
include visual, device and stateblock tests. They use Windows windowing and Wine's
test framework, so the Windows executables cannot simply run as Switch homebrew.
Port selected rendering cases and their expected results to this harness; retain
their LGPL notices when copying code. Do not import the whole API suite before
separating unsupported optional features from real rendering failures.

Next priorities are scalar MRT outputs and depth sampling, VPOS, render-target
reuse / sampling barriers, instancing and packed vertex inputs, sampler changes,
sRGB, dynamic DISCARD / NOOVERWRITE uploads, and partial compressed mip updates.
Each new shader must also enter the normal offline Switch translation pipeline.
Game-specific failures should become minimal regression scenes. The richer
manual dumps help extract them, but are not yet complete API traces/replay files.

[Apitrace](https://github.com/doitsujin/dxvk/wiki/Using-Apitrace) is useful for
desktop captures; its Windows replay binaries also need a native Switch port or
a smaller custom replay layer. Native Windows D3D9 results would provide a useful
third reference for ambiguous behavior. DXMT implements D3D10/11, so the direct
D3D9 reference here is DXVK.

## First Switch run

The initial console run passed37 of41 cases. All37 passing images matched DXVK
byte-for-byte. The four failures were SetLOD changes after a texture was already
bound, consistently across RGBA8 and BC1/2/3. The backend updated the LOD value
without refreshing its cached sampler. All three texture types now notify the
device when their LOD changes, invalidating bound samplers. The console rerun
passes all41 cases, with every image byte-identical to DXVK (maximum channel
difference0). The existing four regression cases and reference images were
unchanged. Startup tests were disabled after collecting the successful run.

Evidence: `build/d3d9-tests/switch-20261019-195447/` and
`build/d3d9-tests/comparison-20261019-195447/`. Basic compressed uploads, alpha,
mip selection, depth behavior and concurrent uploading passed these cases; that
does not establish correctness for every game shader or resource usage pattern.

Successful rerun evidence: `build/d3d9-tests/switch-20261019-200113-lod-fix/`
and `build/d3d9-tests/comparison-lod-fix/`.

## Broad upstream Wine inventory

A separate [Wine runner](../tests/d3d9_wine/README.md) is now built and installed
as **D3D9 Wine Tests**. It accounts for all269 pinned upstream entries, dispatches
220 original case bodies and explicitly records49 unavailable Windows/Ex cases.
It uses isolated, resumable, first-failure case runs. The complete Mac reference
run is saved. The first Switch attempt exposed a device reference leak, then a
runner crash during driver initialization after NRO reload. Explicit GPU/SDL
teardown now precedes reload; its hardware retest is pending. This is independent of the
41 custom regressions above and does not change the installed game.


## Completed Switch Mac-parity run and combined candidate

The full 144-target Switch run completed with 119 passes, 24 assertion failures
and one partial skip. Evidence is in
`build/wine-d3d9-results/switch-mac-comprehensive-complete-20261019-221959`.
The shared GPU sample-mask fault, physical MSAA layout and plain-surface render
usage fixes passed on hardware, including all four rendering self-checks.

The next combined candidate repairs implicit SM1 color-output linkage/masks and
post-shader alpha/fog/clamp/sRGB processing, programmable flat/shadow sampling,
resident mip views, depth texture channels, viewport range, stream-frequency
validation, depth-copy restrictions, buffer hints, additional swapchains and
back-buffer rotation. Runtime-generated NRM shader bodies are all collected.
A pinned patched UAM removes its unbound OpenGL Y-transform read from DSY.
All 897 game shaders compile and validate; local regressions pass. These are
build/compiler/CPU checks; the combined candidate still requires a full hardware
rerun. Packed YUV GPU textures remain unsupported and explicitly skipped.


The next hardware run completed with 133 passes, 10 assertion failures and one
partial skip, with no interrupted cases. Evidence:
`build/wine-d3d9-results/switch-combined-20261019-224036`.
The following candidate repairs mixed direct/indirect uniform offsets, implicit
PS1.4 coordinates and sampler dimensions, PS1 depth-plus-color output masks,
SM2 TEXKILL's fourth component, coissued CND, pre-SM3 color input clamping,
the ATI RESZ trigger, and render/depth target visibility when sampled or copied.
The 20 local regressions and the 897-shader compilation/validation pass; hardware
confirmation remains required. Packed YUV GPU formats remain explicitly skipped.

## Shader pack compatibility

The original version-2 game pack has interface records ending at the color-output
mask. Its pixel shaders rely on native alpha testing. The extended interface adds
the flat-variant length/data record together with draw-state wrappers (including
shader alpha testing and vertex clip planes). Even a zero-length variant marks
that extended interface. The device records this distinction as `has_draw_state`:
original shaders use native alpha testing and skip unused draw-state UBOs;
extended shaders and the fixed-function implementation retain their shader alpha
tests. Applying both tests can change floating-point alpha comparisons, while
disabling both produces solid leaf cards and incorrect transparent masks.

`test_alpha_pack_compatibility.py` exercises the real color-state emission for
original/extended/fixed-function shaders across all eight comparison operations,
alpha references, and alpha-to-coverage. This checks emitted state, not console
pixel correctness. The 2026-10-20 game candidate preserves the user-verified
original 897-shader pack; the conformance runner remains separate.

## Asynchronous command batching

The game can record many texture uploads before its first Present. The normal
renderer now submits after 16 completed upload/copy/clear/draw operations through
`Gpu::checkpoint()`. This calls the full deko queue flush without an explicit GPU
idle wait. It does not recycle frame command memory, staging allocations, or
deferred frees; those retain the final frame-fence lifetime. Starting another
list invalidates draw bindings through the existing list generation. Readbacks
still wait explicitly when CPU visibility is required. The temporary native
AutoKick-threshold override was ineffective and is removed; diagnostic wrappers
now preserve SDK submission arguments exactly.

`test_async_command_batches.py` exercises the actual batching, list-start and
flush methods with finite-queue and staging-lifetime models. It establishes
command ordering and lack of explicit per-batch CPU waits, not hardware timeout
correctness. Build 97308ab launched successfully without forced per-draw waits,
but the user reported more spontaneous flickering. The live rendering issue
remains open; successful launch and passing CPU models do not establish correct
asynchronous rendering.

## Coverage audit of the remaining game artifacts

The latest saved hardware Wine results (2026-10-20 01:05 capture) contain 141
passes, two failures and one partial capability skip from the 144 Mac-passing
cases. The remaining failures are `visual.depth_blit_test` and
`visual.srgbwrite_format_test`; `device.test_surface_blocks` is partial because
UYVY is unsupported. Later source changes still need a new hardware run.
Alpha testing, blending, MRTs, pixel/vertex constant clamps, `dp3_alpha_test`,
and attached-render-target sampling already passed. Their pass status does not
cover every instruction combination or state sequence in a game material.
Some related upstream cases fail or skip on the Mac reference as well; those
are retained as coverage limitations, not counted as Switch successes.

The roadside material's base-texture-only diagnostic was confirmed clean by
the user. A full material rewrite, scoped to its exact bytecode identity, keeps
lighting, specular, shadow, fog and tone mapping. CPU comparison of 20,000 finite
random inputs matched the original arithmetic within 5.4e-7; both original and
extended wrapper variants compile. This is a candidate compiler-path workaround,
not a proven diagnosis of the underlying compiler defect or a hardware pass.

## Comparison with the local DXVK implementation

The working Mac DXVK source is also useful as a reference for D3D9 behavior.
Its `D3D9DeviceEx::PrepareDraw` limits managed uploads and mip generation to
the active shader sampler masks. `UpdateActiveHazardsRT` similarly considers
whether a shader actually samples an attached texture. The deko backend had
been snapshotting every bound attachment and generating mips for unused bound
textures. It now derives a mask from the selected programmable/FFP shader
metadata, with separate pixel and vertex sampler ranges. Unused old feedback
bindings are cleared; active feedback still snapshots the source. This removes
unnecessary copies and barriers, but the performance effect needs measurement.

DXVK's `SetRenderTargetInternal` and `UpdateAlphaToCoverangeAndAlphaTest` also
update alpha testing when RT0's multisampling or ATOC state changes. Our native
alpha test depended on those values but was not marked dirty on target changes
or `D3DRS_ADAPTIVETESS_Y`. Both now invalidate blend/alpha state. The actual
setters and state emission pass a repeated transition test without per-draw
flushes, including negative checks against each old invalidation path.

Validation: `test_gpu_state_paths.py` checks active pixel/vertex sampler masks,
unused attachment suppression, old feedback binding removal and active copies;
`test_alpha_target_transitions.py` checks single/MSAA target switching and
ATOC toggling under ASAN/UBSAN. These establish the corrected CPU-side command
decisions, not that the user's remaining live flicker is fixed.

DXVK allocates fresh constant-buffer slices for updates. deko instead provides
GPU-ordered constant loads through `dkCmdBufPushConstants`; those mechanisms
must not be assumed equivalent at the implementation level. Likewise Vulkan
image layouts and access barriers cannot be copied directly into deko commands.
Port D3D9 semantics and resource dependencies, then validate the native ordering
and pixels on Switch. The current installed build remains the preceding capture
and 46-case test build while these additional source changes await testing.


## Startup test run and subsequent allocation failure

The 685f68c3 hardware run completed all 46 tests in about 2.8 seconds. The game
then failed at `dkMemBlockCreate` around 19.1 seconds, before its first Present.
The pass report establishes the test assertions, not successful game startup.
The offscreen suite performs no Presents, and resource destruction is deferred
until frame retirement; test allocations can therefore remain live during the
following game load. This is a plausible contributor to memory pressure, not
an established cause of every allocation failure. `DIRT2_D3D9_TESTS` has been
removed from the next deployment. Keep normal game startup free of this flag;
validate complete test-resource retirement or use a standalone runner before
reenabling tests immediately ahead of a game load.


The broader source-level comparison, confirmed gaps, tested local fixes and
prioritized coverage work are recorded in [the global DXVK audit](D3D9_DXVK_AUDIT.md).

### Live video state-cache isolation (20 October)

Five Switch recordings show intermittent green menu flashes, a blue billboard,
and purple race frames which detailed GPU dumps had missed. Evidence and exact
candidate intervals are in `build/package/switch/video-evidence-20261020/`.
The cause remains unconfirmed.

`DIRT2_DEKO_REBIND_STATE=1` is an opt-in diagnostic: restore all native draw state
without per-operation CPU waits. Normal render-target publication barriers are
preserved; rebinding unchanged targets does not add another explicit barrier.
The renderer logs when this mode is enabled. Removing the line restores the
normal state cache. Keep `DIRT2_DEKO_SYNC` and startup conformance tests disabled
for this comparison. A clean result narrows the cause to state restoration or
binding-sensitive timing; it does not identify a particular missing dirty flag.
