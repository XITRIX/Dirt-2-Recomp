#!/usr/bin/env python3
"""Run actual Switch shader constant accessors with overflow/boundary inputs."""
import os,re,subprocess,tempfile
from cpu_stateblock_support import declarations
from pathlib import Path
root=Path(__file__).resolve().parents[2]
s=(root/'shims/d3d9/deko/device.cpp').read_text();body=s[s.index('#define CONSTANT_SETTER'):s.index('HRESULT Device::SetStreamSource')]
setters=re.findall(r'^CONSTANT_SETTER\((\w+), (\w+), (\d+), (\w+), (\d+), (\w+), \w+\)',body,re.M)
header='''#include <cassert>
#include <cstring>
#include <cstdint>
#include <limits>
using UINT=uint32_t;using BOOL=int;using HRESULT=int;
constexpr int D3D_OK=0,D3DERR_INVALIDCALL=-1,kVsConst=1,kPsConst=2;
struct ApiLock{explicit ApiLock(const char*){}};
struct Device{int dirty_=0;
'''
for name,array,limit,typ,width,flag in setters:
 header+=f'{typ} {array}[{limit}]'+(f'[{width}]' if width!='1' else '')+'{};\n'
 header+=f'HRESULT {name}(UINT,const {typ}*,UINT);\n'
header+='HRESULT GetVertexShaderConstantI(UINT,int*,UINT);HRESULT GetVertexShaderConstantB(UINT,BOOL*,UINT);HRESULT GetPixelShaderConstantI(UINT,int*,UINT);HRESULT GetPixelShaderConstantB(UINT,BOOL*,UINT);HRESULT GetVertexShaderConstantF(UINT,float*,UINT);HRESULT GetPixelShaderConstantF(UINT,float*,UINT);};\n'
test='int main(){Device d;constexpr UINT huge=std::numeric_limits<UINT>::max();\n'
for name,array,limit,typ,width,flag in setters:
 test+='{'+f'{typ} input[{int(limit)*int(width)}]{{}};input[0]=7;\n'
 test+=f'assert(d.{name}(0,input,1)==0);assert(d.{array}[0]'+('[0]' if width!='1' else '')+'==7);\n'
 test+=f'assert(d.{name}({limit}-1,input,1)==0);assert(d.{name}(0,input,{limit})==0);assert(d.{name}({limit},input,0)==0);\n'
 test+=f'auto snapshot=d;d.dirty_=0;assert(d.{name}(huge,input,1)==-1);assert(d.{name}(huge,input,2)==-1);assert(d.{name}(1,input,huge)==-1);assert(d.{name}({limit}-1,input,2)==-1);assert(d.{name}({limit}+1,input,0)==-1);assert(d.{name}(0,nullptr,1)==-1);assert(d.dirty_==0);assert(!std::memcmp(d.{array},snapshot.{array},sizeof(d.{array})));'+'}\n'
for setter,array,limit,typ,width,flag in setters:
 name=setter.replace('Set','Get',1);limit=int(limit)
 test+='{'+typ+' output[1024]{};'+f'assert(d.{name}(0,output,{limit})==0);assert(d.{name}({limit}-1,output,1)==0);assert(d.{name}({limit},output,0)==0);output[0]=42;assert(d.{name}(huge,output,1)==-1);assert(d.{name}(huge,output,2)==-1);assert(d.{name}(1,output,huge)==-1);assert(d.{name}({limit}-1,output,2)==-1);assert(d.{name}({limit}+1,output,0)==-1);assert(d.{name}(0,nullptr,1)==-1);assert(output[0]==42);'+'}\n'
test+='}\n'
with tempfile.TemporaryDirectory(prefix='wine-constant-bounds-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header.replace('struct Device{',declarations()+'struct Device{')+body+test)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual 6 shader constant setters and 6 getters: valid boundaries, UINT overflow, null inputs and unchanged state on rejection passed (ASAN/UBSAN)')
