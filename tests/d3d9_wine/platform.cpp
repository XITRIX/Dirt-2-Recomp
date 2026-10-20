#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <SDL3/SDL.h>
#include "platform_compat.h"
#undef min
#undef max
// Geometry helpers are exact value operations. APIs requiring GDI/desktop
// ownership abort the case as a platform skip rather than returning fake data.
extern "C" void wine_platform_skip(const char*);
extern "C" BOOL AdjustWindowRect(RECT*,DWORD,BOOL) { return TRUE; }
#ifdef __SWITCH__
// HOS has one physical surface, but D3D9 can own multiple logical windows and
// devices. Keep distinct HWND identities while sharing SDL's physical window.
namespace { struct LogicalWindow { int width,height; }; SDL_Window* physical_window=nullptr; unsigned logical_windows=0; }
#endif
extern "C" HWND CreateWindowA(const char*,const char*,DWORD,int,int,int w,int h,HWND,HANDLE,HINSTANCE,void*) {
    SDL_WindowFlags flags=SDL_WINDOW_HIDDEN;
#ifndef __SWITCH__
    flags|=SDL_WINDOW_VULKAN;
#endif
#ifdef __SWITCH__
    if(!physical_window)physical_window=SDL_CreateWindow("Wine D3D9 test",w>0?w:640,h>0?h:480,flags);
    if(!physical_window){std::fprintf(stderr,"Wine test window creation failed: %s\n",SDL_GetError());return nullptr;}
    ++logical_windows;
    return reinterpret_cast<HWND>(new LogicalWindow{w>0?w:640,h>0?h:480});
#else
    auto* window=SDL_CreateWindow("Wine D3D9 test",w>0?w:640,h>0?h:480,flags);
    if(!window) std::fprintf(stderr,"Wine test window creation failed: %s\n",SDL_GetError());
    return reinterpret_cast<HWND>(window);
#endif
}
extern "C" BOOL DestroyWindow(HWND window) {
#ifdef __SWITCH__
    if(!window)return FALSE;
    delete reinterpret_cast<LogicalWindow*>(window);
    if(!--logical_windows){SDL_DestroyWindow(physical_window);physical_window=nullptr;}
#else
    SDL_DestroyWindow(reinterpret_cast<SDL_Window*>(window));
#endif
    return TRUE;
}
extern "C" HWND GetDesktopWindow() { wine_platform_skip("GetDesktopWindow requires a Windows desktop");return nullptr; }
extern "C" HDC GetDC(HWND) { wine_platform_skip("GetDC requires Windows GDI");return nullptr; }
extern "C" BOOL SetRect(RECT* r,int l,int t,int right,int b) { *r={l,t,right,b};return TRUE; }
extern "C" BOOL EqualRect(const RECT* a,const RECT* b) { return !std::memcmp(a,b,sizeof *a); }
extern "C" BOOL PtInRect(const RECT* r,POINT p) { return p.x>=r->left && p.x<r->right && p.y>=r->top && p.y<r->bottom; }
extern "C" BOOL QueryPerformanceCounter(LARGE_INTEGER* n) { n->QuadPart=SDL_GetPerformanceCounter();return TRUE; }
extern "C" BOOL QueryPerformanceFrequency(LARGE_INTEGER* n) { n->QuadPart=SDL_GetPerformanceFrequency();return TRUE; }
extern "C" void Sleep(DWORD ms) { SDL_Delay(ms); }
extern "C" DWORD GetTickCount() { return DWORD(SDL_GetTicks()); }
extern "C" DWORD GetCurrentThreadId() { return DWORD(SDL_GetCurrentThreadID()); }
extern "C" LONG InterlockedIncrement(LONG* p) { return __atomic_add_fetch(p,1,__ATOMIC_SEQ_CST); }
