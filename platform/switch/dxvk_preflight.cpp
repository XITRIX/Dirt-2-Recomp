// Small native DX11 test: no guest executable, COM bridge or game shaders.
#include <switch.h>
#include <SDL3/SDL.h>
#include <d3d11.h>
#include <dxgi.h>
#include <AL/alc.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <memory>
#include "preflight_pixels.h"
#include "recomp/host.h"

extern "C" void dirt2_probe_final_cleanup() __attribute__((weak));
namespace switch_platform {
namespace {
struct ReleaseCom { template<class T> void operator()(T* p) const { p->Release(); } };
template<class T> auto own(T* p) { return std::unique_ptr<T, ReleaseCom>(p); }
std::atomic<const char*> current_phase{"starting"};
std::atomic<uint64_t> phase_tick{0};
std::atomic<bool> done{false};
void phase(const char* name) {
    current_phase.store(name);
    phase_tick.store(armGetSystemTick());
    rt::log("DX11 preflight: %s", name);
}
bool result(HRESULT hr) {
    if (FAILED(hr)) rt::log("DX11 preflight: FAILED %08X in %s", unsigned(hr), current_phase.load());
    return SUCCEEDED(hr);
}
bool wait_gpu(ID3D11Device* device, ID3D11DeviceContext* context) {
    ID3D11Query* query = nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    if (!result(device->CreateQuery(&desc, &query))) return false;
    auto query_owner = own(query);
    context->End(query);
    context->Flush();
    const auto start = std::chrono::steady_clock::now();
    HRESULT hr;
    while ((hr = context->GetData(query, nullptr, 0, 0)) == S_FALSE) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
            rt::log("DX11 preflight: GPU completion timed out; requesting crash report");
            std::fflush(stderr);
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_Timeout));
        }
        svcSleepThread(1000000);
    }
    return result(hr);
}
bool offscreen(ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target = nullptr, *readback = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    if (!result(device->CreateTexture2D(&desc, nullptr, &target))) return false;
    auto target_owner = own(target);
    if (!result(device->CreateRenderTargetView(target, nullptr, &view))) return false;
    auto view_owner = own(view);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!result(device->CreateTexture2D(&desc, nullptr, &readback))) return false;
    auto readback_owner = own(readback);
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    context->ClearRenderTargetView(view, clear);
    context->CopyResource(readback, target);
    if (!wait_gpu(device, context)) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // DO_NOT_WAIT can legitimately report a pending readback even after an
    // event query. Retry without turning this asynchronous status into failure.
    const auto map_start = std::chrono::steady_clock::now();
    HRESULT map_result;
    while ((map_result = context->Map(readback, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (std::chrono::steady_clock::now() - map_start > std::chrono::seconds(10)) {
            rt::log("DX11 preflight: readback Map timed out");
            return false;
        }
        svcSleepThread(1000000);
    }
    if (!result(map_result)) return false;
    const unsigned bytes_per_pixel = format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
    const auto* first_pixel = static_cast<const unsigned char*>(mapped.pData);
    rt::log("DX11 preflight: format %u pitch %u first pixel %02X %02X %02X %02X %02X %02X %02X %02X",
        unsigned(format), mapped.RowPitch, first_pixel[0], first_pixel[1], first_pixel[2], first_pixel[3],
        first_pixel[4], first_pixel[5], first_pixel[6], first_pixel[7]);
    char dump_path[256];
    std::snprintf(dump_path, sizeof dump_path, "sdmc:/switch/dirt2-dx11/preflight-format-%u.bin", unsigned(format));
    FILE* dump = std::fopen(dump_path, "wb");
    bool ok = true;
    unsigned mismatches = 0;
    for (unsigned y = 0; y < 16; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        if (dump) std::fwrite(row, bytes_per_pixel, 16, dump);
        for (unsigned x = 0; x < 16; ++x) {
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                if (!preflight_rgba8_matches(row + 4*x)) { ok = false; ++mismatches; }
            } else {
                const uint16_t expected[4] = {0x3400, 0x3800, 0x3a00, 0x3c00};
                if (std::memcmp(row + 8*x, expected, 8)) { ok = false; ++mismatches; }
            }
        }
    }
    if (dump) std::fclose(dump);
    rt::log("DX11 preflight: format %u mismatching pixels %u/256", unsigned(format), mismatches);
    context->Unmap(readback, 0);
    rt::log("DX11 preflight: format %u clear/copy/readback %s", unsigned(format), ok ? "PASS" : "FAIL");
    return ok;
}
bool run_test() {
    phase("audio context creation");
    ALCdevice* audio = alcOpenDevice(nullptr);
    if (!audio) { rt::log("DX11 preflight: no audio device"); return false; }
    ALCcontext* audio_context = alcCreateContext(audio, nullptr);
    rt::log("DX11 preflight: audio context %s", audio_context ? "PASS" : "FAIL");
    if (!audio_context) { alcCloseDevice(audio); return false; }
    alcDestroyContext(audio_context); alcCloseDevice(audio);

    phase("SDL Vulkan window");
    SDL_Window* window = SDL_CreateWindow("DiRT 2 DX11 graphics test", 1280, 720, SDL_WINDOW_VULKAN);
    if (!window) { rt::log("DX11 preflight: SDL window failed: %s", SDL_GetError()); return false; }
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window_owner(window, SDL_DestroyWindow);
    phase("DX11 device and lazy swapchain");
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 1280; desc.BufferDesc.Height = 720;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.OutputWindow = reinterpret_cast<HWND>(window);
    desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (!result(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &level, 1, D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &context))) return false;
    auto device_owner = own(device);
    auto context_owner = own(context);
    auto swapchain_owner = own(swapchain);
    phase("RGBA8 clear/copy/GPU completion/readback");
    const bool rgba8_ok = offscreen(device, context, DXGI_FORMAT_R8G8B8A8_UNORM);
    phase("RGBA16F clear/copy/GPU completion/readback");
    const bool rgba16_ok = offscreen(device, context, DXGI_FORMAT_R16G16B16A16_FLOAT);
    phase("swapchain backbuffer");
    ID3D11Texture2D* backbuffer = nullptr;
    if (!result(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) return false;
    auto backbuffer_owner = own(backbuffer);
    ID3D11RenderTargetView* view = nullptr;
    if (!result(device->CreateRenderTargetView(backbuffer, nullptr, &view))) return false;
    auto view_owner = own(view);
    const float blue[4] = {0.05f, 0.2f, 0.65f, 1};
    context->ClearRenderTargetView(view, blue);
    phase("first Present");
    if (!result(swapchain->Present(1, 0))) return false;
    phase("post-present GPU completion");
    if (!wait_gpu(device, context)) return false;
    phase("show test frame");
    const uint64_t start = SDL_GetTicks();
    while (SDL_GetTicks() - start < 1500) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {}
        SDL_Delay(10);
    }
    phase("graphics teardown");
    context->ClearState(); context->Flush();
    // Owners release every resource on both success and early error paths.
    // In particular a failed Map must not keep the entire GPU device alive.
    return rgba8_ok && rgba16_ok;
}
}
int run_dx11_preflight() {
    phase("begin isolated graphics test (game will not start)");
    done = false;
    std::thread watchdog([] {
        while (!done.load()) {
            svcSleepThread(100000000);
            const uint64_t last = phase_tick.load();
            const uint64_t now = armGetSystemTick();
            if (now >= last && armTicksToNs(now - last) > 30000000000ull && phase_tick.load() == last) {
                // Avoid stdio/filesystem locks: the stalled thread may own
                // them. The last phase has already been written to dirt2.log.
                diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_Timeout));
            }
        }
    });
    const bool ok = run_test();
    rt::log("DX11 preflight: final result %s", ok ? "PASS" : "FAIL");
    // SDL shutdown can itself wait on the backend, so include it in the watchdog.
    phase("SDL teardown");
    SDL_Quit();
    if (dirt2_probe_final_cleanup) {
        phase("NVK service shutdown and transfer-memory verification");
        dirt2_probe_final_cleanup();
    }
    done = true;
    watchdog.join();
    return ok ? 0 : 1;
}
}
