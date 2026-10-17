// Texture sampling check for DXVK on MoltenVK: fills a texture with a known
// value, draws it full screen with the fixed-function pipeline and reads the
// result back. Usage: dxvk_texture [L8|ARGB] [dynamic|managed]
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include <d3d9.h>

int main(int argc, char** argv) {
    const bool l8 = argc < 2 || !std::strcmp(argv[1], "L8");
    const bool dynamic = argc < 3 || !std::strcmp(argv[2], "dynamic");
    const bool via_surface = argc > 3 && !std::strcmp(argv[3], "surface");  // lock through GetSurfaceLevel, like the game
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* win = SDL_CreateWindow("texture test", 256, 256, SDL_WINDOW_VULKAN);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.hDeviceWindow = reinterpret_cast<HWND>(win);
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) return 1;

    IDirect3DTexture9* tex = nullptr;
    const D3DFORMAT fmt = l8 ? D3DFMT_L8 : D3DFMT_A8R8G8B8;
    HRESULT hr = dev->CreateTexture(2048, 1024, 1, dynamic ? D3DUSAGE_DYNAMIC : 0, fmt,
                                    dynamic ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &tex, nullptr);
    if (FAILED(hr)) {
        std::printf("CreateTexture failed %08X\n", unsigned(hr));
        return 1;
    }
    D3DLOCKED_RECT lr{};
    const DWORD lf = dynamic ? D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK : 0;
    IDirect3DSurface9* level = nullptr;
    if (via_surface) {
        tex->GetSurfaceLevel(0, &level);
        level->LockRect(&lr, nullptr, lf);
    } else {
        tex->LockRect(0, &lr, nullptr, lf);
    }
    for (int y = 0; y < 1024; ++y) {
        uint8_t* row = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
        if (l8) std::memset(row, 200, 2048);
        else
            for (int x = 0; x < 2048; ++x) reinterpret_cast<uint32_t*>(row)[x] = 0xFFC8C8C8;
    }
    if (via_surface) {
        level->UnlockRect();
        level->Release();
    } else {
        tex->UnlockRect(0);
    }

    struct V { float x, y, z, rhw, u, v; };
    const V quad[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {255.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, 255.5f, 0, 1, 0, 1}, {255.5f, 255.5f, 0, 1, 1, 1}};
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0);
    dev->BeginScene();
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    dev->EndScene();

    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    dev->GetRenderTarget(0, &bb);
    dev->CreateOffscreenPlainSurface(256, 256, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT out{};
    sys->LockRect(&out, nullptr, D3DLOCK_READONLY);
    const unsigned px = static_cast<unsigned*>(out.pBits)[128 * out.Pitch / 4 + 128] & 0xFFFFFF;
    sys->UnlockRect();
    std::printf("%s %s%s: centre pixel %06X (expected C8C8C8)\n", l8 ? "L8" : "ARGB", dynamic ? "dynamic" : "managed", via_surface ? " via surface" : "", px);
    return px == 0xC8C8C8 ? 0 : 2;
}
