// Runs a dumped D3D9 pixel shader (the game's Bink YUV->RGB shader) on three
// constant L8 textures with the game's constants and reads back the result.
// Usage: dxvk_video_shader <ps.dxso> <Y> <V> <U> [vs.dxso]
// PLANES=<prefix> fills the textures from <prefix>_{y,cr,cb}.raw (real decoded
// frames, see tests/video) instead of constants; OUT=<file.ppm> dumps the frame.
// With a vertex shader, draws like the game: vertex declaration with
// position / texcoord / colour in three streams.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

    const UINT sizes[3][2] = {{2048, 1024}, {1024, 512}, {1024, 512}};
    std::vector<uint8_t> planes[3];
    if (const char* prefix = std::getenv("PLANES")) {
        const char* names[3] = {"y", "cr", "cb"};
        for (int i = 0; i < 3; ++i) {
            planes[i].resize(size_t(sizes[i][0]) * sizes[i][1]);
            FILE* pf = std::fopen((std::string(prefix) + "_" + names[i] + ".raw").c_str(), "rb");
            if (!pf || std::fread(planes[i].data(), 1, planes[i].size(), pf) != planes[i].size()) return 2;
            std::fclose(pf);
        }
    }
    const UINT bw = planes[0].empty() ? 256 : 1280, bh = planes[0].empty() ? 256 : 720;

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* win = SDL_CreateWindow("video shader test", int(bw), int(bh), SDL_WINDOW_VULKAN);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = bw;
    pp.BackBufferHeight = bh;
    pp.hDeviceWindow = reinterpret_cast<HWND>(win);
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow, (std::getenv("DEVFLAGS") ? DWORD(std::strtoul(std::getenv("DEVFLAGS"), nullptr, 16)) : DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING)), &pp, &dev))) return 1;

    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(dev->CreatePixelShader(code.data(), &ps))) {
        std::printf("CreatePixelShader failed\n");
        return 1;
    }
    // Two texture sets, alternated and re-filled with DISCARD every frame, as the game does.
    IDirect3DTexture9* tex[2][3];
    for (int set = 0; set < 2; ++set)
        for (int i = 0; i < 3; ++i)
            dev->CreateTexture(sizes[i][0], sizes[i][1], 1, D3DUSAGE_DYNAMIC, std::getenv("FMT") ? D3DFORMAT(std::atoi(std::getenv("FMT"))) : D3DFMT_L8, D3DPOOL_DEFAULT, &tex[set][i], nullptr);
    const int frames = std::getenv("FRAMES") ? std::atoi(std::getenv("FRAMES")) : 60;
    struct V { float x, y, z, rhw; DWORD color; float u, v; };
    const float qx = float(bw) - 0.5f, qy = float(bh) - 0.5f;
    const V quad[4] = {{-0.5f, -0.5f, 0, 1, 0xFFFFFFFF, 0, 0}, {qx, -0.5f, 0, 1, 0xFFFFFFFF, 0.625f, 0},
                       {-0.5f, qy, 0, 1, 0xFFFFFFFF, 0, 0.703125f}, {qx, qy, 0, 1, 0xFFFFFFFF, 0.625f, 0.703125f}};
    for (int frame = 0; frame < frames; ++frame) {
        const int set = frame & 1;
        for (int i = 0; i < 3; ++i) {
            D3DLOCKED_RECT lr{};
            tex[set][i]->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD | D3DLOCK_NOSYSLOCK);
            for (UINT y = 0; y < sizes[i][1]; ++y) {
                uint8_t* row = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
                if (planes[i].empty())
                    std::memset(row, val[i], sizes[i][0]);
                else
                    std::memcpy(row, planes[i].data() + size_t(y) * sizes[i][0], sizes[i][0]);
            }
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
        dev->SetPixelShader(ps);
        if (argc > 5) {
            static IDirect3DVertexShader9* vs = nullptr;
            static IDirect3DVertexDeclaration9* decl = nullptr;
            static IDirect3DVertexBuffer9* vb[3] = {};
            if (!vs) {
                FILE* vf = std::fopen(argv[5], "rb");
                std::vector<DWORD> vcode(65536);
                vcode.resize(std::fread(vcode.data(), 4, vcode.size(), vf));
                std::fclose(vf);
                dev->CreateVertexShader(vcode.data(), &vs);
                const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0},
                                                {1, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0},
                                                {2, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
                dev->CreateVertexDeclaration(el, &decl);
                const float pos[] = {1, -0.75f, 1, -1, -0.75f, 1, 1, 0.75f, 1, -1, 0.75f, 1};
                const float uv[] = {0.625f, 0.703125f, 0, 0.703125f, 0.625f, 0, 0, 0};
                const float col[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
                const void* data[3] = {pos, uv, col};
                const UINT size[3] = {sizeof pos, sizeof uv, sizeof col};
                for (int i = 0; i < 3; ++i) {
                    dev->CreateVertexBuffer(size[i], D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb[i], nullptr);
                    void* p = nullptr;
                    vb[i]->Lock(0, 0, &p, 0);
                    std::memcpy(p, data[i], size[i]);
                    vb[i]->Unlock();
                }
            }
            const UINT strides[3] = {12, 8, 16};
            for (int i = 0; i < 3; ++i) dev->SetStreamSource(i, vb[i], 0, strides[i]);
            dev->SetVertexDeclaration(decl);
            dev->SetVertexShader(vs);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            // RS="k=v k=v ..." (hex values) / SS="k=v ..." replay the game's state.
            auto apply = [](const char* env, auto&& set) {
                if (const char* e = std::getenv(env)) {
                    const char* p = e;
                    while (*p) {
                        char* end = nullptr;
                        const unsigned long k = std::strtoul(p, &end, 10);
                        if (*end != '=') break;
                        const unsigned long v = std::strtoul(end + 1, &end, 16);
                        set(DWORD(k), DWORD(v));
                        p = end;
                        while (*p == ' ') ++p;
                    }
                }
            };
            apply("RS", [&](DWORD k, DWORD v) { dev->SetRenderState(D3DRENDERSTATETYPE(k), v); });
            for (DWORD st = 0; st < 3; ++st) apply("SS", [&](DWORD k, DWORD v) { dev->SetSamplerState(st, D3DSAMPLERSTATETYPE(k), v); });
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        } else {
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        }
        dev->EndScene();
        if (frame + 1 < frames) dev->Present(nullptr, nullptr, nullptr, nullptr);
    }

    IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
    dev->GetRenderTarget(0, &bb);
    dev->CreateOffscreenPlainSurface(bw, bh, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT out{};
    sys->LockRect(&out, nullptr, D3DLOCK_READONLY);
    const unsigned px = static_cast<unsigned*>(out.pBits)[(bh / 2) * out.Pitch / 4 + bw / 4] & 0xFFFFFF;
    if (const char* path = std::getenv("OUT")) {
        FILE* of = std::fopen(path, "wb");
        std::fprintf(of, "P6\n%u %u\n255\n", bw, bh);
        for (UINT y = 0; y < bh; ++y)
            for (UINT x = 0; x < bw; ++x) {
                const unsigned c = static_cast<unsigned*>(out.pBits)[y * out.Pitch / 4 + x];
                const uint8_t rgb[3] = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
                std::fwrite(rgb, 1, 3, of);
            }
        std::fclose(of);
    }
    sys->UnlockRect();
    std::printf("Y=%d V=%d U=%d -> pixel %06X\n", val[0], val[1], val[2], px);
    return 0;
}
