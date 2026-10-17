// Guest address space: one 4 GB demand-zero mapping. Pages only cost memory
// once touched, so VirtualAlloc "commit" is bookkeeping plus zeroing on reuse.
// The low 64 KB stay inaccessible to catch null-pointer accesses.
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

#include "recomp/host.h"

uint8_t* recomp_mem = nullptr;

namespace rt {

namespace {

constexpr uint64_t kSpace = 1ull << 32;
constexpr uint32_t kAllocLo = 0x02000000;   // above the image (0x400000-0x1B38000)
constexpr uint32_t kAllocHi = 0x7FFE0000;   // the exe is not large-address-aware

struct Region {
    uint32_t size = 0;
    bool committed = false;
};

std::mutex g_mu;
std::map<uint32_t, Region> g_regions;  // allocation base -> region
uint32_t g_hint = kAllocLo;

uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

bool overlaps(uint32_t addr, uint32_t size) {
    auto it = g_regions.upper_bound(addr);
    if (it != g_regions.end() && it->first < addr + uint64_t(size)) return true;
    if (it != g_regions.begin()) {
        --it;
        if (uint64_t(it->first) + it->second.size > addr) return true;
    }
    return false;
}

uint32_t find_free(uint32_t size) {
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t a = pass == 0 ? g_hint : kAllocLo;
        while (uint64_t(a) + size <= kAllocHi) {
            auto it = g_regions.lower_bound(a);
            if (it != g_regions.begin()) {
                auto prev = std::prev(it);
                const uint64_t pend = uint64_t(prev->first) + prev->second.size;
                if (pend > a) a = round_up(uint32_t(pend), kGranularity);
            }
            it = g_regions.lower_bound(a);
            if (it == g_regions.end() || it->first >= uint64_t(a) + size) return a;
            a = round_up(uint32_t(uint64_t(it->first) + it->second.size), kGranularity);
        }
    }
    return 0;
}

void zero_range(uint32_t addr, uint32_t size) {
    // Replacing the mapping returns the pages to the OS and guarantees zeroes.
    const uintptr_t p = reinterpret_cast<uintptr_t>(recomp_mem) + addr;
    const long pg = getpagesize();
    const uintptr_t lo = (p + pg - 1) & ~uintptr_t(pg - 1), hi = (p + size) & ~uintptr_t(pg - 1);
    if (hi > lo)
        mmap(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    if (lo > p) std::memset(reinterpret_cast<void*>(p), 0, std::min<uintptr_t>(lo - p, size));
    if (hi >= lo && p + size > hi && hi >= p) std::memset(reinterpret_cast<void*>(hi), 0, p + size - hi);
}

}  // namespace

bool mem_init() {
    void* m = mmap(nullptr, kSpace, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        log("mem_init: cannot reserve 4 GB of address space (errno %d)", errno);
        return false;
    }
    recomp_mem = static_cast<uint8_t*>(m);
    mprotect(recomp_mem, 0x10000, PROT_NONE);
    return true;
}

uint32_t vm_alloc(uint32_t addr, uint32_t size, bool commit) {
    std::lock_guard lk(g_mu);
    if (!size) return 0;
    if (addr) {
        // Commit inside an existing reservation, or reserve at a fixed address.
        auto it = g_regions.upper_bound(addr);
        if (it != g_regions.begin()) {
            --it;
            if (addr >= it->first && uint64_t(addr) + size <= uint64_t(it->first) + it->second.size) {
                if (commit) it->second.committed = true;
                return addr & ~(kPage - 1);
            }
        }
        const uint32_t base = addr & ~(kGranularity - 1);
        const uint32_t sz = round_up(addr + size - base, kPage);
        if (overlaps(base, sz)) return 0;
        g_regions[base] = Region{sz, commit};
        return base;
    }
    const uint32_t sz = round_up(size, kPage);
    const uint32_t a = find_free(round_up(sz, kGranularity));
    if (!a) return 0;
    g_regions[a] = Region{sz, commit};
    g_hint = a + round_up(sz, kGranularity);
    return a;
}

bool vm_free(uint32_t addr, uint32_t size, bool release) {
    std::lock_guard lk(g_mu);
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return false;
    --it;
    if (addr < it->first || addr >= uint64_t(it->first) + it->second.size) return false;
    if (release) {
        zero_range(it->first, it->second.size);
        if (it->first < g_hint) g_hint = it->first;
        g_regions.erase(it);
        return true;
    }
    // Decommit: content must read back as zero when committed again.
    const uint32_t len = size ? size : it->second.size;
    zero_range(addr, len);
    return true;
}

VmInfo vm_query(uint32_t addr) {
    std::lock_guard lk(g_mu);
    VmInfo info{addr & ~(kPage - 1), 0, kPage, false, true};
    auto it = g_regions.upper_bound(addr);
    if (it != g_regions.begin()) {
        --it;
        if (addr < uint64_t(it->first) + it->second.size) {
            info.alloc_base = it->first;
            info.base = addr & ~(kPage - 1);
            info.size = it->first + it->second.size - info.base;
            info.committed = it->second.committed;
            info.free = false;
            return info;
        }
    }
    // Free range up to the next region.
    auto next = g_regions.upper_bound(addr);
    const uint64_t end = next == g_regions.end() ? kSpace : next->first;
    info.size = uint32_t(std::min<uint64_t>(end - info.base, 0xFFFFF000u));
    return info;
}

uint32_t guest_strdup(const std::string& s) {
    const uint32_t p = heap_alloc(uint32_t(s.size() + 1));
    std::memcpy(host_ptr(p), s.c_str(), s.size() + 1);
    return p;
}

}  // namespace rt
