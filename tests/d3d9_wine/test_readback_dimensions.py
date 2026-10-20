#!/usr/bin/env python3
"""Actual readback rejects incompatible surfaces before native DMA."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2];s=(root/'shims/d3d9/deko/device.cpp').read_text();start=s.index('HRESULT Device::GetRenderTargetData(');opening=s.index('{',start);end=opening+1;depth=1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <cassert>
struct ApiLock{explicit ApiLock(const char*){}};
struct Alloc{bool valid=true;DkGpuAddr gpu()const{return 4096;}explicit operator bool()const{return valid;}};
struct Surface{unsigned width=64,height=64,cpu_pitch=256;D3DFORMAT d3dfmt=D3DFMT_A8R8G8B8;D3DMULTISAMPLE_TYPE multisample=D3DMULTISAMPLE_NONE;Alloc cpu;bool has_image=true;int*image(){static int i;return has_image?&i:nullptr;}DkImageView view(){return {};}};
#define IDirect3DSurface9 Surface
unsigned copies=0,waits=0,barriers=0;DkImageRect recorded{};
extern "C" void dkCmdBufBarrier(DkCmdBuf,DkBarrier,uint32_t){++barriers;}
extern "C" void dkCmdBufCopyImageToBuffer(DkCmdBuf,const DkImageView*,const DkImageRect*r,const DkCopyBuf*,uint32_t){++copies;recorded=*r;}
struct Gpu{DkCmdBuf cmd=nullptr;void flush(bool wait){assert(wait);++waits;}};Gpu&gpu(){static Gpu g;return g;}
struct Device{HRESULT GetRenderTargetData(IDirect3DSurface9*,IDirect3DSurface9*);};
'''
main=r'''
int main(){
 Device d;Surface source,target;
 auto reject=[&]{unsigned n=copies,b=barriers,w=waits;assert(d.GetRenderTargetData(&source,&target)==D3DERR_INVALIDCALL);assert(copies==n&&barriers==b&&waits==w);};
 target.width=32;reject();target.width=128;reject();target.width=64;
 target.height=32;reject();target.height=64;
 target.d3dfmt=D3DFMT_R32F;reject();target.d3dfmt=D3DFMT_A8R8G8B8;
 source.d3dfmt=D3DFMT_A32B32G32R32F;reject();source.d3dfmt=D3DFMT_A8R8G8B8;
 assert(d.GetRenderTargetData(&source,&target)==D3D_OK&&copies==1&&waits==1);
 assert(recorded.width==64&&recorded.height==64&&recorded.depth==1);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-readback-extent-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+s[start:end]+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I/opt/devkitpro/libnx/include','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual readback: format/extent mismatches rejected before DMA; valid copy and wait preserved (ASAN/UBSAN).')
