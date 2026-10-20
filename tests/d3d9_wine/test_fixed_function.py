#!/usr/bin/env python3
"""Check real FFP state packing against the compiled GLSL ABI and native math."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[2];backend=root/'shims/d3d9/deko';source=(backend/'fixed_function.cpp').read_text()
helpers=source[source.index('namespace {')+len('namespace {'):source.index('\nFixedFunction::~FixedFunction()')]
assert helpers.rstrip().endswith('}');helpers=helpers.rstrip()[:-1]
def method(signature):
 start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
h=(backend/'d3d9_deko.h').read_text();meta=h[h.index('struct PackMeta {'):h.index('class VertexShader :')]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <deko3d.h>
#include <vector>
#include <map>
#include <array>
#include <string>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <cassert>
namespace deko {
'''+meta+r'''
struct TextureCore{struct{DkImageType type=DkImageType_2D;}img;struct Format{bool depth=false;}format;Format*fmt=&format;D3DFORMAT d3dfmt=D3DFMT_A8R8G8B8;};TextureCore texture;
TextureCore* texture_core(IDirect3DBaseTexture9*p){return p?&texture:nullptr;}
DkImageFormat srgb_format(DkImageFormat f){return f;}
struct Device{
 bool alpha_to_coverage()const{return false;}bool point_list_=false;
 struct Surface{struct Image{DkImageFormat format=DkImageFormat_RGBA8_Unorm;}img;Image*image(){return &img;}};Surface*targets_[4]{};
 DWORD rs_[256]{},tss_[8][33]{};D3DMATRIX transforms_[512]{};D3DMATERIAL9 material_{};
 struct Light{D3DLIGHT9 value{};BOOL enabled=FALSE;};std::map<DWORD,Light>lights_;
 struct Declaration{std::vector<D3DVERTEXELEMENT9>elements;};Declaration decl;Declaration*decl_=&decl;
 D3DVIEWPORT9 viewport_{0,0,640,480,0,1};float clip_planes_[6][4]{};void*vs_=nullptr;struct PS{unsigned major_version()const{return 3;}};PS*ps_=nullptr;
 IDirect3DBaseTexture9*textures_[20]{};
 Device(){for(auto&m:transforms_)for(unsigned i=0;i<4;++i)m.m[i][i]=1;for(unsigned i=0;i<8;++i){tss_[i][D3DTSS_COLOROP]=i?D3DTOP_DISABLE:D3DTOP_MODULATE;tss_[i][D3DTSS_COLORARG1]=D3DTA_TEXTURE;tss_[i][D3DTSS_COLORARG2]=D3DTA_CURRENT;tss_[i][D3DTSS_ALPHAOP]=D3DTOP_SELECTARG1;tss_[i][D3DTSS_ALPHAARG1]=D3DTA_TEXTURE;}}
};
struct FixedFunction{
 Device*device_;std::vector<uint8_t>vertex_data_,pixel_data_,spec_data_,render_data_,clip_data_,blend_data_;
 PackMeta vertex_meta_,pixel_meta_;explicit FixedFunction(Device*d):device_(d){};void build_state();
};
'''
main=r'''
}
using namespace deko;
template<class T>T get(const std::vector<uint8_t>&v,size_t offset){assert(offset+sizeof(T)<=v.size());T out;memcpy(&out,v.data()+offset,sizeof(out));return out;}
int main(){Device d;FixedFunction f(&d);d.transforms_[D3DTS_WORLD].m[3][0]=3;d.transforms_[D3DTS_WORLD].m[3][1]=4;d.transforms_[D3DTS_VIEW].m[0][0]=2;d.transforms_[D3DTS_VIEW].m[1][1]=3;
 d.rs_[D3DRS_LIGHTING]=TRUE;d.rs_[D3DRS_COLORVERTEX]=TRUE;d.rs_[D3DRS_DIFFUSEMATERIALSOURCE]=D3DMCS_COLOR1;
 d.decl.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0},{0,16,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0}};
 d.lights_[3].enabled=128;d.lights_[3].value.Type=D3DLIGHT_DIRECTIONAL;d.lights_[3].value.Direction={1,0,0};d.lights_[3].value.Diffuse={1,.5f,.25f,1};
 d.rs_[D3DRS_CLIPPLANEENABLE]=1u<<4;d.clip_planes_[4][2]=1;d.clip_planes_[4][3]=7;d.rs_[D3DRS_FOGEND]=0x41200000;d.rs_[D3DRS_FOGSTART]=0x40000000;
 f.build_state();assert(f.blend_data_.size()==16384);auto wv=get<D3DMATRIX>(f.vertex_data_,0);assert(wv.m[0][0]==2&&wv.m[1][1]==3&&wv.m[3][0]==6&&wv.m[3][1]==12);
 auto inv=get<D3DMATRIX>(f.vertex_data_,64);assert(inv.m[0][0]==.5f&&inv.m[3][0]==-3);
 assert(get<uint32_t>(f.vertex_data_,1856+6*16)==2); // float2 texcoord declaration
 assert((get<uint32_t>(f.vertex_data_,1856+10*16)&0xffff)==0x101); // lighting + one enabled light
 assert(get<float>(f.vertex_data_,864+48)==0);assert(get<float>(f.vertex_data_,864+64)==1); // directional transform ignores translation
 assert(get<uint32_t>(f.vertex_data_,1856+10*16)>>16==D3DMCS_COLOR1);
 assert(get<float>(f.clip_data_,8)==1&&get<float>(f.clip_data_,12)==7&&((get<uint32_t>(f.spec_data_,0)>>16)&7)==1);
 assert((get<uint32_t>(f.spec_data_,32)&31)==D3DTOP_DISABLE); // missing color texture disables stage
 assert(get<float>(f.render_data_,4)==.125f);
 d.transforms_[D3DTS_VIEW].m[3][0]=10;f.build_state();auto invview=get<D3DMATRIX>(f.vertex_data_,128);assert(invview.m[0][3]==-5&&invview.m[3][0]==0);d.transforms_[D3DTS_VIEW].m[3][0]=0;
 d.rs_[D3DRS_POINTSCALE_A]=0x3f800000;f.build_state();assert(std::abs(get<float>(f.render_data_,48)-1.f/(480.f*480.f))<1e-10f);
 d.textures_[0]=reinterpret_cast<IDirect3DBaseTexture9*>(uintptr_t(1));texture.img.type=DkImageType_Cubemap;
 d.tss_[0][D3DTSS_CONSTANT]=0x11223344;d.tss_[0][D3DTSS_BUMPENVMAT10]=0x3f800000;d.tss_[0][D3DTSS_TEXTURETRANSFORMFLAGS]=D3DTTFF_COUNT3|D3DTTFF_PROJECTED;
 d.rs_[D3DRS_VERTEXBLEND]=D3DVBF_2WEIGHTS;d.rs_[D3DRS_ALPHATESTENABLE]=1;d.rs_[D3DRS_ALPHAFUNC]=D3DCMP_GREATER;d.rs_[D3DRS_ALPHAREF]=42;
 f.build_state();assert(f.vertex_meta_.inputs[2].usage==D3DDECLUSAGE_BLENDWEIGHT&&f.vertex_meta_.inputs[3].usage==D3DDECLUSAGE_BLENDINDICES);
 assert((get<uint32_t>(f.spec_data_,16)&3)==2&&get<uint32_t>(f.pixel_data_,0)==0x11223344&&get<float>(f.pixel_data_,48)==1);
 assert((get<uint32_t>(f.spec_data_,0)&7)==D3DCMP_GREATER-1&&get<uint32_t>(f.render_data_,0)>>24==42);
 assert(f.blend_data_.size()==16384&&get<D3DMATRIX>(f.blend_data_,0).m[3][0]==6);
 // Pretransformed vertices bypass lighting and use viewport inverse mapping.
 d.decl.elements[0].Usage=D3DDECLUSAGE_POSITIONT;d.viewport_={10,20,100,50,0,1};f.build_state();assert(f.vertex_meta_.inputs[0].usage==D3DDECLUSAGE_POSITIONT);
 assert((get<uint32_t>(f.vertex_data_,1856+10*16)&255)==0);assert(std::abs(get<float>(f.vertex_data_,832)+1.2f)<1e-6&&std::abs(get<float>(f.vertex_data_,836)-1.8f)<1e-6);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-ffp-state-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+helpers+method('void FixedFunction::build_state(')+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I/opt/devkitpro/libnx/include','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 subprocess.run(['python3',str(root/'tools/scripts/build_deko_ffp_shaders.py')],check=True)
 for stage,expected in [('vert',{'ShaderData':2048,'VertexBlendData':16384,'ClipPlanes':96,'SpecConsts':80,'RenderStates':64}),('frag',{'SharedData':768,'SpecConsts':80,'RenderStates':48})]:
  result=subprocess.run(['glslangValidator','-G','-S',stage,'-q','-l','--reflect-all-block-variables',str(root/f'build/shaders/deko-ffp/fixed_function.{stage}.glsl'),'-o',str(p/(stage+'.spv'))],text=True,capture_output=True,check=True)
  for name,size in expected.items():assert re.search(re.escape(name)+r': offset -1, type \w+, size '+str(size)+r',',result.stdout),(name,size)
print('Actual FFP state builder: transforms/inverses, lighting/materials, clips, texture stages, projection, blending, viewport and std140 ABI passed; VS/smooth+flat PS compile with UAM (ASAN/UBSAN; GPU images still require Switch)')
