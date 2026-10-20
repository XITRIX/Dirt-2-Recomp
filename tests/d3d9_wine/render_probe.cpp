// Controlled rendering comparison, separate from the Wine results/cursor.
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "../../shims/d3d9/deko/device.h"

bool wine_switch_render_probe() {
    const char* root="sdmc:/switch/d3d9-wine/results/";
    setenv("DIRT2_DEKO_SYNC","1",1);
    if(!SDL_Init(SDL_INIT_VIDEO)){std::fprintf(stderr,"PROBE SDL failure: %s\n",SDL_GetError());return false;}
    auto* window=SDL_CreateWindow("D3D9 draw diagnostic",640,480,SDL_WINDOW_HIDDEN);
    if(!window){std::fprintf(stderr,"PROBE window failure: %s\n",SDL_GetError());SDL_Quit();return false;}
    bool passed=true;
    // Baseline first; then each FFP stage alone; then the original pair.
    for(unsigned variant:{3u,2u,1u,0u}) {
        char value[8];std::snprintf(value,sizeof value,"%u",variant);
        setenv("DIRT2_DEKO_FFP_DIAGNOSTIC",value,1);
        std::fprintf(stderr,"PROBE variant %u START: vertex=%s fragment=%s\n",variant,
            variant&1?"simple":"FFP",variant&2?"simple":"FFP");std::fflush(stderr);
        auto* d3d=Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow=reinterpret_cast<HWND>(window);pp.BackBufferWidth=640;pp.BackBufferHeight=480;
        pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24S8;
        IDirect3DDevice9* device=nullptr;
        HRESULT hr=d3d?d3d->CreateDevice(0,D3DDEVTYPE_HAL,pp.hDeviceWindow,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device):E_FAIL;
        std::fprintf(stderr,"PROBE variant %u CreateDevice=0x%lx\n",variant,(unsigned long)hr);std::fflush(stderr);
        if(!device){passed=false;if(d3d)d3d->Release();deko::gpu().shutdown();continue;}
        IDirect3DTexture9* texture=nullptr;
        hr=device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr);
        if(FAILED(hr)||!texture)passed=false;
        if(SUCCEEDED(hr)&&texture){D3DLOCKED_RECT lr{};hr=texture->LockRect(0,&lr,nullptr,0);
            if(FAILED(hr))passed=false;
            if(SUCCEEDED(hr)){*static_cast<DWORD*>(lr.pBits)=0xffffffff;texture->UnlockRect(0);}device->SetTexture(0,texture);}
        device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_ZENABLE,TRUE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        device->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1);
        const float quad[]={-1,-1,.1f,0,0, -1,1,.1f,0,1, 1,-1,.1f,1,0, 1,1,.1f,1,1};
        device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff0000ff,1,0);
        device->BeginScene();hr=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,5*sizeof(float));
        const bool fault=dkQueueIsInErrorState(deko::gpu().queue);
        if(fault||FAILED(hr))passed=false;
        std::fprintf(stderr,"PROBE variant %u DRAW hr=0x%lx GPU_fault=%u\n",variant,(unsigned long)hr,unsigned(fault));std::fflush(stderr);
        if(!fault){
            device->EndScene();
            IDirect3DSurface9 *rt=nullptr,*readback=nullptr;
            hr=device->GetRenderTarget(0,&rt);
            if(SUCCEEDED(hr))hr=device->CreateOffscreenPlainSurface(640,480,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr);
            if(SUCCEEDED(hr))hr=device->GetRenderTargetData(rt,readback);
            D3DLOCKED_RECT lr{};if(SUCCEEDED(hr))hr=readback->LockRect(&lr,nullptr,D3DLOCK_READONLY);
            if(SUCCEEDED(hr)){const auto pixel=*reinterpret_cast<const DWORD*>(static_cast<const char*>(lr.pBits)+240*lr.Pitch+320*4);
                const DWORD expected=variant&2?0xff00ff00u:0xffffffffu;
                if(pixel!=expected)passed=false;
                std::fprintf(stderr,"PROBE variant %u center=0x%08lx expected=0x%08x\n",variant,(unsigned long)pixel,expected);readback->UnlockRect();}
            else {passed=false;std::fprintf(stderr,"PROBE variant %u readback hr=0x%lx\n",variant,(unsigned long)hr);}
            if(!dkQueueIsInErrorState(deko::gpu().queue)) {
                if(readback)readback->Release();if(rt)rt->Release();
                device->SetTexture(0,nullptr);if(texture)texture->Release();device->Release();d3d->Release();
            } else passed=false;
        }
        // A faulted queue cannot accept COM destructor submissions. Abandon
        // those objects and reclaim allocations centrally before the next pair.
        deko::gpu().shutdown();
        std::fprintf(stderr,"PROBE variant %u END\n",variant);std::fflush(stderr);
    }
    SDL_DestroyWindow(window);SDL_Quit();
    std::ofstream(std::string(root)+"render-probe-complete.txt")<<(passed?"all four rendering checks passed\n":"rendering checks failed; see render-probe.log\n");
    std::remove((std::string(root)+"render-probe.txt").c_str());
    std::fprintf(stderr,"PROBE ALL COMPLETE passed=%u; Wine test progress unchanged\n",unsigned(passed));std::fflush(stderr);
    return passed;
}
