#include "shims/d3d11/render_size.h"
#include <cassert>
using shim::d3d11::RenderSize;
using shim::d3d11::resize_size;
void expect(RenderSize actual, RenderSize expected) { assert(actual.width == expected.width && actual.height == expected.height); }
int main() {
    // Changing a virtual target must not fall back to the fixed 720p window.
    expect(resize_size({0,0}, {2560,1440}, {1280,720}), {2560,1440});
    expect(resize_size({0,0}, {640,360}, {1280,720}), {640,360});
    // Explicit buffer dimensions take precedence over an older target.
    expect(resize_size({1920,1080}, {640,360}, {1280,720}), {1920,1080});
    expect(resize_size({0,0}, {0,0}, {1280,720}), {1280,720});
    expect(resize_size({1920,0}, {0,0}, {1280,720}), {1920,720});
}
