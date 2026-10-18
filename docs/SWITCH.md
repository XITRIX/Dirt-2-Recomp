# Nintendo Switch port

Homebrew (libnx/devkitPro, Atmosphère), NRO started by title takeover.

## Decisions

**Graphics: two stages, with a gate between them.**
1. **Bring-up:** DX9 → our D3D9 bridge → DXVK 3.1.1 → Mesa NVK, from `etonedemid/mesa-switch` (the "VKNX" driver). Everything above Vulkan is reused. Autorun (Wine-NX) already runs DXVK on it.
2. **Performance:** our own `IDirect3D9` on **deko3d** behind the bridge.
   - Every SM3 shader (about 4,300, plus our 8 immediate-mode ones) is translated offline: vkd3d-shader → SPIR-V → SPIRV-Cross → GLSL → uam → DKSH.
   - That costs about 1–3 µs of CPU per draw instead of DXVK's 15–30, with no shader compiles at run time.
   - It is built only if Gate 2 passes.

DX11 stays an Apple-only path: tessellation and compute buy nothing at this GPU's power level.

**SDL3**, which gives one code path with Apple:
- devkitPro's SDL3 Switch port;
- etonedemid's lifecycle, rumble, motion and NVK additions;
- our touch fix.

The source is `XITRIX/SDL` branch `switch-3.4.18`, applied as `third_party/patches/sdl3/0001-switch-port.patch` to `switch` builds only. OpenAL Soft outputs through SDL3's audio.

**Guest memory:**
- The 4 GB window is an ASLR-region reservation.
- Committed ranges get heap memory in 64 KB chunks: `svcMapProcessCodeMemory`, then `svcMapProcessMemory`. This is the method sh3-nx and nfsmw-nx use on hardware (`runtime/src/memory.cpp`).
- Uncommitted pages fault, as on Windows.

**Thread stacks** are committed up front on Horizon: 64 MB for the game's main thread, 16 MB for every other guest thread (`runtime/src/thread.cpp`).

## The constraint: CPU

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
