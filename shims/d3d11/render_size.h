#pragma once
#include <cstdint>
namespace shim::d3d11 {
struct RenderSize { uint32_t width, height; };
// Switch's physical window cannot represent a virtual render-scale change.
inline RenderSize resize_size(RenderSize requested, RenderSize target, RenderSize window) {
    return {requested.width ? requested.width : (target.width ? target.width : window.width),
            requested.height ? requested.height : (target.height ? target.height : window.height)};
}
}
