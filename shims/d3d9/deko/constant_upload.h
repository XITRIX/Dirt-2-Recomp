#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace deko {

// Mirrors bytes already recorded into a persistent uniform buffer. Updates
// stay in the GPU command stream: CPU writes to a live UBO would race draws.
template <size_t Capacity>
class ConstantUpload {
public:
    struct Range { uint32_t offset, size; };

    void reset() { valid_ = 0; }

    // Packed shader constants are whole 16-byte registers. The caller supplies
    // at most Capacity bytes; bytes beyond size remain untouched in the UBO.
    Range update(const uint8_t* data, uint32_t size) {
        uint32_t begin = 0;
        const uint32_t known = size < valid_ ? size : valid_;
        if (known && std::memcmp(bytes_.data(), data, known) == 0) {
            begin = known;
        } else {
            while (begin < known && std::memcmp(bytes_.data() + begin, data + begin, 16) == 0)
                begin += 16;
        }
        uint32_t end = size;
        if (size <= valid_) {
            while (end > begin && std::memcmp(bytes_.data() + end - 16, data + end - 16, 16) == 0)
                end -= 16;
        }
        if (end > begin) std::memcpy(bytes_.data() + begin, data + begin, end - begin);
        if (size > valid_) valid_ = size;
        return {begin, end - begin};
    }

private:
    std::array<uint8_t, Capacity> bytes_{};
    uint32_t valid_ = 0;
};

} // namespace deko
