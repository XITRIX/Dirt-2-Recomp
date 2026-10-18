# Debugging a recompiled game

## Method

1. **Get the evidence first.** That means the log (`dirt2.log` plus `dirt2.prev.log`), the crash registers, the host backtrace with `f_<addr>` frames, and the last lines before the crash. Don't change code before reading them.
2. **Isolate by layer** when output is wrong: decoder → raw API → translation layer standalone → game. Build a small harness for each layer (`tests/video`, `tests/dxvk_smoke`).
3. **Measure the symptom** with a number that will tell you when it's fixed: black frames per N in a recording, fps, draw-time compile count, bytes that differ.
4. **Find the writer** of corrupt memory with a guest write-watch, not by inspection.
5. **Fix the cause, then add the safety net:** for example, scratch memory for failed locks so the game can't write through a stale pointer.

## Tools

| Tool | Use |
|---|---|
| Generated C | `grep -n "/\* 00ABCDEF:" generated/src/*.c`. Every instruction is a comment, so this is a disassembler for free. A shell helper `dis f_XXXX` prints a function's instructions |
| `recomp disasm <exe> <addr> [n]` | Decoder and flow-classifier check |
| `rea` MCP (Hopper backend) | Naming subsystems, call graphs, strings, decompiling when the generated C is too low-level |
| Python on the PE | Find a string's VA, scan `.rdata` for vtables (runs of code pointers), read tables of (string pointer, code) pairs |
| `RECOMP_TRACE=1` | Every shim call with its arguments, result and caller. Note that it changes timing |
| `RECOMP_WATCH=addr,len` | Guest write-watch |
| `--check-esp` | Stack imbalance per function |
| `MTL_DEBUG_LAYER=1`, lldb | Metal validation; host backtraces of Metal asserts |
| `DXVK_PIPELINE_TIMING`, `DXVK_LOG_PATH` | Stutter metric; DXVK logs in the data folder |
| `DIRT2_INPUT_SCRIPT` + `DIRT2_SCREENSHOT_DIR/_EVERY` | Scripted, headless runs that check menus and rendering by screenshot (BMP → `sips -s format png -Z 900`) |
| `xcrun devicectl device copy from` | Pull logs from the device |
| Screen recording + per-frame analysis | Flicker that the back buffer doesn't show |

**Scripted-run pattern:**
- Copy the user's data folder to a scratch directory (`DIRT2_DATA_DIR`).
- Launch with a script and a time limit, then `kill -INT`, then `kill -9`.
- Map a time to a frame number through the fps log lines.
- Note that a copied profile resumes where it was saved, and user input in the window mixes into the run.

## Crash triage patterns

- **Fault at a small address equal to a known constant:** a value was used as a pointer. Check what the faulting `memcpy`'s destination came from (`edi` at the fault is the start when it faults on the first page). If a stdcall callee popped the wrong number of bytes, the caller's `[esp+N]` reads an argument instead of its local. The pop count tells you which method actually ran, which identifies a wrong vtable or proxy.
- **Null vtable call (`call [eax+N]` with eax = 0):** an object's memory was overwritten. Usual causes: a heap double hand-out, or a stale or too-small shadow buffer overrunning into the next block. Watch the object's first dword.
- **`unknown target` abort:** a function not discovered statically. Add it to `entries.txt` (from `recomp_discovered.txt`), and lift with `--include-dead`.
- **A crash that goes away under tracing:** a timing race. Suspect memory ordering (enable TSO), sync shims (lost wake-ups) or the heap.
- **SIGBUS at host `base + small`:** guest null-page access (the first 64 KB is `PROT_NONE`).

## Casebook (symptom → cause → fix)

| Symptom | Cause | Fix |
|---|---|---|
| Green Bink video; sepia, overexposed 3D | SPIRV-Cross typed a mixed plain/compare image as `depth2d`, so alpha = red | Keep it `texture2d`, emulate compares (SPIRV-Cross patch) |
| Alternate black frames on some launches | MoltenVK cached a swapchain view of the first drawable forever | Rebuild the view when the drawable changes |
| GPU device lost after ~4–40 s; fullscreen killed macOS | (1) multithreaded device use; (2) MoltenVK removed residency while unretained command buffers were in flight | `D3DCREATE_MULTITHREADED`; defer residency removal until every queue drains |
| Gamepad connected but silent (macOS) | SDL MFi driver delivers no input for some pads | MFi off, HIDAPI/IOKit with `gamecontrollerdb.txt` |
| Races crash with `unknown target` | Gap-scan functions reached through computed pointers | Lift dead code (`--include-dead`) |
| Crash after putting the iOS app in the background | GPU work submitted in the background; failed Lock left a stale pointer the game memcpy'd to | Park the render thread at Present; scratch memory for failed locks |
| Crash at a null vtable (`0x00F688C0`) loading a race | Heap `free` check-then-clear race handed one block out twice | Check and clear the magic under the lock; guard corrupt free-list heads |
| Heap block's vtable zeroed | Buffer shadow keyed by proxy address outlived its buffer; new larger buffer at the same address | Reallocate the shadow when the size differs |
| Crash memcpy to `0x2800` when changing resolution | Dead surface proxy returned for a new VB at a reused host address (GetContainer ran instead of Lock) | Retire a proxy when the same host pointer comes back as a different leaf interface |
| `LockRect` failure, then crash after `Reset` at odd sizes | Non-multiple-of-8 render sizes | Round modes to multiples of 8; scratch fallback |
| Game picked ×2 on first run | Saved resolution not in the offered list | Rewrite the saved config to ×1 before launch |
| Windows twice too large on Retina | Sized in points, rendered in pixels | Divide by `pixel_density` |
| Heavy shader stutter | MoltenVK has no pipeline libraries; DXVK compiler threads at priority 0 on Darwin; shader cache lost on exit | Pipeline record and warm-up, default thread priority, flush the cache when idle |
| Second device started a fresh save | iCloud Drive reports no file until downloaded | CloudKit record with Found/Missing/Unknown; never upload on Unknown |
| Stack corrupted far from a call | Wrong stdcall argument count in a shim (xlive) | Exact arity per ordinal |
| Arrow keys mixed up in one menu only | Patched menu list joined with spaces; the game splits it on line breaks | Edit the data text in place, keeping its format |
| A new label shows as `<id>` | Missing localisation ids are formatted `"<%s>"` | Hook the lookup function to answer the new id |

## Process lessons

- Before blaming the latest change, check whether the same signature happened before. The ×1 and ×0.75 crashes had the same address. The first "fix" (failed `LockRect`) was real, but it wasn't this bug.
- Don't patch a format string that is shared: `"<%s>"` was also used by an XML tag writer. List every user of a constant before changing it.
- When a build seems slow, look for a compile error the script didn't stop on.
