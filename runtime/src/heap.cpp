// Guest heap behind HeapAlloc / LocalAlloc / GlobalAlloc. Small blocks come
// from per-size-class free lists carved out of 1 MB slabs; large blocks get
// their own VirtualAlloc region. Every block has a 16-byte header so the
// payload stays 16-byte aligned for SSE code.
#include <cstring>
#include <mutex>
#include <vector>

#include "recomp/host.h"

namespace rt {

namespace {

constexpr uint32_t kHeader = 16;
constexpr uint32_t kMaxSmall = 64 * 1024;
constexpr uint32_t kSlab = 1024 * 1024;
constexpr uint32_t kLargeTag = 0xFFFFFFFF;

struct Header {
    uint32_t size;   // requested size
    uint32_t cls;    // size class, or kLargeTag
    uint32_t magic;
    uint32_t next;   // free-list link: kept out of the payload, so a freed object's
                     // contents (its vtable pointer) survive until the block is reused,
                     // as on the Windows heap; some game code touches objects right after freeing them
};
constexpr uint32_t kMagic = 0x48454150;  // "HEAP"

std::mutex g_mu;
std::vector<uint32_t> g_class_size;
std::vector<uint32_t> g_free;  // head of each class's free list (guest address of header)
uint32_t g_slab_cur = 0, g_slab_end = 0;

void init_classes() {
    if (!g_class_size.empty()) return;
    for (uint32_t s = 16; s <= kMaxSmall;) {
        g_class_size.push_back(s);
        s = s < 256 ? s + 16 : (s + s / 4 + 15) & ~15u;
    }
    g_class_size.back() = kMaxSmall;
    g_free.assign(g_class_size.size(), 0);
}

uint32_t class_for(uint32_t size) {
    size_t lo = 0, hi = g_class_size.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_class_size[mid] >= size) hi = mid;
        else lo = mid + 1;
    }
    return uint32_t(lo);
}

Header* hdr(uint32_t block) { return reinterpret_cast<Header*>(host_ptr(block)); }

}  // namespace

uint32_t heap_alloc(uint32_t size, bool zero) {
    if (!size) size = 1;
    if (size > kMaxSmall) {
        const uint32_t block = vm_alloc(0, size + kHeader, true);
        if (!block) return 0;
        *hdr(block) = Header{size, kLargeTag, kMagic, 0};
        return block + kHeader;  // fresh pages are already zero
    }
    std::lock_guard lk(g_mu);
    init_classes();
    const uint32_t cls = class_for(size);
    const uint32_t total = g_class_size[cls] + kHeader;
    uint32_t block = g_free[cls];
    if (block) {
        g_free[cls] = hdr(block)->next;
    } else {
        if (g_slab_cur + total > g_slab_end) {
            const uint32_t slab = vm_alloc(0, kSlab, true);
            if (!slab) return 0;
            g_slab_cur = slab;
            g_slab_end = slab + kSlab;
        }
        block = g_slab_cur;
        g_slab_cur += total;
    }
    *hdr(block) = Header{size, cls, kMagic, 0};
    if (zero) std::memset(host_ptr(block + kHeader), 0, g_class_size[cls]);
    return block + kHeader;
}

void heap_free(uint32_t p) {
    if (!p) return;
    const uint32_t block = p - kHeader;
    Header* h = hdr(block);
    if (h->magic != kMagic) {
        log("heap_free: bad pointer 0x%08X", p);
        return;
    }
    h->magic = 0;
    if (h->cls == kLargeTag) {
        vm_free(block, 0, true);
        return;
    }
    std::lock_guard lk(g_mu);
    h->next = g_free[h->cls];
    g_free[h->cls] = block;
}

uint32_t heap_size(uint32_t p) {
    if (!p) return 0;
    const Header* h = hdr(p - kHeader);
    return h->magic == kMagic ? h->size : 0;
}

uint32_t heap_realloc(uint32_t p, uint32_t size) {
    if (!p) return heap_alloc(size);
    const uint32_t old = heap_size(p);
    const Header* h = hdr(p - kHeader);
    if (h->cls != kLargeTag && size <= g_class_size[h->cls]) {
        hdr(p - kHeader)->size = size;
        return p;
    }
    const uint32_t n = heap_alloc(size);
    if (!n) return 0;
    std::memcpy(host_ptr(n), host_ptr(p), std::min(old, size));
    heap_free(p);
    return n;
}

}  // namespace rt
