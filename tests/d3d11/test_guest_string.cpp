#include "../../shims/d3d11/guest_string.h"
#include <cassert>
int main() {
    const char32_t gpu[128] = U"NVIDIA GM20B";
    auto a = shim::d3d11::guest_string(gpu);
    assert(a[0] == 'N' && a[1] == 'V' && a[11] == 'B' && a[12] == 0 && a[127] == 0);
    const char32_t unicode[8] = U"A\U0001f697B";
    auto b = shim::d3d11::guest_string(unicode);
    assert(b[0] == 'A' && b[1] == 0xd83d && b[2] == 0xde97 && b[3] == 'B' && b[4] == 0);
    const char32_t small[3] = U"A\U0001f697";
    auto c = shim::d3d11::guest_string(small);
    assert(c[0] == 'A' && c[1] == 0 && c[2] == 0);
    const char16_t native16[8] = u"A\U0001f697B";
    assert(shim::d3d11::guest_string(native16) == b);
    const char32_t invalid[3] = {0x110000, 0xd800, 0};
    auto d = shim::d3d11::guest_string(invalid);
    assert(d[0] == 0xfffd && d[1] == 0xfffd && d[2] == 0);
}
