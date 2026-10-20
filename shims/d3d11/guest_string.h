#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace shim::d3d11 {

// Native DXVK/DXMT WCHAR can be 32 bits; guest DXGI strings are UTF-16.
template <typename Char, size_t N>
std::array<uint16_t, N> guest_string(const Char (&source)[N]) {
    std::array<uint16_t, N> result{};
    size_t out = 0;
    for (size_t i = 0; i < N && source[i] && out + 1 < N; ++i) {
        uint32_t c = uint32_t(source[i]);
        if constexpr (sizeof(Char) > 2) {
            if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
            if (c >= 0x10000) {
                if (out + 2 >= N) break;
                c -= 0x10000;
                result[out++] = uint16_t(0xd800 + (c >> 10));
                result[out++] = uint16_t(0xdc00 + (c & 1023));
                continue;
            }
        }
        result[out++] = uint16_t(c);
    }
    return result;
}

} // namespace shim::d3d11
