// CPU accounting and optional diagnostic sampling for guest threads. Native
// instruction sampling briefly pauses one selected guest worker; no stack is
// inspected. The native compiler/driver threads are not
// registered here; DXVK counters report that side separately.
#include "recomp/host.h"
#include "recomp/pc_sampling.h"
#if defined(__SWITCH__)
#include <switch.h>
#include <array>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdlib>

int main(int, char**);

namespace rt {
std::atomic<bool> profile_indirect_enabled{false};
thread_local std::atomic<uint64_t> profile_indirect_context{0};
namespace {
struct Tracked { Handle handle = 0; uint32_t guest_id = 0, entry = 0; u64 ticks = 0, affinity = 0; s32 preferred = 0; Result affinity_rc = 0; bool have = false; std::atomic<uint64_t>* context = nullptr; };
std::array<Tracked, 128> tracked;
std::mutex mutex;
std::atomic<bool> enabled{false};
bool pc_enabled = false;
uint32_t pc_tid = 264;
struct PcResult {
    sampling::CaptureResult calls{};
    u64 pc = 0, lr = 0, ticks = 0;
    bool found = false;
};
PcResult sample_pc() {
    PcResult out;
    ThreadContext context{};
    // Hold the registry lock to pin the original pthread handle until resumed.
    // Target register/unregister cannot own this mutex while we hold it.
    std::lock_guard guard(mutex);
    for (const auto& t : tracked) if (t.handle && t.guest_id == pc_tid) {
        out.found = true;
        const u64 start = armGetSystemTick();
        out.calls = sampling::capture(
            [&] { return svcSetThreadActivity(t.handle, ThreadActivity_Paused); },
            [&] { return svcGetThreadContext3(&context, t.handle); },
            [&] { return svcSetThreadActivity(t.handle, ThreadActivity_Runnable); });
        out.ticks = armGetSystemTick() - start;
        if (out.calls.suspended && R_FAILED(out.calls.resume)) {
            // Never continue with a worker left paused. Avoid userspace logging:
            // the stopped worker could own its locks. Request a kernel dump.
            svcBreak(BreakReason_Panic, uintptr_t(&out.calls.resume), sizeof(out.calls.resume));
            svcExitProcess();
        }
        out.pc = context.pc.x; out.lr = context.lr;
        break;
    }
    return out;
}
::Thread profiler;
alignas(0x1000) unsigned char stack[0x10000];
u32 tick_info() { return hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount : InfoType_ThreadTickCountDeprecated; }
void run(void*) {
    u64 last = armGetSystemTick();
    const bool indirect = profile_indirect_enabled.load();
    // Wall samples of the innermost active indirect call, not instruction PC
    // or self CPU samples. No target thread is paused or inspected unsafely.
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> contexts;
    std::map<std::pair<u64,u64>, uint32_t> pcs;
    u64 pc_ticks = 0; uint32_t pc_samples = 0, pc_failed = 0;
    Result first_pc_error = 0;
    uint32_t jitter = uint32_t(last);
    for (;;) {
        // Jitter around 20Hz to avoid locking onto a fixed phase of a frame.
        jitter = jitter * 1664525u + 1013904223u;
        const s64 delay = pc_enabled ? 40000000ll + jitter % 20000001u
                                    : (indirect ? 10000000ll : 10000000000ll);
        svcSleepThread(delay);
        const u64 now = armGetSystemTick();
        if (pc_enabled) {
            const PcResult sample = sample_pc();
            if (sample.found) {
                pc_ticks += sample.ticks;
                if (!sample.calls.pause && !sample.calls.read && sample.pc) {
                    ++pcs[{sample.pc,sample.lr}]; ++pc_samples;
                } else {
                    ++pc_failed;
                    if (!first_pc_error) first_pc_error = sample.calls.pause ? sample.calls.pause : sample.calls.read;
                }
            }
        }
        if (indirect) {
            std::array<std::pair<uint32_t, uint64_t>, 128> captured{};
            size_t n = 0;
            {
                std::lock_guard guard(mutex);
                for (const auto& t : tracked)
                    if (t.handle && t.context) captured[n++] = {t.guest_id, t.context->load(std::memory_order_relaxed)};
            }
            for (size_t i = 0; i < n; ++i) {
                auto it = contexts.find(captured[i]);
                if (it != contexts.end()) ++it->second;
                else if (contexts.size() < 8192) contexts.emplace(captured[i], 1);
            }
        }
        if ((indirect || pc_enabled) && now - last < armGetSystemTickFreq() * 10) continue;
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
            if (indirect && cpu_ms > wall_ms * 0.20) {
                std::vector<std::pair<uint32_t, uint64_t>> ranked;
                uint64_t total = 0;
                for (const auto& [key, n] : contexts) if (key.first == t.guest_id) { ranked.emplace_back(n, key.second); total += n; }
                std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (size_t k = 0; k < ranked.size() && k < 12; ++k) {
                    const uint32_t target = uint32_t(ranked[k].second >> 32), caller = uint32_t(ranked[k].second);
                    const auto fn = target ? lookup(target) : nullptr;
                    const long long native = fn ? (long long)(uintptr_t)fn - (long long)(uintptr_t)&::main : 0;
                    log("prof-indirect: tid %u target %08X caller %08X native main%+lld wall-samples %u/%llu", t.guest_id, target, caller, native, ranked[k].first, (unsigned long long)total);
                }
            }
        }
        contexts.clear();
        if (pc_enabled) {
            log("prof-pc: tid %u samples %u failures %u capture %.3f ms/window error %08X; wall PC snapshots, not CPU-weighted", pc_tid, pc_samples, pc_failed, double(pc_ticks)*1000.0/double(armGetSystemTickFreq()), unsigned(first_pc_error));
            std::vector<std::pair<uint32_t,std::pair<u64,u64>>> ranked;
            for (const auto& [key,n] : pcs) ranked.push_back({n,key});
            std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first>b.first;});
            for (size_t i=0;i<ranked.size();++i) {
                const auto [pc,lr]=ranked[i].second;
                log("prof-pc: tid %u pc main%+lld lr main%+lld samples %u/%u",pc_tid,(long long)pc-(long long)(uintptr_t)&::main,(long long)lr-(long long)(uintptr_t)&::main,ranked[i].first,pc_samples);
            }
            if (!pc_samples && pc_failed) {
                pc_enabled = false;
                log("prof-pc: disabled after failed kernel sampling; CPU accounting continues");
            }
            pcs.clear(); pc_ticks=0; pc_samples=pc_failed=0; first_pc_error=0;
        }
        if (first_error) log("prof-switch: thread CPU accounting unavailable for some threads: %08X", unsigned(first_error));
    }
}
}
void profile_register_thread(uint32_t entry, uint32_t guest_id) {
    if (!enabled.load()) return;
    std::lock_guard guard(mutex);
    for (auto& t : tracked) if (!t.handle) {
        t = {}; t.handle = threadGetSelf()->handle; t.entry = entry; t.guest_id = guest_id; t.context = &profile_indirect_context;
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
    const char* setting = std::getenv("DIRT2_PROFILE_INDIRECT");
    profile_indirect_enabled = setting && setting[0] == '1';
    const char* pc_setting = std::getenv("DIRT2_PROFILE_PC");
    pc_enabled = pc_setting && pc_setting[0] == '1';
    if (pc_enabled && (!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33))) {
        pc_enabled = false;
        log("prof-pc: disabled; loader does not advertise thread pause/context syscalls");
    }
    if (pc_enabled) profile_indirect_enabled = false; // Keep instruction sampling near 20Hz.
    if (const char* tid = std::getenv("DIRT2_PROFILE_PC_TID")) pc_tid = uint32_t(std::strtoul(tid,nullptr,0));
    Result rc = threadCreate(&profiler, run, nullptr, stack, sizeof(stack), 0x2c, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&profiler);
        if (R_FAILED(rc)) threadClose(&profiler);
    }
    if (R_FAILED(rc)) { enabled = false; log("prof-switch: disabled; thread creation failed %08X", unsigned(rc)); return; }
    log("prof-switch: guest CPU accounting enabled; 10s reporting interval, no per-draw logging");
    if (pc_enabled) log("prof-pc: 20Hz instruction sampling enabled for guest tid %u; briefly suspends only that worker, captures PC/LR, then resumes before allocation/logging",pc_tid);
    if (profile_indirect_enabled.load()) log("prof-switch: 100Hz indirect-call wall sampling enabled; inclusive context, not instruction PC/self CPU time");
}
}
#endif
