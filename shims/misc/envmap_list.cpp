// The renderer's list of dynamic environment maps: a std::vector of entry
// pointers at owner+0x5840 (owner = [renderer+0x6B4]). 00CD70D0 walks it to
// render each "temp_envmap"; 00D95690 appends (only caller 00D8F0E0, per car,
// from a loader thread); 00D8A040 and 00D8A0E0 remove entries.
//
// The walk's second loop (00CD7340) loads begin once into ebx and keeps it
// across virtual calls, re-reading only end. An append that grows the buffer
// frees the one ebx points into; a removal can move end below ebx. An iPhone
// session crashed there reading entry 0x00006563 from a freed buffer.
//
// The hook makes the walk iterate a private copy of the entry pointers. It
// hands the lifted walk stand-in renderer and owner blocks: the walk reads the
// renderer only at +0x84 and +0x6B4, and the owner only at +0x38 and the list
// (+0x5844 begin, +0x5848 end); neither pointer reaches a callee. The stand-ins
// carry the real +0x84 and +0x38 and a list over the copy. Nothing live is
// written and nothing waits: concurrent appends and removals change the real
// list and show up on the next frame.
#include <atomic>
#include <cstdint>

#include "../shim.h"

namespace {

constexpr uint32_t kRendererOwner = 0x6B4, kRendererDevice = 0x84;  // renderer fields the walk reads
constexpr uint32_t kOwnerList = 0x5840, kOwnerTarget = 0x38;         // owner fields the walk reads
constexpr uint32_t kOwnerAt = 0x800;                                  // stand-in owner inside the block
constexpr uint32_t kBlockSize = kOwnerAt + kOwnerList + 0x10;
constexpr uint32_t kMaxEntries = 1024;  // more means a torn read
constexpr int kTries = 4;

struct Stand {
    uint32_t block = 0, buf = 0, cap = 0;
};
thread_local Stand t_stand;
std::atomic<int> g_logged{0};

// Copies the live list into the snapshot buffer; returns the entry count, or ~0
// when the list kept changing under the copy (the frame then skips envmaps).
uint32_t snapshot(Stand& s, uint32_t list, uint32_t& begin, uint32_t& end) {
    for (int i = 0; i < kTries; ++i) {
        begin = rd32(list + 4), end = rd32(list + 8);
        if (begin == end) return 0;
        if (!begin || end < begin || (end - begin) / 4 > kMaxEntries || (end - begin) % 4) continue;
        const uint32_t n = (end - begin) / 4;
        if (n > s.cap) {
            if (s.buf) rt::heap_free(s.buf);
            s.cap = n < 64 ? 64 : n;
            s.buf = rt::heap_alloc(s.cap * 4);
        }
        for (uint32_t k = 0; k < n; ++k) wr32(s.buf + 4 * k, rd32(begin + 4 * k));
        if (rd32(list + 4) == begin && rd32(list + 8) == end) return n;
    }
    begin = rd32(list + 4), end = rd32(list + 8);
    return begin == end ? 0 : ~0u;
}

}  // namespace

// thiscall (renderer, arg), ret 4
extern "C" void f_00CD70D0_orig(X86* c);
extern "C" void f_00CD70D0(X86* c) {
    const uint32_t renderer = c->ecx, owner = rd32(renderer + kRendererOwner);
    if (!owner) return f_00CD70D0_orig(c);
    Stand& s = t_stand;
    if (!s.block) s.block = rt::heap_alloc(kBlockSize, true);
    const uint32_t list = owner + kOwnerList;
    uint32_t begin, end, n = snapshot(s, list, begin, end);
    if (n == ~0u) {
        if (g_logged++ < 20) rt::log("envmap walk: list %08X kept changing during the copy; envmaps skipped this frame", list);
        n = 0;
    }
    const uint32_t fr = s.block, fo = s.block + kOwnerAt;
    wr32(fr + kRendererDevice, rd32(renderer + kRendererDevice));
    wr32(fr + kRendererOwner, fo);
    wr32(fo + kOwnerTarget, rd32(owner + kOwnerTarget));
    wr32(fo + kOwnerList + 4, s.buf);
    wr32(fo + kOwnerList + 8, s.buf + 4 * n);
    c->ecx = fr;
    f_00CD70D0_orig(c);
    if (n && (rd32(list + 4) != begin || rd32(list + 8) != end) && g_logged++ < 20)
        rt::log("envmap walk: list %08X changed during the walk (%u -> %u entries%s); walked the copy", list, n,
                (rd32(list + 8) - rd32(list + 4)) / 4, rd32(list + 4) != begin ? ", buffer moved" : "");
}
