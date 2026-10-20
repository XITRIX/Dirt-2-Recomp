// Best-effort fault recording without allocator, stdio, or backend locks in the
// exception handler. Never reload an NRO after an arbitrary memory fault: only
// terminating the Horizon process reclaims unknown driver/memory state safely.
#include <switch.h>
#include <switch/nro.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "runner.h"
int main(int,char**);
namespace {
struct FaultRecord {
    uint32_t magic,version,index,phase;
    uint64_t main_address,pc,lr,far,sp;
    uint32_t esr,error;
    uint8_t build_id[32];
    uint64_t registers[29],fp;
};
static_assert(sizeof(FaultRecord)==336);
constexpr uint32_t magic=0x57435231,timeout_error=0xffffffffu;
FaultRecord record{};
FsFile fault_file{};
bool opened=false;
std::atomic<uint32_t> phase{1};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
std::atomic_flag recording=ATOMIC_FLAG_INIT;
void write_fault(const ThreadExceptionDump* ctx) {
    if(!opened || recording.test_and_set(std::memory_order_relaxed))return;
    if(ctx) {
        record.pc=ctx->pc.x;record.lr=ctx->lr.x;record.far=ctx->far.x;record.sp=ctx->sp.x;
        record.esr=ctx->esr;record.error=ctx->error_desc;record.fp=ctx->fp.x;
        for(unsigned i=0;i<29;++i)record.registers[i]=ctx->cpu_gprs[i].x;
    } else record.error=timeout_error;
    record.phase=phase.load(std::memory_order_relaxed);
    record.magic=magic;
    // Raw FS service calls avoid stdio locks potentially held by the faulting
    // thread. If this fails, pending.txt + Atmosphere remain the fallback.
    fsFileWrite(&fault_file,0,&record,sizeof record,FsWriteOption_Flush);
}
}
void wine_switch_fault_arm(unsigned index,const char* nro_path) {
    phase.store(1,std::memory_order_relaxed);recording.clear(std::memory_order_relaxed);
    record={};record.version=1;record.index=index;record.phase=1;
    record.main_address=reinterpret_cast<uintptr_t>(&main);
    FILE* f=std::fopen(nro_path,"rb");
    if(!f)f=std::fopen("sdmc:/switch/d3d9-wine/d3d9_wine.nro","rb");
    if(f) {
        NroHeader header{};
        if(!std::fseek(f,sizeof(NroStart),SEEK_SET) && std::fread(&header,sizeof header,1,f)==1 && header.magic==NROHEADER_MAGIC)
            std::memcpy(record.build_id,header.build_id,sizeof record.build_id);
        std::fclose(f);
    }
    char path[128];std::snprintf(path,sizeof path,"/switch/d3d9-wine/results/fault-%04u.bin",index);
    auto* fs=fsdevGetDeviceFileSystem("sdmc");
    if(!fs)return;
    fsFsCreateFile(fs,path,sizeof record,0);
    if(R_FAILED(fsFsOpenFile(fs,path,FsOpenMode_Write,&fault_file)))return;
    if(R_FAILED(fsFileSetSize(&fault_file,sizeof record)) || R_FAILED(fsFileWrite(&fault_file,0,&record,sizeof record,FsWriteOption_Flush))) {
        fsFileClose(&fault_file);return;
    }
    opened=true;
}
void wine_switch_fault_cleanup_phase() { phase.store(2,std::memory_order_relaxed); }
void wine_switch_fault_close() { if(opened) { opened=false;fsFileClose(&fault_file); } }
void wine_switch_fault_timeout() { write_fault(nullptr);svcExitProcess(); }
bool wine_switch_fault_report(unsigned index,FILE* report) {
    char path[160];std::snprintf(path,sizeof path,"sdmc:/switch/d3d9-wine/results/fault-%04u.bin",index);
    FaultRecord previous{};
    FILE* file=std::fopen(path,"rb");if(!file)return false;
    const bool valid=std::fread(&previous,sizeof previous,1,file)==1 && previous.magic==magic && previous.version==1 && previous.index==index;
    std::fclose(file);if(!valid)return false;
    const auto* test=wine_case_at(index);if(!test)return false;
    char build[65];for(unsigned i=0;i<32;++i)std::snprintf(build+i*2,3,"%02x",previous.build_id[i]);
    std::fprintf(report,"\n{\"index\":%u,\"name\":\"%s\",\"status\":\"%s\",\"phase\":\"%s\",\"build_id\":\"%s\",\"exception\":%u,\"esr\":%u,\"pc_main_offset\":%lld,\"lr_main_offset\":%lld,\"fault_address\":\"0x%llx\",\"message\":\"%s; raw registers in fault-%04u.bin\"}\n",
        index,test->name,previous.error==timeout_error?"timeout":"crash",previous.phase==2?"cleanup":"test",build,previous.error,previous.esr,
        static_cast<long long>(previous.pc-previous.main_address),static_cast<long long>(previous.lr-previous.main_address),static_cast<unsigned long long>(previous.far),
        previous.error==timeout_error?"90 second watchdog expired":"native CPU fault",index);
    std::fflush(report);return true;
}
extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size=sizeof __nx_exception_stack;
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    write_fault(ctx);
    // Preserve the OS/Atmosphere crash path too; nonzero result leaves the
    // exception unhandled, rather than resuming at its faulting instruction.
    svcReturnFromException(MAKERESULT(Module_Libnx,LibnxError_BadInput));
}
// _Exit still enters libnx's service cleanup. Record reaching that boundary
// before the SD mount is removed; previously even SDL/join/next-load faults
// happened after our raw fault file had already been closed.
void userAppExit() {
    std::fprintf(stderr,"Runner entered libnx service cleanup\n");std::fflush(stderr);
    wine_switch_fault_close();
}
}
