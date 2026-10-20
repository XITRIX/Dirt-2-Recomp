#include "../../shims/d3d9/deko/constant_upload.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>

int main() {
    constexpr uint32_t capacity = 8192;
    deko::ConstantUpload<capacity> cache;
    std::array<uint8_t, capacity> source{}, gpu{};
    gpu.fill(0xcd); // Newly allocated GPU memory is not assumed to be zero.
    auto apply = [&](uint32_t size) {
        auto range = cache.update(source.data(), size);
        assert(range.offset % 16 == 0 && range.size % 16 == 0);
        assert(range.offset + range.size <= size);
        std::copy_n(source.data() + range.offset, range.size, gpu.data() + range.offset);
        assert(std::equal(source.begin(), source.begin() + size, gpu.begin()));
        return range;
    };
    assert(apply(capacity).size == capacity);
    assert(apply(capacity).size == 0);
    source[64] = 1;
    auto one = apply(capacity);
    assert(one.offset == 64 && one.size == 16);
    assert(apply(16).size == 0);
    source[capacity - 1] = 2;
    assert(apply(capacity).size == 16);
    cache.reset();
    gpu.fill(0xcd);
    assert(apply(16).size == 16);
    assert(apply(capacity).size == capacity - 16);
    assert(apply(0).size == 0);

    // Replay changing shader layouts, register values, and allocation lifetimes.
    std::mt19937 random(0xD172);
    for (unsigned i = 0; i < 10000; ++i) {
        if (i % 113 == 0) { cache.reset(); gpu.fill(0xcd); }
        uint32_t size = (random() % (capacity / 16 + 1)) * 16;
        for (unsigned j = 0; j < 8; ++j) source[random() % capacity] = uint8_t(random());
        apply(size);
        assert(apply(size).size == 0);
    }
    std::puts("constant uploads: deterministic and 10000 randomized replays passed");
}
