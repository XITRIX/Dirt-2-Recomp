#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
#define WS_OVERLAPPEDWINDOW 0x00cf0000
#define WS_MAXIMIZE 0x01000000
#define WS_CAPTION 0x00c00000
#define WS_VISIBLE 0x10000000
#define CW_USEDEFAULT 0
#undef THIS
#undef THIS_
#define THIS INTERFACE *This
#define THIS_ INTERFACE *This,
#define LOWORD(v) ((WORD)(uintptr_t)(v))
#define HIWORD(v) ((WORD)((uintptr_t)(v)>>16))
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
BOOL AdjustWindowRect(RECT*,DWORD,BOOL);
HWND CreateWindowA(const char*,const char*,DWORD,int,int,int,int,HWND,HANDLE,HINSTANCE,void*);
BOOL DestroyWindow(HWND);
HWND GetDesktopWindow(void);
HDC GetDC(HWND);
BOOL SetRect(RECT*,int,int,int,int);
BOOL PtInRect(const RECT*,POINT);
BOOL QueryPerformanceCounter(LARGE_INTEGER*);
BOOL QueryPerformanceFrequency(LARGE_INTEGER*);
void Sleep(DWORD);
typedef uintptr_t DWORD_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef struct { HWND hwnd,hwndInsertAfter;int x,y,cx,cy;UINT flags; } WINDOWPOS;
typedef struct { unsigned dmSize,dmPelsWidth,dmPelsHeight;POINT dmPosition; } DEVMODEW;
typedef struct { int unused; } D3D9ON12_ARGS;
typedef unsigned D3D_FEATURE_LEVEL;
DWORD GetTickCount(void);
DWORD GetCurrentThreadId(void);
LONG InterlockedIncrement(LONG*);
#ifndef CALLBACK
#define CALLBACK
#endif
#define IUnknown_QueryInterface(p,a,b) (p)->lpVtbl->QueryInterface(p,a,b)
BOOL EqualRect(const RECT*,const RECT*);

#ifdef __cplusplus
}
#endif
