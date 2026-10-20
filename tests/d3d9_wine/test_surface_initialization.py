#!/usr/bin/env python3
"""Plain GPU surfaces need render capability for internal clears, not API usage."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'shims/d3d9/deko/resources.cpp').read_text()
def method(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
header = r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
struct FormatInfo{DkImageFormat dk;uint8_t block,bytes,block_height;bool depth;DkImageSwizzle swizzle[4]{};};
const FormatInfo*format_info(D3DFORMAT f){
    static const FormatInfo rgba{DkImageFormat_BGRA8_Unorm,1,4,1,false};
    static const FormatInfo depth{DkImageFormat_Z16,1,2,1,true};
    static const FormatInfo bc{DkImageFormat_RGBA_BC1,4,8,4,false};
    return f==D3DFMT_D16?&depth:f==D3DFMT_DXT1?&bc:&rgba;
}
uint32_t row_pitch(const FormatInfo&f,unsigned w){return ((w+f.block-1)/f.block)*f.bytes;}
uint32_t level_rows(const FormatInfo&f,unsigned h){return (h+f.block_height-1)/f.block_height;}
constexpr D3DFORMAT fourcc(char a,char b,char c,char d){return D3DFORMAT(uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24));}
struct Alloc{uint32_t size=0;};struct Pool{Alloc alloc(uint32_t n,unsigned){return {n};}};
struct Gpu{Pool readback;};Gpu&gpu(){static Gpu g;return g;}
struct GpuImage{DkImageSwizzle swizzle[4]{};uint32_t flags=0;bool created=false;
    bool create(DkImageType,DkImageFormat,unsigned,unsigned,unsigned,unsigned,unsigned,uint32_t f,DkMsMode){flags=f;created=true;return true;}};
struct Device{unsigned clears=0;void clear_new_image(GpuImage&img,bool){assert(img.flags&DkImageFlags_UsageRender);++clears;}};
struct Surface{
    UINT width,height;D3DFORMAT d3dfmt;const FormatInfo*fmt;DWORD usage;
    D3DMULTISAMPLE_TYPE multisample;DWORD multisample_quality;D3DPOOL pool;
    Device*dev_;GpuImage own;uint32_t cpu_pitch=0;Alloc cpu;
    void SetDeviceParent(Device*){}
    Surface(Device*,UINT,UINT,D3DFORMAT,DWORD,D3DPOOL,bool,D3DMULTISAMPLE_TYPE,DWORD);
};
'''
main = r'''
int main(){
    Device d;
    Surface plain(&d,128,128,D3DFMT_A8R8G8B8,0,D3DPOOL_DEFAULT,false,D3DMULTISAMPLE_NONE,0);
    assert(plain.own.created&&(plain.own.flags&DkImageFlags_UsageRender)&&plain.usage==0&&d.clears==1);
    Surface rt(&d,128,128,D3DFMT_A8R8G8B8,D3DUSAGE_RENDERTARGET,D3DPOOL_DEFAULT,false,D3DMULTISAMPLE_NONE,0);
    assert(rt.usage==D3DUSAGE_RENDERTARGET&&d.clears==2);
    Surface depth(&d,128,128,D3DFMT_D16,D3DUSAGE_DEPTHSTENCIL,D3DPOOL_DEFAULT,false,D3DMULTISAMPLE_NONE,0);
    assert(d.clears==3);
    Surface bc(&d,128,128,D3DFMT_DXT1,0,D3DPOOL_DEFAULT,false,D3DMULTISAMPLE_NONE,0);
    assert(bc.own.created&&!(bc.own.flags&DkImageFlags_UsageRender)&&d.clears==3);
    Surface cpu(&d,128,128,D3DFMT_A8R8G8B8,0,D3DPOOL_SYSTEMMEM,true,D3DMULTISAMPLE_NONE,0);
    assert(!cpu.own.created&&cpu.cpu.size==128*128*4&&d.clears==3&&cpu.usage==0);
}
'''
body = '\n'.join(method(x) for x in ['void set_swizzle(', 'uint32_t image_flags(', 'DkMsMode multisample_mode(', 'Surface::Surface(Device* d, UINT w,'])
with tempfile.TemporaryDirectory(prefix='deko-surface-init-') as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(header + body + main)
    native = root / 'build/src/dxvk/include/native'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-fsanitize=address,undefined',
                    '-I/opt/devkitpro/libnx/include', '-I' + str(native / 'windows'),
                    '-I' + str(native / 'directx'), str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Actual surface constructor: internal render flags, preserved Usage=0, RT/depth clears, no compressed render clears, CPU-only allocation passed (ASAN/UBSAN)')
