#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <algorithm>

// Most resource-binding calls contain only a few pointers. Keep those on the
// call's stack; larger valid arrays still use dynamically sized storage.
template <typename Pointer, unsigned Capacity = 16>
class PointerArray {
    std::array<Pointer, Capacity> local{};
    std::vector<Pointer> overflow;
    uint32_t size = 0;
public:
    Pointer* resize(uint32_t count, bool clear = false) {
        size = count;
        if (count > Capacity) overflow.resize(count);
        Pointer* result = data();
        if (clear && count) std::fill_n(result, count, nullptr);
        return result;
    }
    Pointer* data() { return !size ? nullptr : (size <= Capacity ? local.data() : overflow.data()); }
    Pointer& operator[](uint32_t i) { return data()[i]; }
};
