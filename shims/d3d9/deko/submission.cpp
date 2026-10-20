// Retain the exact libnx error before the driver turns it into a break.
#include <switch.h>
#include <algorithm>
#include "recomp/host.h"

extern "C" Result __real_nvGpuChannelAppendEntry(NvGpuChannel*, iova_t, size_t, u32, u32);
extern "C" Result __real_nvGpuChannelKickoff(NvGpuChannel*);

static void report_submission_error(NvGpuChannel* channel, Result result, const char* operation) {
    NvError error{};
    const Result detail = nvGpuChannelGetErrorInfo(channel, &error);
    rt::log("deko3d: native %s failed result 0x%X, fd %u, pending entries %u, fence %u+%u, error-info result 0x%X",
        operation, unsigned(result), channel->base.fd, channel->num_entries,
        channel->fence.value, channel->fence_incr, unsigned(detail));
    if (R_SUCCEEDED(detail)) {
        // Preserve the complete driver record without relying on an SDK-specific
        // interpretation of its union. Decoding can use the matching headers.
        const auto* words = reinterpret_cast<const unsigned char*>(&error);
        for (size_t offset = 0; offset < sizeof(error); offset += 4) {
            u32 word = 0;
            __builtin_memcpy(&word, words + offset, std::min<size_t>(4, sizeof(error) - offset));
            rt::log("deko3d: native error-info +%u = %08X", unsigned(offset), unsigned(word));
        }
    }
}

extern "C" Result __wrap_nvGpuChannelAppendEntry(NvGpuChannel* channel, iova_t start, size_t commands, u32 flags, u32 threshold) {
    // Leave deko's AutoKick/atomic boundaries unchanged. Client batching belongs
    // at completed API operations, where the full queue flush can run safely.
    const Result result = __real_nvGpuChannelAppendEntry(channel, start, commands, flags, threshold);
    if (R_FAILED(result)) {
        rt::log("deko3d: rejected command entry address 0x%llX words %llu flags %08X threshold %u",
            static_cast<unsigned long long>(start), static_cast<unsigned long long>(commands), flags, threshold);
        report_submission_error(channel, result, "append");
    }
    return result;
}

extern "C" Result __wrap_nvGpuChannelKickoff(NvGpuChannel* channel) {
    const Result result = __real_nvGpuChannelKickoff(channel);
    if (R_FAILED(result)) report_submission_error(channel, result, "kickoff");
    return result;
}
