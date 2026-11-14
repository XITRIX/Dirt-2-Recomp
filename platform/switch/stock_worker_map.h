#pragma once
#include "recomp/sha256.h"
#include <string_view>
#include <bit>

namespace switch_platform {
// Eligibility comes from process capabilities. A startup/UI thread may be
// deliberately pinned to one core while guest workers can use all six.
inline bool stock_parallel_process_allowed(uint32_t result, uint64_t process_mask) {
    return result == 0 && std::popcount(process_mask) >= 6;
}

// Pin the original game's complete graph. Do not collapse/reassign workers:
// nested jobs can require independent workers to make progress.
inline bool known_stock_parallel_map(std::string_view xml) {
    return xml.size() == 7705 &&
        recomp::sha256_hex(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()) ==
        "f5e5c5ad973068ce0d67b9a8d17b1f9cd3ee78a32f74bba7637ae110295a4d89";
}
}
