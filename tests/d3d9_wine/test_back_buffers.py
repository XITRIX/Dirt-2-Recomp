#!/usr/bin/env python3
"""Exercise actual flip storage rotation and stable COM back-buffer lookup."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'shims/d3d9/deko/device.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <vector>
#include <algorithm>
#include <cassert>
struct ApiLock{explicit ApiLock(const char*){}};
struct Surface {int own;unsigned references=1;void AddRef(){++references;}};
struct Device {
 D3DPRESENT_PARAMETERS pp_{};Surface*back_buffer_=nullptr;std::vector<Surface*>extra_back_buffers_;
 void advance_back_buffers();HRESULT GetBackBuffer(UINT,UINT,D3DBACKBUFFER_TYPE,IDirect3DSurface9**);
};
'''
main=r'''
int main(){
 Surface red{1},green{2},blue{3};Device d;d.back_buffer_=&red;d.extra_back_buffers_={&green,&blue};d.pp_.SwapEffect=D3DSWAPEFFECT_DISCARD;
 IDirect3DSurface9*out=nullptr;
 for(unsigned i=0;i<3;++i){assert(d.GetBackBuffer(0,i,D3DBACKBUFFER_TYPE_MONO,&out)==0);assert(out==reinterpret_cast<IDirect3DSurface9*>(i==0?&red:i==1?&green:&blue));}
 d.advance_back_buffers();assert(red.own==2&&green.own==3&&blue.own==1);
 assert(d.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&out)==0&&out==reinterpret_cast<IDirect3DSurface9*>(&red));
 d.advance_back_buffers();assert(red.own==3&&green.own==1&&blue.own==2);
 d.pp_.SwapEffect=D3DSWAPEFFECT_COPY;d.advance_back_buffers();assert(red.own==3&&green.own==1&&blue.own==2);
 assert(d.GetBackBuffer(0,3,D3DBACKBUFFER_TYPE_MONO,&out)==D3DERR_INVALIDCALL&&!out);
 assert(d.GetBackBuffer(1,0,D3DBACKBUFFER_TYPE_MONO,&out)==D3DERR_INVALIDCALL&&!out);
 assert(d.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_LEFT,&out)==D3DERR_INVALIDCALL&&!out);
 assert(d.GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,nullptr)==D3DERR_INVALIDCALL);
}
'''
# Only the final COM pointer conversion differs in the small CPU fixture.
lookup=method('HRESULT Device::GetBackBuffer(').replace('*out = buffer;', '*out = reinterpret_cast<IDirect3DSurface9*>(buffer);')
with tempfile.TemporaryDirectory(prefix='deko-back-buffers-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+lookup+method('void Device::advance_back_buffers(')+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual back-buffer lookup/flip: stable identities, rotating storage, COPY preservation and invalid index/type rejection passed (ASAN/UBSAN)')
