#!/usr/bin/env python3
"""Exercise real pool allocation/free accounting independently of GPU services."""
from pathlib import Path
import tempfile,subprocess,os
root=Path(__file__).resolve().parents[2];source=(root/'shims/d3d9/deko/gpu.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <cstdint>
#include <cassert>
using DkMemBlock=int;std::set<int> live;int serial=0;bool fail=false;
DkMemBlock make_block(uint32_t,uint32_t){if(fail)return 0;live.insert(++serial);return serial;}
void dkMemBlockDestroy(int block){assert(live.erase(block)==1);}
struct Alloc{int block=0;uint32_t offset=0,size=0;explicit operator bool()const{return block!=0;}};
struct Pool{Pool(uint32_t flags,uint32_t size):flags_(flags),block_size_(size){}
 struct Block{int mem;std::map<uint32_t,uint32_t>free;bool dedicated=false;};
 uint32_t flags_,block_size_;std::vector<Block>blocks_;uint64_t allocated_=0;
 Alloc alloc(uint32_t,uint32_t);void free_now(const Alloc&);void destroy();};
'''
body='\n'.join(method(x) for x in ['uint32_t align_up(', 'Alloc Pool::alloc(', 'void Pool::free_now(', 'void Pool::destroy()'])
main=r'''
int main(){Pool p(0,1024);auto a=p.alloc(100,32);assert(a&&a.offset%32==0&&a.size==112&&p.allocated_==112);
 auto b=p.alloc(512,64);assert(b&&b.offset%64==0&&p.allocated_==624);
 p.free_now(a);assert(p.allocated_==512);auto c=p.alloc(64,32);assert(c.offset==a.offset&&p.allocated_==576);
 p.free_now(b);p.free_now(c);assert(p.allocated_==0);
 auto d=p.alloc(2048,64);assert(d&&p.allocated_==2048);p.free_now(d);assert(p.allocated_==0);
 fail=true;assert(!p.alloc(2048,64)&&p.allocated_==0);fail=false;
 auto e=p.alloc(32,16);assert(e&&p.allocated_==32);p.destroy();assert(p.allocated_==0&&live.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='wine-pool-lifetime-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+body+main)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual pool methods: aligned allocation, reuse/coalescing, dedicated blocks, failed allocation, live-byte accounting and final reclamation passed (ASAN/UBSAN)')
