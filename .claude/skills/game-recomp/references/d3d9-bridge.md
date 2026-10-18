# D3D9 bridge: guest COM proxies to a host IDirect3DDevice9

`Direct3DCreate9` returns a guest-memory COM object whose vtable entries are thunks into host
methods holding the real (DXVK) objects. The bridge targets the plain `IDirect3DDevice9`
interface, so a native Metal d3d9 could replace DXVK later without touching the game side.

## Structure

- **Proxies:** a 16-byte guest block holding a vtable pointer and a magic word. There are two maps, `g_by_guest` (proxy → {host, vtable descriptor}) and `g_by_host` (host → proxy).
  - Vtables are generated from the interface list (`gen_d3d9_vtables.py`; 17 interfaces, `SLOT(I, Method)` entries).
  - Specific slots are overridden (`OVERRIDE(IDirect3DVertexBuffer9, Lock, vb_lock)`).
  - Wrap objects by their **dynamic type**: when the static type is `IDirect3DResource9` or `IDirect3DBaseTexture9`, switch on `GetType()`.
  - Proxy memory is never freed (the game may still compare the pointer), only forgotten from the maps.
- **Arguments** are converted by C++ signature, one 32-bit guest stack slot per argument. Pointer-free structs are passed zero-copy. Interface out-parameters are wrapped. `D3DPRESENT_PARAMETERS` and HWNDs are marshalled.
- **Thunks pop the stack:** `c->esp += 4 * (1 + nargs)` (return address plus stdcall arguments). Get this wrong and a value lands where a pointer was expected.
- **Fullscreen:** exclusive fullscreen becomes borderless desktop fullscreen (`DIRT2_WINDOWED=1` for a window).
- **Threading:** force `D3DCREATE_MULTITHREADED`. The game uses the device from several threads, and without it the device was lost after about 40 s.

## Lock/Unlock staging

- **Vertex and index buffers:** a persistent guest shadow per proxy (`BufferShadow`). `Lock` returns `shadow + offset`. Copy host → shadow only for READONLY locks; copy shadow → host at `Unlock`.
- **The shadow must match the buffer size.** Shadows are keyed by proxy address, and the heap reuses that address after a buffer is released and recreated larger. A stale, smaller shadow let a memcpy overrun into the next heap block (the new proxy, whose vtable it zeroed). Reallocate whenever `shadow.size != desc.Size` and no lock is open.
- **SYSTEMMEM surfaces and textures:** persistent guest shadows too. The locked pointer stays valid after unlock, as on Windows, and is flushed at unlock and before `UpdateTexture`/`UpdateSurface`.
- **DISCARD on DEFAULT pool:** release the host lock straight away and upload in one step at unlock.
- **Failed Lock or LockRect:** the game never checks the result and memcpys into the pointer anyway. A stale pointer there overwrote its own buffer objects (seen when iOS backgrounded the app and after `Reset`). On failure, hand out scratch guest memory (pitch `max(16, Width*16)` for surfaces) and log it; the matching unlock copies nothing and returns `D3D_OK`.

## Proxy lifetime: the address-reuse bug

- **Symptom:** a crash memcpy-ing to guest `0x2800` after changing resolution (a `Reset`) on iPad, at ×1 and at ×0.75.
- **Diagnosis:** the engine's VB lock wrapper calls `Lock(0, 0, &p, 0x2800)` and returns `p`. Getting the flags back as the pointer means the method at vtable slot 0x2C popped 3 arguments instead of 5: that is `IDirect3DSurface9::GetContainer`, not `IDirect3DVertexBuffer9::Lock`. The "vertex buffer" was a surface proxy.
- **Cause:** surfaces die with their container (texture or swap chain) without the game's `Release` ever reaching 0, so their `g_by_host` entry outlives them. DXVK reuses the address for a new object (a vertex buffer after `Reset`), and `wrap()` returned the dead surface's proxy.
- **Fix:** in `wrap_with`, if the existing entry's descriptor is a different leaf interface (surface, texture, VB, IB, shader, query, ...), retire the old proxy and make a new one. Base-interface lookups (IUnknown, IDirect3DResource9) still reuse the entry.
- **General lesson:** any host → guest identity map must handle host objects dying behind your back.

## Display modes and render scale

- Override `GetAdapterModeCount`, `EnumAdapterModes` and `GetAdapterDisplayMode` (for X8R8G8B8 and A8R8G8B8). Offer native pixels × {0.5, 0.75, 1, 1.25, 1.5, 2}, landscape, at the display refresh rate.
- **Round every size to a multiple of 8.** The game makes half, quarter and eighth-size buffers; odd sizes (2422×1668) made `LockRect` fail after `Reset` and crashed.
- Native means pixels, not points: SDL reports points, so multiply by `pixel_density`, and divide by it when sizing windows. Otherwise Retina windows double the drawable.
- **Labels:** hook the game's resolution label formatter (a vtable slot) to append the multiplier, e.g. `2704 x 1760 (0.75x)`. Use the game's own printf.
- **First run:** the game falls back to its largest mode when the saved resolution isn't offered (its default 1024×768). Rewrite the saved config to the ×1 mode before the game reads it.

## Debug switches worth having

`DIRT2_LOG_LOCKS` (locks, updates, texture bindings, shader changes), `DIRT2_DUMP_SHADERS=<dir>`,
`DIRT2_WATCH_PS_CONST=<name>` (a full state dump for draws using the pixel shader whose CTAB names it),
`DIRT2_SCREENSHOT_DIR` / `_EVERY` / `_FROM` / `_COUNT` (back-buffer captures as BMP; convert them with `sips -s format png`),
`DIRT2_LOG_PRESENT`, and an fps line every 300 presents.
