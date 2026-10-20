#!/usr/bin/env python3
"""Exercise actual CPU-side device state methods against D3D9 contracts."""
from pathlib import Path
import tempfile,subprocess,re,os
from cpu_stateblock_support import declarations
root=Path(__file__).resolve().parents[2];cpp=(root/'shims/d3d9/deko/device.cpp').read_text()
body=cpp[cpp.index('HRESULT Device::BeginScene()'):cpp.index('HRESULT Device::SetFVF(')]
start=cpp.index('HRESULT Device::SetStreamSourceFreq(');opening=cpp.index('{',start);end=opening+1;depth=1
while depth:depth+=(cpp[end]=='{')-(cpp[end]=='}');end+=1
body+='\n'+cpp[start:end]
prototypes='\n'.join('HRESULT '+m.group(1)+'('+m.group(2)+');' for m in re.finditer(r'HRESULT Device::(\w+)\(([^)]*)\)',body))
init=cpp[cpp.index('    for (auto& matrix : transforms_)'):cpp.index('    for (auto& s : ss_)')]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <array>
#include <algorithm>
#include <map>
#include <cstring>
#include <cassert>
struct ApiLock{explicit ApiLock(const char*){}};
constexpr unsigned kVsConst=1,kTextures=2,kShaders=4,kVertexInput=8,kStreams=16;
struct Device{
 void resolve_depth_command(){};
 DWORD tss_[8][33]{},rs_[256]{};D3DMATRIX transforms_[512]{};D3DMATERIAL9 material_{};
 struct Light{D3DLIGHT9 value{};BOOL enabled=FALSE;};std::map<DWORD,Light>lights_;
 float clip_planes_[6][4]{};DWORD fvf_=0,dirty_=0;bool in_scene_=false;
 std::map<UINT,std::array<PALETTEENTRY,256>>palettes_;UINT current_palette_=0;
 struct Stream{UINT freq=1;};Stream streams_[16];
'''+prototypes+'\nDevice(){'+init+'}\n};\n'
main=r'''
int main(){Device d;DWORD value=0;
 assert(d.GetTextureStageState(0,D3DTSS_COLOROP,&value)==0&&value==D3DTOP_MODULATE);
 assert(d.GetTextureStageState(1,D3DTSS_COLOROP,&value)==0&&value==D3DTOP_DISABLE);
 for(unsigned s=0;s<8;++s){assert(d.SetTextureStageState(s,D3DTSS_CONSTANT,0x12345678+s)==0);assert(d.GetTextureStageState(s,D3DTSS_CONSTANT,&value)==0&&value==0x12345678+s);}
 assert(d.SetTextureStageState(8,D3DTSS_COLOROP,0)==D3DERR_INVALIDCALL);
 assert(d.GetTextureStageState(0,D3DTSS_COLOROP,nullptr)==D3DERR_INVALIDCALL);
 assert(d.EndScene()==D3DERR_INVALIDCALL);assert(d.BeginScene()==0);assert(d.BeginScene()==D3DERR_INVALIDCALL);assert(d.EndScene()==0);
 assert(d.ValidateDevice(&value)==0&&value==1);assert(d.ValidateDevice(nullptr)==0);
 D3DMATRIX a{},b{},out{};for(unsigned i=0;i<4;++i)a.m[i][i]=1;a.m[3][0]=3;
 for(unsigned i=0;i<4;++i)b.m[i][i]=2;b.m[3][3]=1;
 assert(d.GetTransform(D3DTS_VIEW,&out)==0&&out.m[0][0]==1&&out.m[3][3]==1);
 assert(d.SetTransform(D3DTS_WORLD,&a)==0);assert(d.MultiplyTransform(D3DTS_WORLD,&b)==0);
 // Microsoft MultiplyTransform contract: incoming matrix times stored matrix.
 assert(d.GetTransform(D3DTS_WORLD,&out)==0&&out.m[3][0]==3&&out.m[0][0]==2);
 assert(d.GetTransform(D3DTRANSFORMSTATETYPE(0),&out)==D3DERR_INVALIDCALL);
 assert(d.SetTransform(D3DTS_WORLD,nullptr)==D3DERR_INVALIDCALL);
 D3DLIGHT9 light{};BOOL enabled=0;assert(d.GetLight(9,&light)==D3DERR_INVALIDCALL);
 assert(d.LightEnable(9,TRUE)==0);assert(d.GetLight(9,&light)==0&&light.Type==D3DLIGHT_DIRECTIONAL&&light.Direction.z==1);
 assert(d.GetLightEnable(9,&enabled)==0&&enabled==128);assert(d.LightEnable(9,FALSE)==0);assert(d.GetLightEnable(9,&enabled)==0&&!enabled);
 light.Type=D3DLIGHT_POINT;light.Attenuation0=-1;assert(d.SetLight(9,&light)==D3DERR_INVALIDCALL);light.Attenuation0=1;assert(d.SetLight(9,&light)==0);
 D3DMATERIAL9 material{};material.Power=12;assert(d.SetMaterial(&material)==0);material.Power=0;assert(d.GetMaterial(&material)==0&&material.Power==12);
 float plane[4]={1,2,3,4},copy[4]{};assert(d.SetClipPlane(5,plane)==0);assert(d.GetClipPlane(5,copy)==0&&!memcmp(plane,copy,16));plane[3]=7;assert(d.SetClipPlane(999,plane)==0);assert(d.GetClipPlane(5,copy)==0&&copy[3]==7);
 PALETTEENTRY palette[256]{};for(auto&entry:palette)entry.peFlags=0xff;palette[255].peRed=77;assert(d.SetPaletteEntries(42,palette)==0);palette[255].peRed=0;assert(d.GetPaletteEntries(42,palette)==0&&palette[255].peRed==77);assert(d.GetPaletteEntries(41,palette)==D3DERR_INVALIDCALL);palette[0].peFlags=0;assert(d.SetPaletteEntries(42,palette)==D3DERR_INVALIDCALL);
 assert(d.SetCurrentTexturePalette(42)==0);assert(d.GetCurrentTexturePalette(&value)==0&&value==42);
 assert(d.SetStreamSourceFreq(0,D3DSTREAMSOURCE_INSTANCEDATA|1)==D3DERR_INVALIDCALL);
 assert(d.SetStreamSourceFreq(1,0)==D3DERR_INVALIDCALL&&d.streams_[1].freq==1);
 assert(d.SetStreamSourceFreq(1,2)==0&&d.streams_[1].freq==2);
 assert(d.SetStreamSourceFreq(1,D3DSTREAMSOURCE_INDEXEDDATA)==0);
 assert(d.SetStreamSourceFreq(1,D3DSTREAMSOURCE_INSTANCEDATA)==0);
 assert(d.SetStreamSourceFreq(1,D3DSTREAMSOURCE_INDEXEDDATA|D3DSTREAMSOURCE_INSTANCEDATA)==D3DERR_INVALIDCALL);
 assert(d.streams_[1].freq==D3DSTREAMSOURCE_INSTANCEDATA);
 assert(d.SetStreamSourceFreq(16,1)==D3DERR_INVALIDCALL);
 assert(d.GetStreamSourceFreq(0,&value)==0&&value==1);assert(d.GetStreamSourceFreq(16,&value)==D3DERR_INVALIDCALL);
}
'''
with tempfile.TemporaryDirectory(prefix='wine-device-state-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header.replace('struct Device{',declarations()+'struct Device{')+body+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual device state methods: defaults, matrix order, lights/material, TSS, palettes, clip planes, scene pairing, stream readback and invalid inputs passed (ASAN/UBSAN)')
