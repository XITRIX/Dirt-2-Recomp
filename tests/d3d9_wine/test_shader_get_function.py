#!/usr/bin/env python3
"""Actual shader GetFunction methods honor byte capacity and preserve size on copies."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2];source=(root/'shims/d3d9/deko/shaders.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <d3d9.h>
#undef min
#undef max
#include <algorithm>
#include <vector>
#include <cstring>
#include <cassert>
#include <cstdint>
struct ApiLock{explicit ApiLock(const char*){}};
struct VertexShader{std::vector<uint32_t>code_={0xfffe0300,1,2,3,0xffff};HRESULT GetFunction(void*,UINT*);};
struct PixelShader{std::vector<uint32_t>code_={0xffff0300,4,5,6,0xffff};HRESULT GetFunction(void*,UINT*);};
'''
main=r'''
template<class T>void test(){
 T shader;const unsigned bytes=shader.code_.size()*4;
 assert(shader.GetFunction(nullptr,nullptr)==D3DERR_INVALIDCALL);
 UINT size=0;assert(shader.GetFunction(nullptr,&size)==D3D_OK&&size==bytes);
 for(unsigned capacity:{0u,1u,3u,bytes-1,bytes,bytes+5}){
  std::vector<uint8_t>out(capacity+16,0xa5);size=capacity;
  assert(shader.GetFunction(out.data(),&size)==D3D_OK&&size==capacity);
  assert(!memcmp(out.data(),shader.code_.data(),std::min(capacity,bytes)));
  for(unsigned i=std::min(capacity,bytes);i<out.size();++i)assert(out[i]==0xa5);
 }
 // ASAN catches an overrun of the actual allocation, independently of canaries.
 auto*out=new uint8_t[1];size=1;assert(shader.GetFunction(out,&size)==0);delete[]out;
}
int main(){test<VertexShader>();test<PixelShader>();}
'''
with tempfile.TemporaryDirectory(prefix='deko-get-function-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+method('HRESULT VertexShader::GetFunction(')+method('HRESULT PixelShader::GetFunction(')+main);native=root/'build/src/dxvk/include/native'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(native/'windows'),'-I'+str(native/'directx'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual VS/PS GetFunction: query, partial-byte capacities, unchanged copy size and allocation bounds passed (ASAN/UBSAN).')
