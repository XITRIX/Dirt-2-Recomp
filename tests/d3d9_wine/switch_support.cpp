// Minimal standalone services for the real deko backend; no game/recompiler.
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "../../shims/d3d9/deko/gpu.h"
#include "../../platform/platform.h"
void wine_switch_shutdown() { deko::gpu().shutdown(); }
bool wine_switch_gpu_probe() { return deko::gpu().init(); }
namespace rt {
void log(const char* format,...) { va_list ap;va_start(ap,format);std::vfprintf(stderr,format,ap);va_end(ap);std::fputc('\n',stderr);std::fflush(stderr); }
uint32_t current_thread_id() { return 1; }
}
namespace shim {
bool gpu_dump_active() { return false; }
const std::string& gpu_dump_directory() { static const std::string empty;return empty; }
void gpu_dump_error(const char* message) { rt::log("capture unavailable in standalone suite: %s",message); }
}
namespace platform {
void display_size(int* w,int* h) { *w=1280;*h=720; }
const std::vector<DisplayMode>& display_modes() {
    static const std::vector<DisplayMode> modes={{640,480,60,.5f},{1280,720,60,1}};return modes;
}
}
