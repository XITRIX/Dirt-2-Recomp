#include "shims/d3d11/shader_bundle.h"
#include <cassert>
#include <fstream>
#include <iterator>
using namespace shim::d3d11;
void put(std::vector<uint8_t>& v,size_t o,uint32_t x) { for(int i=0;i<4;++i)v[o+i]=uint8_t(x>>(8*i)); }
int main(int argc,char**argv) {
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);
    std::vector<uint8_t> good((std::istreambuf_iterator<char>(f)),{});
    std::vector<ShaderBytecode> shaders;
    assert(parse_shader_bundle(good,shaders));assert(shaders.size()==1117);
    for(const auto& s:shaders)assert(s.stage<=5 && s.code.size()>=36);
    auto bad=good;bad.pop_back();assert(!parse_shader_bundle(bad,shaders) && shaders.empty());
    bad=good;bad.push_back(0);assert(!parse_shader_bundle(bad,shaders));
    bad=good;put(bad,16,UINT32_MAX);assert(!parse_shader_bundle(bad,shaders));
    bad=good;put(bad,8,UINT32_MAX);assert(!parse_shader_bundle(bad,shaders));
    bad=good;put(bad,12,6);assert(!parse_shader_bundle(bad,shaders));
    bad=good;put(bad,20+32,UINT32_MAX);assert(!parse_shader_bundle(bad,shaders));
    // The directory's stage must agree with the actual bytecode declaration.
    bad=good;put(bad,12,(bundle_u32(good,12)+1)%6);assert(!parse_shader_bundle(bad,shaders));
    for(size_t i=0;i<60;++i)assert(!parse_shader_bundle(std::span(good).first(i),shaders));
}
