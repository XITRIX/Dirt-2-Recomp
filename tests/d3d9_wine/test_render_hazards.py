#!/usr/bin/env python3
"""Exercise actual target/copy commands with independent native-cache models.

This verifies command ordering, not hardware pixel correctness.
"""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'shims/d3d9/deko/device.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <cassert>
#include <vector>
struct ApiLock{explicit ApiLock(const char*){}};
struct GpuImage {
 DkImageFormat format=DkImageFormat_Z24S8; DkMsMode ms_mode=DkMsMode_1x;
 unsigned width=640,height=480; DkImage native{};
 float pixels=1.f; // Represents the depth data read directly by DMA.
 DkImageView view(){DkImageView v{};v.pImage=&native;return v;}
 void create(DkImageType,DkImageFormat,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned){}
 void destroy(){}
};
struct Surface {
 struct Format{bool depth=true;}info;Format*fmt=&info;
 GpuImage*img;unsigned width=640,height=480;
 GpuImage*image(){return img;}DkImageView view(){return img->view();}
};
struct Gpu {DkCmdBuf cmd=nullptr;void*state_owner=nullptr;};Gpu&gpu(){static Gpu g;return g;}
std::vector<GpuImage*>images;
GpuImage*find(const DkImageView*v){for(auto*i:images)if(v->pImage==&i->native)return i;assert(false);return nullptr;}
// This models the native ConditionalZcullInvalidate contract: only a change
// of depth target rebuilds the summary after an external DMA update.
const DkImage*last_depth=nullptr;bool stale_depth_summary=true;
bool render_pending=false,texture_can_read=false;unsigned depth_binds=0,copies=0;
extern "C" void dkCmdBufBarrier(DkCmdBuf,DkBarrier mode,uint32_t flags){
 if(mode==DkBarrier_Fragments && (flags&DkInvalidateFlags_L2Cache))render_pending=false;
 if((flags&DkInvalidateFlags_Image) && !render_pending)texture_can_read=true;
}
extern "C" void dkCmdBufCopyImage(DkCmdBuf,const DkImageView*s,const DkImageRect*,const DkImageView*d,const DkImageRect*,uint32_t){
 find(d)->pixels=find(s)->pixels;++copies;stale_depth_summary=true;
}
extern "C" void dkCmdBufBlitImage(DkCmdBuf,const DkImageView*,const DkImageRect*,const DkImageView*,const DkImageRect*,uint32_t,uint32_t){assert(false);}
extern "C" void dkCmdBufResolveImage(DkCmdBuf,const DkImageView*,const DkImageView*){assert(false);}
extern "C" void dkCmdBufBindRenderTargets(DkCmdBuf,const DkImageView*const*,uint32_t,const DkImageView*depth){
 if(depth){if(last_depth!=depth->pImage)stale_depth_summary=false;last_depth=depth->pImage;++depth_binds;}
}
void log_unimplemented(const char*,...){assert(false);}
void probe_surface_pixels(Surface*,const char*){}
DkImageFormat srgb_format(DkImageFormat format){return format;}
struct Device {
 Surface*targets_[4]{};Surface*depth_=nullptr;DWORD rs_[256]{};unsigned dirty_=0;
 static constexpr unsigned kRaster=1;bool copy_barrier_=false;
 bool resolve_depth(Surface*,Surface*,const RECT*,const RECT*){assert(false);return false;}
 void sync(const char*,...){}
 void blit(Surface*,const RECT*,Surface*,const RECT*,bool);void flush_targets(bool publish=true);
};
'''
main=r'''
int main(){
 GpuImage src,dst;images={&src,&dst};Surface s{ {},nullptr,&src },d{ {},nullptr,&dst };s.fmt=&s.info;d.fmt=&d.info;
 Device device;device.depth_=&d;device.targets_[0]=&s;
 src.pixels=.5f;dst.pixels=1.f;last_depth=&dst.native;stale_depth_summary=false;
 device.blit(&s,nullptr,&d,nullptr,false);
 assert(copies==1 && dst.pixels==.5f);
 assert(last_depth==&dst.native && !stale_depth_summary);
 assert(depth_binds==2 && device.depth_==&d && device.copy_barrier_);
 // Ordinary color transfers have no reason to change the native depth binding.
 s.info.depth=false;d.info.depth=false;depth_binds=0;
 device.blit(&s,nullptr,&d,nullptr,false);assert(depth_binds==0);
 // End-of-render visibility must include dirty L2 data, not only texture tags.
 render_pending=true;texture_can_read=false;device.flush_targets();
 assert(!render_pending && texture_can_read);
 // Diagnostic rebinding keeps attachment bindings but must not introduce an
 // extra publication barrier, which would confound a state-vs-ordering test.
 render_pending=true;texture_can_read=false;device.flush_targets(false);
 assert(render_pending && !texture_can_read);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-render-hazards-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+method('void Device::blit(')+method('void Device::flush_targets(')+main)
 native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I/opt/devkitpro/libnx/include','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual render/copy methods: depth-summary reset after DMA, color-copy binding preservation, and render-to-sample publication passed (CPU model; ASAN/UBSAN)')
