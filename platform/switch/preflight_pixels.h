#pragma once
#include <cstdint>
namespace switch_platform {
// D3D11 FLOAT->UNORM permits 0.6 ULP on the integer side. For the test's
// (0.25,0.5,0.75,1), scaled values are (63.75,127.5,191.25,255).
// https://learn.microsoft.com/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-data-conversion
constexpr bool preflight_rgba8_matches(const uint8_t* p) {
    return p[0] == 64 && (p[1] == 127 || p[1] == 128) && p[2] == 191 && p[3] == 255;
}
}
