// Low-frequency CPU accounting for guest threads on Horizon. No thread is
// paused and no stack is inspected. The native compiler/driver threads are not
// registered here; DXVK counters report that side separately.
#include "recomp/host.h"
#if defined(__SWITCH__)
#include <switch.h>
#include <array>
#include <atomic>
#include <mutex>

namespace rt {
namespace {
struct Tracked { Handle handle = 0; uint32_t guest_id = 0, entry = 0; u64 ticks = 0, affinity = 0; s32 preferred = 0; Result affinity_rc = 0; bool have = false; };
std::array<Tracked, 128> tracked;
std::mutex mutex;
std::atomic<bool> enabled{false};
::Thread profiler;
alignas(0x1000) unsigned char stack[0x10000];
u32 tick_info() { return hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount : InfoType_ThreadTickCountDeprecated; }
void run(void*) {
    u64 last = armGetSystemTick();
    for (;;) {
        svcSleepThread(10000000000ll);
        const u64 now = armGetSystemTick();
        const double wall_ms = double(now - last) * 1000.0 / double(armGetSystemTickFreq());
        last = now;
        std::array<Tracked, 128> samples{};
        size_t count = 0; Result first_error = 0;
        {
            std::lock_guard guard(mutex);
            for (auto& t : tracked) {
                if (!t.handle) continue;
                u64 ticks = 0;
                const Result rc = svcGetInfo(&ticks, tick_info(), t.handle, TickCountInfo_Total);
                if (R_FAILED(rc)) { if (!first_error) first_error = rc; continue; }
                if (t.have && ticks >= t.ticks) {
                    auto s = t; s.ticks = ticks - t.ticks;
                    samples[count++] = s;
                }
                t.ticks = ticks; t.have = true;
            }
        }
        log("prof-switch: guest CPU window %.1f ms, %zu threads (native driver/audio/compiler excluded)", wall_ms, count);
        for (size_t i = 0; i < count; ++i) {
            const auto& t = samples[i];
            const double cpu_ms = double(t.ticks) * 1000.0 / double(armGetSystemTickFreq());
            log("prof-switch: tid %u entry %08X cpu %.3f ms, %.1f%% of one core, preferred %d mask %llX rc %08X", t.guest_id, t.entry, cpu_ms, 100.0 * cpu_ms / wall_ms, int(t.preferred), (unsigned long long)t.affinity, unsigned(t.affinity_rc));
        }
        if (first_error) log("prof-switch: thread CPU accounting unavailable for some threads: %08X", unsigned(first_error));
    }
}
}
void profile_register_thread(uint32_t entry, uint32_t guest_id) {
    if (!enabled.load()) return;
    std::lock_guard guard(mutex);
    for (auto& t : tracked) if (!t.handle) {
        t = {}; t.handle = threadGetSelf()->handle; t.entry = entry; t.guest_id = guest_id;
        t.affinity_rc = svcGetThreadCoreMask(&t.preferred, &t.affinity, t.handle);
        t.have = R_SUCCEEDED(svcGetInfo(&t.ticks, tick_info(), t.handle, TickCountInfo_Total));
        return;
    }
}
void profile_unregister_thread() {
    if (!enabled.load()) return;
    const Handle handle = threadGetSelf()->handle;
    std::lock_guard guard(mutex);
    for (auto& t : tracked) if (t.handle == handle) t = {};
}
void start_profiler() {
    if (enabled.exchange(true)) return;
    Result rc = threadCreate(&profiler, run, nullptr, stack, sizeof(stack), 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&profiler);
        if (R_FAILED(rc)) threadClose(&profiler);
    }
    if (R_FAILED(rc)) { enabled = false; log("prof-switch: disabled; thread creation failed %08X", unsigned(rc)); return; }
    log("prof-switch: guest CPU accounting enabled; 10s interval, no suspension or per-draw logging");
}
}
#endif
