#!/usr/bin/env python3
"""Exercise alpha state invalidation across target and ATOC changes without flushes."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'shims/d3d9/deko/device.cpp').read_text()
def method(signature):
 start=s.index(signature);opening=s.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <cassert>
#include <initializer_list>
#include <cstdint>
struct ApiLock{explicit ApiLock(const char*){}};
struct Gpu{DkCmdBuf cmd=nullptr;};Gpu&gpu(){static Gpu g;return g;}
DkCompareOp compare=DkCompareOp_Always;
extern "C" void dkCmdBufBindColorState(DkCmdBuf,const DkColorState*s){compare=s->alphaCompareOp;}
extern "C" void dkCmdBufSetAlphaRef(DkCmdBuf,float){}
extern "C" void dkCmdBufBindColorWriteState(DkCmdBuf,const DkColorWriteState*){}
extern "C" void dkCmdBufBindBlendStates(DkCmdBuf,uint32_t,const DkBlendState*,uint32_t){}
extern "C" void dkCmdBufSetBlendConst(DkCmdBuf,float,float,float,float){}
DkBlendFactor blend_factor(DWORD){return DkBlendFactor_One;}
void argb_to_float(DWORD,float*){}
struct StateBlock{enum{Render};template<class...T>HRESULT record(T...){assert(false);return 0;}};
struct Image{DkMsMode ms_mode=DkMsMode_1x;};
struct Surface{void*device=nullptr;DWORD usage=D3DUSAGE_RENDERTARGET;unsigned width=64,height=64;Image img;
 void*owner()const{return device;}Image*image(){return &img;}};
// COM lifetime is independent of these state-emission assertions.
#define IDirect3DSurface9 Surface
template<class T>void set_ref(T*&dst,T*src){dst=src;}
struct Meta{bool fixed_function=false,has_draw_state=false;uint32_t color_outputs=1;};
struct Device{
 enum{ kTargets=1,kBlend=2,kRaster=4,kDepthStencil=8,kViewport=16 };
 DWORD rs_[256]{};unsigned dirty_=0;bool ffp_dirty_=false,copy_barrier_=false;
 unsigned mrt_probe_frame_=0,present_count_=0;StateBlock*recording_=nullptr;
 Surface*targets_[4]{};D3DVIEWPORT9 viewport_{};RECT scissor_{};
 struct{Meta*meta=nullptr;}draw_ps_;
 void resolve_depth_command(){}void probe_mrt(const char*,bool){}
 bool alpha_to_coverage()const;void flush_blend_state();
 HRESULT SetRenderTarget(DWORD,IDirect3DSurface9*);HRESULT SetRenderState(D3DRENDERSTATETYPE,DWORD);
 void emit_changed(){if(dirty_&kBlend)flush_blend_state();dirty_=0;}
};
'''
main=r'''
int main(){
 Device d;Meta original;d.draw_ps_.meta=&original;
 Surface single,multi;single.device=multi.device=&d;multi.img.ms_mode=DkMsMode_2x;
 d.SetRenderTarget(0,&single);d.SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);
 d.SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);d.SetRenderState(D3DRS_ALPHAREF,127);
 d.SetRenderState(D3DRS_ADAPTIVETESS_Y,0x434f5441);d.emit_changed();assert(compare==DkCompareOp_Greater);
 // No shader, alpha test or alpha reference changes between these draws.
 for(unsigned i=0;i<100;++i){
  d.SetRenderTarget(0,&multi);d.emit_changed();assert(compare==DkCompareOp_Always);
  d.SetRenderTarget(0,&single);d.emit_changed();assert(compare==DkCompareOp_Greater);
 }
 d.SetRenderTarget(0,&multi);d.emit_changed();assert(compare==DkCompareOp_Always);
 for(unsigned i=0;i<100;++i){
  d.SetRenderState(D3DRS_ADAPTIVETESS_Y,0);d.emit_changed();assert(compare==DkCompareOp_Greater);
  d.SetRenderState(D3DRS_ADAPTIVETESS_Y,0x434f5441);d.emit_changed();assert(compare==DkCompareOp_Always);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='deko-alpha-transitions-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+'\n'.join(method(x) for x in [
 'HRESULT Device::SetRenderTarget(', 'HRESULT Device::SetRenderState(',
 'bool Device::alpha_to_coverage(', 'void Device::flush_blend_state('])+main)
 native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',
 '-I/opt/devkitpro/libnx/include','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual setters and alpha emission: MSAA target and ATOC transitions restore native alpha testing without per-draw flushes (ASAN/UBSAN).')
