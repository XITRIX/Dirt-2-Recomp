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

Gate 0 has not been run on hardware yet.
