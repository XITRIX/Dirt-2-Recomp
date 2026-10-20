#!/usr/bin/env python3
"""Actual UpdateSurface places a source subrectangle at destination zero by default."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2];s=(root/'shims/d3d9/deko/device.cpp').read_text();start=s.index('HRESULT Device::UpdateSurface(');opening=s.index('{',start);end=opening+1;depth=1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <memory>
#include <vector>
#include <cstring>
#include <cassert>
struct ApiLock{explicit ApiLock(const char*){}};
struct Alloc{std::shared_ptr<std::vector<unsigned char>>data;unsigned char*cpu()const{return data->data();}explicit operator bool()const{return bool(data);}};
struct Pool{Alloc alloc(unsigned n,unsigned){return{std::make_shared<std::vector<unsigned char>>(n)};}};
struct Frame{Pool transient;};struct Gpu{Frame frame;Frame&cur(){return frame;}};Gpu&gpu(){static Gpu g;return g;}
struct Format{unsigned block=1,bytes=4;};unsigned row_pitch(const Format&f,unsigned w){return w*f.bytes;}unsigned level_rows(const Format&,unsigned h){return h;}
struct GpuImage{};struct Texture{bool autogen_dirty=false;};
struct Surface{D3DMULTISAMPLE_TYPE multisample=D3DMULTISAMPLE_NONE;unsigned width=8,height=8,cpu_pitch=32,layer=0,level=0;Format format;Format*fmt=&format;Alloc cpu;Texture*tex=nullptr;GpuImage img;GpuImage*image(){return &img;}};
#define IDirect3DSurface9 Surface
RECT destination{};unsigned uploads=0,copies=0;Alloc uploaded;
struct Device{
 HRESULT UpdateSurface(IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const POINT*);
 void upload(GpuImage&,unsigned,unsigned,const Alloc&data,unsigned,const RECT&r){destination=r;uploaded=data;++uploads;}
 void blit(Surface*,const RECT*,Surface*,const RECT*r,bool){destination=*r;++copies;}
};
'''
main=r'''
int main(){
 Device d;Surface source,target;source.cpu=gpu().cur().transient.alloc(256,256);
 for(unsigned i=0;i<256;++i)source.cpu.cpu()[i]=i;
 RECT rect{2,3,6,5};assert(d.UpdateSurface(&source,&rect,&target,nullptr)==0);
 assert(uploads==1&&destination.left==0&&destination.top==0&&destination.right==4&&destination.bottom==2);
 for(unsigned y=0;y<2;++y)assert(!memcmp(uploaded.cpu()+y*16,source.cpu.cpu()+(y+3)*32+8,16));
 POINT point{1,2};assert(d.UpdateSurface(&source,&rect,&target,&point)==0);
 assert(destination.left==1&&destination.top==2&&destination.right==5&&destination.bottom==4);
 source.cpu={};assert(d.UpdateSurface(&source,&rect,&target,nullptr)==0&&copies==1);
 assert(destination.left==0&&destination.top==0);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-update-origin-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+s[start:end]+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual UpdateSurface: nonzero source rectangle, default destination origin, explicit destination and staged pixels passed (ASAN/UBSAN).')
