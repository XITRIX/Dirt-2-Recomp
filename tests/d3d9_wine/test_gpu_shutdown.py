#!/usr/bin/env python3
"""Exercise actual final-teardown methods with dependency-checking driver mocks."""
import os, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
source = (root/'shims/d3d9/deko/gpu.cpp').read_text()
def method(signature):
    start=source.index(signature); opening=source.index('{',start); depth=1; end=opening+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]
header = r'''
#include <cassert>
#include <vector>
#include <functional>
#include <set>
using DkMemBlock=int; using DkDevice=int; using DkQueue=int; using DkCmdBuf=int; using DkSwapchain=int;
std::set<int> live; bool idle=false,queueAlive=true,cmdAlive=true,swapAlive=true,deviceAlive=true;
void dkQueueWaitIdle(int){idle=true;}
void dkSwapchainDestroy(int){assert(idle);swapAlive=false;}
void dkCmdBufDestroy(int){assert(idle);cmdAlive=false;}
void dkQueueDestroy(int){assert(idle&&!swapAlive&&!cmdAlive);queueAlive=false;}
void dkMemBlockDestroy(int b){assert(idle&&!queueAlive&&deviceAlive);assert(live.erase(b)==1);}
void dkDeviceDestroy(int){assert(live.empty());deviceAlive=false;}
namespace rt {void log(const char*){}}
struct Pool {struct Block{int mem;};std::vector<Block> blocks_;unsigned long long allocated_=0;void destroy();};
struct Ring {int used_=99,mem_=0,size_=32;std::vector<int> overflow_;void reset();void destroy();};
struct Ids {explicit Ids(unsigned){};};
// Pointer handles in the implementation are represented by nullable handles.
struct Gpu {
    void* device=(void*)1;void* queue=(void*)2;void* cmd=(void*)3;void* swapchain=(void*)4;
    void* swap_mem=(void*)5;void* descriptors=(void*)6;void* code=(void*)7;
    struct Frame{std::vector<std::function<void()>> on_done;Ring transient;std::vector<int> cmd_extra;int cmd_mem=0;bool submitted=true;};
    Frame frames[2];Pool buffers,images,readback;static constexpr unsigned kMaxImages=8192,kMaxSamplers=2048;
    Ids image_ids{kMaxImages},sampler_ids{kMaxSamplers};unsigned code_used=99;int frame=1;void shutdown();
};
void dkQueueWaitIdle(void*){dkQueueWaitIdle(1);}
void dkSwapchainDestroy(void*){dkSwapchainDestroy(1);}
void dkCmdBufDestroy(void*){dkCmdBufDestroy(1);}
void dkQueueDestroy(void*){dkQueueDestroy(1);}
void dkMemBlockDestroy(void* b){dkMemBlockDestroy(int((long)b));}
void dkDeviceDestroy(void*){dkDeviceDestroy(1);}
'''
# Mock low-level handles are integers; the method uses nullptr for these fields.
methods='\n'.join(method(x) for x in ('void Pool::destroy()', 'void Ring::reset()', 'void Ring::destroy()', 'void Gpu::shutdown()'))
methods=methods.replace('mem_ = nullptr','mem_ = 0').replace('f.cmd_mem = nullptr','f.cmd_mem = 0')
main=r'''
int main(){
    Gpu empty;empty.device=nullptr;empty.shutdown();assert(!idle);
    Gpu g;for(int i=5;i<=19;++i)live.insert(i);
    g.frames[0].transient.mem_=8;g.frames[0].transient.overflow_={9,10};g.frames[0].cmd_extra={11};g.frames[0].cmd_mem=12;
    g.frames[1].transient.mem_=13;g.frames[1].transient.overflow_={14};g.frames[1].cmd_extra={15};g.frames[1].cmd_mem=16;
    g.buffers.blocks_={{17}};g.images.blocks_={{18}};g.readback.blocks_={{19}};
    bool deferred=false;g.frames[0].on_done.push_back([&]{assert(live.count(17));deferred=true;});
    g.shutdown();assert(deferred&&!deviceAlive&&live.empty()&&!g.device&&!g.queue&&!g.cmd&&!g.swapchain);
    assert(g.buffers.blocks_.empty()&&g.frames[0].on_done.empty()&&!g.frames[1].submitted);
    g.shutdown();assert(live.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='wine-gpu-shutdown-') as directory:
    p=Path(directory);(p/'test.cpp').write_text(header+methods+main)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Actual GPU teardown methods: dependency order, deferred frees, abandoned allocations, empty/idempotent teardown passed (ASAN/UBSAN)')
