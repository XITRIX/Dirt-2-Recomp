#!/usr/bin/env python3
"""Exercise actual state-block classes and device setters with native COM stubs."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[2];backend=root/'shims/d3d9/deko';cpp=(backend/'device.cpp').read_text()
def method(signature):
    start=cpp.index(signature);opening=cpp.index('{',start);end=opening+1;depth=1
    while depth:depth+=(cpp[end]=='{')-(cpp[end]=='}');end+=1
    return cpp[start:end]
names=['SetRenderState','GetRenderState','SetSamplerState','GetSamplerState','SetTexture','GetTexture','SetViewport','GetViewport','SetScissorRect','GetScissorRect','SetVertexDeclaration','GetVertexDeclaration','SetVertexShader','GetVertexShader','SetPixelShader','GetPixelShader','SetStreamSource','GetStreamSource','SetStreamSourceFreq','SetIndices','GetIndices']
methods='\n'.join(method('HRESULT Device::'+n+'(') for n in names)
# This contiguous region includes CPU state getters, transform validation and setters.
methods+=cpp[cpp.index('HRESULT Device::BeginScene()'):cpp.index('HRESULT Device::SetFVF(')]
constants=cpp[cpp.index('#define CONSTANT_SETTER'):cpp.index('HRESULT Device::SetStreamSource(')]
methods+=constants
prototypes='\n'.join('HRESULT '+m[1]+'('+m[2]+');' for m in re.finditer(r'HRESULT Device::(\w+)\(([^)]*)\)',methods) if m[1]!='Name')
for m in re.finditer(r'^CONSTANT_SETTER\((\w+), \w+, \d+, (\w+),',constants,re.M):prototypes+='HRESULT '+m[1]+'(UINT,const '+m[2]+'*,UINT);\n'
init=cpp[cpp.index('    // D3D9\'s default render states.'):cpp.index('Device::~Device()')].rstrip();assert init.endswith('}');init=init[:-1]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <cassert>
#include <cstring>
#include <array>
#include <vector>
#include <map>
#include <type_traits>
#include <algorithm>
struct ApiLock{explicit ApiLock(const char*){}};
#include "object_refs.h"
namespace deko {
template<class T>T stub_result(const char*){return T{};}
#include "stubs.inc"
class Device;
struct TextureCore { Device*dev=nullptr; };
struct Texture:StubIDirect3DTexture9{TextureCore core;};
struct VertexDeclaration:StubIDirect3DVertexDeclaration9{DWORD fvf=0;};
struct VertexShader:StubIDirect3DVertexShader9{};
struct PixelShader:StubIDirect3DPixelShader9{};
struct VertexBuffer:StubIDirect3DVertexBuffer9{};
struct IndexBuffer:StubIDirect3DIndexBuffer9{};
TextureCore* texture_core(IDirect3DBaseTexture9*p){return p?&static_cast<Texture*>(p)->core:nullptr;}
void texture_add_private(IDirect3DBaseTexture9*p){static_cast<Texture*>(p)->AddRefPrivate();}
void texture_release_private(IDirect3DBaseTexture9*p){static_cast<Texture*>(p)->ReleasePrivate();}
void texture_add_ref(IDirect3DBaseTexture9*p){static_cast<Texture*>(p)->AddRef();}
void log_unimplemented(const char*){}
int sampler_index(DWORD s){return s>=D3DVERTEXTEXTURESAMPLER0?int(s-D3DVERTEXTEXTURESAMPLER0)+16:int(s);}
}
#include "stateblock.h"
namespace deko {
struct Device:StubIDirect3DDevice9 {
 void resolve_depth_command(){};
 static constexpr unsigned kSamplers=20,kStreams=16,kVsConst=1,kPsConst=2,kTextures=4,kShaders=8,kVertexInput=16,kRaster=32,kBlend=64,kDepthStencil=128,kViewport=256,kTargets=512;
 StateBlock* recording_=nullptr;bool ffp_dirty_=true;unsigned dirty_=0;
 D3DPRESENT_PARAMETERS pp_{};
 DWORD rs_[256]{},ss_[20][14]{},tss_[8][33]{};D3DMATRIX transforms_[512]{};D3DMATERIAL9 material_{};
 struct Light{D3DLIGHT9 value{};BOOL enabled=FALSE;};std::map<DWORD,Light>lights_;
 float clip_planes_[6][4]{};DWORD fvf_=0;bool in_scene_=false;
 std::map<UINT,std::array<PALETTEENTRY,256>>palettes_;UINT current_palette_=0;
 float vsf_[256][4]{},psf_[224][4]{};int vsi_[16][4]{},psi_[16][4]{};BOOL vsb_[16]{},psb_[16]{};
 IDirect3DBaseTexture9*textures_[20]{};
 struct Stream{VertexBuffer*vb=nullptr;UINT offset=0,stride=0,freq=1;};Stream streams_[16];
 VertexDeclaration*decl_=nullptr;VertexShader*vs_=nullptr;PixelShader*ps_=nullptr;IndexBuffer*indices_=nullptr;
 D3DVIEWPORT9 viewport_{0,0,640,480,0,1};RECT scissor_{0,0,640,480};
 template<class T>void set_ref(T*&slot,T*value);
 HRESULT BeginStateBlock();HRESULT EndStateBlock(IDirect3DStateBlock9**);HRESULT CreateStateBlock(D3DSTATEBLOCKTYPE,IDirect3DStateBlock9**);
'''+prototypes+'Device(){const auto&pp=pp_;'+init+r'''}
 ~Device(){if(recording_)recording_->ReleasePrivate();for(auto*&t:textures_)set_ref(t,static_cast<IDirect3DBaseTexture9*>(nullptr));set_ref(decl_,static_cast<VertexDeclaration*>(nullptr));}
};
'''
body=method('template <typename T>\nvoid Device::set_ref(')+methods+'\n}\n'+(backend/'stateblock.cpp').read_text().replace('#include "device.h"','')
main=r'''
using namespace deko;
int main(){
 auto*d=new Device;IDirect3DStateBlock9*b=nullptr,*other=nullptr;
 assert(d->rs_[D3DRS_LIGHTING]==TRUE);assert(d->BeginStateBlock()==0);assert(d->SetRenderState(D3DRS_LIGHTING,FALSE)==0);assert(d->rs_[D3DRS_LIGHTING]==TRUE);
 assert(d->EndStateBlock(&b)==0);assert(d->EndStateBlock(&other)==D3DERR_INVALIDCALL);
 assert(d->BeginStateBlock()==0&&d->BeginStateBlock()==D3DERR_INVALIDCALL);
 assert(b->Apply()==D3DERR_INVALIDCALL&&b->Capture()==D3DERR_INVALIDCALL);assert(d->CreateStateBlock(D3DSBT_ALL,&other)==D3DERR_INVALIDCALL);
 assert(d->EndStateBlock(&other)==0);other->Release();assert(b->Apply()==0&&d->rs_[D3DRS_LIGHTING]==FALSE);
 d->SetRenderState(D3DRS_LIGHTING,TRUE);assert(b->Capture()==0);d->SetRenderState(D3DRS_LIGHTING,FALSE);assert(b->Apply()==0&&d->rs_[D3DRS_LIGHTING]==TRUE);b->Release();
 // Later overlapping constant writes replace just the corresponding registers.
 float first[8]={1,2,3,4,5,6,7,8},last[4]={9,10,11,12};d->BeginStateBlock();d->SetVertexShaderConstantF(0,first,2);d->SetVertexShaderConstantF(1,last,1);d->EndStateBlock(&b);
 assert(d->vsf_[0][0]==0);b->Apply();assert(d->vsf_[0][0]==1&&d->vsf_[1][0]==9);b->Release();
 // Capture masks: PIXEL changes pixel states but excludes vertex constants.
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_LIGHTING,FALSE);d->CreateStateBlock(D3DSBT_PIXELSTATE,&b);
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_LIGHTING,TRUE);d->SetVertexShaderConstantF(0,last,1);b->Apply();
 assert(d->rs_[D3DRS_ALPHABLENDENABLE]==TRUE&&d->rs_[D3DRS_LIGHTING]==TRUE&&d->vsf_[0][0]==9);b->Release();
 // A NULL captured declaration leaves the currently bound declaration intact.
 auto*decl=new VertexDeclaration;decl->fvf=0x1234;d->BeginStateBlock();d->SetVertexDeclaration(decl);d->EndStateBlock(&b);assert(d->decl_==nullptr);b->Apply();assert(d->decl_==decl&&d->fvf_==0x1234);
 d->SetVertexDeclaration(nullptr);b->Capture();d->SetVertexDeclaration(decl);b->Apply();assert(d->decl_==decl);b->Release();decl->Release();
 // Resource ownership remains private and survives application Release.
 auto*t=new Texture;t->core.dev=d;d->BeginStateBlock();d->SetTexture(0,t);d->EndStateBlock(&b);assert(t->Release()==0);b->Apply();assert(d->textures_[0]==t);b->Release();d->SetTexture(0,nullptr);
 // Typed Capture keeps original stream offsets; explicit recording updates them.
 auto*vb=new VertexBuffer;d->SetStreamSource(0,vb,0,8);d->CreateStateBlock(D3DSBT_ALL,&b);
 d->SetStreamSource(0,vb,4,64);b->Capture();d->SetStreamSource(0,vb,16,32);b->Apply();assert(d->streams_[0].offset==0&&d->streams_[0].stride==64);b->Release();
 d->BeginStateBlock();d->SetStreamSource(0,vb,4,16);d->EndStateBlock(&b);d->SetStreamSource(0,vb,16,32);b->Capture();d->SetStreamSource(0,vb,0,8);b->Apply();assert(d->streams_[0].offset==16);b->Release();d->SetStreamSource(0,nullptr,0,0);vb->Release();
 // ALL captures viewport/transforms/material; MultiplyTransform bypasses recording.
 D3DVIEWPORT9 vp{10,20,30,40,.25f,.75f};d->SetViewport(&vp);d->CreateStateBlock(D3DSBT_ALL,&b);vp.Width=99;d->SetViewport(&vp);b->Apply();assert(d->viewport_.Width==30);b->Release();
 D3DMATRIX m{};for(unsigned i=0;i<4;++i)m.m[i][i]=2;d->BeginStateBlock();d->MultiplyTransform(D3DTS_WORLD,&m);d->EndStateBlock(&b);assert(d->transforms_[D3DTS_WORLD].m[0][0]==2);b->Capture();m.m[0][0]=7;d->SetTransform(D3DTS_WORLD,&m);b->Apply();assert(d->transforms_[D3DTS_WORLD].m[0][0]==7);b->Release();
 IDirect3DDevice9*parent=nullptr;d->CreateStateBlock(D3DSBT_ALL,&b);assert(b->GetDevice(&parent)==0&&parent==d);parent->Release();b->Release();
 assert(d->Release()==0);
 // Abandoning an unfinished recording must not form a parent/device cycle.
 d=new Device;d->BeginStateBlock();assert(d->Release()==0);
}
'''
with tempfile.TemporaryDirectory(prefix='wine-stateblocks-') as directory:
    p=Path(directory);(p/'test.cpp').write_text(header+body+main);native=root/'build/src/dxvk/include/native'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(backend),'-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Actual state blocks: recording isolation, capture/apply masks, overlapping constants, private resource lifetime, declaration NULL behavior, transforms, parent references and abandoned recording passed (ASAN/UBSAN)')
