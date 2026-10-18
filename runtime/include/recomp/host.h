// Host-side runtime API used by shims and the platform layer (C++ only).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "recomp/runtime.h"

namespace rt {

// ---- guest virtual memory (memory.cpp) ----
constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kGranularity = 0x10000;
constexpr uint32_t kThunkBase = 0xFFF00000;  // import thunks (dispatch only, never executed)
constexpr uint32_t kReturnSentinel = 0xFFEFFFF0;  // return address for host -> guest calls

bool mem_init();
// Reserves and commits [addr, addr+size). addr 0 picks a free range. Returns 0 on failure.
uint32_t vm_alloc(uint32_t addr, uint32_t size, bool commit);
bool vm_free(uint32_t addr, uint32_t size, bool release);
// Information about the region containing addr, for VirtualQuery.
struct VmInfo { uint32_t base, alloc_base, size; bool committed, free; };
VmInfo vm_query(uint32_t addr);
// Host memory backing committed guest pages (Horizon, where commits are real); 0 elsewhere.
uint64_t committed_bytes();

inline uint8_t* host_ptr(uint32_t a) { return recomp_mem + a; }
inline const char* host_str(uint32_t a) { return a ? reinterpret_cast<const char*>(recomp_mem + a) : nullptr; }
uint32_t guest_strdup(const std::string& s);  // allocated from the runtime heap

// ---- guest heap (heap.cpp): used by HeapAlloc/malloc shims ----
uint32_t heap_alloc(uint32_t size, bool zero = false);
void heap_free(uint32_t p);
uint32_t heap_realloc(uint32_t p, uint32_t size);
uint32_t heap_size(uint32_t p);

// ---- image (loader.cpp) ----
bool load_image(const std::string& exe_path, std::string& error);
uint32_t thunk_for_import(uint32_t index);

// ---- threads (thread.cpp) ----
struct Thread;
X86* current_cpu();
Thread* current_thread();
uint32_t current_thread_id();
X86* create_main_thread();
uint32_t current_teb();
// Starts a guest thread at entry(param) (stdcall). Returns the thread id.
// on_exit runs on the thread after the entry returns or ExitThread is called.
uint32_t start_thread(uint32_t entry, uint32_t param, uint32_t stack_size, bool suspended,
                      std::function<void(uint32_t)> on_exit);
// Decrements the suspend count; returns the previous count or -1.
int resume_thread(uint32_t id);
[[noreturn]] void exit_current_thread(uint32_t code);

// ---- dispatch (dispatch.cpp) ----
recomp_fn lookup(uint32_t addr);
// Calls a guest function from host code: args pushed right to left (stdcall/cdecl
// by `callee_pops`), returns eax.
uint32_t invoke(X86* c, uint32_t addr, std::initializer_list<uint32_t> args, bool callee_pops);
void register_thunk(uint32_t addr, recomp_fn fn);
// Where unknown call targets are appended (for --entries on the next lift).
void set_discovery_log(const std::string& path);
// Allocates a guest address that dispatches to fn (COM vtable slots, callbacks).
uint32_t alloc_thunk(recomp_fn fn);

// ---- SEH (seh.cpp) ----
// Called with esp at the RaiseException/RtlUnwind return address.
void raise_exception(X86* c, uint32_t code, uint32_t flags, uint32_t nparams, uint32_t params);
void rtl_unwind(X86* c, uint32_t target_frame, uint32_t record, uint32_t retval);

// ---- misc ----
[[noreturn]] void fatal(const char* fmt, ...);
void install_crash_handler();
void dump_watch();  // watch.cpp  // signal handler with a guest-aware report
void log(const char* fmt, ...);
// In-process sampling profiler (runtime/src/profiler.cpp); logs hot spots every 10 s.
void start_profiler();
std::string game_dir();
void set_game_dir(const std::string& d);

}  // namespace rt
