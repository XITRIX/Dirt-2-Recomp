// Smoke test for the graphics stack on Apple: SDL3 window -> DXVK d3d9 ->
// MoltenVK -> Metal. Clears the back buffer, presents, reads a pixel back.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>

#include <d3d9.h>

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("DXVK smoke test", 640, 360, SDL_WINDOW_VULKAN);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        std::fprintf(stderr, "Direct3DCreate9 failed\n");
        return 1;
    }
    D3DADAPTER_IDENTIFIER9 id{};
    d3d->GetAdapterIdentifier(0, 0, &id);
    std::printf("adapter: %s (vendor %04X device %04X)\n", id.Description, id.VendorId, id.DeviceId);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 360;
    pp.hDeviceWindow = reinterpret_cast<HWND>(win);
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, reinterpret_cast<HWND>(win),
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateDevice failed: 0x%08X\n", unsigned(hr));
        return 1;
    }

    const int frames = std::getenv("SMOKE_FRAMES") ? std::atoi(std::getenv("SMOKE_FRAMES")) : 120;
    for (int i = 0; i < frames; ++i) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) i = frames;
        const D3DCOLOR col = D3DCOLOR_XRGB(32, uint8_t(64 + i % 128), 160);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, col, 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }

    // Read back the last frame's colour.
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(11, 22, 33), 1.0f, 0);
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* sys = nullptr;
    dev->GetRenderTarget(0, &bb);
    dev->CreateOffscreenPlainSurface(640, 360, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    hr = dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr{};
    unsigned pixel = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        pixel = *static_cast<unsigned*>(lr.pBits) & 0xFFFFFF;
        sys->UnlockRect();
    }
    std::printf("readback pixel: %06X (expected 0B1621)\n", pixel);

    sys->Release();
    bb->Release();
    dev->Release();
    d3d->Release();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return pixel == 0x0B1621 ? 0 : 2;
}
