#pragma once
#include <d3d9.h>

namespace d3d9_tests {
// Opt-in, once per process. Uses its own device before the game device exists.
unsigned run_if_requested(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags);
}
