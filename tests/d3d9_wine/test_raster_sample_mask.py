#!/usr/bin/env python3
"""Record the actual raster-state path and reject reserved sample-mask bits."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'shims/d3d9/deko/device.cpp').read_text()
start = s.index('void Device::flush_raster_state()')
opening = s.index('{', start)
end, depth = opening + 1, 1
while depth:
    depth += (s[end] == '{') - (s[end] == '}')
    end += 1
header = r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <initializer_list>
struct ApiLock{explicit ApiLock(const char*){}};
float as_float(DWORD v){float f;memcpy(&f,&v,4);return f;}
struct Gpu{DkCmdBuf cmd=nullptr;};Gpu&gpu(){static Gpu g;return g;}
uint32_t recorded_mask;DkMsMode recorded_mode;
extern "C" void dkCmdBufBindRasterizerState(DkCmdBuf,const DkRasterizerState*){}
extern "C" void dkMultisampleStateSetLocations(DkMultisampleState*,const DkSampleLocation*,uint32_t){}
extern "C" void dkCmdBufBindMultisampleState(DkCmdBuf,const DkMultisampleState* s){recorded_mode=s->mode;}
extern "C" void dkCmdBufSetSampleMask(DkCmdBuf,uint32_t mask){
    // Real hardware rejected method 0x3ef with 0xffffffff. Reserved bits must
    // never reach the SDK, which forwards this word unchanged to all 4 masks.
    assert((mask&0xffff0000u)==0);recorded_mask=mask;
}
extern "C" void dkCmdBufSetDepthBias(DkCmdBuf,float,float,float){}
struct Surface {
    struct Image{DkMsMode ms_mode=DkMsMode_1x;}img;
    struct Format{DkImageFormat dk=DkImageFormat_Z24S8;}format;Format*fmt=&format;
    Image*image(){return &img;}
};
struct Device{
    DWORD rs_[256]{};Surface*targets_[4]{},*depth_=nullptr;
    bool alpha_to_coverage()const{return false;}void flush_raster_state();
};
'''
main = r'''
int main(){
    Device d;Surface rt;d.targets_[0]=&rt;
    d.rs_[D3DRS_FILLMODE]=D3DFILL_SOLID;d.rs_[D3DRS_CULLMODE]=D3DCULL_NONE;
    d.rs_[D3DRS_MULTISAMPLEANTIALIAS]=TRUE;
    for(auto mode:{DkMsMode_1x,DkMsMode_2x,DkMsMode_4x,DkMsMode_8x}){
        rt.img.ms_mode=mode;
        for(uint32_t input:{0u,1u,0x55aau,0xffffu,0xffffffffu,0x80000000u,0xabcd1234u}){
            d.rs_[D3DRS_MULTISAMPLEMASK]=input;d.flush_raster_state();
            assert(recorded_mask==(input&0xffffu)&&recorded_mode==mode);
            assert(d.rs_[D3DRS_MULTISAMPLEMASK]==input); // API state stays a DWORD
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='deko-raster-mask-') as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(header + s[start:end] + main)
    native = root / 'build/src/dxvk/include/native'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-fsanitize=address,undefined',
                    '-I/opt/devkitpro/libnx/include', '-I' + str(native / 'windows'),
                    '-I' + str(native / 'directx'), str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Actual raster-state emission: DWORD masks preserve lower 16 bits and reject reserved bits for 1x/2x/4x/8x targets (ASAN/UBSAN)')
