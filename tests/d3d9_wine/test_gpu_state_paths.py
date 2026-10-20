#!/usr/bin/env python3
"""Exercise actual resource synchronization and image command paths with a CPU recorder."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2];backend=root/'shims/d3d9/deko'
def method(file,signature):
 s=(backend/file).read_text();start=s.index(signature);opening=s.index('{',start);end=opening+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <algorithm>
#include <array>
#include <vector>
#include <memory>
#include <cstring>
#include <cassert>
#include <cstdint>
struct Alloc {std::shared_ptr<std::vector<uint8_t>> data;uint32_t size=0;uint8_t*cpu()const{return data->data();}explicit operator bool()const{return bool(data);} };
struct Pool{Alloc alloc(uint32_t n,unsigned){return{std::make_shared<std::vector<uint8_t>>(n),n};}};
constexpr unsigned kFrames=2;
struct Gpu{Pool buffers;uint64_t frame_count=3;const void*state_owner=nullptr;DkCmdBuf cmd=nullptr;unsigned waits=0;void flush(bool wait,const char*){assert(wait);++waits;}};
Gpu&gpu(){static Gpu g;return g;}
namespace rt{void log(const char*,...){}}
struct BufferStore{struct Version{Alloc mem;uint64_t last_used;};std::vector<Version>versions;int current=0;uint32_t size=0;bool init(uint32_t);uint8_t*lock(uint32_t,DWORD);};
struct Copy{unsigned from,to,layer;DkImageRect src,dst;uint32_t flags;};std::vector<Copy>copies;unsigned barriers=0;
extern "C" void dkCmdBufBarrier(DkCmdBuf,DkBarrier,uint32_t){++barriers;}
extern "C" void dkCmdBufBlitImage(DkCmdBuf,const DkImageView*s,const DkImageRect*sr,const DkImageView*d,const DkImageRect*dr,uint32_t flags,uint32_t){copies.push_back({s->mipLevelOffset,d->mipLevelOffset,s->layerOffset,*sr,*dr,flags});}
extern "C" void dkCmdBufCopyImage(DkCmdBuf c,const DkImageView*s,const DkImageRect*sr,const DkImageView*d,const DkImageRect*dr,uint32_t flags){dkCmdBufBlitImage(c,s,sr,d,dr,flags,0);}
uint32_t mip(uint32_t n,unsigned l){return std::max(n>>l,1u);}
struct GpuImage{DkImage image{};bool mem=false;DkImageType type=DkImageType_2D;DkImageFormat format=DkImageFormat_RGBA8_Unorm;DkMsMode ms_mode=DkMsMode_1x;uint32_t width=8,height=8,depth=1,levels=1,layers=1;DkImageSwizzle swizzle[4]{};unsigned allocations=0;
 DkImageView view(unsigned lv=0,unsigned ly=0){DkImageView v{};v.pImage=&image;v.mipLevelOffset=lv;v.layerOffset=ly;return v;}
 bool create(DkImageType t,DkImageFormat f,uint32_t w,uint32_t h,uint32_t d,uint32_t l,uint32_t a,uint32_t){type=t;format=f;width=w;height=h;depth=d;levels=l;layers=a;mem=true;++allocations;return true;}
 void destroy(){mem=false;}
};
struct TextureCore{GpuImage img;DWORD usage=0;bool autogen_dirty=true;D3DTEXTUREFILTERTYPE autogen_filter=D3DTEXF_LINEAR;HRESULT set_autogen_filter(D3DTEXTUREFILTERTYPE);void generate_mips();};
struct Surface{TextureCore*tex=nullptr;GpuImage own;GpuImage*image(){return tex?&tex->img:&own;}};
TextureCore*core_of(IDirect3DBaseTexture9*p){return reinterpret_cast<TextureCore*>(p);}
struct PackMeta{struct Sampler{unsigned reg;};std::vector<Sampler>samplers;};
struct Device{struct DrawShader{PackMeta*meta=nullptr;};DrawShader draw_ps_,draw_vs_;uint32_t active_texture_mask()const;static constexpr unsigned kSamplers=20,kTextures=1;unsigned dirty_=0;IDirect3DBaseTexture9*textures_[20]{};Surface*targets_[4]{},*depth_=nullptr;GpuImage feedback_images_[20];TextureCore*feedback_sources_[20]{};DWORD rs_[256]{};bool prepare_feedback(uint32_t);bool alpha_to_coverage()const;};
'''
main=r'''
int main(){
 BufferStore b;assert(b.init(64));auto*first=b.lock(0,0);first[7]=42;b.versions[0].last_used=gpu().frame_count+1;
 assert(b.lock(0,0)==first&&gpu().waits==1&&first[7]==42);b.versions[0].last_used=gpu().frame_count+1;
 assert(b.lock(4,D3DLOCK_NOOVERWRITE)==first+4&&gpu().waits==1);
 assert(b.lock(4,D3DLOCK_NOOVERWRITE|D3DLOCK_DISCARD)==first+4&&b.versions.size()==1&&gpu().waits==1);
 auto*discard=b.lock(0,D3DLOCK_DISCARD);assert(discard&&discard!=first&&b.versions.size()==2&&gpu().waits==1);
 TextureCore t;t.img.mem=true;t.img.width=16;t.img.height=8;t.img.levels=5;t.img.layers=6;t.usage=D3DUSAGE_AUTOGENMIPMAP;
 assert(t.set_autogen_filter(D3DTEXF_NONE)==D3DERR_INVALIDCALL);assert(t.set_autogen_filter(D3DTEXF_LINEAR)==0);
 t.generate_mips();assert(copies.size()==24&&!t.autogen_dirty&&gpu().state_owner==nullptr);
 assert(copies[0].from==0&&copies[0].to==1&&copies[0].src.width==16&&copies[0].dst.width==8&&copies[0].dst.height==4);
 assert(copies[23].layer==5&&copies[23].from==3&&copies[23].to==4&&copies[23].dst.height==1&&copies[23].flags==DkBlitFlag_FilterLinear);
 t.generate_mips();assert(copies.size()==24);t.set_autogen_filter(D3DTEXF_POINT);t.generate_mips();assert(copies.size()==48&&copies.back().flags==0);
 copies.clear();Device d;
 PackMeta ps,vs;ps.samplers={{0},{15},{16}};vs.samplers={{0},{3},{4}};
 d.draw_ps_.meta=&ps;d.draw_vs_.meta=&vs;
 assert(d.active_texture_mask()==((1u<<0)|(1u<<15)|(1u<<16)|(1u<<19)));
 d.draw_ps_.meta=nullptr;assert(d.active_texture_mask()==((1u<<16)|(1u<<19)));
 d.draw_vs_.meta=nullptr;assert(d.active_texture_mask()==0);
 Surface surface;surface.tex=&t;d.targets_[0]=&surface;d.textures_[0]=reinterpret_cast<IDirect3DBaseTexture9*>(&t);
 assert(d.prepare_feedback(1)&&copies.size()==30&&d.feedback_sources_[0]==&t&&d.feedback_images_[0].mem);auto allocations=d.feedback_images_[0].allocations;
 // Bound but unused attachment: no copy, and invalidate an old snapshot binding.
 d.dirty_=0;assert(d.prepare_feedback(0)&&copies.size()==30&&!d.feedback_sources_[0]&&(d.dirty_&Device::kTextures));
 d.dirty_=0;assert(d.prepare_feedback(0)&&copies.size()==30&&!d.dirty_);
 assert(d.prepare_feedback(1)&&copies.size()==60&&d.feedback_images_[0].allocations==allocations);
 d.targets_[0]=nullptr;assert(d.prepare_feedback(1)&&copies.size()==60&&!d.feedback_sources_[0]);
 d.depth_=&surface;assert(d.prepare_feedback(1)&&copies.size()==90);t.img.width=32;assert(d.prepare_feedback(1)&&d.feedback_images_[0].allocations==allocations+1);
 d.targets_[0]=&surface;d.rs_[D3DRS_ADAPTIVETESS_Y]=0x434f5441;d.rs_[D3DRS_ALPHATESTENABLE]=1;
 assert(!d.alpha_to_coverage());t.img.ms_mode=DkMsMode_2x;assert(d.alpha_to_coverage());d.rs_[D3DRS_ALPHATESTENABLE]=0;assert(!d.alpha_to_coverage());
 assert(multisample_valid(D3DMULTISAMPLE_NONE,0)&&multisample_valid(D3DMULTISAMPLE_8_SAMPLES,0));assert(!multisample_valid(D3DMULTISAMPLE_3_SAMPLES,0));
 assert(multisample_valid(D3DMULTISAMPLE_NONMASKABLE,2)&&!multisample_valid(D3DMULTISAMPLE_NONMASKABLE,3));assert(!multisample_valid(D3DMULTISAMPLE_2_SAMPLES,1));assert(multisample_mode(D3DMULTISAMPLE_NONMASKABLE,1)==DkMsMode_4x);
}
'''
methods='\n'.join(method(f,s) for f,s in [('resources.cpp','bool BufferStore::init('),('resources.cpp','uint8_t* BufferStore::lock('),('resources.cpp','HRESULT TextureCore::set_autogen_filter('),('resources.cpp','void TextureCore::generate_mips('),('device.cpp','uint32_t Device::active_texture_mask('),('device.cpp','bool Device::prepare_feedback('),('device.cpp','bool Device::alpha_to_coverage('),('resources.cpp','DkMsMode multisample_mode('),('resources.cpp','bool multisample_valid(')])
with tempfile.TemporaryDirectory(prefix='wine-gpu-state-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+methods+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I/opt/devkitpro/libnx/include','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual buffer waits/renames, mip generation commands, attachment snapshots, ATOC gating and MSAA validation passed (CPU command recorder; ASAN/UBSAN)')
