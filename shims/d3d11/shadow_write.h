#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shim::d3d11 {
// NO_OVERWRITE permits writes only to the region the application updates.
// Copying an entire persistent shadow also writes regions still read by the
// GPU, even if their values happen to be unchanged. Never read the host map:
// it can be write-combined memory. Compare against a normal-memory baseline.
inline size_t write_shadow_changes(void* destination, const void* shadow, const void* baseline, size_t size) {
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(shadow);
    const auto* old = static_cast<const uint8_t*>(baseline);
    size_t written = 0, i = 0;
    while (i < size) {
        if (size - i >= 64 && std::memcmp(src + i, old + i, 64) == 0) { i += 64; continue; }
        if (src[i] == old[i]) { ++i; continue; }
        const size_t first = i++;
        while (i < size && src[i] != old[i]) ++i;
        std::memcpy(dst + first, src + first, i - first);
        written += i - first;
    }
    return written;
}
} // namespace shim::d3d11
