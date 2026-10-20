#!/usr/bin/env python3
"""Exercise the actual Switch DXVK cache-key reader against captured/malformed data."""
import pathlib, subprocess, tempfile, sys
root=pathlib.Path(__file__).resolve().parents[2]
src=root/'build/src/dxvk-switch'
code=(src/'src/dxvk/dxvk_shader_cache.cpp').read_text()
def method(name):
 start=code.index('  bool DxvkShaderCache::'+name+'(')
 end=code.index('{',start)+1
 depth=1
 while depth:
  depth += (code[end]=='{') - (code[end]=='}')
  end+=1
 return code[start:end]
with tempfile.TemporaryDirectory(prefix='dirt2-cache-') as d:
 p=pathlib.Path(d)
 # Preinclude dependencies so private access applies only to the tested class.
 harness='''#include <array>
#include <optional>
#include <string>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "src/util/thread.h"
#include "src/util/util_env.h"
#include "src/util/util_file.h"
#include "src/dxvk/dxvk_shader_ir.h"
#define private public
#include "src/dxvk/dxvk_shader_cache.h"
#undef private
#include <cassert>
#include <fstream>
#include <iostream>
namespace dxvk {
'''+method('readShaderXfbInfo')+'\n'+method('readShaderLutKey')+'''\n}
using Cache=dxvk::DxvkShaderCache;
using namespace dxvk::util;
int main(int argc,char**argv) {
 File file(argv[1],FileFlag::AllowRead);
 size_t offset=0;
 Cache::LutHeader header;
 assert(Cache::readBytes(file,header.magic.data(),offset,4));
 assert(Cache::readString(file,offset,header.versionString));
 unsigned count=0;
 while(offset<file.size()) {
   Cache::LutKey key;
   if(!Cache::readShaderLutKey(file,offset,key)) break;
   Cache::LutEntry entry;
   if(!Cache::read(file,offset,entry)) break;
   ++count;
 }
 assert(count==728); // last captured entry has an impossible XFB count
 std::cout<<"Rejected torn cache after "<<count<<" valid entries\\n";
 size_t overflow=SIZE_MAX-1;
 char bytes[8];
 assert(!Cache::readBytes(file,bytes,overflow,8));
 assert(overflow==SIZE_MAX-1);
 // The write-only implementation must retain flags for size() after append.
 File output(argv[2],FileFlags(FileFlag::AllowWrite,FileFlag::Truncate));
 assert(output.append(4,"DXVK"));assert(output.size()==4);
 assert(output.append(4,"TEST"));assert(output.size()==8);
 assert(output.flush());
}
'''
 (p/'test.cpp').write_text(harness)
 includes=[src,src/'include/native/windows',src/'include/native/directx',src/'include/vulkan/include',src/'include/spirv/include',src/'subprojects/dxbc-spirv']
 cmd=['c++','-std=c++20','-fms-extensions','-fsanitize=address,undefined','-g']
 cmd += ['-I'+str(i) for i in includes]
 cmd += [str(p/'test.cpp'),str(src/'src/util/util_file.cpp'),'-o',str(p/'test')]
 subprocess.run(cmd,check=True)
 capture=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else root/'build/package/switch-dx11/crash-tls-20261020/dirt2.dxvk.lut'
 subprocess.run([str(p/'test'),str(capture),str(p/'write-only.bin')],check=True)
