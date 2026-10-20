#!/usr/bin/env python3
"""Exercise actual cached LOD views and depth descriptor channels."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'shims/d3d9/deko/gpu.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <deko3d.h>
#include <algorithm>
#include <map>
#include <cstring>
#include <cassert>
#include <vector>
#include <functional>
struct Alloc{bool valid=true;explicit operator bool()const{return valid;}};
struct Ids{int next=0;std::vector<int>freed;int take(){return next++;}void give(int id){freed.push_back(id);}};
struct Pool{unsigned frees=0;void free_now(Alloc){++frees;}};
struct Gpu{Ids image_ids;Pool images;DkImageDescriptor table[32]{};bool descriptors_dirty=false;
 DkImageDescriptor*image_desc(){return table;}void defer(std::function<void()> fn){fn();}};
Gpu&gpu(){static Gpu g;return g;}
DkImageView recorded;unsigned initialized=0;
extern "C" void dkImageDescriptorInitialize(DkImageDescriptor* d,const DkImageView* v,bool,bool){
 recorded=*v;++initialized;
 // deko3d maxwell/tic_generate.cpp depth default: RGB depth (physical G), A one.
 uint32_t word=(3u<<19)|(3u<<22)|(3u<<25)|(7u<<28);memcpy(d,&word,4);
}
DkImageFormat srgb_format(DkImageFormat f){return f==DkImageFormat_RGBA8_Unorm?DkImageFormat_RGBA8_Unorm_sRGB:f;}
'''
h=(root/'shims/d3d9/deko/gpu.h').read_text();start=h.index('struct GpuImage {');end=h.index('\n};',start)+3
main=r'''
uint32_t channel(int id,unsigned i){uint32_t word;memcpy(&word,&gpu().table[id],4);return(word>>(19+3*i))&7;}
int main(){
 GpuImage image;image.format=DkImageFormat_RGBA8_Unorm;image.levels=4;
 const int base=image.descriptor();assert(recorded.mipLevelOffset==0&&recorded.mipLevelCount==4);
 const int lod=image.descriptor(false,2);assert(lod!=base&&recorded.mipLevelOffset==2&&recorded.mipLevelCount==2);
 assert(image.descriptor(false,2)==lod&&initialized==2);
 const int srgb=image.descriptor(true,2);assert(srgb!=lod&&recorded.format==DkImageFormat_RGBA8_Unorm_sRGB);
 image.descriptor(false,999);assert(recorded.mipLevelOffset==3&&recorded.mipLevelCount==1);
 assert(image.descriptor(false,3)==image.descriptor(false,999));
 image.destroy();assert(gpu().image_ids.freed.size()==4&&gpu().images.frees==1&&image.lod_descriptors.empty());
 GpuImage intz;intz.format=DkImageFormat_Z24S8;for(auto&sw:intz.swizzle)sw=DkImageSwizzle_Red;
 int id=intz.descriptor();for(unsigned c=0;c<4;++c)assert(channel(id,c)==3);
 GpuImage df;df.format=DkImageFormat_Z24S8;df.swizzle[0]=DkImageSwizzle_Red;df.swizzle[1]=df.swizzle[2]=DkImageSwizzle_Zero;df.swizzle[3]=DkImageSwizzle_One;
 id=df.descriptor();assert(channel(id,0)==3&&channel(id,1)==0&&channel(id,2)==0&&channel(id,3)==7);
}
'''
methods='\n'.join(method(sig) for sig in ['void GpuImage::destroy(', 'DkImageView GpuImage::view(', 'int GpuImage::descriptor('])
with tempfile.TemporaryDirectory(prefix='deko-image-descriptors-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+h[start:end]+methods+main)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I/opt/devkitpro/libnx/include',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual texture descriptors: resident base mip, sRGB/cache isolation, LOD clamping, deferred IDs and INTZ/DF depth channels passed (ASAN/UBSAN)')
