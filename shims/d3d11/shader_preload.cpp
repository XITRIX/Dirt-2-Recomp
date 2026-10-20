// Optional read-only startup warm-up. The native DXVK compiler chooses all
// Switch-specific options; the bundle contains original DXBC, never Mac GPU code.
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include "shader_bundle.h"
#include "recomp/host.h"
extern "C" void dirt2D3D11GetCompilerStats(ID3D11Device*, uint64_t*, uint64_t*);
extern "C" void dirt2_dx11_startup_progress();
namespace shim::d3d11 {
namespace {
template<class T> struct Release { void operator()(T* p) const { if(p) p->Release(); } };
template<class T> using Owner=std::unique_ptr<T,Release<T>>;
HRESULT create_shader(ID3D11Device* device,const ShaderBytecode& shader) {
    const void* code=shader.code.data(); const size_t size=shader.code.size();
    HRESULT hr=E_INVALIDARG;
#define CREATE(stage_id, Type, Method) case stage_id: { Type* p=nullptr; hr=device->Method(code,size,nullptr,&p); if(p)p->Release(); break; }
    switch(shader.stage) {
        CREATE(0,ID3D11PixelShader,CreatePixelShader)
        CREATE(1,ID3D11VertexShader,CreateVertexShader)
        CREATE(2,ID3D11GeometryShader,CreateGeometryShader)
        CREATE(3,ID3D11HullShader,CreateHullShader)
        CREATE(4,ID3D11DomainShader,CreateDomainShader)
        CREATE(5,ID3D11ComputeShader,CreateComputeShader)
    }
#undef CREATE
    return hr;
}
}
void preload_switch_shaders(ID3D11Device* device,IDXGISwapChain* swapchain) {
    const char* setting=std::getenv("DIRT2_DX11_SHADER_PRELOAD");
    if(!setting || !*setting || !device || !swapchain) return;
    static std::once_flag once;
    std::call_once(once,[&] {
        const auto start=std::chrono::steady_clock::now();
        std::ifstream file(setting,std::ios::binary|std::ios::ate);
        const auto length=file ? file.tellg() : std::streampos(-1);
        if(length<=0 || length>64*1024*1024) { rt::log("DX11 preload: missing/oversized bundle %s",setting); return; }
        std::vector<uint8_t> data(static_cast<size_t>(length));
        file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(data.data()),data.size())) { rt::log("DX11 preload: bundle read failed"); return; }
        file.close(); // Read once; shader creation and compilation do not write a cache.
        std::vector<ShaderBytecode> shaders;
        if(!parse_shader_bundle(data,shaders)) { rt::log("DX11 preload: malformed bundle; skipped"); return; }
        rt::log("DX11 preload: %zu unique shaders, %zu bytes; compiling native libraries",shaders.size(),data.size());
        ID3D11DeviceContext* raw_context=nullptr;device->GetImmediateContext(&raw_context);Owner<ID3D11DeviceContext> context(raw_context);
        ID3D11Texture2D* raw_buffer=nullptr;swapchain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&raw_buffer));Owner<ID3D11Texture2D> buffer(raw_buffer);
        ID3D11RenderTargetView* raw_view=nullptr;
        if(buffer)device->CreateRenderTargetView(buffer.get(),nullptr,&raw_view);
        Owner<ID3D11RenderTargetView> view(raw_view);
        auto show_progress=[&] {
            if(!context || !view)return;
            const float color[4]={.025f,.035f,.055f,1.f};
            context->ClearRenderTargetView(view.get(),color);
            const HRESULT hr=swapchain->Present(1,0);
            if(FAILED(hr))rt::log("DX11 preload: progress Present failed %08X",uint32_t(hr));
        };
        size_t created=0,failed=0;
        show_progress();
        for(size_t i=0;i<shaders.size();++i) {
            dirt2_dx11_startup_progress();
            const HRESULT hr=create_shader(device,shaders[i]);
            if(SUCCEEDED(hr))++created;
            else { ++failed; if(failed<=16)rt::log("DX11 preload: shader %zu stage %u failed %08X",i,shaders[i].stage,uint32_t(hr)); }
            if((i+1)%64==0) {
                uint64_t done=0,total=0;dirt2D3D11GetCompilerStats(device,&done,&total);
                rt::log("DX11 preload: created %zu/%zu; compiler %llu/%llu",i+1,shaders.size(),(unsigned long long)done,(unsigned long long)total);
                show_progress();
            }
        }
        uint64_t last_done=UINT64_MAX;
        auto last_show=std::chrono::steady_clock::now();
        for(;;) {
            uint64_t done=0,total=0;dirt2D3D11GetCompilerStats(device,&done,&total);
            if(done!=last_done) { dirt2_dx11_startup_progress(); last_done=done; }
            if(done>=total)break;
            if(std::chrono::steady_clock::now()-last_show>std::chrono::seconds(1)) { show_progress();last_show=std::chrono::steady_clock::now(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        rt::log("DX11 preload: finished in %.1fs, %zu created, %zu failed; continuing game",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),created,failed);
        dirt2_dx11_startup_progress();
    });
}
}
