#!/usr/bin/env python3
"""Test actual Switch fault recorder/report importer against mocked FS/SVC APIs."""
from pathlib import Path
import os,subprocess,tempfile,json
root=Path(__file__).resolve().parents[2]
header=r'''
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using Result=int;
#define R_FAILED(x) ((x)<0)
#define MAKERESULT(a,b) 1
constexpr int Module_Libnx=1,LibnxError_BadInput=1,FsOpenMode_Write=1,FsWriteOption_Flush=1;
union CpuRegister{uint64_t x;};
struct ThreadExceptionDump{uint32_t error_desc;CpuRegister cpu_gprs[29],fp,lr,sp,pc,far;uint32_t esr;};
struct FsFile{FILE*f=nullptr;};struct FsFileSystem{};
inline FsFileSystem*fsdevGetDeviceFileSystem(const char*){static FsFileSystem fs;return &fs;}
inline Result fsFsCreateFile(FsFileSystem*,const char*path,int64_t,unsigned){char p[256];snprintf(p,sizeof p,"sdmc:%s",path);if(FILE*f=fopen(p,"wb")){fclose(f);return 0;}return -1;}
inline Result fsFsOpenFile(FsFileSystem*,const char*path,unsigned,FsFile*out){char p[256];snprintf(p,sizeof p,"sdmc:%s",path);out->f=fopen(p,"r+b");return out->f?0:-1;}
inline Result fsFileSetSize(FsFile*,int64_t){return 0;}
inline Result fsFileWrite(FsFile*f,int64_t offset,const void*buf,uint64_t size,unsigned){if(fseek(f->f,offset,SEEK_SET))return -1;auto n=fwrite(buf,1,size,f->f);fflush(f->f);return n==size?0:-1;}
inline void fsFileClose(FsFile*f){fclose(f->f);f->f=nullptr;}
struct FaultExit{};struct TimeoutExit{};
[[noreturn]]inline void svcReturnFromException(Result){throw FaultExit{};}
[[noreturn]]inline void svcExitProcess(){throw TimeoutExit{};}
'''
main=r'''
#include <cassert>
#include <filesystem>
extern "C" const WineCase*wine_case_at(unsigned index){static WineCase cases[]={{"test.one","device",""},{"test.two","device",""},{"test.three","device",""}};return index<3?&cases[index]:nullptr;}
int main(int,char**){
 std::filesystem::create_directories("sdmc:/switch/d3d9-wine/results");
 NroStart start{};NroHeader header{};header.magic=NROHEADER_MAGIC;for(unsigned i=0;i<32;++i)header.build_id[i]=i;
 FILE*nro=fopen("runner.nro","wb");fwrite(&start,sizeof start,1,nro);fwrite(&header,sizeof header,1,nro);fclose(nro);
 FILE*out=fopen("results.jsonl","w");
 wine_switch_fault_arm(0,"runner.nro");assert(!wine_switch_fault_report(0,out));
 ThreadExceptionDump ctx{};ctx.pc.x=(uintptr_t)&main+256;ctx.lr.x=(uintptr_t)&main+128;ctx.error_desc=257;ctx.esr=42;ctx.far.x=0xdeadbeef;ctx.cpu_gprs[28].x=99;ctx.fp.x=77;
 wine_switch_fault_cleanup_phase();
 try{__libnx_exception_handler(&ctx);assert(false);}catch(FaultExit&){}
 wine_switch_fault_close();assert(wine_switch_fault_report(0,out));
 FILE*raw=fopen("sdmc:/switch/d3d9-wine/results/fault-0000.bin","rb");FaultRecord saved{};assert(fread(&saved,sizeof saved,1,raw)==1);fclose(raw);assert(saved.registers[28]==99&&saved.fp==77&&saved.phase==2);
 wine_switch_fault_arm(1,"runner.nro");try{wine_switch_fault_timeout();assert(false);}catch(TimeoutExit&){}
 wine_switch_fault_close();assert(wine_switch_fault_report(1,out));
 wine_switch_fault_arm(2,"runner.nro");wine_switch_fault_close();assert(!wine_switch_fault_report(2,out));
 assert(!wine_switch_fault_report(999,out));fclose(out);
}
'''
with tempfile.TemporaryDirectory(prefix='wine-fault-capture-') as directory:
 p=Path(directory);(p/'switch').mkdir();(p/'switch.h').write_text(header)
 (p/'switch/nro.h').write_text(Path('/opt/devkitpro/libnx/include/switch/nro.h').read_text())
 (p/'test.cpp').write_text('#include "'+str(root/'tests/d3d9_wine/switch_fault.cpp')+'"\n'+main)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I'+str(p),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],cwd=p,check=True)
 rows=[json.loads(x) for x in (p/'results.jsonl').read_text().splitlines() if x.strip()]
 assert len(rows)==2 and rows[0]['status']=='crash' and rows[0]['phase']=='cleanup' and rows[0]['pc_main_offset']==256 and rows[0]['lr_main_offset']==128
 assert rows[0]['fault_address']=='0xdeadbeef' and rows[0]['esr']==42 and rows[0]['build_id']==bytes(range(32)).hex()
 assert rows[1]['status']=='timeout' and rows[1]['phase']=='test'
 (p/'inventory.json').write_text(json.dumps({'revision':'fixture','cases':[{'name':'test.one'},{'name':'test.two'},{'name':'test.three'}]}))
 # Raw fault import also works before the next console launch has appended JSON.
 (p/'sdmc:/switch/d3d9-wine/results/results.jsonl').write_text('')
 subprocess.run(['python3',str(root/'tools/scripts/summarize_wine_d3d9_tests.py'),str(p/'inventory.json'),str(p/'sdmc:/switch/d3d9-wine/results/results.jsonl'),'--out',str(p/'summary.json')],check=True,stdout=subprocess.DEVNULL)
 summary=json.loads((p/'summary.json').read_text())
 assert summary['counts']=={'crash':1,'timeout':1,'not-run':1} and summary['cases'][0]['pc_main_offset']==256
 assert len(summary['failure_signature_groups'])==2

print('Actual fault capture/import: exception registers, build ID, cleanup/test phase, timeout, invalid/unwritten record rejection passed (ASAN/UBSAN with FS/SVC mocks)')
