#!/usr/bin/env python3
"""Actual raw shader cache must re-upload after a GPU device is recreated."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'shims/d3d9/deko/shaders.cpp').read_text()
start = s.index('bool load_shader_binary(')
opening = s.index('{', start)
end, depth = opening + 1, 1
while depth:
    depth += (s[end] == '{') - (s[end] == '}')
    end += 1
header = r'''
#include <deko3d.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
struct DkshHeader {uint32_t magic,header_sz,control_sz,code_sz,programs_off,num_programs;};
struct Gpu {uint64_t generation=1;uint32_t code_used=0;DkMemBlock code=reinterpret_cast<DkMemBlock>(uintptr_t(1));};
Gpu&gpu(){static Gpu g;return g;}
uint8_t backing[4096];unsigned initialized=0;
extern "C" uint32_t dkMemBlockGetSize(DkMemBlock){return sizeof(backing);}
extern "C" void* dkMemBlockGetCpuAddr(DkMemBlock){return backing;}
extern "C" void dkShaderInitialize(DkShader* shader,const DkShaderMaker*){
    memset(shader,0,sizeof(*shader));reinterpret_cast<uint32_t*>(shader)[0]=++initialized;
}
extern "C" bool dkShaderIsValid(const DkShader* shader){return reinterpret_cast<const uint32_t*>(shader)[0]!=0;}
namespace recomp {std::string sha256_hex(const uint8_t*,size_t){return "single-fixture";}}
'''
main = r'''
int main(){
    std::vector<uint8_t> binary(512,0);
    const DkshHeader header{0x48534b44,24,256,256,24,1};memcpy(binary.data(),&header,sizeof(header));
    binary[256]=0xa5;DkShader a{},b{},c{};
    assert(load_shader_binary(binary,&a)&&initialized==1&&gpu().code_used==256&&backing[0]==0xa5);
    assert(load_shader_binary(binary,&b)&&initialized==1&&gpu().code_used==256&&memcmp(&a,&b,sizeof(a))==0);
    // Same CPU cache, freshly allocated GPU code segment.
    ++gpu().generation;gpu().code_used=0;memset(backing,0,sizeof(backing));
    assert(load_shader_binary(binary,&c)&&initialized==2&&gpu().code_used==256&&backing[0]==0xa5);
    assert(memcmp(&a,&c,sizeof(a))!=0);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-shader-lifetime-') as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(header + s[start:end] + main)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-fsanitize=address,undefined',
                    '-I/opt/devkitpro/libnx/include', str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Actual raw shader loader: reuse within a device and re-upload/reinitialize after recreation passed (ASAN/UBSAN)')
