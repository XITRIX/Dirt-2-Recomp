#pragma once
#include "../../platform/sdl/virtual_controller.h"
#ifdef DIRT2_TOUCH_CONTROLLER
namespace platform::touch {
namespace dx11 {
// Owned by the swap chain's private-data store. Resources follow its lifetime
// (including device recreation), and the dynamic vertex buffer is reused.
template<class T> struct Owned {
    T* p=nullptr;
    Owned()=default;
    Owned(const Owned&)=delete;
    Owned& operator=(const Owned&)=delete;
    ~Owned() { if(p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
};
inline constexpr GUID resourcesID={0x32444952,0x5443,0x4f55,{0x91,0x26,0x50,0x41,0x44,0,0,1}};
struct Resources final : IUnknown {
    std::atomic<ULONG> refs{1};
    Owned<ID3D11VertexShader> vs;
    Owned<ID3D11PixelShader> ps;
    Owned<ID3D11InputLayout> layout;
    Owned<ID3D11Buffer> constants, vertices;
    Owned<ID3D11BlendState> blend;
    Owned<ID3D11RasterizerState> raster;
    Owned<ID3D11DepthStencilState> depth;
    UINT capacity=0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out) return E_POINTER;
        *out=nullptr;
        if(id!=__uuidof(IUnknown)) return E_NOINTERFACE;
        *out=this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n=--refs; if(!n) delete this; return n; }
    bool init(ID3D11Device* d) {
        // Reuse the game's existing portable, precompiled color-only shaders.
        #include "../d3d11/immediate_shaders.inc"
        if(FAILED(d->CreateVertexShader(kImm_vsPrelitUntextured,sizeof kImm_vsPrelitUntextured,nullptr,vs.out())) ||
           FAILED(d->CreatePixelShader(kImm_psPrelitUntextured,sizeof kImm_psPrelitUntextured,nullptr,ps.out()))) return false;
        (void)kImmediateShaders;
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
        if(FAILED(d->CreateInputLayout(elements,2,kImm_vsPrelitUntextured,sizeof kImm_vsPrelitUntextured,layout.out()))) return false;
        const float identity[20]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, 1,1,1,1};
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof identity; cb.Usage=D3D11_USAGE_IMMUTABLE; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{identity,0,0};
        if(FAILED(d->CreateBuffer(&cb,&data,constants.out()))) return false;
        D3D11_BLEND_DESC bd{}; auto& rt=bd.RenderTarget[0]; rt.BlendEnable=TRUE;
        rt.SrcBlend=D3D11_BLEND_SRC_ALPHA; rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp=D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha=D3D11_BLEND_ONE; rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        return SUCCEEDED(d->CreateBlendState(&bd,blend.out())) && SUCCEEDED(d->CreateRasterizerState(&rd,raster.out())) &&
            SUCCEEDED(d->CreateDepthStencilState(&dd,depth.out()));
    }
};
// Save only changed pipeline slots; each shader's class instances are retained.
template<class T> struct Shader {
    Owned<T> object;
    ID3D11ClassInstance* instances[256]{};
    UINT count=256;
    ~Shader() { for(UINT i=0;i<count;++i) if(instances[i]) instances[i]->Release(); }
};
}
inline void present_d3d11(IDXGISwapChain* swap) {
    if(!swap) return;
    auto f=frame(); if(f.vertices.empty()) return;
    using namespace dx11;
    Owned<ID3D11Device> device;
    if(FAILED(swap->GetDevice(__uuidof(ID3D11Device),reinterpret_cast<void**>(device.out())))) return;
    Owned<Resources> resources; UINT size=sizeof resources.p;
    if(FAILED(swap->GetPrivateData(resourcesID,&size,resources.out()))) {
        resources.p=new Resources;
        if(!resources->init(device.p)) return;
        if(FAILED(swap->SetPrivateDataInterface(resourcesID,resources.p))) return;
    }
    struct V { float x,y,z,w; uint32_t color; };
    std::vector<V> mesh; mesh.reserve(f.vertices.size());
    for(auto& v:f.vertices) mesh.push_back({v.x/f.width*2-1,1-v.y/f.height*2,0,1,v.color});
    if(resources->capacity<mesh.size()) {
        if(resources->vertices.p) { resources->vertices.p->Release(); resources->vertices.p=nullptr; }
        resources->capacity=0;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=UINT(mesh.size()*sizeof(V)); bd.Usage=D3D11_USAGE_DYNAMIC;
        bd.BindFlags=D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(FAILED(device->CreateBuffer(&bd,nullptr,resources->vertices.out()))) return;
        resources->capacity=UINT(mesh.size());
    }
    Owned<ID3D11Texture2D> backBuffer;
    if(FAILED(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(backBuffer.out())))) return;
    Owned<ID3D11RenderTargetView> target;
    if(FAILED(device->CreateRenderTargetView(backBuffer.p,nullptr,target.out()))) return;
    Owned<ID3D11DeviceContext> ctx; device->GetImmediateContext(ctx.out());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(ctx->Map(resources->vertices.p,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return;
    std::memcpy(mapped.pData,mesh.data(),mesh.size()*sizeof(V)); ctx->Unmap(resources->vertices.p,0);

    Owned<ID3D11Predicate> predicate; BOOL predicateValue=FALSE;
    ctx->GetPredication(predicate.out(),&predicateValue);
    ctx->SetPredication(nullptr,FALSE);
    Shader<ID3D11VertexShader> vs; Shader<ID3D11PixelShader> ps; Shader<ID3D11GeometryShader> gs;
    Shader<ID3D11HullShader> hs; Shader<ID3D11DomainShader> ds;
    ctx->VSGetShader(vs.object.out(),vs.instances,&vs.count); ctx->PSGetShader(ps.object.out(),ps.instances,&ps.count);
    ctx->GSGetShader(gs.object.out(),gs.instances,&gs.count); ctx->HSGetShader(hs.object.out(),hs.instances,&hs.count);
    ctx->DSGetShader(ds.object.out(),ds.instances,&ds.count);
    Owned<ID3D11InputLayout> inputLayout; ctx->IAGetInputLayout(inputLayout.out());
    Owned<ID3D11Buffer> vertex,constant; UINT stride,offset;
    ctx->IAGetVertexBuffers(0,1,vertex.out(),&stride,&offset); ctx->VSGetConstantBuffers(0,1,constant.out());
    D3D11_PRIMITIVE_TOPOLOGY topology; ctx->IAGetPrimitiveTopology(&topology);
    Owned<ID3D11BlendState> blend; float factor[4]; UINT mask;
    ctx->OMGetBlendState(blend.out(),factor,&mask);
    Owned<ID3D11DepthStencilState> depth; UINT stencil; ctx->OMGetDepthStencilState(depth.out(),&stencil);
    Owned<ID3D11RasterizerState> raster; ctx->RSGetState(raster.out());
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; ctx->RSGetViewports(&viewportCount,viewports);
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Owned<ID3D11DepthStencilView> depthView; ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,depthView.out());
    D3D11_TEXTURE2D_DESC desc{}; backBuffer->GetDesc(&desc);
    D3D11_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1}; ctx->RSSetViewports(1,&viewport);
    ctx->RSSetState(resources->raster.p); ctx->OMSetRenderTargets(1,&target.p,nullptr);
    ctx->OMSetBlendState(resources->blend.p,nullptr,~0u); ctx->OMSetDepthStencilState(resources->depth.p,0);
    ctx->VSSetShader(resources->vs.p,nullptr,0); ctx->PSSetShader(resources->ps.p,nullptr,0);
    ctx->GSSetShader(nullptr,nullptr,0); ctx->HSSetShader(nullptr,nullptr,0); ctx->DSSetShader(nullptr,nullptr,0);
    ctx->VSSetConstantBuffers(0,1,&resources->constants.p); ctx->IASetInputLayout(resources->layout.p);
    UINT overlayStride=sizeof(V),zero=0; ctx->IASetVertexBuffers(0,1,&resources->vertices.p,&overlayStride,&zero);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->Draw(UINT(mesh.size()),0);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,depthView.p);
    for(auto* t:targets) if(t) t->Release();
    ctx->OMSetBlendState(blend.p,factor,mask); ctx->OMSetDepthStencilState(depth.p,stencil);
    ctx->RSSetState(raster.p); ctx->RSSetViewports(viewportCount,viewports);
    ctx->IASetInputLayout(inputLayout.p); ctx->IASetVertexBuffers(0,1,&vertex.p,&stride,&offset); ctx->IASetPrimitiveTopology(topology);
    ctx->VSSetConstantBuffers(0,1,&constant.p);
    ctx->VSSetShader(vs.object.p,vs.instances,vs.count); ctx->PSSetShader(ps.object.p,ps.instances,ps.count);
    ctx->GSSetShader(gs.object.p,gs.instances,gs.count); ctx->HSSetShader(hs.object.p,hs.instances,hs.count);
    ctx->DSSetShader(ds.object.p,ds.instances,ds.count);
    ctx->SetPredication(predicate.p,predicateValue);
}
}
#endif
