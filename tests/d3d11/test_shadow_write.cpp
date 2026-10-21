#include "shims/d3d11/shadow_write.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

int main() {
    std::array<uint8_t, 257> baseline{}, shadow{}, native{};
    uint32_t seed = 12345;
    for (unsigned run = 0; run < 1000; ++run) {
        size_t dirty = 0;
        for (size_t i = 0; i < shadow.size(); ++i) {
            seed = seed * 1664525 + 1013904223;
            baseline[i] = uint8_t(seed >> 24);
            shadow[i] = seed & 7 ? baseline[i] : uint8_t(baseline[i] ^ 0x55);
            native[i] = 0xDA;
            dirty += shadow[i] != baseline[i];
        }
        assert(shim::d3d11::write_shadow_changes(native.data(), shadow.data(), baseline.data(), shadow.size()) == dirty);
        for (size_t i = 0; i < shadow.size(); ++i)
            assert(native[i] == (shadow[i] != baseline[i] ? shadow[i] : 0xDA));
    }
    // Untouched ranges stand in for GPU-owned data. Any write there faults,
    // catching full-buffer copies as well as over-wide aligned dirty spans.
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    auto* mapped = static_cast<uint8_t*>(mmap(nullptr, page * 3, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0));
    assert(mapped != MAP_FAILED);
    std::vector<uint8_t> old(page * 3, 0), current(old);
    current[page] = 1; current[page * 2 - 1] = 2;
    assert(mprotect(mapped, page, PROT_READ) == 0);
    assert(mprotect(mapped + page * 2, page, PROT_READ) == 0);
    assert(shim::d3d11::write_shadow_changes(mapped, current.data(), old.data(), old.size()) == 2);
    assert(mapped[page] == 1 && mapped[page * 2 - 1] == 2);
    assert(munmap(mapped, page * 3) == 0);
}
