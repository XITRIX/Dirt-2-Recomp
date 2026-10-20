#pragma once
#include <array>
#include <cstdint>
namespace shim::d3d11 {
// Must match the pinned Switch DXVK export in patch0005. Tick counters are
// microseconds of aggregate waits/queue inactivity, not GPU shader timings.
enum PerfCounter : unsigned {
    Draws, Dispatches, RenderPasses, Barriers, Submits, Presents,
    GpuWaits, GpuWaitUs, GpuIdleUs, CsWaits, CsWaitUs, CsIdleUs,
    CsChunks, CompileDone, CompileTotal, PerfCounterCount
};
using PerfStats = std::array<uint64_t, PerfCounterCount>;
inline PerfStats perf_delta(const PerfStats& now, const PerfStats& before) {
    PerfStats result{};
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = now[i] >= before[i] ? now[i] - before[i] : 0;
    return result;
}
}
