// Debug write-watch: RECOMP_WATCH=<hex addr>[,<hex len>] records the last
// guest stores into that range (thread, value, guest return address, host
// backtrace) and prints them on fatal errors. Needs the DIRT2_WATCH build.
#if !defined(__SWITCH__)
#include <execinfo.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "recomp/host.h"

extern "C" {
uint32_t recomp_watch_lo = 0xFFFFFFFF, recomp_watch_hi = 0;
}

namespace rt {

namespace {

struct Hit {
    uint32_t addr, size, tid, esp_ret, esp;
    uint64_t value;
    void* frames[8];
    int nframes;
};

constexpr int kRing = 64;
std::mutex g_mu;
Hit g_ring[kRing];
uint64_t g_count = 0;

struct Init {
    Init() {
        if (const char* e = std::getenv("RECOMP_WATCH")) {
            char* end = nullptr;
            recomp_watch_lo = uint32_t(std::strtoul(e, &end, 16));
            uint32_t len = 4;
            if (end && *end == ',') len = uint32_t(std::strtoul(end + 1, nullptr, 16));
            recomp_watch_hi = recomp_watch_lo + len;
            std::fprintf(stderr, "[rt] watching guest writes to [%08X, %08X)\n", recomp_watch_lo, recomp_watch_hi);
        }
    }
} g_init;

}  // namespace

void dump_watch() {
    std::lock_guard lk(g_mu);
    if (!g_count) return;
    const uint64_t n = g_count < kRing ? g_count : kRing;
    std::fprintf(stderr, "[rt] last %llu of %llu watched writes (oldest first):\n", (unsigned long long)n,
                 (unsigned long long)g_count);
    for (uint64_t i = g_count - n; i < g_count; ++i) {
        const Hit& h = g_ring[i % kRing];
        std::fprintf(stderr, "  #%llu t%u [%08X]/%u <- %08llX  (esp %08X, [esp] %08X)\n", (unsigned long long)i, h.tid,
                     h.addr, h.size, (unsigned long long)h.value, h.esp, h.esp_ret);
#if !defined(__SWITCH__)  // no execinfo on Horizon: nframes stays 0
        char** syms = backtrace_symbols(h.frames, h.nframes);
        for (int k = 1; syms && k < h.nframes; ++k) std::fprintf(stderr, "      %s\n", syms[k]);
        std::free(syms);
#endif
    }
}

}  // namespace rt

extern "C" void recomp_watch_hit(uint32_t a, uint64_t v, uint32_t size) {
    rt::Hit h{};
    h.addr = a;
    h.size = size;
    h.value = v;
    h.tid = rt::current_thread_id();
    if (X86* c = rt::current_cpu()) {
        h.esp = c->esp;
        h.esp_ret = c->esp > 0x10000 ? rd32(c->esp) : 0;
    }
#if !defined(__SWITCH__)
    h.nframes = backtrace(h.frames, 8);
#endif
    std::lock_guard lk(rt::g_mu);
    rt::g_ring[rt::g_count++ % rt::kRing] = h;
}
