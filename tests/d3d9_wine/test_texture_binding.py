#!/usr/bin/env python3
"""Exercise real registry/binding methods with deliberately replaced COM vtables."""
from pathlib import Path
import tempfile,subprocess,os
from cpu_stateblock_support import declarations
root=Path(__file__).resolve().parents[2]
r=(root/'shims/d3d9/deko/resources.cpp').read_text();d=(root/'shims/d3d9/deko/device.cpp').read_text()
registry=r[r.index('// Binding uses implementation'):r.index('// ---- Formats ----')]
def method(signature):
 start=d.index(signature);opening=d.index('{',start);depth=1;end=opening+1
 while depth:
  depth+=(d[end]=='{')-(d[end]=='}');end+=1
 return d[start:end]
header=r'''
#include <unordered_map>
#include <type_traits>
#include <cstdint>
#include <cstring>
#include <cassert>
using DWORD=uint32_t;using UINT=uint32_t;using HRESULT=int;
constexpr int D3D_OK=0,D3DERR_INVALIDCALL=-1,kTextures=1,kSamplers=20;
constexpr DWORD D3DVERTEXTEXTURESAMPLER0=256;
struct ApiLock{explicit ApiLock(const char*){}};
struct Device;
struct TextureCore{Device*dev=nullptr;};
struct IDirect3DBaseTexture9{unsigned refs=1,private_refs=0;void AddRefPrivate(){++private_refs;}void ReleasePrivate(){if(!--private_refs&&!refs)delete this;}virtual ~IDirect3DBaseTexture9()=default;
 virtual unsigned AddRef(){return ++refs;}virtual unsigned Release(){auto n=--refs;if(!n&&!private_refs)delete this;return n;}
};
namespace {void unregister_texture(IDirect3DBaseTexture9*);}
struct Texture:IDirect3DBaseTexture9{TextureCore core;~Texture(){unregister_texture(this);}};
struct CubeTexture:IDirect3DBaseTexture9{TextureCore core;~CubeTexture(){unregister_texture(this);}};
struct VolumeTexture:IDirect3DBaseTexture9{TextureCore core;~VolumeTexture(){unregister_texture(this);}};
struct Device{IDirect3DBaseTexture9*textures_[kSamplers]{};unsigned dirty_=0;
 template<class T>void set_ref(T*&,T*);
 HRESULT SetTexture(DWORD,IDirect3DBaseTexture9*);
 HRESULT GetTexture(DWORD,IDirect3DBaseTexture9**);
};
'''
methods='\n'.join(method(x) for x in ['int sampler_index(', 'template <typename T>\nvoid Device::set_ref(', 'HRESULT Device::SetTexture(', 'HRESULT Device::GetTexture('])
main=r'''
template<class T>void test(){
 Device device,other;auto*t=new T;t->core.dev=&device;register_texture(t);
 assert(texture_core(t)==&t->core);
 assert(other.SetTexture(0,t)==D3DERR_INVALIDCALL);
 void* original;std::memcpy(&original,t,sizeof original);
 for(void* replacement:{reinterpret_cast<void*>(0xdeadbeef),static_cast<void*>(nullptr)}){
  std::memcpy(t,&replacement,sizeof replacement);
  assert(device.SetTexture(0,t)==0);assert(t->refs==1&&t->private_refs==1);
  assert(device.SetTexture(0,t)==0);assert(t->refs==1&&t->private_refs==1);
  IDirect3DBaseTexture9*out=nullptr;assert(device.GetTexture(0,&out)==0&&out==t);assert(t->refs==2);
  texture_release(out);assert(t->refs==1&&t->private_refs==1);
  assert(device.SetTexture(0,nullptr)==0);assert(t->refs==1);
 }
 std::memcpy(t,&original,sizeof original);
 assert(device.SetTexture(0,t)==0);texture_release(t);assert(t->refs==0&&t->private_refs==1);
 assert(device.SetTexture(0,nullptr)==0);assert(texture_registry().empty());
}
int main(){test<Texture>();test<CubeTexture>();test<VolumeTexture>();
 Device d;IDirect3DBaseTexture9 foreign;assert(!texture_core(&foreign));assert(d.SetTexture(0,&foreign)==-1);
 assert(d.SetTexture(99,nullptr)==-1);assert(d.GetTexture(0,nullptr)==-1);
}
'''
with tempfile.TemporaryDirectory(prefix='wine-texture-binding-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header.replace('struct Device;',declarations()+'struct Device;')+registry+methods+main)
 # Public COM vtable mutation is deliberate in the upstream compatibility test.
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-fno-sanitize=vptr','-Wno-dynamic-class-memaccess',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual texture registry/binding: 2D/cube/volume, corrupt/null public vtables, get/rebind/unbind, final destruction, foreign/device/stage rejection passed (ASAN/UBSAN; deliberate vptr replacement excluded)')
