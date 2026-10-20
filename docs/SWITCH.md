# Nintendo Switch port

Homebrew (libnx/devkitPro, Atmosphère), NRO started by title takeover.

## Primary renderer and current state

DX11 → native DXVK → Mesa NVK is now the primary Switch renderer. The user
reports clean graphics in menus and races. The latest low-settings0.75x test
runs around20 FPS in menus and11–13 FPS racing, so performance remains below
our30 FPS target. These are hardware observations, not estimated abstraction costs.

`cmake --preset switch` selects the isolated NVK SDK and `build/switch-dxvk`.
`tools/scripts/build_switch_dx11.sh` prepares its dependencies and builds the game;
`tools/scripts/package_switch.sh` creates the primary `DiRT2-switch.zip` package.
The NRO is named **DiRT 2**. Existing DX11 install paths are retained to preserve
settings and diagnostics; saves continue to use the original shared save tree.
See [the DX11 implementation notes](SWITCH_DX11_PROTOTYPE.md).

D3D9/deko3d is retained in full. Select `switch-deko` or explicitly configure
`DIRT2_SWITCH_DXVK=OFF`; use `package_switch_deko.sh` to package it separately.
The following bring-up notes and deko3d repairs are historical context for that
fallback, not a description of the current primary renderer.

**SDL3**, which gives one code path with Apple:
- devkitPro's SDL3 Switch port;
- etonedemid's lifecycle, rumble, motion and NVK additions;
- our touch fix.

The source is `XITRIX/SDL` branch `switch-3.4.18`, applied as `third_party/patches/sdl3/0001-switch-port.patch` to `switch` builds only. OpenAL Soft outputs through SDL3's audio.

**Guest memory:**
- The 4 GB window is an ASLR-region reservation.
- A second 4 GB address window is reserved for code aliases. Backing is committed
  in 2 MB aligned runs (at most16 MB per allocation), using
  `svcMapProcessCodeMemory`, then `svcMapProcessMemory`. This avoids the thousands
  of small, randomly placed aliases that exhausted mapping resources during race
  loading (`0xCE01`). The low64 KB guard stays inaccessible; Windows commitment
  bookkeeping is finer than physical backing on Horizon. Release/decommit zero
  backing for reuse; backing is retained for the process lifetime.

**Thread stacks** are committed up front on Horizon: 64 MB for the game's main thread, 16 MB for every other guest thread (`runtime/src/thread.cpp`).

## Historical CPU estimates (not current profiling results)

The Cortex-A57 runs at 1020 MHz stock and up to about 1785 MHz on Erista, or about 2.4 GHz on Mariko, with sys-clk. Single-threaded, that is about 6–18× slower than the A17 Pro. The iPhone 15 Pro is already CPU-bound at about 30 fps in races.

Without CPU work, that means about **2–4 fps stock and 4–8 fps overclocked**. Feral's native GRID Autosport, on the same engine family, runs at 45–60 fps on Switch. The engine fits the hardware; the recompilation overhead is the gap.

## Phases and gates

| Phase | Done when |
|---|---|
| 1. Toolchain, platform layer, Horizon memory | **Gate 0:** headless boot on hardware reaches `Direct3DCreate9` (logged with seconds since start and guest MB committed). Compare with the Mac for a first CPU ratio. |
| 2. First image: DXVK on NVK | **Gate 1:** frontend and one race measured at stock and overclocked clocks. |
| 3. CPU efficiency, which also speeds up iOS: selective TSO, per-call-site inline caches, native hot routines, codegen | **Gate 2:** a race at 20–25 fps or more overclocked, or 30 fps or more on iOS. Below that, stop the Switch port and keep the iOS gains. |
| 4. D3D9 on deko3d, offline DKSH shaders | **Gate 3:** screenshot parity with the DXVK route, and lower CPU time per frame. |
| 5. Packaging, CI (devkitPro Docker image) | NRO artifact per push. |

## State

Phase 1 is built:
- `cmake --preset switch`;
- `platform/switch/` (launch-mode check, error applet, software keyboard, the Gate 0 `Direct3DCreate9`);
- the Horizon memory backend and libnx crash handler (`runtime/src/cpu.cpp`);
- `tools/scripts/package_switch.sh`.

**Gate 0 passed on hardware (2026-10-18).** The game ran its CRT and engine start-up and reached `Direct3DCreate9` in 3.5–5.2 s from launch, with 97 MB of guest memory committed.

Most of the launch time is loading the 20 MB exe from the SD card (2.7–4.2 s). The game's own start-up, from entering the guest to `Direct3DCreate9`, took:

| Device | Time |
|---|---|
| Switch | 0.65–0.85 s |
| iPhone 15 Pro | 0.11 s |
| Mac (M-series) | 0.17 s |

That is about **6–8× slower than the iPhone**, the good end of the estimate, on a short sample. Projected, it gives a race of about 4–5 fps at stock clocks before CPU work. Gate 1 measures it properly.

**Shader pipeline (Phase 4, step 1) is done offline.** The game has **888 unique DX9 shaders**: 336 vertex and 552 pixel, from the three dx9 PSSG packs plus the d3dx9 shim's built-ins. All of them translate and compile:

```bash
tools/scripts/build_shader_tools.sh        # MojoShader (pinned + patch) + SPIRV-Cross -> build/tools/dx9shader
tools/scripts/dx9_shader_census.py         # -> build/shaders/dx9/{glsl,dksh}, census.md
```

The route is d3dbc → MojoShader (SPIR-V) → SPIRV-Cross (GLSL 4.60) → uam (DKSH), 3.5 MB of DKSH in all.

**DXT2/3 texture alpha workaround.** Static BC2 textures use RGBA8 storage on
Switch, with decompression at upload while D3D locks retain their compressed
layout. The CREATE AUTOSAVE alert's smooth alpha edge became repeated rectangular
gaps when sampled as native BC2; the source blocks were correct, and a shader
discriminator ruled out vertex alpha and missing geometry. The user verified
the alert is clean with expansion enabled. The precise native sampling defect
remains unresolved. Expansion preserves explicit alpha and supports mip levels,
cube faces, volume slices and edge blocks. BC2 GPU storage grows from one to four
bytes per pixel; decompression adds work during upload. Set
`DIRT2_DEKO_EXPAND_BC2=0` only for comparison with the native path.

**Normal Switch defaults after the graphics investigation:** Zcull is enabled;
the experimental MRT depth-test bypass has been removed. Frame/texture dumps,
GPU waits after every draw and the clear-only self-test remain opt-in via their
diagnostic environment flags. Resource/adapter tracing requires
`DIRT2_DEKO_LOG_RESOURCES`; periodic FPS logging requires `DIRT2_DEKO_LOG_PERF`.
The standard package includes a comment-only `env.txt` to replace stale capture
settings during an overlay install. Dedicated diagnostic packages retain their
own settings as investigation artifacts. Error/fault logging remains enabled.

**Shader constant upload optimization (2026-10-20):** programmable shaders now
upload only the changed span of packed 16-byte registers. Repeated constant
setters with identical bytes leave the state clean. Each device/stage retains
its own mirror of its persistent GPU buffer; shader layout changes are compared
after packing, and uploads remain ordered GPU commands. Boolean padding is
zeroed. `DIRT2_DEKO_REBIND_STATE` still forces full uploads for debugging.
`DIRT2_DEKO_LOG_PERF=1` reports uploaded bytes versus full blocks alongside FPS.
The byte-saving figure excludes unchanged setters skipped before packing and
is not an FPS improvement measurement. Compare the same race/camera and clock
settings on hardware; menu, race, and loading behavior still need console
validation. The standalone host regression test can be run with:

```sh
c++ -std=c++17 -O2 -fsanitize=address,undefined tests/switch/test_constant_upload.cpp -o /tmp/dirt2-constant-test
/tmp/dirt2-constant-test
```

Rendering barriers, initialization of new targets, shader-aware colour masks and
the two verified graphics workarounds are retained. Offscreen render-target
hardware compression remains disabled: sampling those compressed targets needs
additional decompression support. That restriction is separate from Zcull and
from uploaded DXT texture formats.

**Player-name keyboard:** the Switch XLive keyboard shim queues the HOS
software keyboard on SDL's main thread and completes the game's XOVERLAPPED
after confirmation/cancellation. It must not use a detached `std::thread`:
the linked Horizon `pthread_detach` is an ENOSYS stub, causing termination.
The modal keyboard also pauses game rendering until it closes. SDL's event
thread stays responsible for the applet transition. A failed dispatch completes
the request with an error rather than leaving it pending.

**Manual GPU dumps:** press **Minus and Plus together**, then release both.
One short rumble acknowledges the request; one longer rumble signals completion
(a longer low-frequency rumble means a partial/failed capture). The chord is
consumed while held; holding it cannot repeat the dump. Busy requests are ignored.
This is available by default on Switch without an `env.txt` flag. It is detected
by XInput gamepad polling; press both buttons together to avoid a separate
Pause/Back action before the chord is recognized.

The next complete frame is saved under
`sdmc:/switch/dirt2/gpu-dumps/<UTC-time>-f<frame>-<unique-suffix>/`. Existing
folders are never replaced, including across launches. Each contains `screen.bmp`,
target BMP previews and tightly packed raw targets (including HDR), `capture.txt`
with statistics/shader identities/input-target references, and a log snapshot.
On deko3d, `textures/` also contains first-sampled texture contents (mip0,
layer0/slice0, up to16 MiB each, excluding depth formats) and each draw's float
constants: 256 vertex float4s followed by224 pixel float4s. BC2 textures expanded
to RGBA8 are labelled with the storage format. Read `capture.txt` for channel-order
notes and incomplete readbacks. Capturing synchronizes the GPU and blocks while
writing files; timing-dependent problems may be affected by capture itself.
Each manual dump also saves `live.bmp`, the ordinary frame immediately before
the detailed capture. Its draws run without the capture's intermediate texture
readbacks; only the final image readback waits. Compare it with `screen.bmp`
when a glitch disappears during capture. It is not a recording of earlier
frames, so very brief flickering can still require another capture.
Release the chord and wait for completion before taking another dump. The old
time-based flags remain available for scripted comparison runs, disabled in the
normal package.

The Switch census applies a hardware-verified workaround to the two game depth-pass
pixel shaders (`d491208d…` and `982fa4f7…`, bytecode SHA-1). Their original
`vec4(1,0,0,0) * depth` MRT1 output produced zero patches in the R32F depth
texture, causing main-menu noise. Replicating depth into all four output components
fixes the captured failure; R32F stores only red. Motion vectors and alpha discard
remain unchanged. This is deliberately restricted to those two shaders: replicating
arbitrary RGBA outputs would change rendering. The exact compiler defect remains
unresolved. The workaround rejects unexpected translated source so a translator
update cannot silently invalidate it. See `fix_depth_mrt_output` in the census script.

Interface conventions for the deko3d device, written per shader to `glsl/<sha1>.json`:
- **Vertex outputs and pixel inputs** use one fixed location per semantic: TEXCOORD0–15 → 0–15, COLOR0–3 → 16–19, NORMAL → 20–21, TANGENT 22, BINORMAL 23, FOG 24, BLENDWEIGHT 25, BLENDINDICES 26, POSITION1–4 → 27–30.
- **Vertex inputs** take the location of their input register; the JSON lists each one's usage and index.
- **Constants** are one std140 block per stage at binding 0: a float4 array, then int4, then bool, with offsets in the JSON.
- **Samplers:** sampler sN is binding N in its stage.

Compiling is not rendering: correctness is checked against DXVK screenshots at Gate 3.

**D3D9 on deko3d (Phase 4, step 2): written, not yet run on hardware.** `shims/d3d9/deko/` implements the host `IDirect3D9` the D3D9 bridge calls, in place of DXVK.
- **Scope.** It covers the 66 methods a frontend run calls through the bridge (`DIRT2_COUNT_CALLS=1` on the Mac), plus the calls the d3dx9 shim makes directly. Those include the shader constant setters: the game sets constants through D3DX constant tables, never through the bridge.
- **Stubs.** Every other method logs "not implemented" once and fails. They come from `stubs.inc`, generated by `tools/scripts/gen_d3d9_stubs.py` from DXVK's `d3d9.h`.

The design:
- **State.** D3D9 state is cached and turned into deko3d state objects at draw time, only what changed. Constants are pushed into per-stage uniform buffers in the command stream (`dkCmdBufPushConstants`), packed in each shader's MojoShader layout.
- **Vertex input.** Each vertex shader input register is matched with a declaration element by usage. `INDEXEDDATA` and `INSTANCEDATA` (every draw in this game is instanced) become the instance count and per-stream divisors.
- **Resources.**
  - Two frames are in flight, each with its own command memory and transient ring.
  - Dynamic buffers keep versions, so a `DISCARD` lock never writes what the GPU reads.
  - Textures upload through staging (`dkCmdBufCopyBufferToImage`).
  - `GetRenderTargetData` copies and waits. The game calls it almost every frame (the luminance read-back), so it is a known stall.
- **Present** blits the back buffer to a swap-chain image on the default NWindow: 1920×1080 docked, 1280×720 handheld. SDL keeps off the window (`SDL_SWITCH_NATIVE_WINDOW=1`, added to our SDL fork).
- **Conventions** follow DXVK's D3D9:
  - the viewport carries the half-pixel offset and a Y flip;
  - front faces are clockwise;
  - depth bias is scaled by the depth format.

Not done yet:
- multisampling (only `D3DMULTISAMPLE_NONE` is offered);
- sRGB reads and writes;
- depth-compare (shadow) sampling;
- textures read back on the CPU;
- render targets locked on the CPU;
- display-mode changes between docked and handheld while running.

The first build crashed after the gate. `std::exit` ran static destructors under the game's running threads; it now uses `_Exit`.

### Additional captured graphics fixes (2026-10-19)

Manual captures exposed another alpha-tested depth shader with the same broken
R32F output expression; the exact-identity workaround now covers it too. Paper
menus had 79–81% zero-depth pixels matching their visible mosaic. Race captures
also isolate a blue-channel failure in the final color LUT shader: CPU evaluation
of captured inputs matches red/green but predicts blue0.423 instead of GPU1.0.
That one shader now expresses the same LUT, matrix, bloom and vignette in explicit
RGB locals. The original 895 other compiled entries were preserved. Both changes
compile and pass host checks; new console confirmation is still required.

Manual dump screenshot/log aliases now use buffered I/O because filesystem
copy_file produced empty files/EIO on console. Earlier captures' original numbered
targets remain valid. RGB10A2 BMP previews now follow deko's native channel order.

### Surface/HUD follow-up (2026-10-19)

Two more captured depth variants now use replicated R32F output. VPOS lowering
subtracts0.5 from gl_FragCoord.xy, matching D3D9 integer pixel centers and DXVK.
Only the183 shaders reading VPOS were recompiled for this correction.

Single-mip 2D BC3 UI textures now expand to RGBA8 like BC2 textures. This is a
candidate fix for clipped level/progress symbols; source alpha is intact but
console A/B confirmation is pending. DIRT2_DEKO_EXPAND_BC3_UI=0 disables it.
Mipmapped scene textures remain compressed. Manual captures now include selected
UI/surface draw geometry and state in geometry/ with deduplicated buffer bytes.
Buffers over8MiB are marked omitted. Texture probes still capture only mip0.
Switch log snapshots now read the existing w+ stderr handle under its stdio lock.

### Resolution scale baseline

Switch scales are relative to1280x720 in both handheld and docked modes, so
1x is720p and0.5x is640x360.1.5x renders1920x1080. Sizes remain aligned to8.
The shared SDL desktop-mode report is no longer used for the Switch baseline.
The Switch build makes a private copy of the generated resolution-list code,
lowering only its height minimum from480 to360; shared generated files and
Apple builds are unchanged. The source-copy script verifies the exact original
check and fails if it cannot find it after a relift. Select the desired size in
the game's graphics menu; existing supported pixel sizes are preserved.

## Automated D3D9 regressions

The opt-in shared D3D9 suite renders41 deterministic cases through either DXVK
on the Mac or deko3d on Switch, checks expected pixels and saves readback images.
Both Mac baseline runs pass41/41 with identical pixels. Switch results are still
pending. See [D3D9_TESTS.md](D3D9_TESTS.md) for running and comparing reports.

The backend now serializes API calls, resource allocation and command recording
with a recursive gate to honor the game's D3DCREATE_MULTITHREADED use. Contention
is logged for the first8 calls. This closes a source-level threading gap; its
effect on the remaining visual symptoms still requires hardware verification.

Manual GPU dumps now include all draw geometry and all texture mip levels /
layers within the per-texture16MiB budget. Volume captures retain slice0 only.
Extra mip files are named texNNN_mipMM_layerLL_WxH_fmtF.raw. Oversized images or
buffers are explicitly reported as omitted. Normal play keeps diagnostics off.
