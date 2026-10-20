#!/usr/bin/env python3
"""Validate native diagnostic wrappers preserve libnx AutoKick semantics."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'shims/d3d9/deko/submission.cpp').read_text()
s='\n'.join(l for l in s.splitlines() if not l.startswith('#include'))
header=r'''
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cassert>
using Result=uint32_t;using u32=uint32_t;using iova_t=uint64_t;
constexpr u32 GPFIFO_QUEUE_SIZE=2048;
#define R_FAILED(r) ((r)!=0)
#define R_SUCCEEDED(r) ((r)==0)
struct NvError{u32 type;u32 info[31];};
struct NvGpuChannel{struct{u32 fd=7;}base;u32 num_entries=0;struct{u32 value=10;}fence;u32 fence_incr=2;};
unsigned logs=0;namespace rt{void log(const char*,...){++logs;}}
Result nvGpuChannelGetErrorInfo(NvGpuChannel*,NvError*e){e->type=9;return 0;}
u32 last_threshold=0,max_batch=0,kicks=0;Result next_error=0;size_t last_commands=0;iova_t last_start=0;u32 last_flags=0;
extern "C" Result __real_nvGpuChannelKickoff(NvGpuChannel*c){
 if(next_error)return next_error;
 max_batch=std::max(max_batch,c->num_entries);++kicks;c->num_entries=0;return 0;
}
extern "C" Result __real_nvGpuChannelAppendEntry(NvGpuChannel*c,iova_t start,size_t commands,u32 flags,u32 threshold){
 last_start=start;last_commands=commands;last_flags=flags;last_threshold=threshold;
 if(next_error)return next_error;
 if(threshold>=GPFIFO_QUEUE_SIZE)return 0x1759;
 if(c->num_entries>=GPFIFO_QUEUE_SIZE-threshold)__real_nvGpuChannelKickoff(c);
 ++c->num_entries;return 0;
}
'''
main=r'''
int main(){NvGpuChannel channel;
 // Match a startup workload with many barriers and 8-entry AutoKick reserves.
 for(unsigned i=0;i<10000;++i)assert(__wrap_nvGpuChannelAppendEntry(&channel,0x12340000+i*4,17,0x80000200,8)==0);
 assert(kicks>0&&max_batch==2040&&logs==0);
 assert(last_start==0x12340000+9999*4&&last_commands==17&&last_flags==0x80000200);
 // Zero means no permission to split an atomic sequence.
 channel.num_entries=300;const auto old_kicks=kicks;
 assert(__wrap_nvGpuChannelAppendEntry(&channel,8,4,0,0)==0);
 assert(last_threshold==0&&channel.num_entries==301&&kicks==old_kicks);
 // Preserve every caller threshold, including stronger reserves and invalid values.
 __wrap_nvGpuChannelAppendEntry(&channel,8,4,0,2000);assert(last_threshold==2000);
 assert(__wrap_nvGpuChannelAppendEntry(&channel,8,4,0,2048)==0x1759&&last_threshold==2048);
 next_error=0x1234;
 assert(__wrap_nvGpuChannelAppendEntry(&channel,8,4,0,8)==0x1234);
 assert(__wrap_nvGpuChannelKickoff(&channel)==0x1234&&logs>0);
}
'''
with tempfile.TemporaryDirectory(prefix='deko-submit-') as d:
 p=Path(d);(p/'test.cpp').write_text(header+s+main)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Submission wrappers: unchanged AutoKick batches, atomic sequence preservation, unchanged command payloads and error propagation passed (CPU model; ASAN/UBSAN)')
