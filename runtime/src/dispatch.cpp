// Indirect control flow: guest address -> generated function or import shim.
// Unknown targets are appended to recomp_discovered.txt so the next lift can
// include them as extra entry points.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>

#include "recomp/host.h"
#include "recomp/seh.h"

namespace rt {

namespace {

std::mutex g_mu;
std::unordered_map<uint32_t, recomp_fn> g_thunks;
std::unordered_map<uint32_t, recomp_fn> g_extra;  // functions registered at run time

std::string g_discovery_log = "recomp_discovered.txt";

void record_unknown(uint32_t addr) {
    if (FILE* f = std::fopen(g_discovery_log.c_str(), "a")) {
        std::fprintf(f, "%08X\n", addr);
        std::fclose(f);
    }
}

}  // namespace

namespace {
void invalidate_cached();
}  // namespace

void set_discovery_log(const std::string& path) { g_discovery_log = path; }

uint32_t alloc_thunk(recomp_fn fn) {
    static std::atomic<uint32_t> next{kThunkBase + 0xC0000};
    const uint32_t a = next.fetch_add(16);
    register_thunk(a, fn);
    return a;
}

void register_thunk(uint32_t addr, recomp_fn fn) {
    std::lock_guard lk(g_mu);
    g_thunks[addr] = fn;
    invalidate_cached();  // retire cached mappings on every guest thread
}

namespace {

// A lookup cache must have one writer. Publishing address/function separately
// in shared slots allows colliding writers to publish A's address with B's
// function, even when readers recheck the address. Per-thread slots also avoid
// cache-line contention between the engine's workers.
constexpr uint32_t kCacheBits = 12;
struct CacheSlot { uint32_t addr = 0; recomp_fn fn = nullptr; };
struct LookupCache {
    uint64_t generation = 0;
    CacheSlot slots[1u << kCacheBits]{};
};
std::atomic<uint64_t> g_cache_generation{1};
// Keep only the owner pointer in static TLS. libnx reserves TLS inside supplied
// thread stacks, including the64KiB native watchdog/profiler stacks.
thread_local std::unique_ptr<LookupCache> g_cache;

CacheSlot& slot_for(LookupCache& cache, uint32_t addr) {
    return cache.slots[(addr * 0x9E3779B1u) >> (32 - kCacheBits)];
}

void invalidate_cached() { g_cache_generation.fetch_add(1, std::memory_order_release); }

recomp_fn lookup_slow(uint32_t addr) {
    const RecompFnEntry* b = recomp_fn_table;
    const RecompFnEntry* e = recomp_fn_table + recomp_fn_count;
    const RecompFnEntry* it = std::lower_bound(b, e, addr, [](const RecompFnEntry& x, uint32_t a) { return x.addr < a; });
    if (it != e && it->addr == addr) return it->fn;
    std::lock_guard lk(g_mu);
    if (auto t = g_thunks.find(addr); t != g_thunks.end()) return t->second;
    if (auto t = g_extra.find(addr); t != g_extra.end()) return t->second;
    return nullptr;
}

}  // namespace

recomp_fn lookup(uint32_t addr) {
    const uint64_t generation = g_cache_generation.load(std::memory_order_acquire);
    if (!g_cache) {
        g_cache = std::make_unique<LookupCache>();
        g_cache->generation = generation;
    }
    LookupCache& cache = *g_cache;
    if (cache.generation != generation) {
        for (auto& slot : cache.slots) slot = {};
        cache.generation = generation;
    }
    CacheSlot& c = slot_for(cache, addr);
    if (c.addr == addr && c.fn) return c.fn;
    recomp_fn fn = lookup_slow(addr);
    if (fn && addr) c = {addr, fn};
    return fn;
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
#if defined(__SWITCH__)
        if (rt::profile_indirect_enabled.load(std::memory_order_relaxed)) {
            struct ContextScope {
                uint64_t previous;
                ~ContextScope() { rt::profile_indirect_context.store(previous, std::memory_order_relaxed); }
            } scope{rt::profile_indirect_context.load(std::memory_order_relaxed)};
            rt::profile_indirect_context.store((uint64_t(target) << 32) | rd32(c->esp), std::memory_order_relaxed);
            fn(c);
            return;
        }
#endif
        fn(c);
        return;
    }
    // A jump into the middle of an active function (catch continuation, __except block).
    recomp_resume_at(target);
    rt::record_unknown(target);
    rt::fatal("call to unknown target 0x%08X (return address 0x%08X)", target, rd32(c->esp));
}
