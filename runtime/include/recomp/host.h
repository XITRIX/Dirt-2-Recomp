// Host-side runtime API used by shims and the platform layer (C++ only).
#pragma once

#include <cstdint>
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
// Starts a guest thread at entry(param). Returns the host thread id.
uint32_t start_thread(uint32_t entry, uint32_t param, uint32_t stack_size, bool suspended, uint32_t* handle_out);

// ---- dispatch (dispatch.cpp) ----
recomp_fn lookup(uint32_t addr);
// Calls a guest function from host code: args pushed right to left (stdcall/cdecl
// by `callee_pops`), returns eax.
uint32_t invoke(X86* c, uint32_t addr, std::initializer_list<uint32_t> args, bool callee_pops);
void register_thunk(uint32_t addr, recomp_fn fn);

// ---- misc ----
[[noreturn]] void fatal(const char* fmt, ...);
void log(const char* fmt, ...);
std::string game_dir();
void set_game_dir(const std::string& d);

}  // namespace rt
