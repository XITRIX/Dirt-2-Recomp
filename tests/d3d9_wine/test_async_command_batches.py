#!/usr/bin/env python3
"""Exercise actual batch/flush code with a finite native queue and live staging.

CPU command-order/lifetime check; hardware timeout behavior still needs a run.
"""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2];source=(root/'shims/d3d9/deko/gpu.cpp').read_text()
def method(signature):
 start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
header=r'''
#include <cstdint>
#include <cassert>
#include <vector>
using DkGpuAddr=uint64_t;
struct DkImageDescriptor{uint8_t data[32];};struct DkFence{};
unsigned recorded=0,native_entries=0,max_entries=0,flushes=0,waits=0,signals=0,bindings=0;
uint64_t dkMemBlockGetGpuAddr(int){return 0x100000;}
void dkCmdBufBindImageDescriptorSet(int,uint64_t,unsigned){++recorded;++bindings;}
void dkCmdBufBindSamplerDescriptorSet(int,uint64_t,unsigned){++recorded;++bindings;}
unsigned dkCmdBufFinishList(int){unsigned n=recorded;recorded=0;return n;}
void submit(int,unsigned entries,const char*){native_entries+=entries;}
void dkQueueSignalFence(int,DkFence*,bool){++signals;native_entries+=2;}
void dkQueueFlush(int){
 // Independent finite queue model. The observed driver failure was at 256;
 // completed-operation batches should reach this boundary much earlier.
 assert(native_entries<256);if(native_entries>max_entries)max_entries=native_entries;
 ++flushes;native_entries=0;
}
void wait_fence(DkFence*,const char*){++waits;}
bool dkQueueIsInErrorState(int){return false;}
namespace rt{void log(const char*,...){}}
struct Gpu{
 int cmd=1,queue=2,descriptors=3;static constexpr unsigned kMaxImages=8192,kMaxSamplers=2048;
 uint64_t list_id=0;uint32_t pending_operations=0;bool descriptors_dirty=false;
 std::vector<uint8_t> staging=std::vector<uint8_t>(4096,0xa5);
 unsigned retired_resources=0; // Frame-owned memory must survive partial submissions.
 void begin_list();void checkpoint();void flush(bool,const char* = "a read-back");
};
'''
main=r'''
int main(){
 Gpu g;g.begin_list();const auto*storage=g.staging.data();
 // Thousands of texture uploads/draws before the first Present, with multiple
 // native command segments each. Queue progress must not require CPU waits.
 for(unsigned i=0;i<3000;++i){
  recorded+=12;g.checkpoint();
  assert(g.staging.data()==storage&&g.staging[0]==0xa5&&g.retired_resources==0);
 }
 assert(flushes>0&&flushes<3000&&waits==0&&signals==0&&max_entries<256);
 assert(g.list_id==flushes+1&&bindings==2*g.list_id&&g.descriptors_dirty);
 // A real readback still waits and resets the batch counter, independently
 // of preceding asynchronous submits; the frame resources stay live.
 g.flush(true,"readback");assert(waits==1&&signals==1&&g.pending_operations==0);
 assert(g.staging.data()==storage&&g.retired_resources==0);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-async-batches-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(header+'\n'.join(method(x) for x in ['void Gpu::begin_list()','void Gpu::checkpoint()','void Gpu::flush('])+main)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Actual asynchronous batching/flush: finite-queue progress, no per-batch CPU waits, descriptor rebinding and live frame staging passed (CPU model; ASAN/UBSAN).')
