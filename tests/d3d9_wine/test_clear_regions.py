#!/usr/bin/env python3
"""Exercise actual Clear command emission with a CPU model of clear/scissor ops."""
from pathlib import Path
import tempfile,subprocess,os
root=Path(__file__).resolve().parents[2];source=(root/'shims/d3d9/deko/device.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cassert>
struct ApiLock{explicit ApiLock(const char*){}};
using DkCmdBuf=int;constexpr unsigned DkColorMask_RGBA=15;
struct DkScissor{uint32_t x,y,w,h;};DkScissor current{};
std::vector<uint32_t>pixels(640*480);unsigned writes=0;
void dkCmdBufSetScissors(int,unsigned,const DkScissor*s,unsigned){current=*s;}
void dkCmdBufClearColorFloat(int,unsigned,unsigned,float r,float g,float b,float a);
void record_color();
void dkCmdBufClearColorFloat(int,unsigned,unsigned,float r,float g,float b,float a){
 record_color();
 ++writes;assert(current.x+current.w<=640&&current.y+current.h<=480);
 uint32_t color=(unsigned(a*255)<<24)|(unsigned(r*255)<<16)|(unsigned(g*255)<<8)|unsigned(b*255);
 for(unsigned y=current.y;y<current.y+current.h;++y)for(unsigned x=current.x;x<current.x+current.w;++x)pixels[y*640+x]=color;
}
struct ClearRecord{unsigned image;DkScissor scissor;bool depth;};std::vector<ClearRecord>clears;unsigned active_image=0;
struct DkImageView{unsigned id;int format=1;};
void dkCmdBufBindRenderTarget(int,const DkImageView*view,void*){active_image=view->id;}
void dkCmdBufBindRenderTargets(int,void*,unsigned,const DkImageView*view){active_image=view->id;}
void dkCmdBufClearDepthStencil(int,bool,float,unsigned,uint8_t){clears.push_back({active_image,current,true});}
int srgb_format(int f){return f;}
void record_color(){clears.push_back({active_image,current,false});}

struct Gpu{const void* state_owner=nullptr;uint64_t list_id=1;int cmd=1;};Gpu&gpu(){static Gpu g;return g;}
struct Image{int format=1;};struct Surface{struct Core{bool autogen_dirty=false;};Core*tex=nullptr;Image i;unsigned id=0,width=640,height=480;Image*image(){return &i;}DkImageView view(){return{id};}};
// Optional GPU evidence is disabled in this CPU clear/scissor model.
void probe_surface_pixels(Surface*, const char*){}
namespace rt{void log(const char*,...){}}
struct Device{
 uint64_t list_seen_=1;unsigned dirty_=0;static constexpr unsigned kTargets=1,kAll=~0u,kViewport=2;
 D3DVIEWPORT9 viewport_{0,0,640,480,0,1};DWORD rs_[256]{};RECT scissor_{};
 Surface target;Surface*targets_[4]{&target};Surface*depth_=nullptr;
 unsigned mrt_probe_frame_=0,present_count_=0;
 void flush_targets(){};void probe_mrt(const char*,bool){};void sync(const char*,...){}
 HRESULT Clear(DWORD,const D3DRECT*,DWORD,D3DCOLOR,float,DWORD);
};
'''
main=r'''
int main(){Device d;assert(d.Clear(0,nullptr,D3DCLEAR_TARGET,0xffffffff,1,0)==0);
 d.rs_[D3DRS_SCISSORTESTENABLE]=TRUE;d.scissor_={160,120,480,360};
 d.Clear(0,nullptr,D3DCLEAR_TARGET,0xff00ff00,1,0);
 assert(pixels[118*640+158]==0xffffffff&&pixels[122*640+162]==0xff00ff00);
 D3DRECT rect{128,128,320,240};d.Clear(1,&rect,D3DCLEAR_TARGET,0xffff0000,1,0);
 assert(pixels[130*640+158]==0xffffffff&&pixels[130*640+162]==0xffff0000&&pixels[350*640+162]==0xff00ff00);
 auto untouched=pixels;D3DRECT ignored{0,0,640,480};d.Clear(0,&ignored,D3DCLEAR_TARGET,0xff0000ff,1,0);assert(pixels==untouched);
 auto previous=pixels;auto count=writes;d.viewport_={480,360,160,120,0,1};d.scissor_={0,0,160,120};
 d.Clear(0,nullptr,D3DCLEAR_TARGET,0xff0000ff,1,0);assert(writes==count&&pixels==previous);
 d.rs_[D3DRS_SCISSORTESTENABLE]=FALSE;d.Clear(0,nullptr,D3DCLEAR_TARGET,0xff0000ff,1,0);
 assert(pixels[400*640+500]==0xff0000ff&&pixels[130*640+162]==0xffff0000);
 // A small MRT must not clip the larger target or the independent depth clear.
 Surface small,depth;small.id=1;small.width=200;small.height=100;depth.id=2;
 d.targets_[1]=&small;d.depth_=&depth;d.viewport_={0,0,640,480,0,1};clears.clear();
 d.Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0);
 assert(clears.size()==3&&clears[0].image==0&&clears[0].scissor.w==640&&clears[0].scissor.h==480);
 assert(clears[1].image==1&&clears[1].scissor.w==200&&clears[1].scissor.h==100);
 assert(clears[2].image==2&&clears[2].depth&&clears[2].scissor.w==640&&clears[2].scissor.h==480);
 assert((d.dirty_&Device::kTargets)!=0);
}
'''
with tempfile.TemporaryDirectory(prefix='wine-clear-regions-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+method('void argb_to_float(')+method('HRESULT Device::Clear(')+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual Clear command path: viewport/scissor/explicit-rectangle intersections, empty intersections and disabled scissor passed (CPU command model; ASAN/UBSAN)')
