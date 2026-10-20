// D3D9 fixed-function state for the adapted DXVK GLSL algorithms.
#pragma once
#include "d3d9_deko.h"
namespace deko {
class FixedFunction {
public:
    explicit FixedFunction(Device* device):device_(device){}
    ~FixedFunction();
    bool prepare(DrawShader* vertex,DrawShader* pixel);
    void bind_constants(bool vertex,bool pixel);
private:
    bool load();
    void build_state();
    void bind(unsigned stage,unsigned binding,const std::vector<uint8_t>& data);
    Device* device_;
    bool loaded_=false;
    std::vector<uint8_t> binaries_[3];
    DkShader shaders_[3]{};
    PackMeta vertex_meta_,pixel_meta_;
    std::vector<uint8_t> vertex_data_,pixel_data_,spec_data_,render_data_,clip_data_,blend_data_;
    Alloc buffers_[2][5]{};
    std::vector<uint8_t> cached_[2][5];
};
} // namespace deko
