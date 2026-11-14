#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace rt::sampling {
// AArch64 frame records: previousFP, savedLR. The caller pins and pauses the
// target and supplies its known readable stack mapping. No allocation/locking.
// Missing frame pointers stop the walk; never scan arbitrary stack contents.
template<std::size_t N, class Read>
std::size_t walk_native_frames(uint64_t fp, uint64_t sp, uint64_t lo, uint64_t hi,
                               std::array<uint64_t, N>& links, Read read) {
    if (hi <= lo || hi - lo < 16 || sp < lo || sp >= hi) return 0;
    std::size_t count = 0;
    while (count < N && fp >= sp && fp >= lo && fp <= hi - 16 && !(fp & 15)) {
        uint64_t previous = 0, link = 0;
        if (!read(fp, previous, link) || !link || (link & 3)) break;
        links[count++] = link;
        if (previous <= fp) break;
        fp = previous;
    }
    return count;
}
}
