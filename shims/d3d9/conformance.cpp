// Small backend-independent rendering regressions. These are original tests,
// not a port of Wine's Windows-dependent conformance suite.
#include "conformance.h"
#include "recomp/host.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace d3d9_tests {
namespace {
struct BuiltinShader {
    const char* function;
    const char* profile;
    const uint32_t* words;
    uint32_t count;
};
// Reuse existing SM3 bytecode: these exact shaders are already in the Switch
// pack and also run unmodified through DXVK. No test-specific shader bypass.
#include "../d3dx9/builtin_shaders.inc"

template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return p; }
    operator T*() const { return p; }
};
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char text[256];
        std::snprintf(text, sizeof text, "%s: HRESULT 0x%08x", operation, unsigned(hr));
        throw std::runtime_error(text);
    }
}
// Arguments are constants/internal identifiers, but escape errors as JSON too.
std::string quote(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += char(c); }
        else if (c >= 32 && c < 127) r += char(c);
        else { char b[7]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
    }
    return r + '"';
}
constexpr unsigned W = 64, H = 64;
constexpr uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff,
                               0xff000000, 0xffffff00, 0xffff00ff};
struct Vertex { float x, y, z, w, u, v; };

struct Suite {
    IDirect3DDevice9* d;
    std::filesystem::path out;
    Com<IDirect3DSurface9> target, readback, depth;
    Com<IDirect3DVertexDeclaration9> decl;
    Com<IDirect3DVertexShader9> vs;
    Com<IDirect3DPixelShader9> solid, textured;
    std::vector<uint32_t> pixels;
    std::vector<std::string> results;
    unsigned failures = 0;

    Suite(IDirect3DDevice9* dev, std::filesystem::path dir) : d(dev), out(std::move(dir)) {
        check(d->CreateRenderTarget(W,H,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target.p,nullptr),"CreateRenderTarget");
        check(d->CreateOffscreenPlainSurface(W,H,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback.p,nullptr),"CreateReadback");
        check(d->CreateDepthStencilSurface(W,H,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&depth.p,nullptr),"CreateDepth");
        const D3DVERTEXELEMENT9 elements[] = {
            {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,16,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}, D3DDECL_END()};
        check(d->CreateVertexDeclaration(elements,&decl.p),"CreateDeclaration");
        check(d->CreateVertexShader(kShader2,&vs.p),"CreateVS");
        check(d->CreatePixelShader(kShader4,&solid.p),"CreateSolidPS");
        check(d->CreatePixelShader(kShader6,&textured.p),"CreateTexturePS");
    }
    void rs(D3DRENDERSTATETYPE state, DWORD value) { check(d->SetRenderState(state,value),"SetRenderState"); }
    void ss(D3DSAMPLERSTATETYPE state, DWORD value) { check(d->SetSamplerState(0,state,value),"SetSamplerState"); }
    void baseline() {
        check(d->SetTexture(0,nullptr),"UnbindTexture");
        check(d->SetRenderTarget(0,target),"SetTarget");
        check(d->SetDepthStencilSurface(depth),"SetDepth");
        check(d->SetVertexDeclaration(decl),"SetDeclaration");
        check(d->SetVertexShader(vs),"SetVS");
        check(d->SetPixelShader(solid),"SetPS");
        const float identity[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        check(d->SetVertexShaderConstantF(0,identity,4),"VSConstants");
        rs(D3DRS_ZENABLE,FALSE); rs(D3DRS_ZWRITEENABLE,TRUE); rs(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
        rs(D3DRS_CULLMODE,D3DCULL_NONE); rs(D3DRS_ALPHABLENDENABLE,FALSE);
        rs(D3DRS_SEPARATEALPHABLENDENABLE,FALSE); rs(D3DRS_ALPHATESTENABLE,FALSE);
        rs(D3DRS_STENCILENABLE,FALSE); rs(D3DRS_SCISSORTESTENABLE,FALSE);
        rs(D3DRS_SRGBWRITEENABLE,FALSE); rs(D3DRS_COLORWRITEENABLE,15);
        rs(D3DRS_BLENDOP,D3DBLENDOP_ADD); rs(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
        rs(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        ss(D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); ss(D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        ss(D3DSAMP_MAGFILTER,D3DTEXF_POINT); ss(D3DSAMP_MINFILTER,D3DTEXF_POINT);
        ss(D3DSAMP_MIPFILTER,D3DTEXF_POINT); ss(D3DSAMP_MAXMIPLEVEL,0);
        ss(D3DSAMP_MIPMAPLODBIAS,0); ss(D3DSAMP_SRGBTEXTURE,FALSE);
        check(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0xff000000,1,0),"Clear");
    }
    void draw(float z = .5f, unsigned viewport = W) {
        const D3DVIEWPORT9 v = {0,0,viewport,viewport,0,1};
        check(d->SetViewport(&v),"SetViewport");
        const Vertex vertices[] = {{-1,-1,z,1,0,1},{-1,1,z,1,0,0},{1,-1,z,1,1,1},{1,1,z,1,1,0}};
        check(d->BeginScene(),"BeginScene");
        const HRESULT hr = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex));
        const HRESULT end = d->EndScene();
        check(hr,"DrawPrimitiveUP"); check(end,"EndScene");
    }
    void color(uint32_t argb) {
        const float c[] = {float((argb>>16)&255)/255, float((argb>>8)&255)/255,
                           float(argb&255)/255, float(argb>>24)/255};
        check(d->SetPixelShaderConstantF(0,c,1),"PSConstants");
    }
    void read() {
        check(d->GetRenderTargetData(target,readback),"GetRenderTargetData");
        D3DLOCKED_RECT lock{};
        check(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"ReadbackLock");
        pixels.resize(W*H);
        for (unsigned y=0;y<H;++y)
            std::memcpy(pixels.data()+y*W, static_cast<char*>(lock.pBits)+y*lock.Pitch,W*4);
        check(readback->UnlockRect(),"ReadbackUnlock");
    }
    void expect(uint32_t expected, unsigned x0=4,unsigned y0=4,unsigned x1=60,unsigned y1=60,
                uint32_t mask=0xffffffff, unsigned tolerance=2) {
        for (unsigned y=y0;y<y1;++y) for (unsigned x=x0;x<x1;++x) {
            const uint32_t got = pixels[y*W+x];
            bool bad=false;
            for (unsigned shift=0;shift<32;shift+=8) if ((mask>>shift)&255)
                bad |= unsigned(std::abs(int((got>>shift)&255)-int((expected>>shift)&255)))>tolerance;
            if (bad) {
                char text[180];
                std::snprintf(text,sizeof text,"pixel (%u,%u): expected %08x, got %08x (mask %08x, tolerance %u)",
                              x,y,expected,got,mask,tolerance);
                throw std::runtime_error(text);
            }
        }
    }
    void test(const std::string& name, const std::function<void()>& body) {
        pixels.clear();
        std::string error;
        try { baseline(); body(); }
        catch (const std::exception& e) { error=e.what(); }
        // Always retain the last readback, including failed pixel comparisons.
        std::string file;
        if (!pixels.empty()) {
            file=name+".bgra";
            FILE* f=std::fopen((out/file).string().c_str(),"wb");
            const bool ok=f && std::fwrite(pixels.data(),4,pixels.size(),f)==pixels.size();
            const bool closed=f && std::fclose(f)==0;
            if (!ok || !closed) { error += " artifact write failed"; file.clear(); }
        }
        failures += !error.empty();
        results.push_back("{\"name\":"+quote(name)+",\"status\":"+quote(error.empty()?"pass":"fail")+
                          ",\"error\":"+quote(error)+",\"image\":"+quote(file)+"}");
        rt::log("d3d9-tests: %s: %s %s",name.c_str(),error.empty()?"PASS":"FAIL",error.c_str());
    }
    void texture(IDirect3DTexture9** outTexture, D3DFORMAT format, bool alpha=false) {
        check(d->CreateTexture(64,64,7,0,format,D3DPOOL_MANAGED,outTexture,nullptr),"CreateMipTexture");
        for(unsigned level=0;level<7;++level) {
            const unsigned size=std::max(1u,64u>>level);
            D3DLOCKED_RECT lock{};
            check((*outTexture)->LockRect(level,&lock,nullptr,0),"TextureLock");
            const uint32_t c=colors[level];
            if(format==D3DFMT_A8R8G8B8) {
                for(unsigned y=0;y<size;++y) for(unsigned x=0;x<size;++x)
                    std::memcpy(static_cast<char*>(lock.pBits)+y*lock.Pitch+x*4,&c,4);
            } else {
                const unsigned blockBytes=format==D3DFMT_DXT1?8:16;
                const unsigned blocks=std::max(1u,(size+3)/4);
                const uint16_t rgb565=uint16_t(((c>>19)&31)<<11 | ((c>>10)&63)<<5 | ((c>>3)&31));
                for(unsigned y=0;y<blocks;++y) for(unsigned x=0;x<blocks;++x) {
                    auto* p=static_cast<unsigned char*>(lock.pBits)+y*lock.Pitch+x*blockBytes;
                    std::memset(p,0,blockBytes);
                    if(format==D3DFMT_DXT3) std::memset(p,alpha?0x88:0xff,8);
                    if(format==D3DFMT_DXT5) { p[0]=alpha?128:255; p[1]=0; }
                    if(blockBytes==16) p+=8;
                    // Equal endpoints, index zero: an exact solid color in BC1/2/3.
                    std::memcpy(p,&rgb565,2); std::memcpy(p+2,&rgb565,2);
                }
            }
            check((*outTexture)->UnlockRect(level),"TextureUnlock");
        }
    }
    void bind(IDirect3DTexture9* t) {
        check(d->SetTexture(0,t),"SetTexture"); check(d->SetPixelShader(textured),"TexturePS");
    }
    void run() {
        test("clear_argb",[&]{ check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0x804080c0,1,0),"ClearColor"); read(); expect(0x804080c0); });
        test("colorfill_rectangle",[&]{ const RECT r={16,16,48,48}; check(d->ColorFill(target,&r,0xffff0000),"ColorFill");
            read(); expect(0xffff0000,18,18,46,46); expect(0xff000000,2,2,14,62); });
        test("stretchrect_subrectangle",[&]{ Com<IDirect3DSurface9> src;
            check(d->CreateRenderTarget(W,H,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&src.p,nullptr),"CopySource");
            check(d->ColorFill(src,nullptr,0xff00ff00),"FillSource"); const RECT r={16,16,48,48};
            check(d->StretchRect(src,nullptr,target,&r,D3DTEXF_POINT),"StretchRect");
            read(); expect(0xff00ff00,18,18,46,46); expect(0xff000000,2,2,14,62); });
        test("solid_shader",[&]{ color(0xff00ff00); draw(); read(); expect(0xff00ff00); });
        test("scissor",[&]{ RECT r={16,16,48,48}; check(d->SetScissorRect(&r),"SetScissor");
            rs(D3DRS_SCISSORTESTENABLE,TRUE); color(0xffff0000); draw(); read();
            expect(0xffff0000,18,18,46,46); expect(0xff000000,2,2,14,62); });
        test("blend_srcalpha",[&]{ check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff0000ff,1,0),"ClearBlue");
            rs(D3DRS_ALPHABLENDENABLE,TRUE); color(0x80ff0000); draw(); read(); expect(0x0080007f,4,4,60,60,0xffffff); });
        test("alpha_test_reject",[&]{ rs(D3DRS_ALPHATESTENABLE,TRUE); rs(D3DRS_ALPHAFUNC,D3DCMP_GREATER);
            rs(D3DRS_ALPHAREF,128); color(0x40ff0000); draw(); read(); expect(0xff000000); });
        test("depth_less",[&]{ rs(D3DRS_ZENABLE,TRUE); rs(D3DRS_ZFUNC,D3DCMP_LESS);
            color(0xffff0000); draw(.25f); color(0xff00ff00); draw(.75f); read(); expect(0xffff0000); });
        test("depth_disabled_no_write",[&]{ color(0xffff0000); draw(.1f); rs(D3DRS_ZENABLE,TRUE);
            color(0xff00ff00); draw(.5f); read(); expect(0xff00ff00); });
        // Keep all dependent draws in flight until the final image readback.
        // Readback after each draw would mask the game's asynchronous failures.
        auto tile = [&](unsigned i) {
            RECT r={LONG(i%8*8),LONG(i/8*8),LONG(i%8*8+8),LONG(i/8*8+8)};
            check(d->SetScissorRect(&r),"BatchScissor");
            rs(D3DRS_SCISSORTESTENABLE,TRUE);
        };
        auto expect_tiles = [&](const std::function<uint32_t(unsigned)>& expected) {
            for(unsigned i=0;i<64;++i)
                expect(expected(i),i%8*8+2,i/8*8+2,i%8*8+6,i/8*8+6);
        };
        test("batched_constants",[&]{
            for(unsigned i=0;i<64;++i) { tile(i);color(colors[i%7]);draw(); }
            read();expect_tiles([](unsigned i){return colors[i%7];});
        });
        test("batched_alpha_state",[&]{
            for(unsigned i=0;i<64;++i) {
                tile(i);rs(D3DRS_ALPHATESTENABLE,i%2);rs(D3DRS_ALPHAFUNC,D3DCMP_GREATER);
                rs(D3DRS_ALPHAREF,128);color(0x40ff0000);draw();
            }
            read();expect_tiles([](unsigned i){return i%2?0xff000000u:0x40ff0000u;});
        });
        test("batched_texture_lod",[&]{
            Com<IDirect3DTexture9> t;texture(&t.p,D3DFMT_A8R8G8B8);bind(t);
            ss(D3DSAMP_MIPFILTER,D3DTEXF_NONE);
            for(unsigned i=0;i<64;++i) { tile(i);t->SetLOD(i%5);draw(); }
            read();expect_tiles([](unsigned i){return colors[i%5];});
        });
        test("batched_resource_retirement",[&]{
            for(unsigned i=0;i<64;++i) {
                Com<IDirect3DTexture9> t;texture(&t.p,D3DFMT_A8R8G8B8);bind(t);t->SetLOD(i%5);
                ss(D3DSAMP_MIPFILTER,D3DTEXF_NONE);tile(i);draw();
            }
            read();expect_tiles([](unsigned i){return colors[i%5];});
        });
        test("batched_render_to_texture",[&]{
            Com<IDirect3DTexture9> t;Com<IDirect3DSurface9> surface;
            check(d->CreateTexture(W,H,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&t.p,nullptr),"BatchRenderTexture");
            check(t->GetSurfaceLevel(0,&surface.p),"BatchRenderSurface");
            for(unsigned i=0;i<64;++i) {
                check(d->SetTexture(0,nullptr),"BatchUnbindSource");
                check(d->SetRenderTarget(0,surface),"BatchSourceTarget");
                rs(D3DRS_SCISSORTESTENABLE,FALSE);check(d->SetPixelShader(solid),"BatchSolidPS");
                color(colors[i%7]);draw();
                check(d->SetRenderTarget(0,target),"BatchOutputTarget");
                bind(t);tile(i);draw();
            }
            read();expect_tiles([](unsigned i){return colors[i%7];});
        });
        for(auto format : {D3DFMT_A8R8G8B8,D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5}) {
            const std::string prefix="texture_fmt"+std::to_string(unsigned(format));
            test(prefix+"_block_pattern",[&]{
                Com<IDirect3DTexture9> t;
                check(d->CreateTexture(20,12,1,0,format,D3DPOOL_MANAGED,&t.p,nullptr),"CreatePatternTexture");
                D3DLOCKED_RECT lock{}; check(t->LockRect(0,&lock,nullptr,0),"PatternLock");
                for(unsigned by=0;by<3;++by) for(unsigned bx=0;bx<5;++bx) {
                    const uint32_t c=colors[(bx+by)%3];
                    if(format==D3DFMT_A8R8G8B8) {
                        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x)
                            std::memcpy(static_cast<char*>(lock.pBits)+(by*4+y)*lock.Pitch+(bx*4+x)*4,&c,4);
                    } else {
                        const unsigned bytes=format==D3DFMT_DXT1?8:16;
                        auto* p=static_cast<unsigned char*>(lock.pBits)+by*lock.Pitch+bx*bytes;
                        std::memset(p,0,bytes);
                        if(format==D3DFMT_DXT3) std::memset(p,0xff,8);
                        if(format==D3DFMT_DXT5) p[0]=255;
                        if(bytes==16) p+=8;
                        const uint16_t rgb=uint16_t(((c>>19)&31)<<11 | ((c>>10)&63)<<5 | ((c>>3)&31));
                        std::memcpy(p,&rgb,2); std::memcpy(p+2,&rgb,2);
                    }
                }
                check(t->UnlockRect(0),"PatternUnlock"); bind(t); draw(); read();
                for(unsigned y=0;y<3;++y) for(unsigned x=0;x<5;++x) {
                    const unsigned cx=(2*x+1)*64/10, cy=(2*y+1)*64/6;
                    expect(colors[(x+y)%3],cx-2,cy-2,cx+2,cy+2);
                }
            });
            for(unsigned level=0;level<5;++level) test(prefix+"_mip"+std::to_string(level),[&]{
                Com<IDirect3DTexture9> t; texture(&t.p,format); bind(t);
                draw(.5f,64u>>level); read(); const unsigned n=64u>>level;
                expect(colors[level],1,1,n-1,n-1); });
            test(prefix+"_lod_rebind",[&]{ Com<IDirect3DTexture9> t; texture(&t.p,format); bind(t);
                draw(); t->SetLOD(2); draw(); read(); expect(colors[2]); });
            if(format==D3DFMT_DXT3 || format==D3DFMT_DXT5) test(prefix+"_alpha",[&]{
                Com<IDirect3DTexture9> t; texture(&t.p,format,true); bind(t); draw(); read();
                expect(format==D3DFMT_DXT3?0x88ff0000:0x80ff0000); });
        }
        test("resource_reuse",[&]{ for(unsigned i=0;i<24;++i) {
            Com<IDirect3DTexture9> t; texture(&t.p,D3DFMT_A8R8G8B8); bind(t); t->SetLOD(i%3);
            draw(); read(); expect(colors[i%3]); check(d->SetTexture(0,nullptr),"ReleaseBinding"); } });
        test("multithread_upload_render",[&]{
            std::atomic<bool> start{false}, workerFailed{false};
            std::vector<std::thread> workers;
            // Join even if thread creation or the main rendering path throws.
            struct Join {
                std::vector<std::thread>& threads; std::atomic<bool>& start;
                ~Join() { start.store(true); for(auto& t:threads) if(t.joinable()) t.join(); }
            } join{workers,start};
            for(unsigned n=0;n<3;++n) workers.emplace_back([&]{
                while(!start.load()) std::this_thread::yield();
                try { for(unsigned i=0;i<24;++i) {
                    Com<IDirect3DTexture9> t; texture(&t.p,D3DFMT_DXT5);
                } } catch(...) { workerFailed.store(true); }
            });
            start.store(true);
            color(0xff00ff00);
            for(unsigned i=0;i<24;++i) { draw(); read(); expect(0xff00ff00); }
            for(auto& t:workers) t.join();
            if(workerFailed.load()) throw std::runtime_error("worker texture upload failed");
        });
        check(d->SetTexture(0,nullptr),"FinalUnbind");
    }
};
} // namespace

unsigned run_if_requested(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags) {
    const char* directory=std::getenv("DIRT2_D3D9_TESTS");
    static std::atomic<bool> ran{false};
    if(!d3d || !directory || !*directory || ran.exchange(true)) return 0;
    const std::filesystem::path out(directory);
    std::error_code ec;
    std::filesystem::create_directories(out,ec);
    if(ec) { rt::log("d3d9-tests: output directory failed: %s",ec.message().c_str()); return 1; }
    Com<IDirect3DDevice9> device;
    std::vector<std::string> results;
    unsigned failures=1;
    std::string error;
    D3DADAPTER_IDENTIFIER9 identifier{};
    d3d->GetAdapterIdentifier(adapter,0,&identifier);
    try {
        D3DPRESENT_PARAMETERS pp{};
        pp.BackBufferWidth=W; pp.BackBufferHeight=H; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow=window; pp.Windowed=TRUE;
        check(d3d->CreateDevice(adapter,type,window,flags|D3DCREATE_MULTITHREADED,&pp,&device.p),"CreateTestDevice");
        Suite suite(device,out);
        suite.run(); results=suite.results; failures=suite.failures;
    } catch(const std::exception& e) { error=e.what(); }
    // A fresh game device is created after this returns: test state cannot leak.
    std::string report="{\"schema\":1,\"suite\":\"dirt2-d3d9-v1\",\"adapter\":"+quote(identifier.Description)+
        ",\"driver\":"+quote(identifier.Driver)+",\"width\":64,\"height\":64,\"failures\":"+
        std::to_string(failures)+",\"error\":"+quote(error)+",\"tests\":[";
    for(size_t i=0;i<results.size();++i) { if(i) report+=','; report+=results[i]; }
    report+="]}\n";
    FILE* f=std::fopen((out/"results.json").string().c_str(),"wb");
    const bool written=f && std::fwrite(report.data(),1,report.size(),f)==report.size();
    const bool closed=f && std::fclose(f)==0;
    rt::log("d3d9-tests: %zu cases, %u failures, %s; report %s",results.size(),failures,error.c_str(),
            written && closed?"saved":"WRITE FAILED");
    return failures + !error.empty() + !(written && closed);
}
} // namespace d3d9_tests
