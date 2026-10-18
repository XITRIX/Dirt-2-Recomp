---
name: game-recomp
description: Static recompilation of Windows x86 PC games (PE32, Direct3D 9, Win32, GFWL) to native arm64 macOS and iOS, and other platforms. Lifter (x86 to C), runtime, Win32/DirectX shims, D3D9 through DXVK on MoltenVK, iOS packaging, and debugging a recompiled game. Use when starting or continuing a game recompilation or port, lifting a PE executable to C, writing Windows API shims, bridging D3D9 to Vulkan/Metal, debugging crashes or rendering bugs in a recompiled game, or patching game data and menus without touching the game folder.
---

# Game static recompilation (Windows x86 to native Apple, portable)

Distilled from taking Colin McRae DiRT 2 (Steam v1.1, 15 MB `.text`, 160k functions, D3D9,
GFWL, Bink, OpenAL) from an `.exe` to races running at 120 Hz on iPhone and iPad, with
CloudKit save sync and MetalFX. The worked example throughout is that project
(`~/Documents/Dev/Dirt2`, see its `HANDOVER.md` and `docs/STATUS.md`).

## Pipeline

```
game.exe ── PE loader + discovery ── lifter (one C function per guest function)
                                         │  generated/src/gen_NNNN.c, funcs.h, table.c, imports.c
runtime (guest memory, threads, SEH, dispatch) ── shims (one folder per DLL) ── platform host (SDL3)
                                         │
         d3d9 calls ── guest COM proxies ── DXVK d3d9 (native, static) ── MoltenVK (static) ── Metal
```

## Phases and their "done" checks

1. **Analysis.** PE loader, function discovery, jump tables, opcode histogram. Done when the report runs clean and coverage of `.text` is above 99%.
2. **Lift and link.** Every function lifts, all imports are weak stubs that log. Done when it links, and Unicorn differential tests pass for every opcode class the binary uses.
3. **Headless boot.** CRT, TLS, heaps and threads come up, and the game reaches its main loop. Done when no `unknown target` aborts happen during boot (feed misses back into the entries file).
4. **Frontend.** Graphics bridge, input, audio, video. Done when the menus render in correct colour with sound.
5. **Gameplay** on the dev host (macOS: lldb, logs, fast iteration).
6. **Mobile target.** Done when one full race (or level) completes on a physical device. The simulator is not a GPU proxy.

Develop and debug on macOS first; iOS is the delivery target, not the debugging environment.

## Principles that paid off

- **Measure, don't guess.** Every long stall in the project was guessing. The video bug took three days of guessing and one hour of layer isolation. When stuck, isolate by layer (decoder, raw Vulkan, DXVK standalone, game) or capture evidence: per-frame screen-recording analysis, a guest write-watch, Metal validation, register dumps.
- **Read the crash registers before theorising.** A fault at a small guest address equal to a known constant (here `0x2800` = `D3DLOCK_DISCARD|D3DLOCK_NOSYSLOCK`) means a value was passed where a pointer was expected. That points at a stack imbalance or a wrong method being called (a wrong vtable), not at a bad allocation.
- **Never touch the user's game folder.** Serve patched data through VFS overrides. Game data is never committed.
- **Third-party code stays pristine.** Submodules are untouched upstream checkouts; changes are `.patch` files applied to exported copies in `build/src`. No `ignore = dirty`.
- **One static recipe for every platform.** Build every dependency from pinned sources as a static library, with per-platform output folders. Homebrew supplies build tools only, and no shared libraries ship.
- **Lift dead code too** (`--include-dead`). Every run-time `unknown target` was a gap-scan function reached through a computed pointer.
- **Prefer data-driven changes over code RE.** Many games define their menus, options and presets in data files. Changing data (through overrides) is far cheaper than reverse-engineering the code that consumes it.
- **Build scripts must stop on the first compile error.** A script that carried on after one cost a long, silent rebuild.

## Reference files (read the one that matches the task)

| File | When |
|---|---|
| [references/lifter-runtime.md](references/lifter-runtime.md) | Lifter design, memory model, flags, SSE/x87, SEH, threads, TSO, dispatch, function hooks, differential tests |
| [references/shims.md](references/shims.md) | Win32, GFWL/xlive, DirectInput/XInput, Bink, OpenAL, D3DX, VFS, heap |
| [references/d3d9-bridge.md](references/d3d9-bridge.md) | Guest COM proxies, Lock/Unlock staging, Reset, display modes, proxy lifetime bugs |
| [references/apple-graphics.md](references/apple-graphics.md) | DXVK on MoltenVK: the patches, SPIRV-Cross, residency, flicker, pipeline warm-up, MetalFX |
| [references/ios.md](references/ios.md) | iOS build, signing, entitlements, devices, Files app, lifecycle, 120 Hz, CloudKit saves, text input |
| [references/build-system.md](references/build-system.md) | Static deps, patches in `build/src`, per-platform outputs, Xcode pitfalls, packaging |
| [references/debugging.md](references/debugging.md) | Toolkit, scripted runs, logs, crash triage, and a casebook of every major bug: symptom, cause, fix |
| [references/game-data.md](references/game-data.md) | Patching game data and menus: compiled UI formats, adding a settings row, localisation, hardware config |

## Working style for these projects

- Keep `HANDOVER.md` (short state, open issues, build, pitfalls) and `docs/STATUS.md` (detailed findings per milestone) current. Commit milestone by milestone, with the root cause and the measurement in the commit message.
- Keep history linear: rebase or cherry-pick side branches, never merge.
- Run slow builds and device installs in the background. Long, independent investigations (for example shader precompilation) go to a separate session or subagent.
- When the user tests on a device, pull its logs yourself (`xcrun devicectl device copy from ...`) rather than asking for them. Keep `dirt2.log` plus `dirt2.prev.log`, so the crash run survives a relaunch.
