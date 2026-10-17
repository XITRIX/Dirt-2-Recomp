// COM objects implemented in C++ and handed to the game (DirectInput, ...).
//
// An object is a guest-memory block { vtable, refcount, user... }. Methods are
// plain functions taking Args where a[0] is `this`; COM_FN<nargs, fn> pops
// this plus nargs stdcall arguments.
#pragma once

#include <initializer_list>
#include <vector>

#include "shim.h"

namespace shim {

template <int N, uint32_t (*F)(X86*, const Args&)>
void COM_FN(X86* c) {
    const Args a{c};
    const uint32_t r = F(c, a);
    c->eax = r;
    c->esp += 4 + 4 * (N + 1);
}

// Builds a guest vtable from host functions (each slot becomes a dispatch thunk).
inline uint32_t make_vtable(std::initializer_list<recomp_fn> fns) {
    const uint32_t vt = rt::heap_alloc(uint32_t(fns.size()) * 4);
    uint32_t i = 0;
    for (recomp_fn f : fns) wr32(vt + 4 * i++, rt::alloc_thunk(f));
    return vt;
}

// Allocates an object: +0 vtable, +4 refcount (starts at 1), user data from +8.
inline uint32_t new_object(uint32_t vtable, uint32_t user_size) {
    const uint32_t o = rt::heap_alloc(8 + user_size, true);
    wr32(o, vtable);
    wr32(o + 4, 1);
    return o;
}

// Shared IUnknown methods for objects that do not support QueryInterface.
inline uint32_t com_addref(X86*, const Args& a) {
    const uint32_t n = rd32(a[0] + 4) + 1;
    wr32(a[0] + 4, n);
    return n;
}
inline uint32_t com_release(X86*, const Args& a) {
    const uint32_t n = rd32(a[0] + 4) - 1;
    wr32(a[0] + 4, n);
    return n;  // objects are tiny and few; memory is not reclaimed
}
inline uint32_t com_qi_self(X86*, const Args& a) {
    // Hand back the same object for any interface: the game only asks for
    // the interface it already holds.
    if (a[2]) wr32(a[2], a[0]);
    com_addref(nullptr, a);
    return 0;
}

}  // namespace shim
