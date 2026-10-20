#!/usr/bin/env python3
"""Exercise actual image allocation against deko's 3D/2D-engine constraint."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'shims/d3d9/deko/gpu.cpp').read_text()
start = source.index('bool GpuImage::create(')
opening = source.index('{', start)
end, depth = opening + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]
header = r'''
#include <deko3d.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
DkImageLayoutMaker recorded;
extern "C" void dkImageLayoutInitialize(DkImageLayout*,const DkImageLayoutMaker* maker){
    // Same constraint enforced by the installed deko3d validation library.
    assert(maker->type!=DkImageType_3D || !(maker->flags&DkImageFlags_Usage2DEngine));
    recorded=*maker;
}
extern "C" uint64_t dkImageLayoutGetSize(const DkImageLayout*){return 4096;}
extern "C" uint32_t dkImageLayoutGetAlignment(const DkImageLayout*){return 512;}
extern "C" void dkImageInitialize(DkImage*,const DkImageLayout*,DkMemBlock,uint32_t){}
struct Alloc {DkMemBlock block=nullptr;uint32_t offset=0;explicit operator bool()const{return block!=nullptr;}};
struct Pool {Alloc alloc(uint32_t size,uint32_t alignment){assert(size==4096&&alignment==512);return {reinterpret_cast<DkMemBlock>(uintptr_t(1)),0};}};
struct Gpu {DkDevice device=nullptr;Pool images;};
Gpu&gpu(){static Gpu g;return g;}
struct GpuImage {
    DkImage image{};DkImageLayout layout{};Alloc mem;
    DkImageType type;DkMsMode ms_mode;DkImageFormat format;
    uint32_t width,height,depth,levels,layers;
    bool create(DkImageType,DkImageFormat,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,DkMsMode);
};
'''
main = r'''
int main(){
    GpuImage image;
    const uint32_t flags=DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine;
    for(auto type:{DkImageType_2D,DkImageType_Cubemap,DkImageType_3D}){
        assert(image.create(type,DkImageFormat_RGBA8_Unorm,16,8,4,3,1,flags,DkMsMode_1x));
        assert(recorded.type==type&&recorded.dimensions[0]==16&&recorded.dimensions[1]==8);
        assert(recorded.dimensions[2]==(type==DkImageType_3D?4:type==DkImageType_Cubemap?6:1));
        assert(recorded.mipLevels==3);
        assert(recorded.flags==(type==DkImageType_3D?DkImageFlags_UsageRender:flags));
    }
    // The fallback volume texture is still genuinely 3D when its depth is one.
    assert(image.create(DkImageType_3D,DkImageFormat_RGBA8_Unorm,1,1,1,1,1,flags,DkMsMode_1x));
    assert(recorded.type==DkImageType_3D&&!(recorded.flags&DkImageFlags_Usage2DEngine));
    for(auto samples:{DkMsMode_2x,DkMsMode_4x,DkMsMode_8x}){
        assert(image.create(DkImageType_2D,DkImageFormat_RGBA8_Unorm,640,480,1,1,1,flags,samples));
        assert(recorded.type==DkImageType_2DMS&&recorded.msMode==samples);
        assert(image.type==DkImageType_2D&&image.ms_mode==samples);
        assert(image.create(DkImageType_2DArray,DkImageFormat_RGBA8_Unorm,640,480,1,1,4,flags,samples));
        assert(recorded.type==DkImageType_2DMSArray&&recorded.dimensions[2]==4&&recorded.msMode==samples);
        assert(!image.create(DkImageType_3D,DkImageFormat_RGBA8_Unorm,4,4,4,1,1,flags,samples));
        assert(!image.create(DkImageType_Cubemap,DkImageFormat_RGBA8_Unorm,4,4,1,1,6,flags,samples));
    }
}
'''
with tempfile.TemporaryDirectory(prefix='deko-image-creation-') as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(header + method + main)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-fsanitize=address,undefined',
                    '-I/opt/devkitpro/libnx/include', str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Actual image allocation: volume flags, 2D/cube preservation, genuine 2x/4x/8x multisample layouts and invalid dimension rejection passed (ASAN/UBSAN)')
