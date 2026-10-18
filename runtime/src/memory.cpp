// Guest address space: 4 GB of host address space at recomp_mem.
//   POSIX: one demand-zero mapping. Pages only cost memory once touched, so
//   VirtualAlloc "commit" is bookkeeping plus zeroing on reuse.
//   Horizon (Switch): no demand paging, so the window is an address-space
//   reservation and committed ranges get memory mapped in, in 64 KB chunks:
//   heap memory aliased as code memory (svcMapProcessCodeMemory) and mapped
//   into the window (svcMapProcessMemory), as the sh3-nx and nfsmw-nx ports do.
//   Committed chunks stay mapped; release and decommit zero them. Touching a
//   reserved but uncommitted page faults, as on Windows.
// The low 64 KB stay inaccessible to catch null-pointer accesses.
#if defined(__SWITCH__)
#include <switch.h>

#include <malloc.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "recomp/host.h"

uint8_t* recomp_mem = nullptr;

namespace rt {

namespace {

constexpr uint64_t kSpace = 1ull << 32;
constexpr uint32_t kAllocLo = 0x02000000;   // above the image (0x400000-0x1B38000)
constexpr uint32_t kAllocHi = 0x7FFE0000;   // the exe is not large-address-aware
constexpr uint32_t kAllocFloor = 0x10000;   // below: never accessible (null pointers)

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

#if defined(__SWITCH__)

constexpr uint32_t kChunk = 0x10000;               // commit granule (allocation granularity)
constexpr uint32_t kBigChunk = 0x200000;           // large commits: 2 MB aligned host blocks
std::vector<bool> g_mapped(kSpace / kChunk);       // chunk is backed by memory
uint64_t g_mapped_bytes = 0;

// Backs [lo, hi) (chunk aligned, all unmapped) with one host block.
void map_run(uint32_t lo, uint32_t hi) {
    const size_t n = hi - lo;
    const size_t align = (n >= kBigChunk && !(lo & (kBigChunk - 1))) ? kBigChunk : 0x1000;
    void* backing = memalign(align, n);
    if (!backing) fatal("out of memory committing %u KB of guest memory at 0x%08X (%llu MB committed)",
                        unsigned(n >> 10), lo, (unsigned long long)(g_mapped_bytes >> 20));
    std::memset(backing, 0, n);
    const Handle self = envGetOwnProcessHandle();
    virtmemLock();
    void* alias = virtmemFindCodeMemory(n, align);
    Result rc = alias ? svcMapProcessCodeMemory(self, u64(alias), u64(backing), n) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_SUCCEEDED(rc)) rc = svcSetProcessMemoryPermission(self, u64(alias), n, Perm_Rw);
    if (R_SUCCEEDED(rc)) rc = svcMapProcessMemory(recomp_mem + lo, self, u64(alias), n);
    if (R_FAILED(rc)) fatal("mapping guest memory 0x%08X+0x%zX failed: 0x%X", lo, n, rc);
    for (uint32_t c = lo / kChunk; c < hi / kChunk; ++c) g_mapped[c] = true;
    g_mapped_bytes += n;
}

// Commit: maps every unmapped chunk of [addr, addr + size), coalescing runs.
void os_commit(uint32_t addr, uint32_t size) {
    if (!size) return;
    uint32_t c = std::max<uint32_t>(addr, kAllocFloor) / kChunk;
    const uint32_t end = uint32_t((uint64_t(addr) + size + kChunk - 1) / kChunk);
    while (c < end) {
        if (g_mapped[c]) {
            ++c;
            continue;
        }
        uint32_t e = c;
        while (e < end && !g_mapped[e]) ++e;
        map_run(c * kChunk, e * kChunk);
        c = e;
    }
}

// Zeroes the backed parts of [addr, addr + size): unbacked pages are not there to clear.
void zero_range(uint32_t addr, uint32_t size) {
    const uint64_t end = uint64_t(addr) + size;
    for (uint64_t a = addr; a < end;) {
        const uint64_t next = std::min<uint64_t>((a / kChunk + 1) * kChunk, end);
        if (g_mapped[a / kChunk]) std::memset(recomp_mem + a, 0, size_t(next - a));
        a = next;
    }
}

#else

void os_commit(uint32_t, uint32_t) {}  // demand-zero mapping: nothing to do

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

#endif

}  // namespace

bool mem_init() {
#if defined(__SWITCH__)
    virtmemLock();
    void* m = virtmemFindAslr(kSpace, kBigChunk);
    if (m) virtmemAddReservation(m, kSpace);
    virtmemUnlock();
    if (!m) {
        log("mem_init: cannot reserve 4 GB of address space");
        return false;
    }
    recomp_mem = static_cast<uint8_t*>(m);  // the low 64 KB are never committed
#else
    void* m = mmap(nullptr, kSpace, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        log("mem_init: cannot reserve 4 GB of address space (errno %d)", errno);
        return false;
    }
    recomp_mem = static_cast<uint8_t*>(m);
    mprotect(recomp_mem, 0x10000, PROT_NONE);
#endif
    return true;
}

uint64_t committed_bytes() {
#if defined(__SWITCH__)
    std::lock_guard lk(g_mu);
    return g_mapped_bytes;
#else
    return 0;
#endif
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
                if (commit) {
                    it->second.committed = true;
                    os_commit(addr, size);
                }
                return addr & ~(kPage - 1);
            }
        }
        const uint32_t base = addr & ~(kGranularity - 1);
        const uint32_t sz = round_up(addr + size - base, kPage);
        if (overlaps(base, sz)) return 0;
        g_regions[base] = Region{sz, commit};
        if (commit) os_commit(base, sz);
        return base;
    }
    const uint32_t sz = round_up(size, kPage);
    const uint32_t a = find_free(round_up(sz, kGranularity));
    if (!a) return 0;
    g_regions[a] = Region{sz, commit};
    if (commit) os_commit(a, sz);
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
