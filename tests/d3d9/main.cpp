// Standalone DXVK reference runner; no recompiled game or game data required.
#include "../../shims/d3d9/conformance.h"
#include <SDL3/SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "util/util_error.h"
namespace rt {
void log(const char* format, ...) {
    va_list ap; va_start(ap,format); std::vfprintf(stderr,format,ap); va_end(ap);
    std::fputc('\n',stderr);
}
}
int main(int argc,char** argv) {
    setenv("DIRT2_D3D9_TESTS",argc>1?argv[1]:"d3d9-reference",1);
    setenv("DXVK_WSI_DRIVER","SDL3",0);
    if(!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr,"%s\n",SDL_GetError()); return 1; }
    SDL_Window* window=SDL_CreateWindow("D3D9 regression tests",64,64,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window) { std::fprintf(stderr,"%s\n",SDL_GetError()); SDL_Quit(); return 1; }
    IDirect3D9* d3d=nullptr;
    try { d3d=Direct3DCreate9(D3D_SDK_VERSION); }
    catch(const dxvk::DxvkError& e) { std::fprintf(stderr,"DXVK initialization: %s\n",e.message().c_str()); }
    const unsigned failures=d3d?d3d9_tests::run_if_requested(d3d,0,D3DDEVTYPE_HAL,reinterpret_cast<HWND>(window),
        D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED):1;
    if(d3d) d3d->Release();
    SDL_DestroyWindow(window); SDL_Quit();
    return failures?1:0;
}
