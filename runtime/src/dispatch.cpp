// Indirect control flow: guest address -> generated function or import shim.
// Unknown targets are appended to recomp_discovered.txt so the next lift can
// include them as extra entry points.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "recomp/host.h"
#include "recomp/seh.h"

namespace rt {

namespace {

std::mutex g_mu;
std::unordered_map<uint32_t, recomp_fn> g_thunks;
std::unordered_map<uint32_t, recomp_fn> g_extra;  // functions registered at run time

void record_unknown(uint32_t addr) {
    if (FILE* f = std::fopen("recomp_discovered.txt", "a")) {
        std::fprintf(f, "%08X\n", addr);
        std::fclose(f);
    }
}

}  // namespace

uint32_t alloc_thunk(recomp_fn fn) {
    static std::atomic<uint32_t> next{kThunkBase + 0xC0000};
    const uint32_t a = next.fetch_add(16);
    register_thunk(a, fn);
    return a;
}

void register_thunk(uint32_t addr, recomp_fn fn) {
    std::lock_guard lk(g_mu);
    g_thunks[addr] = fn;
}

recomp_fn lookup(uint32_t addr) {
    const RecompFnEntry* b = recomp_fn_table;
    const RecompFnEntry* e = recomp_fn_table + recomp_fn_count;
    const RecompFnEntry* it = std::lower_bound(b, e, addr, [](const RecompFnEntry& x, uint32_t a) { return x.addr < a; });
    if (it != e && it->addr == addr) return it->fn;
    std::lock_guard lk(g_mu);
    if (auto t = g_thunks.find(addr); t != g_thunks.end()) return t->second;
    if (auto t = g_extra.find(addr); t != g_extra.end()) return t->second;
    return nullptr;
}

uint32_t invoke(X86* c, uint32_t addr, std::initializer_list<uint32_t> args, bool callee_pops) {
    recomp_fn fn = lookup(addr);
    if (!fn) {
        record_unknown(addr);
        fatal("invoke: no function at 0x%08X", addr);
    }
    const uint32_t esp0 = c->esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) push32(c, *it);
    push32(c, kReturnSentinel);
    fn(c);
    if (!callee_pops) c->esp += uint32_t(args.size()) * 4;
    if (c->esp != esp0) log("invoke 0x%08X: stack imbalance %d bytes", addr, int(c->esp - esp0));
    c->esp = esp0;
    return c->eax;
}

}  // namespace rt

extern "C" void recomp_call(X86* c, uint32_t target) {
    if (recomp_fn fn = rt::lookup(target)) {
        fn(c);
        return;
    }
    // A jump into the middle of an active function (catch continuation, __except block).
    recomp_resume_at(target);
    rt::record_unknown(target);
    rt::fatal("call to unknown target 0x%08X (return address 0x%08X)", target, rd32(c->esp));
}
