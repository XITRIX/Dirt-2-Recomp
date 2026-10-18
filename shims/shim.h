// Framework for Win32 import shims.
//
//   STDCALL(kernel32, GetTickCount, 0) { return now_ms(); }
//
// defines imp_kernel32_GetTickCount, which overrides the generated weak stub,
// pops the arguments like the real stdcall function and registers itself for
// GetProcAddress. Arguments are read with a[0], a[1], ...; the return value
// goes to eax. CDECL leaves argument cleanup to the caller. Ordinal-only
// imports use STDCALL_ORD(ws2_32, 23, 3).
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "recomp/host.h"

namespace shim {

struct Args {
    X86* c;
    uint32_t operator[](int i) const { return rd32(c->esp + 4 + 4 * i); }
    const char* str(int i) const { return rt::host_str((*this)[i]); }
    template <typename T>
    T* ptr(int i) const { return (*this)[i] ? reinterpret_cast<T*>(rt::host_ptr((*this)[i])) : nullptr; }
};

using ShimFn = recomp_fn;
void register_shim(const char* dll, const char* name, uint16_t ordinal, ShimFn fn);
// GetProcAddress support: returns a dispatchable guest address or 0.
uint32_t resolve_export(const std::string& dll, const char* name, uint16_t ordinal);

struct Registrar {
    Registrar(const char* dll, const char* name, uint16_t ord, ShimFn fn) { register_shim(dll, name, ord, fn); }
};

// ---- last error (TEB + 0x34) ----
inline void set_last_error(uint32_t e) {
    if (uint32_t teb = rt::current_teb()) wr32(teb + 0x34, e);
}
inline uint32_t last_error() {
    const uint32_t teb = rt::current_teb();
    return teb ? rd32(teb + 0x34) : 0;
}

// ---- strings ----
std::string from_utf16(uint32_t guest_wstr, int len = -1);   // UTF-16LE -> UTF-8
std::u16string to_utf16(const std::string& utf8);
// Copies s into a guest buffer of cap chars (incl. terminator). Returns chars written without terminator.
uint32_t copy_out(uint32_t dst, uint32_t cap, const std::string& s);
uint32_t copy_out_w(uint32_t dst, uint32_t cap_chars, const std::string& utf8);
inline uint32_t wlen(uint32_t w) {
    uint32_t n = 0;
    while (rd16(w + 2 * n)) ++n;
    return n;
}

// ---- kernel objects ----
enum class Kind { File, Event, Mutex, Semaphore, Thread, Timer, Find, Process, Module, Other };

struct KObject {
    explicit KObject(Kind k) : kind(k) {}
    virtual ~KObject() = default;
    Kind kind;
};

uint32_t handle_new(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> handle_get(uint32_t h);
template <typename T>
std::shared_ptr<T> handle_as(uint32_t h) { return std::dynamic_pointer_cast<T>(handle_get(h)); }
bool handle_close(uint32_t h);
uint32_t handle_dup(uint32_t h);

constexpr uint32_t kInvalidHandle = 0xFFFFFFFF;
constexpr uint32_t kCurrentProcess = 0xFFFFFFFF;
constexpr uint32_t kCurrentThread = 0xFFFFFFFE;

// Win32 error codes used across shims.
enum : uint32_t {
    ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_NO_MORE_FILES = 18, ERROR_HANDLE_EOF = 38,
    ERROR_NOT_SUPPORTED = 50, ERROR_FILE_EXISTS = 80, ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127, ERROR_ALREADY_EXISTS = 183, ERROR_ENVVAR_NOT_FOUND = 203,
    ERROR_MORE_DATA = 234, ERROR_NO_MORE_ITEMS = 259, ERROR_IO_PENDING = 997, ERROR_CANCELLED = 1223, ERROR_CALL_NOT_IMPLEMENTED = 120,
};

// Queues a window message (user32.cpp). hwnd 0 targets the main window.
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

// Opens a file by Windows path (kernel32/file.cpp); CreateFile semantics.
uint32_t open_file(const std::string& win, uint32_t access, uint32_t disposition);
uint32_t open_file_ex(const std::string& win, uint32_t access, uint32_t disposition, uint32_t flags_attrs);

// Signals an event handle (kernel32/sync.cpp).
bool set_event(uint32_t h);

// RECOMP_TRACE=1 logs every shim call with its first arguments and result.
extern bool g_trace;
void trace_call(const char* sym, uint32_t ret_addr, const Args& a, uint32_t result);

// Calls a guest callback (stdcall) from a shim.
inline uint32_t callback(X86* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    return rt::invoke(c, fn, args, true);
}

}  // namespace shim

#define SHIM_DEFINE(sym, dll, name, ord, pop)                                       \
    static uint32_t sym##_impl(X86* c, const shim::Args& a);                         \
    extern "C" void sym(X86* c) {                                                    \
        const shim::Args a{c};                                                       \
        const uint32_t ret_addr = rd32(c->esp);                                      \
        const uint32_t r = sym##_impl(c, a);                                         \
        if (shim::g_trace) shim::trace_call(#sym, ret_addr, a, r);                   \
        c->eax = r;                                                                  \
        c->esp += 4 + (pop);                                                         \
    }                                                                                \
    static shim::Registrar sym##_reg(#dll ".dll", name, ord, sym);                   \
    static uint32_t sym##_impl([[maybe_unused]] X86* c, [[maybe_unused]] const shim::Args& a)

#define STDCALL(dll, name, nargs) SHIM_DEFINE(imp_##dll##_##name, dll, #name, 0, 4 * (nargs))
#define CDECL(dll, name) SHIM_DEFINE(imp_##dll##_##name, dll, #name, 0, 0)
#define STDCALL_ORD(dll, ord, nargs) SHIM_DEFINE(imp_##dll##_ord##ord, dll, "", ord, 4 * (nargs))
