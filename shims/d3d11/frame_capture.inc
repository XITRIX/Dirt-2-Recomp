// Included inside the bridge's anonymous namespace. One frame, explicitly
// requested through <data>/gpu-capture.request; no GPU reads in normal play.
namespace frame_capture {
FILE* log = nullptr;
std::string dir;
uint32_t event = 0;
std::set<void*> inputs;

float half(uint16_t v) {
    const unsigned e = (v >> 10) & 31, m = v & 1023;
    const float x = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 0.0f : std::ldexp(float(m + 1024), int(e) - 25);
    return v & 32768 ? -x : x;
}

void texture(ID3D11DeviceContext* ctx, ID3D11Resource* resource, const std::string& name, bool raw = false) {
    D3D11_RESOURCE_DIMENSION dim{}; resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    auto* tex = static_cast<ID3D11Texture2D*>(resource);
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    std::fprintf(log, "texture %s %p %ux%u fmt=%u samples=%u mips=%u arrays=%u\n", name.c_str(), tex, d.Width, d.Height, unsigned(d.Format), d.SampleDesc.Count, d.MipLevels, d.ArraySize);
    // Capture mip zero/slice zero only; preserve a descriptor for omissions.
    if (!d.Width || !d.Height || d.Width > 4096 || d.Height > 4096 || d.SampleDesc.Count != 1) return;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    d.MipLevels = 1; d.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &staging); dev->Release();
    if (FAILED(hr)) { std::fprintf(log, "staging failed %08X\n", unsigned(hr)); return; }
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (SUCCEEDED(hr)) {
        unsigned bpp = 0;
        switch (d.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: bpp = 4; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: bpp = 8; break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: bpp = 16; break;
            default: break;
        }
        if (bpp) {
            const unsigned step = std::max(1u, (d.Width + 639) / 640), w = (d.Width + step - 1) / step, h = (d.Height + step - 1) / step;
            FILE* color = std::fopen((dir + "/" + name + ".ppm").c_str(), "wb");
            FILE* alpha = std::fopen((dir + "/" + name + "-alpha.pgm").c_str(), "wb");
            if (color && alpha) {
                std::fprintf(color, "P6\n%u %u\n255\n", w, h); std::fprintf(alpha, "P5\n%u %u\n255\n", w, h);
                for (unsigned y=0; y<d.Height; y+=step) for (unsigned x=0; x<d.Width; x+=step) {
                    const auto* p = static_cast<const uint8_t*>(m.pData) + size_t(y)*m.RowPitch + size_t(x)*bpp;
                    uint8_t rgba[4]{};
                    for (unsigned k=0; k<4; ++k) {
                        float f = bpp == 8 ? half(reinterpret_cast<const uint16_t*>(p)[k]) : bpp == 16 ? reinterpret_cast<const float*>(p)[k] : p[k] / 255.0f;
                        rgba[k] = uint8_t(std::clamp(std::isfinite(f) ? f : 0.0f, 0.0f, 1.0f)*255.0f);
                    }
                    if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) std::swap(rgba[0], rgba[2]);
                    std::fwrite(rgba, 1, 3, color); std::fwrite(rgba+3, 1, 1, alpha);
                }
            }
            if (color) std::fclose(color); if (alpha) std::fclose(alpha);
        }
        if (raw) {
            const bool bc = (d.Format >= DXGI_FORMAT_BC1_TYPELESS && d.Format <= DXGI_FORMAT_BC5_SNORM) || (d.Format >= DXGI_FORMAT_BC6H_TYPELESS && d.Format <= DXGI_FORMAT_BC7_UNORM_SRGB);
            const size_t rows = bc ? (d.Height+3)/4 : d.Height, bytes = rows*m.RowPitch;
            std::fprintf(log, "raw %s pitch=%u bytes=%zu\n", name.c_str(), m.RowPitch, bytes);
            if (bytes <= 16*1024*1024) if (FILE* f = std::fopen((dir+"/"+name+".bin").c_str(), "wb")) { std::fwrite(m.pData, 1, bytes, f); std::fclose(f); }
        }
        ctx->Unmap(staging, 0);
    } else std::fprintf(log, "map failed %08X\n", unsigned(hr));
    staging->Release();
}

void after(ID3D11DeviceContext* ctx, const char* op, uint32_t caller) {
    if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !log || event >= 1500) return;
    ++event;
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11PixelShader* ps = nullptr; ctx->PSGetShader(&ps, nullptr, nullptr);
    ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
    std::fprintf(log, "event %04u %s caller=%08X rtv=%p dsv=%p ps=%p vs=%p\n", event, op, caller, rtv, dsv, ps, vs);
    if (ps) ps->Release(); if (vs) vs->Release();
    ID3D11BlendState* blend = nullptr; FLOAT factors[4]{}; UINT mask=0; ctx->OMGetBlendState(&blend, factors, &mask);
    if (blend) {
        D3D11_BLEND_DESC d{}; blend->GetDesc(&d); const auto& b=d.RenderTarget[0];
        std::fprintf(log, "blend enable=%u src=%u dst=%u op=%u srcAlpha=%u dstAlpha=%u opAlpha=%u write=%u\n", unsigned(b.BlendEnable),unsigned(b.SrcBlend),unsigned(b.DestBlend),unsigned(b.BlendOp),unsigned(b.SrcBlendAlpha),unsigned(b.DestBlendAlpha),unsigned(b.BlendOpAlpha),unsigned(b.RenderTargetWriteMask)); blend->Release();
    }
    ID3D11DepthStencilState* depth=nullptr; UINT ref=0; ctx->OMGetDepthStencilState(&depth,&ref);
    if (depth) { D3D11_DEPTH_STENCIL_DESC d{}; depth->GetDesc(&d); std::fprintf(log,"depth enable=%u write=%u func=%u stencil=%u ref=%u masks=%u/%u front=%u/%u/%u/%u back=%u/%u/%u/%u\n",unsigned(d.DepthEnable),unsigned(d.DepthWriteMask),unsigned(d.DepthFunc),unsigned(d.StencilEnable),ref,unsigned(d.StencilReadMask),unsigned(d.StencilWriteMask),unsigned(d.FrontFace.StencilFailOp),unsigned(d.FrontFace.StencilDepthFailOp),unsigned(d.FrontFace.StencilPassOp),unsigned(d.FrontFace.StencilFunc),unsigned(d.BackFace.StencilFailOp),unsigned(d.BackFace.StencilDepthFailOp),unsigned(d.BackFace.StencilPassOp),unsigned(d.BackFace.StencilFunc)); depth->Release(); }
    D3D11_VIEWPORT vp[16]{}; UINT n=16; ctx->RSGetViewports(&n,vp); for (UINT i=0;i<n;++i) std::fprintf(log,"viewport %.1f %.1f %.1f %.1f %.2f %.2f\n",vp[i].TopLeftX,vp[i].TopLeftY,vp[i].Width,vp[i].Height,vp[i].MinDepth,vp[i].MaxDepth);
    for (unsigned slot=0;slot<8;++slot) {
        ID3D11ShaderResourceView* srv=nullptr; ctx->PSGetShaderResources(slot,1,&srv);
        if (srv) { ID3D11Resource* r=nullptr; srv->GetResource(&r); std::fprintf(log,"input %u srv=%p resource=%p\n",slot,srv,r); if (inputs.insert(r).second) { char name[64]; std::snprintf(name,sizeof(name),"input-%p",r); texture(ctx,r,name,true); } r->Release(); srv->Release(); }
        ID3D11SamplerState* sampler=nullptr; ctx->PSGetSamplers(slot,1,&sampler); if(sampler) { D3D11_SAMPLER_DESC d{}; sampler->GetDesc(&d); std::fprintf(log,"sampler %u filter=%u address=%u/%u/%u\n",slot,unsigned(d.Filter),unsigned(d.AddressU),unsigned(d.AddressV),unsigned(d.AddressW)); sampler->Release(); }
    }
    if (rtv) { ID3D11Resource* r=nullptr; rtv->GetResource(&r); char name[64]; std::snprintf(name,sizeof(name),"draw-%04u",event); texture(ctx,r,name); r->Release(); rtv->Release(); }
    if (dsv) dsv->Release();
    std::fflush(log);
}
void boundary() {
    static unsigned frames=0;
    if (log) { std::fprintf(log,"complete %u events\n",event); std::fclose(log); log=nullptr; rt::log("GPU frame capture complete: %s (%u events)",dir.c_str(),event); return; }
    if (++frames%30) return;
    const char* data=std::getenv("DIRT2_DATA_DIR"); if (!data) return;
    const std::string request=std::string(data)+"/gpu-capture.request";
    std::error_code ec; if (!std::filesystem::exists(request,ec)) return;
    std::filesystem::remove(request,ec); if (ec) return;
    dir=std::string(data)+"/gpu-capture-"+std::to_string(frames);
    std::filesystem::create_directories(dir,ec); if(ec) return;
    log=std::fopen((dir+"/draws.txt").c_str(),"w"); event=0; inputs.clear();
    rt::log("GPU frame capture requested: %s",dir.c_str());
}
} // namespace frame_capture

void ctx_capture_draw(X86* c) { const Args a{c}; auto* s=static_cast<ID3D11DeviceContext*>(to_host(a[0])); if(s) { s->Draw(a[1],a[2]); frame_capture::after(s,"Draw",rd32(c->esp)); } c->esp+=16; }
void ctx_capture_indexed(X86* c) { const Args a{c}; auto* s=static_cast<ID3D11DeviceContext*>(to_host(a[0])); if(s) { s->DrawIndexed(a[1],a[2],int32_t(a[3])); frame_capture::after(s,"DrawIndexed",rd32(c->esp)); } c->esp+=20; }
void ctx_capture_instanced(X86* c) { const Args a{c}; auto* s=static_cast<ID3D11DeviceContext*>(to_host(a[0])); if(s) { s->DrawInstanced(a[1],a[2],a[3],a[4]); frame_capture::after(s,"DrawInstanced",rd32(c->esp)); } c->esp+=24; }
void ctx_capture_indexed_instanced(X86* c) { const Args a{c}; auto* s=static_cast<ID3D11DeviceContext*>(to_host(a[0])); if(s) { s->DrawIndexedInstanced(a[1],a[2],a[3],int32_t(a[4]),a[5]); frame_capture::after(s,"DrawIndexedInstanced",rd32(c->esp)); } c->esp+=28; }
void ctx_capture_execute(X86* c) { const Args a{c}; auto* s=static_cast<ID3D11DeviceContext*>(to_host(a[0])); auto* list=static_cast<ID3D11CommandList*>(to_host(a[1])); if(s&&list) { s->ExecuteCommandList(list,a[2]); frame_capture::after(s,"ExecuteCommandList",rd32(c->esp)); } c->esp+=16; }
OVERRIDE(ID3D11DeviceContext, Draw, ctx_capture_draw)
OVERRIDE(ID3D11DeviceContext, DrawIndexed, ctx_capture_indexed)
OVERRIDE(ID3D11DeviceContext, DrawInstanced, ctx_capture_instanced)
OVERRIDE(ID3D11DeviceContext, DrawIndexedInstanced, ctx_capture_indexed_instanced)
OVERRIDE(ID3D11DeviceContext, ExecuteCommandList, ctx_capture_execute)
