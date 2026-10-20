#!/usr/bin/env python3
"""Exercise actual alpha-state emission for original and extended shader packs."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'shims/d3d9/deko/device.cpp').read_text()
start=s.index('void Device::flush_blend_state()');opening=s.index('{',start);end=opening+1;depth=1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <cassert>
#include <cmath>
#include <cstdint>
struct ApiLock{explicit ApiLock(const char*){}};
struct Gpu{DkCmdBuf cmd=nullptr;};Gpu&gpu(){static Gpu g;return g;}
DkCompareOp compare;float reference;
extern "C" void dkCmdBufBindColorState(DkCmdBuf,const DkColorState*s){compare=s->alphaCompareOp;}
extern "C" void dkCmdBufSetAlphaRef(DkCmdBuf,float v){reference=v;}
extern "C" void dkCmdBufBindColorWriteState(DkCmdBuf,const DkColorWriteState*){}
extern "C" void dkCmdBufBindBlendStates(DkCmdBuf,uint32_t,const DkBlendState*,uint32_t){}
extern "C" void dkCmdBufSetBlendConst(DkCmdBuf,float,float,float,float){}
DkBlendFactor blend_factor(DWORD){return DkBlendFactor_One;}
void argb_to_float(DWORD,float*){}
struct Meta{bool fixed_function=false,has_draw_state=false;uint32_t color_outputs=1;};
struct Device{
 DWORD rs_[256]{};struct{Meta*meta=nullptr;}draw_ps_;bool atoc=false;
 bool alpha_to_coverage()const{return atoc;}void flush_blend_state();
};
'''
main=r'''
int main(){
 Device d;Meta original,extended,ffp;extended.has_draw_state=true;ffp.fixed_function=true;
 for(auto*m:{&original,&extended,&ffp})for(unsigned enabled=0;enabled<2;++enabled)
 for(unsigned coverage=0;coverage<2;++coverage)for(unsigned op=1;op<=8;++op){
  d.draw_ps_.meta=m;d.rs_[D3DRS_ALPHATESTENABLE]=enabled;d.rs_[D3DRS_ALPHAFUNC]=op;d.atoc=coverage;
  for(unsigned ref:{0u,1u,127u,255u}){
   d.rs_[D3DRS_ALPHAREF]=ref;d.flush_blend_state();
   assert(compare==((enabled&&!coverage&&m==&original)?DkCompareOp(op):DkCompareOp_Always));
   if(enabled)assert(std::abs(reference-float(ref)/255.f)<1e-7f);
  }
 }
}
'''
with tempfile.TemporaryDirectory(prefix='deko-alpha-packs-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+s[start:end]+main)
 native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-include','initializer_list','-fsanitize=address,undefined',
 '-I/opt/devkitpro/libnx/include','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual alpha-state emission: original pack uses hardware tests; extended/FFP and alpha-to-coverage avoid duplicate tests (ASAN/UBSAN).')
