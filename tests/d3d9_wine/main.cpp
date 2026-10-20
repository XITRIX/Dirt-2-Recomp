#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "runner.h"
#ifdef __SWITCH__
#include <switch.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <set>
void wine_switch_shutdown();
bool wine_switch_gpu_probe();
bool wine_switch_render_probe();
void wine_switch_fault_arm(unsigned,const char*);
void wine_switch_fault_cleanup_phase();
void wine_switch_fault_close();
void wine_switch_fault_timeout();
bool wine_switch_fault_report(unsigned,FILE*);
#endif
int main(int argc,char** argv) {
#ifdef __SWITCH__
    const std::string root="sdmc:/switch/d3d9-wine";
    std::filesystem::create_directories(root+"/results");
    setenv("DIRT2_DATA_DIR",root.c_str(),1);
    setenv("SDL_SWITCH_NATIVE_WINDOW","1",1);
    std::filesystem::create_directories(root+"/results/shaders");
    setenv("DIRT2_DEKO_CAPTURE_SHADERS",(root+"/results/shaders").c_str(),1);
    if(std::ifstream(root+"/results/render-probe.txt").good()) {
        std::freopen((root+"/results/render-probe.log").c_str(),"a",stderr);
        wine_switch_fault_arm(wine_case_count()+3,argv && argv[0] && *argv[0]?argv[0]:"sdmc:/switch/d3d9-wine/d3d9_wine.nro");
        std::mutex probe_mutex;std::condition_variable probe_changed;bool probe_finished=false;
        std::thread probe_watchdog([&]{
            std::unique_lock lock(probe_mutex);
            if(!probe_changed.wait_for(lock,std::chrono::seconds(90),[&]{return probe_finished;}))wine_switch_fault_timeout();
        });
        const bool rendering_ok=wine_switch_render_probe();
        {std::lock_guard lock(probe_mutex);probe_finished=true;}probe_changed.notify_one();probe_watchdog.join();
        if(rendering_ok && envHasNextLoad()) {
            const char* path=argv && argv[0] && *argv[0]?argv[0]:"sdmc:/switch/d3d9-wine/d3d9_wine.nro";
            const Result next_result=envSetNextLoad(path,path);
            std::fprintf(stderr,"PROBE rendering passed; resume Wine suite, next-load=0x%x\n",next_result);std::fflush(stderr);
        }
        std::fflush(nullptr);std::_Exit(0);
    }
    // Optional deployment-time diagnostic. Keep this separate from the case
    // cursor/results: a loader failure must not masquerade as a test failure.
    const std::string probe_progress=root+"/results/reload-probe.txt";
    unsigned probe_step=0;
    if(std::ifstream(probe_progress)>>probe_step) {
        std::freopen((root+"/results/reload-probe.log").c_str(),"a",stderr);
        std::fprintf(stderr,"RELOAD PROBE %u entered; next-load=%u, NSO=%u, last-load=0x%x\n",
            probe_step,unsigned(envHasNextLoad()),unsigned(envIsNso()),envGetLastLoadResult());
        if(probe_step>=3) {
            std::fprintf(stderr,"RELOAD PROBE complete; test progress unchanged\n");std::fflush(stderr);
            std::ofstream(root+"/results/reload-probe-complete.txt")<<"loader, SDL and GPU reloads completed\n";
            std::remove(probe_progress.c_str());return 0;
        }
        const char* path=argv && argv[0] && *argv[0]?argv[0]:"sdmc:/switch/d3d9-wine/d3d9_wine.nro";
        wine_switch_fault_arm(wine_case_count()+probe_step,path);
        if(probe_step>=1) {
            if(!SDL_Init(SDL_INIT_VIDEO)) {
                std::fprintf(stderr,"RELOAD PROBE SDL init failed: %s\n",SDL_GetError());return 2;
            }
            std::fprintf(stderr,"RELOAD PROBE SDL initialized\n");std::fflush(stderr);
        }
        if(probe_step>=2) {
            if(!wine_switch_gpu_probe())return 2;
            std::fprintf(stderr,"RELOAD PROBE GPU initialized\n");std::fflush(stderr);
            wine_switch_shutdown();
        }
        if(probe_step>=1)SDL_Quit();
        std::fprintf(stderr,"RELOAD PROBE %u cleanup completed\n",probe_step);std::fflush(stderr);
        if(!envHasNextLoad())return 2;
        Result reload_result=envSetNextLoad(path,path);
        std::fprintf(stderr,"RELOAD PROBE next-load result 0x%x\n",reload_result);std::fflush(stderr);
        if(R_FAILED(reload_result))return 2;
        {std::ofstream next(probe_progress);next<<probe_step+1;next.flush();if(!next)return 2;}
        std::fflush(nullptr);std::_Exit(0);
    }
    if(argc<=1) {
        const std::string progress=root+"/results/next.txt",pending=root+"/results/pending.txt";
        unsigned index=0,interrupted=0;std::ifstream(progress)>>index;
        std::set<unsigned> targets;
        std::ifstream target_file(root+"/reference-pass.txt");
        const bool selected_only=target_file.is_open();
        if(selected_only) {
            unsigned target;
            while(target_file>>target) { if(target>=wine_case_count())return 2;targets.insert(target); }
            if(!target_file.eof() || targets.empty())return 2;
        }
        FILE* report=std::fopen((root+"/results/results.jsonl").c_str(),"a");if(!report)return 2;
        if(std::ifstream(pending)>>interrupted) {
            if(const auto* test=wine_case_at(interrupted)) {
                unsigned timedout=~0u;std::ifstream(root+"/results/timeout.txt")>>timedout;
                const char* status=timedout==interrupted?"timeout":"interrupted";
                if(!wine_switch_fault_report(interrupted,report)) std::fprintf(report,"\n{\"index\":%u,\"name\":\"%s\",\"status\":\"%s\",\"message\":\"previous process did not finish; see suite.log / crash report\"}\n",interrupted,test->name,status);
                std::remove((root+"/results/timeout.txt").c_str());
                std::fflush(report);index=std::max(index,interrupted+1);
                std::remove(pending.c_str());
            }
        }
        while(index<wine_case_count() && (*wine_case_at(index)->platform_block || (selected_only && !targets.count(index)))) {
            if(selected_only && !targets.count(index)) {
                std::fprintf(report,"{\"index\":%u,\"name\":\"%s\",\"status\":\"not-selected\",\"message\":\"outside the Mac-passing target set\"}\n",index,wine_case_at(index)->name);
                std::fflush(report);
            } else wine_execute(index,report);
            ++index;std::ofstream(progress)<<index;
        }
        if(index>=wine_case_count()) { std::ofstream(root+"/results/complete.txt")<<wine_case_count()<<" entries processed\n";std::fclose(report);std::remove(pending.c_str());return 0; }
        std::freopen((root+"/results/suite.log").c_str(),"a",stderr);
        if(!envHasNextLoad()) {
            std::fprintf(stderr,"Loader does not support automatic next-load; rerun NRO to resume each case.\n");
        }
        { std::ofstream pendingFile(pending);pendingFile<<index;pendingFile.flush();if(!pendingFile)return 2; }
        { std::ofstream nextFile(progress);nextFile<<index+1;nextFile.flush();if(!nextFile)return 2; }
        const char* nro_path=argv && argv[0] && *argv[0]?argv[0]:"sdmc:/switch/d3d9-wine/d3d9_wine.nro";
        // Optional diagnostic runs must not silently add synchronization to
        // an ordinary correctness run. Keep the same test selection either way.
        if((index==65 || index==71 || index==82) && std::ifstream(root+"/pixel-probe.txt").good())
            setenv("DIRT2_DEKO_PIXEL_PROBE","1",1);
        wine_switch_fault_arm(index,nro_path);
        std::mutex mutex;std::condition_variable changed;bool finished=false;
        std::thread watchdog([&]{
            std::unique_lock lock(mutex);
            if(!changed.wait_for(lock,std::chrono::seconds(90),[&]{return finished;})) {
                wine_switch_fault_timeout();
            }
        });
        int result;
        if(!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr,"SDL: %s\n",SDL_GetError());result=2; }
        else result=wine_execute(index,report);
        wine_switch_fault_cleanup_phase();
        std::fprintf(stderr,"CLEANUP case %u\n",index);std::fflush(stderr);
        wine_switch_shutdown();
        SDL_Quit();
        std::fprintf(stderr,"SDL cleanup complete for case %u\n",index);std::fflush(stderr);
        { std::lock_guard lock(mutex);finished=true; }changed.notify_one();watchdog.join();
        std::fprintf(stderr,"Watchdog joined for case %u\n",index);std::fflush(stderr);
        std::fclose(report);if(result!=2)std::remove(pending.c_str());
        // Arm the next load only after all driver mappings and SDL services are
        // released. _Exit deliberately avoids destructing abandoned COM objects.
        if(result!=2 && envHasNextLoad()) {
            const char* path=argv && argv[0] && *argv[0]?argv[0]:"sdmc:/switch/d3d9-wine/d3d9_wine.nro";
            const Result reload_result=envSetNextLoad(path,path);
            std::fprintf(stderr,"Next-load result for case %u: 0x%x\n",index,reload_result);std::fflush(stderr);
            if(R_FAILED(reload_result))return 2;
        }
        std::fflush(nullptr);std::_Exit(0);
    }
#endif
    if(argc==2 && !std::strcmp(argv[1],"--list")) {
        for(unsigned i=0;i<wine_case_count();++i)std::printf("%u\t%s\t%s\n",i,wine_case_at(i)->name,wine_case_at(i)->platform_block);
        return 0;
    }
    if(argc!=4 || std::strcmp(argv[1],"--case")) { std::fprintf(stderr,"Usage: d3d9_wine --list | --case INDEX REPORT.jsonl\n");return 2; }
    unsigned index=unsigned(std::strtoul(argv[2],nullptr,10));if(!wine_case_at(index))return 2;
    setenv("DXVK_WSI_DRIVER","SDL3",0);
    if(!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr,"SDL: %s\n",SDL_GetError());return 2; }
    FILE* report=std::fopen(argv[3],"a");if(!report)return 2;
    int result=wine_execute(index,report);std::fclose(report);
    // A failed assertion may leave COM references alive. Process isolation is
    // intentional: do not run another case or tear down SDL under that device.
    std::fflush(nullptr);std::_Exit(result);
}
