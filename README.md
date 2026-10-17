# DiRT 2 Recompiled

Static recompilation of Colin McRae: DiRT 2 (PC, v1.1, `dirt2_game.exe`) from 32-bit x86 to portable C, compiled natively for ARM64 Apple platforms (iOS first, then macOS, tvOS, visionOS) and anything else with a C compiler.

The game's own code is lifted mechanically. Windows, DirectX, OpenAL, Bink and Games for Windows Live imports are replaced with host implementations:

| Original | Replacement |
|---|---|
| Direct3D 9 | DXVK (d3d9) → Vulkan → MoltenVK → Metal |
| OpenAL / Rapture3D | OpenAL Soft |
| XInput / DirectInput | SDL3 gamepad (GameController on Apple) |
| Win32 window and messages | SDL3 |
| Bink 1 video | FFmpeg `bink` / `binkaudio` |
| GFWL (`xlive.dll`) | Offline stub |
| Winsock | BSD sockets |

No game data or executable is included. You need your own copy of the game.

## Layout

```
tools/recomp/   host-side recompiler (PE loader, analysis, C code generator)
runtime/        guest CPU context, memory, dispatch, SEH, threads
shims/          per-DLL import implementations
platform/       SDL3 host and per-platform glue (platform/apple)
generated/      recompiler output (not committed)
```

## Building the recompiler

```bash
cmake --preset macos-arm64
cmake --build build/macos-arm64
./build/macos-arm64/tools/recomp/recomp analyze "<game dir>/dirt2_game.exe" --out generated
```

`generated/analysis.md` summarises function discovery, coverage, jump tables, the instruction mix and the import call sites. `generated/functions.csv` lists every function.

Debugging aids:

- `recomp disasm <exe> <hex addr> [count]` prints decoded instructions and their flow classification.
- `RECOMP_PROBE=addr,addr` prints the blocks around given addresses after analysis.
- `RECOMP_NO_GAPSCAN=1` restricts discovery to code that is actually referenced.

## Status

See [docs/STATUS.md](docs/STATUS.md).
