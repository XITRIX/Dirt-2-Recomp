// Runs a dumped D3D9 pixel shader (the game's Bink YUV->RGB shader) on three
// constant L8 textures with the game's constants and reads back the result.
// Usage: dxvk_video_shader <ps.dxso> <Y> <V> <U>
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <d3d9.h>

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<DWORD> code(65536);
    code.resize(std::fread(code.data(), 4, code.size(), f));
    std::fclose(f);
    const int val[3] = {std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4])};

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* win = SDL_CreateWindow("video shader test", 256, 256, SDL_WINDOW_VULKAN);
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

    const UINT sizes[3][2] = {{2048, 1024}, {1024, 512}, {1024, 512}};
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(dev->CreatePixelShader(code.data(), &ps))) {
        std::printf("CreatePixelShader failed\n");
        return 1;
    }
    // Two texture sets, alternated and re-filled with DISCARD every frame, as the game does.
    IDirect3DTexture9* tex[2][3];
    for (int set = 0; set < 2; ++set)
        for (int i = 0; i < 3; ++i)
            dev->CreateTexture(sizes[i][0], sizes[i][1], 1, D3DUSAGE_DYNAMIC, D3DFMT_L8, D3DPOOL_DEFAULT, &tex[set][i], nullptr);
    const int frames = std::getenv("FRAMES") ? std::atoi(std::getenv("FRAMES")) : 60;
    struct V { float x, y, z, rhw; DWORD color; float u, v; };
    const V quad[4] = {{-0.5f, -0.5f, 0, 1, 0xFFFFFFFF, 0, 0}, {255.5f, -0.5f, 0, 1, 0xFFFFFFFF, 0.625f, 0},
                       {-0.5f, 255.5f, 0, 1, 0xFFFFFFFF, 0, 0.703125f}, {255.5f, 255.5f, 0, 1, 0xFFFFFFFF, 0.625f, 0.703125f}};
    for (int frame = 0; frame < frames; ++frame) {
        const int set = frame & 1;
        for (int i = 0; i < 3; ++i) {
            D3DLOCKED_RECT lr{};
            tex[set][i]->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK);
            for (UINT y = 0; y < sizes[i][1]; ++y) std::memset(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, val[i], sizes[i][0]);
            tex[set][i]->UnlockRect(0);
            dev->SetTexture(i, tex[set][i]);
            dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        const float consts[8] = {1280, 720, 0, 0, 2048, 1024, 0, 0};  // vidSize c0, bufferSize c1
        dev->SetPixelShaderConstantF(0, consts, 2);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0);
        dev->BeginScene();
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetPixelShader(ps);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        dev->EndScene();
        if (frame + 1 < frames) dev->Present(nullptr, nullptr, nullptr, nullptr);
    }

    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    dev->GetRenderTarget(0, &bb);
    dev->CreateOffscreenPlainSurface(256, 256, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT out{};
    sys->LockRect(&out, nullptr, D3DLOCK_READONLY);
    const unsigned px = static_cast<unsigned*>(out.pBits)[128 * out.Pitch / 4 + 64] & 0xFFFFFF;
    sys->UnlockRect();
    std::printf("Y=%d V=%d U=%d -> pixel %06X\n", val[0], val[1], val[2], px);
    return 0;
}
