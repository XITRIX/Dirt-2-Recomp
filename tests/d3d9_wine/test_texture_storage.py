#!/usr/bin/env python3
"""Run real texture lock methods against a memory/copy-command model, under sanitizers."""
from pathlib import Path
import tempfile, subprocess, os
root=Path(__file__).resolve().parents[2]
source=(root/'shims/d3d9/deko/resources.cpp').read_text()
def method(signature):
    start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <cstdint>
struct ApiLock{explicit ApiLock(const char*){}};
struct Alloc{std::shared_ptr<std::vector<uint8_t>>data;int block=0;size_t offset=0,size=0;
 uint8_t*cpu()const{return data?data->data():nullptr;}explicit operator bool()const{return bool(data);}};
struct Pool{Alloc alloc(size_t n,unsigned){return {std::make_shared<std::vector<uint8_t>>(n),0,0,n};}void free_now(const Alloc&){};};
struct Gpu{Pool readback;struct Frame{Pool transient;}frame;Frame&cur(){return frame;}};Gpu&gpu(){static Gpu g;return g;}
constexpr D3DFORMAT fourcc(char a,char b,char c,char d){return D3DFORMAT(uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24));}
struct FormatInfo{unsigned block=1,bytes=4,block_height=1;};
struct GpuImage{unsigned width=8,height=8,depth=1,layers=1,levels=1,expanded_bc=0;bool mem=true;std::vector<uint8_t>pixels;};
struct Device{std::vector<Alloc>uploads;unsigned reads=0;
 void read_image(GpuImage&img,unsigned,unsigned,const Alloc&out,unsigned,const RECT&,unsigned=1){++reads;assert(out.size<=img.pixels.size());memcpy(out.cpu(),img.pixels.data(),out.size);}
 void upload(GpuImage&img,unsigned,unsigned level,const Alloc&in,unsigned,const RECT&,unsigned=0,unsigned=1,unsigned=0){uploads.push_back(in);if(!level)img.pixels.assign(in.cpu(),in.cpu()+in.size);}
};
struct Surface;
'''
h=(root/'shims/d3d9/deko/d3d9_deko.h').read_text();core=h[h.index('struct TextureCore {'):h.index('// Resolve backend-owned')]
main=r'''
int main(){
 // Native test_mipmap_upload writes the entire chain through a level-0 lock.
 Device chain_dev;FormatInfo chain_format;TextureCore chain;chain.dev=&chain_dev;chain.fmt=&chain_format;
 chain.pool=D3DPOOL_MANAGED;chain.img.width=32;chain.img.height=32;chain.img.levels=6;
 D3DLOCKED_RECT chain_lock;assert(chain.lock(0,0,&chain_lock,nullptr,0)==0);auto*cursor=(uint8_t*)chain_lock.pBits;
 for(unsigned level=0;level<6;++level){unsigned w=32>>level;memset(cursor,0x11*(level+1),w*w*4);cursor+=w*w*4;}
 assert(chain.unlock(0,0)==0&&chain_dev.uploads.size()==6);
 for(unsigned level=0;level<6;++level){assert(chain_dev.uploads[level].cpu()[0]==0x11*(level+1));assert(chain.lock(0,level,&chain_lock,nullptr,D3DLOCK_READONLY)==0);assert(*(uint8_t*)chain_lock.pBits==0x11*(level+1));chain.unlock(0,level);}
 Device dev;FormatInfo rgba;TextureCore t;t.dev=&dev;t.fmt=&rgba;t.pool=D3DPOOL_MANAGED;t.img.levels=2;
 D3DLOCKED_RECT lock;assert(t.lock(0,0,&lock,nullptr,0)==0);auto*base=(uint8_t*)lock.pBits;
 assert(lock.Pitch==32);memset(base,0x55,256);assert(t.lock(0,0,&lock,nullptr,0)==D3DERR_INVALIDCALL);
 assert(t.unlock(0,0)==0);assert(dev.uploads.size()==2&&dev.uploads[0].cpu()[40]==0x55);
 RECT r{2,2,4,4};assert(t.lock(0,0,&lock,&r,0)==0);assert(lock.Pitch==32&&(uint8_t*)lock.pBits-base==72);
 assert(*(uint8_t*)lock.pBits==0x55);*(uint8_t*)lock.pBits=0x99;
 assert(dev.uploads[0].cpu()[72]==0x55);assert(t.unlock(0,0)==0);
 assert(dev.uploads[2].cpu()[72]==0x99&&dev.uploads[2].cpu()[0]==0x55);
 auto count=dev.uploads.size();assert(t.lock(0,0,&lock,nullptr,D3DLOCK_READONLY)==0);assert(t.unlock(0,0)==0&&dev.uploads.size()==count);
 assert(t.lock(0,1,&lock,nullptr,0)==0&&lock.Pitch==16);assert(t.unlock(0,1)==0);
 t.pool=D3DPOOL_DEFAULT;RECT invalid{0,0,9,8};assert(t.lock(0,0,&lock,&invalid,0)==D3DERR_INVALIDCALL);assert(t.lock(1,0,&lock,nullptr,0)==D3DERR_INVALIDCALL);
 t.pool=D3DPOOL_DEFAULT;t.img.pixels.assign(256,0x33);assert(t.lock(0,0,&lock,nullptr,0)==0&&dev.reads==1&&*(uint8_t*)lock.pBits==0x33);assert(t.unlock(0,0)==0);
 assert(t.lock(0,0,&lock,nullptr,D3DLOCK_DISCARD|D3DLOCK_READONLY)==D3DERR_INVALIDCALL);
 TextureCore v;v.dev=&dev;v.fmt=&rgba;v.pool=D3DPOOL_SCRATCH;v.img.width=4;v.img.height=4;v.img.depth=2;
 D3DLOCKED_BOX box;assert(v.lock_box(0,&box,nullptr,0)==0&&box.RowPitch==16&&box.SlicePitch==64);auto*volume=(uint8_t*)box.pBits;
 memset(volume,0x77,128);assert(v.unlock_box(0)==0);D3DBOX region{2,2,4,4,1,2};assert(v.lock_box(0,&box,&region,0)==0);
 assert((uint8_t*)box.pBits-volume==104&&box.RowPitch==16&&*(uint8_t*)box.pBits==0x77);
 assert(v.lock_box(0,&box,nullptr,0)==D3DERR_INVALIDCALL);assert(v.unlock_box(0)==0);assert(v.unlock_box(0)==D3DERR_INVALIDCALL);
 region.Right=5;assert(v.lock_box(0,&box,&region,0)==D3DERR_INVALIDCALL);
 v.pool=D3DPOOL_DEFAULT;assert(v.lock_box(0,&box,nullptr,0)==D3DERR_INVALIDCALL&&box.pBits==nullptr);
 v.usage=D3DUSAGE_DYNAMIC;v.img.pixels.assign(128,0x88);assert(v.lock_box(0,&box,nullptr,0)==0&&*(uint8_t*)box.pBits==0x88);assert(v.unlock_box(0)==0);
 // CPU-pool invalid rectangles succeed without submitting unsafe GPU regions.
 t.pool=D3DPOOL_SYSTEMMEM;RECT inverted{4,4,2,2};assert(t.lock(0,0,&lock,&inverted,0)==0);assert(t.unlock(0,0)==0);
 FormatInfo bc{4,8,4};v.fmt=&bc;v.pool=D3DPOOL_SCRATCH;region={1,0,4,4,0,1};assert(v.lock_box(0,&box,&region,0)==D3DERR_INVALIDCALL);
 v.cpu_layout.clear();v.cpu_storage.clear();v.d3dfmt=fourcc('A','T','I','1');v.img.width=24;v.img.height=8;v.img.depth=8;
 assert(v.lock_box(0,&box,nullptr,0)==0&&box.RowPitch==24&&box.SlicePitch==192);auto*ati=(uint8_t*)box.pBits;v.unlock_box(0);
 region={1,0,4,4,0,1};assert(v.lock_box(0,&box,&region,0)==D3DERR_INVALIDCALL);
 region={4,4,8,8,3,8};assert(v.lock_box(0,&box,&region,0)==0&&(uint8_t*)box.pBits-ati==3*192+4*24+4);v.unlock_box(0);
 // Packed YUV stores a pair horizontally and one row vertically.
 FormatInfo yuv{2,4,1};TextureCore y;y.dev=&dev;y.fmt=&yuv;y.pool=D3DPOOL_SCRATCH;y.img.width=4;y.img.height=4;y.img.depth=2;
 assert(y.lock_box(0,&box,nullptr,0)==0&&box.RowPitch==8&&box.SlicePitch==32);auto*ybase=(uint8_t*)box.pBits;y.unlock_box(0);
 region={2,1,4,3,1,2};assert(y.lock_box(0,&box,&region,0)==0&&(uint8_t*)box.pBits-ybase==44);y.unlock_box(0);
}
'''
with tempfile.TemporaryDirectory(prefix='wine-texture-storage-') as directory:
    p=Path(directory);(p/'test.cpp').write_text(header+core+method('uint32_t mip(')+method('uint32_t row_pitch(')+method('uint32_t level_rows(')+''.join(method(x) for x in ['void TextureCore::ensure_cpu_storage(','HRESULT TextureCore::upload_cpu_level(','HRESULT TextureCore::lock(','HRESULT TextureCore::unlock(','HRESULT TextureCore::lock_box(','HRESULT TextureCore::unlock_box('])+main)
    native=root/'build/src/dxvk/include/native'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Actual texture/volume locks: persistent contents, full pitches/offsets, immutable upload snapshots, GPU readback, readonly, nested/invalid locks passed (CPU copy model; ASAN/UBSAN)')
