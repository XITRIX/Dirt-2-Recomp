#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
namespace shim::d3d11 {
struct ShaderBytecode { uint32_t stage; std::span<const uint8_t> code; };
inline uint32_t bundle_u32(std::span<const uint8_t> data, size_t offset) {
    return uint32_t(data[offset]) | uint32_t(data[offset+1])<<8 | uint32_t(data[offset+2])<<16 | uint32_t(data[offset+3])<<24;
}
inline bool parse_shader_bundle(std::span<const uint8_t> data, std::vector<ShaderBytecode>& shaders) {
    shaders.clear();
    if (data.size()<12 || data.size()>64u*1024*1024 || std::memcmp(data.data(),"D2SP",4) || bundle_u32(data,4)!=1) return false;
    const uint32_t count=bundle_u32(data,8);
    if (!count || count>4096) return false;
    size_t offset=12;
    std::vector<ShaderBytecode> parsed;
    for (uint32_t i=0;i<count;++i) {
        if (data.size()-offset<8) return false;
        const uint32_t stage=bundle_u32(data,offset), size=bundle_u32(data,offset+4); offset+=8;
        if (stage>5 || size<36 || size>data.size()-offset) return false;
        const auto code=data.subspan(offset,size);
        if (std::memcmp(code.data(),"DXBC",4) || bundle_u32(code,24)!=size) return false;
        const uint32_t chunks=bundle_u32(code,28);
        if (!chunks || chunks>64 || chunks>(size-32)/4) return false;
        bool found=false;
        for (uint32_t c=0;c<chunks;++c) {
            const uint32_t p=bundle_u32(code,32+4*c);
            if (p>size-8) return false;
            const uint32_t n=bundle_u32(code,p+4);
            if (n>size-p-8) return false;
            if (!std::memcmp(code.data()+p,"SHEX",4) || !std::memcmp(code.data()+p,"SHDR",4)) {
                if (found || n<8 || (bundle_u32(code,p+8)>>16)!=stage) return false;
                found=true;
            }
        }
        if (!found) return false;
        parsed.push_back({stage,code}); offset+=size;
    }
    if (offset!=data.size()) return false;
    shaders=std::move(parsed); return true;
}
}
