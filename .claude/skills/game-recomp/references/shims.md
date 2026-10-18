# Shims: replacing Windows, DirectX and GFWL

One folder per DLL, registered in a single import table (`{dll, name, ordinal, fn}`). The lifter emits
weak stubs that report missing imports by name; a shim is a strong definition. Build the
shims as a CMake **OBJECT library**, so every shim links and overrides its weak stub (a static
archive pulls in only referenced members).

## Inventory and decisions (DiRT 2: 520 imports from 23 DLLs)

| DLL | Replacement |
|---|---|
| kernel32 (186) | POSIX/host: waitables, threads, critical sections, TLS, heaps, virtual memory, files with overlapped I/O, paths, locale/code pages, time, SEH. Also the CRT's dynamically resolved APIs (`Fls*`, `EncodePointer`) |
| user32 / gdi32 | Headless window table and message queue, synchronous `WM_NCCREATE`/`WM_CREATE`, display modes |
| advapi32 | In-memory registry, per run (`DIRT2_LOG_REG` logs it) |
| xlive (GFWL, 134 ordinals) | Always offline with one signed-in local profile. Async calls complete through XOVERLAPPED |
| ws2_32 | Network reported down (the online services are gone) |
| xinput1_3 / dinput8 | SDL3 gamepads and keyboard |
| binkw32 | A real Bink player on FFmpeg (`binkvideo` + `binkaudio` only) |
| OpenAL32 | OpenAL Soft passthrough (1:1) |
| vorbis/ogg DLLs | Native libvorbisfile, linked directly |
| d3d9 | Guest COM bridge to DXVK (see d3d9-bridge.md) |
| d3dx9 | Constant tables from CTAB, built-in run-time shaders, input semantics, mip filter/fill |
| dsound | Only `GetDeviceID` and `DllGetClassObject` (hardware detection) |
| Copy protection, launcher, ad SDKs, motion seats | Dropped. The SecuROM launcher handshake (a named file mapping) is emulated, so the launcher exe is not needed |

- Force the D3D9 path by refusing `LoadLibrary("d3d11.dll"/"dxgi.dll")`.

## Hard-won rules

- **stdcall argument counts must be exact.** A wrong count corrupts the guest stack far from the call site; `XUserCheckPrivilege` cost a day. xlive ordinals are stdcall. When you see a value appear where a pointer belongs, suspect a callee that popped the wrong number of bytes (see the debugging casebook).
- **Synchronisation:** give each waiter its own condition variable, make the critical-section lookup lock-free and the handle table reader/writer. One shared `notify_all` caused lock convoys across the game's roughly 40 threads.
- **Heap (`HeapAlloc`, shared with the bridge's staging):**
  - small size classes up to 64 KB come from 1 MB slabs;
  - `heap_free` must check and clear the block magic **under the lock**, because a check-then-act race handed the same block out twice (it surfaced as a buffer staging area aliasing its own proxy);
  - reject a corrupt free-list head (log it and drop the list) rather than handing out garbage;
  - `heap_realloc` takes the lock too.
- **Files and the VFS:**
  - `C:\<InstallDir>\...` maps to the game folder, and `C:\...` maps to `<data>/C/...` (saves, settings, user profile);
  - lookups are case-insensitive with a per-directory listing cache, since iOS file systems are case-sensitive; invalidate the cache on create, delete and rename;
  - **overrides:** `vfs::set_override("system/states.bin", host_file)` serves a patched copy in place of a game-folder file;
  - log every open, read and write behind a switch (`DIRT2_LOG_FILES`).
- **Saves location:** `C:\Users\Player\Documents\My Games\<Game>` lands in the data folder. On iOS that is the app container's `Library/Application Support`, not Documents (Documents holds the user-copied game data).

## Input

- XInput from SDL3 gamepads. Pad axes: XInput's Y axis is positive up, so invert SDL's.
- With no pad, the keyboard drives a **virtual pad 0**: arrows to the d-pad (left/right also to the stick X axis), Return/Space = A, Escape = B, Tab = Start, W/S = triggers. DirectInput keyboard state comes from `SDL_GetKeyboardState` with an SDL-scancode → DIK table (arrows are extended codes: 0xC8, 0xCB, 0xCD, 0xD0).
- **SDL's MFi (GameController) joystick driver on macOS** opened some pads but delivered no input (Backbone One). Turn it off (`SDL_HINT_JOYSTICK_MFI=0`), and use HIDAPI/IOKit with the community `gamecontrollerdb.txt` bundled next to the executable (`SDL_AddGamepadMappingsFromFile`). Allow a user override file in the data folder. On iOS, MFi is the only route, so enable it there.
- **Scripted input** for reproducible runs: `DIRT2_INPUT_SCRIPT="20 start; 26 A; 32 down 0.2; 40 lx=-1"` (seconds from the first poll, inputs joined with `+`, an optional hold). Combine it with `DIRT2_SCREENSHOT_DIR` to drive menus headlessly and check the result.
- **Text entry** (a profile name): the game draws its own on-screen keyboard. Replace it with a native prompt (`NSAlert` / `UIAlertController`) through a `platform::prompt_text`, and have a `DIRT2_TEXT_INPUT` answer it in scripted runs.

## Video and audio

- **Bink:** implement the player API in the shim and decode on FFmpeg. Frame-buffer mode, frame-rate timing, seek, pause, volume, and audio through SDL. Keep the decoder host-only (`bink::Decoder`) so tests can use it. Skip intros by default for iteration (`DIRT2_PLAY_VIDEOS=1` plays them).
- The game's video planes were `D3DFMT_A8`, not L8. That mattered for the green-video bug (apple-graphics.md).
- **FFmpeg build:** enable only the needed decoders and demuxer, and pass `--disable-autodetect`. It found Homebrew's X11 otherwise.

## D3DX

- The game compiled one fixed HLSL file at run time (8 immediate-mode shaders). Rather than ship an HLSL compiler, assemble them to SM3 bytecode with CTAB using a small assembler (`sm3asm.py`), and serve them from `D3DXCompileShader`.
- Constant tables are parsed from the shaders' CTAB comment block (`DIRT2_LOG_CTAB`).
