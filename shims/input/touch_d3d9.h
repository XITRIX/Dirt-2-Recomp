#pragma once
#include "../../platform/sdl/virtual_controller.h"
#ifdef DIRT2_TOUCH_CONTROLLER
namespace platform::touch {
inline void draw_d3d9(IDirect3DDevice9* device, IDirect3DSurface9* target) {
    auto f=frame();
    if(f.vertices.empty() || !target) return;
    D3DSURFACE_DESC desc{}; if(FAILED(target->GetDesc(&desc))) return;
    IDirect3DStateBlock9* saved=nullptr;
    if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&saved))) return;
    IDirect3DSurface9* oldTargets[4]{}; IDirect3DSurface9* oldDepth=nullptr;
    D3DCAPS9 caps{};
    if(FAILED(device->GetDeviceCaps(&caps))) { saved->Release(); return; }
    const UINT targetCount=std::min(4u,caps.NumSimultaneousRTs);
    for(UINT i=0;i<targetCount;++i) device->GetRenderTarget(i,&oldTargets[i]);
    device->GetDepthStencilSurface(&oldDepth);
    struct V { float x,y,z,w; uint32_t color; };
    std::vector<V> vertices; vertices.reserve(f.vertices.size());
    for(auto& v:f.vertices) {
        // D3D9's diffuse DWORD is BGRA; the shared mesh uses RGBA.
        uint32_t c=(v.color&0xff00ff00)|((v.color&255)<<16)|((v.color>>16)&255);
        vertices.push_back({v.x*desc.Width/f.width-.5f,v.y*desc.Height/f.height-.5f,0,1,c});
    }
    if(SUCCEEDED(device->BeginScene())) {
        device->SetDepthStencilSurface(nullptr);
        for(UINT i=1;i<targetCount;++i) device->SetRenderTarget(i,nullptr);
        device->SetRenderTarget(0,target);
        // Only the presented back buffer receives overlay pixels.
        device->SetRenderState(D3DRS_COLORWRITEENABLE1,0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE2,0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE3,0);
        D3DVIEWPORT9 viewport{0,0,desc.Width,desc.Height,0,1}; device->SetViewport(&viewport);
        device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
        device->SetStreamSourceFreq(0,1);
        device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
        device->SetTexture(0,nullptr);
        device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
        device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
        device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
        device->SetTextureStageState(0,D3DTSS_RESULTARG,D3DTA_CURRENT);
        device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
        device->SetRenderState(D3DRS_ZENABLE,FALSE); device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE,FALSE); device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE); device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);
        device->SetRenderState(D3DRS_FOGENABLE,FALSE); device->SetRenderState(D3DRS_LIGHTING,FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        device->SetRenderState(D3DRS_MULTISAMPLEMASK,~0u);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(vertices.size()/3),vertices.data(),sizeof(V));
        device->EndScene();
    }
    for(UINT i=0;i<targetCount;++i) device->SetRenderTarget(i,oldTargets[i]);
    device->SetDepthStencilSurface(oldDepth);
    saved->Apply(); saved->Release();
    for(auto* t:oldTargets) if(t) t->Release();
    if(oldDepth) oldDepth->Release();
}
inline void present_d3d9(IDirect3DDevice9* device, IDirect3DSwapChain9* swap=nullptr) {
    if(!device) return;
    IDirect3DSurface9* target=nullptr;
    HRESULT hr=swap?swap->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&target):device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&target);
    if(SUCCEEDED(hr)) { draw_d3d9(device,target); target->Release(); }
}
}
#endif
