// IDirect3DDevice9 on deko3d (see device.h). deko3d's default device already
// has D3D's conventions (depth 0..1, upper-left origin with NDC +Y at the top),
// so the viewport only carries D3D9's half-pixel offset; front faces are
// clockwise; depth bias and the rest follow DXVK's D3D9 (build/src/dxvk/src/d3d9).
// No <switch.h> here: libnx and DXVK's Windows headers do not mix.
#include "device.h"
#include "fixed_function.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <set>
#include <type_traits>

#include "recomp/host.h"
#include "recomp/sha256.h"
#include "../capture.h"

namespace deko {

// Opt-in conformance evidence. Queue DMA snapshots without a CPU wait or a
// new command list, then inspect them only after the frame fence completes.
// This keeps shader/state bindings and the test's command ordering intact.
static void probe_surface_pixels(Surface* surface, const char* operation) {
    if (!std::getenv("DIRT2_DEKO_PIXEL_PROBE") || !surface || !surface->image()) return;
    static unsigned serial = 0;
    if (serial >= 96 || surface->image()->ms_mode != DkMsMode_1x || surface->fmt->block != 1) return;
    const unsigned bytes = surface->fmt->bytes;
    if (bytes != 4 && bytes != 8 && bytes != 16) return;
    auto& g = gpu();
    // Eight samples include the depth-blit boundaries and RESZ's last row.
    const unsigned xs[] = {80, 240, 400, 560, 80, 240, 400, 560};
    const unsigned ys[] = {60, 60, 60, 60, 450, 450, 450, 450};
    const auto memory = g.readback.alloc(8 * 256, 256);
    if (!memory) return;
    const auto view = surface->view();
    dkCmdBufBarrier(g.cmd, DkBarrier_Full, 0);
    for (unsigned i = 0; i < 8; ++i) {
        const DkImageRect rect{surface->width < 80 ? (i % 4) * (surface->width - 1) / 3 : std::min(xs[i], surface->width - 1), surface->height < 60 ? (i / 4) * (surface->height - 1) : std::min(ys[i], surface->height - 1), 0, 1, 1, 1};
        const DkCopyBuf buffer{memory.gpu() + i * 256, bytes, 0};
        dkCmdBufCopyImageToBuffer(g.cmd, &view, &rect, &buffer, 0);
    }
    const unsigned id = serial++, format = unsigned(surface->d3dfmt), width = surface->width, height = surface->height;
    const std::string label(operation);
    g.defer([memory, id, format, width, height, bytes, label] {
        dkMemBlockFlushCpuCache(memory.block, memory.offset, memory.size);
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t words[4]{};
            std::memcpy(words, memory.cpu() + i * 256, bytes);
            float floats[4]{}; std::memcpy(floats, words, sizeof floats);
            rt::log("deko3d: pixel-probe %u %s fmt %u %ux%u sample %u raw %08X %08X %08X %08X float %.9g %.9g %.9g %.9g",
                id, label.c_str(), format, width, height, i, words[0], words[1], words[2], words[3],
                floats[0], floats[1], floats[2], floats[3]);
        }
        gpu().readback.free_now(memory);
    });
}

class SwapChain final : public StubIDirect3DSwapChain9 {
public:
    explicit SwapChain(Device* device):device_(device){SetDeviceParent(device);}
    SwapChain(Device* device, const D3DPRESENT_PARAMETERS& parameters, Surface* buffer)
        :device_(device), parameters_(parameters), buffer_(buffer) {
        SetDeviceParent(device);
        buffer_->AddRefPrivate();
        buffer_->Release();
    }
    ~SwapChain() override { if (buffer_) buffer_->ReleasePrivate(); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override{ApiLock lock(__func__);if(!out)return D3DERR_INVALIDCALL;*out=device_;device_->AddRef();return D3D_OK;}
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS* out) override{ApiLock lock(__func__);if(!out)return D3DERR_INVALIDCALL;*out=buffer_?parameters_:device_->pp_;return D3D_OK;}
    STDMETHOD(GetBackBuffer)(UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface9** out) override {
        if (!buffer_) return device_->GetBackBuffer(0,index,type,out);
        ApiLock lock(__func__);
        if (!out) return D3DERR_INVALIDCALL;
        *out=nullptr;
        if (index || type!=D3DBACKBUFFER_TYPE_MONO) return D3DERR_INVALIDCALL;
        buffer_->AddRef();*out=buffer_;return D3D_OK;
    }
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9* out) override{return device_->GetFrontBufferData(0,out);}
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE* out) override{return device_->GetDisplayMode(0,out);}
    STDMETHOD(Present)(const RECT*src,const RECT*dst,HWND window,const RGNDATA*dirty,DWORD) override{return device_->Present(src,dst,window,dirty);}
private:
    Device* device_;
    D3DPRESENT_PARAMETERS parameters_{};
    Surface* buffer_=nullptr;
};
HRESULT Device::GetSwapChain(UINT index,IDirect3DSwapChain9** out){
    ApiLock lock(__func__);if(!out)return D3DERR_INVALIDCALL;*out=nullptr;if(index)return D3DERR_INVALIDCALL;
    if(!swap_chain_){swap_chain_=new SwapChain(this);swap_chain_->AddRefPrivate();swap_chain_->Release();}
    swap_chain_->AddRef();*out=swap_chain_;return D3D_OK;
}
HRESULT Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* parameters, IDirect3DSwapChain9** out) {
    ApiLock lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    if (!parameters || !parameters->Windowed || parameters->BackBufferCount > 1 ||
        !multisample_valid(parameters->MultiSampleType, parameters->MultiSampleQuality)) return D3DERR_INVALIDCALL;
    auto pp=*parameters;
    if (!pp.BackBufferWidth) pp.BackBufferWidth=pp_.BackBufferWidth;
    if (!pp.BackBufferHeight) pp.BackBufferHeight=pp_.BackBufferHeight;
    if (pp.BackBufferFormat==D3DFMT_UNKNOWN) pp.BackBufferFormat=pp_.BackBufferFormat;
    pp.BackBufferCount=1;
    if (!pp.hDeviceWindow) pp.hDeviceWindow=window_;
    const auto* format=format_info(pp.BackBufferFormat);
    if (!format || format->depth || format->block>1 || format->dk==DkImageFormat_None) return D3DERR_INVALIDCALL;
    auto* buffer=new Surface(this,pp.BackBufferWidth,pp.BackBufferHeight,pp.BackBufferFormat,
        D3DUSAGE_RENDERTARGET,D3DPOOL_DEFAULT,false,pp.MultiSampleType,pp.MultiSampleQuality);
    if (!buffer->image() || !buffer->image()->mem) { buffer->Release(); return D3DERR_OUTOFVIDEOMEMORY; }
    *out=new SwapChain(this,pp,buffer);
    *parameters=pp;
    return D3D_OK;
}

HRESULT Device::GetFrontBufferData(UINT swapchain,IDirect3DSurface9* out){
    ApiLock lock(__func__);auto*surface=static_cast<Surface*>(out);
    if(swapchain || !surface || surface->owner()!=this || !front_buffer_.mem || surface->multisample!=D3DMULTISAMPLE_NONE ||
       (surface->d3dfmt!=D3DFMT_A8R8G8B8 && surface->d3dfmt!=D3DFMT_X8R8G8B8) || surface->pool!=D3DPOOL_SYSTEMMEM)return D3DERR_INVALIDCALL;
    const RECT rect{0,0,LONG(surface->width),LONG(surface->height)};
    GpuImage scaled;GpuImage*image=&front_buffer_;
    if(surface->width!=front_buffer_.width || surface->height!=front_buffer_.height){
        if(!scaled.create(DkImageType_2D,front_buffer_.format,surface->width,surface->height,1,1,1,DkImageFlags_Usage2DEngine|DkImageFlags_UsageRender))return D3DERR_OUTOFVIDEOMEMORY;
        const auto src=front_buffer_.view(),dst=scaled.view();const DkImageRect sr{0,0,0,front_buffer_.width,front_buffer_.height,1},dr{0,0,0,surface->width,surface->height,1};
        dkCmdBufBarrier(gpu().cmd,DkBarrier_Full,0);dkCmdBufBlitImage(gpu().cmd,&src,&sr,&dst,&dr,0,0);image=&scaled;
    }
    const uint32_t pitch=row_pitch(*surface->fmt,surface->width);
    if(surface->cpu)read_image(*image,0,0,surface->cpu,pitch,rect);
    else if(surface->tex){
        auto&core=*surface->tex;core.ensure_cpu_storage();const auto&layout=core.cpu_layout[size_t(surface->layer)*core.img.levels+surface->level];
        auto memory=gpu().readback.alloc(size_t(pitch)*surface->height,256);if(!memory){scaled.destroy();return D3DERR_OUTOFVIDEOMEMORY;}
        read_image(*image,0,0,memory,pitch,rect);std::memcpy(core.cpu_storage.data()+layout.offset,memory.cpu(),size_t(pitch)*surface->height);gpu().readback.free_now(memory);
    }else{scaled.destroy();return D3DERR_INVALIDCALL;}
    scaled.destroy();return D3D_OK;
}

namespace {

float as_float(DWORD v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void argb_to_float(D3DCOLOR c, float out[4]) {
    out[0] = float((c >> 16) & 0xFF) / 255.0f;
    out[1] = float((c >> 8) & 0xFF) / 255.0f;
    out[2] = float(c & 0xFF) / 255.0f;
    out[3] = float((c >> 24) & 0xFF) / 255.0f;
}

DkPrimitive primitive(D3DPRIMITIVETYPE t) {
    switch (t) {
        case D3DPT_POINTLIST: return DkPrimitive_Points;
        case D3DPT_LINELIST: return DkPrimitive_Lines;
        case D3DPT_LINESTRIP: return DkPrimitive_LineStrip;
        case D3DPT_TRIANGLESTRIP: return DkPrimitive_TriangleStrip;
        case D3DPT_TRIANGLEFAN: return DkPrimitive_TriangleFan;
        default: return DkPrimitive_Triangles;
    }
}

uint32_t vertex_count(D3DPRIMITIVETYPE t, uint32_t prims) {
    switch (t) {
        case D3DPT_POINTLIST: return prims;
        case D3DPT_LINELIST: return prims * 2;
        case D3DPT_LINESTRIP: return prims + 1;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return prims + 2;
        default: return prims * 3;
    }
}

// D3D9 blend factors 1..11 are deko3d's; the rest need mapping.
DkBlendFactor blend_factor(DWORD b) {
    switch (b) {
        case D3DBLEND_BOTHSRCALPHA: return DkBlendFactor_SrcAlpha;
        case D3DBLEND_BOTHINVSRCALPHA: return DkBlendFactor_InvSrcAlpha;
        case D3DBLEND_BLENDFACTOR: return DkBlendFactor_ConstColor;
        case D3DBLEND_INVBLENDFACTOR: return DkBlendFactor_InvConstColor;
        default: return b >= 1 && b <= 11 ? DkBlendFactor(b) : DkBlendFactor_One;
    }
}

DkWrapMode wrap_mode(DWORD a) {
    switch (a) {
        case D3DTADDRESS_MIRROR: return DkWrapMode_MirroredRepeat;
        case D3DTADDRESS_CLAMP: return DkWrapMode_ClampToEdge;
        case D3DTADDRESS_BORDER: return DkWrapMode_ClampToBorder;
        case D3DTADDRESS_MIRRORONCE: return DkWrapMode_MirrorClampToEdge;
        default: return DkWrapMode_Repeat;
    }
}

// D3DDECLTYPE -> deko3d attribute format.
bool attrib_format(BYTE type, DkVtxAttribState& a) {
    a.isBgra = 0;
    switch (type) {
        case D3DDECLTYPE_FLOAT1: a.size = DkVtxAttribSize_1x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT2: a.size = DkVtxAttribSize_2x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT3: a.size = DkVtxAttribSize_3x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT4: a.size = DkVtxAttribSize_4x32; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_D3DCOLOR: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Unorm; a.isBgra = 1; return true;
        case D3DDECLTYPE_UBYTE4: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Uscaled; return true;
        case D3DDECLTYPE_SHORT2: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Sscaled; return true;
        case D3DDECLTYPE_SHORT4: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Sscaled; return true;
        case D3DDECLTYPE_UBYTE4N: a.size = DkVtxAttribSize_4x8; a.type = DkVtxAttribType_Unorm; return true;
        case D3DDECLTYPE_SHORT2N: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Snorm; return true;
        case D3DDECLTYPE_SHORT4N: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Snorm; return true;
        case D3DDECLTYPE_USHORT2N: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Unorm; return true;
        case D3DDECLTYPE_USHORT4N: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Unorm; return true;
        case D3DDECLTYPE_UDEC3: a.size = DkVtxAttribSize_10_10_10_2; a.type = DkVtxAttribType_Uscaled; return true;
        case D3DDECLTYPE_DEC3N: a.size = DkVtxAttribSize_10_10_10_2; a.type = DkVtxAttribType_Snorm; return true;
        case D3DDECLTYPE_FLOAT16_2: a.size = DkVtxAttribSize_2x16; a.type = DkVtxAttribType_Float; return true;
        case D3DDECLTYPE_FLOAT16_4: a.size = DkVtxAttribSize_4x16; a.type = DkVtxAttribType_Float; return true;
        default: return false;
    }
}

TextureCore* core_of(IDirect3DBaseTexture9* t) { return texture_core(t); }

int sampler_index(DWORD s) { return s >= D3DVERTEXTEXTURESAMPLER0 ? int(s - D3DVERTEXTEXTURESAMPLER0) + 16 : int(s); }

constexpr uint32_t kUboSize = 8192;  // 256 float4 + 16 int4 + 16 bool registers, as MojoShader packs them

// FVF (d3dx9 helpers draw with FVFs) -> vertex elements.
std::vector<D3DVERTEXELEMENT9> fvf_elements(DWORD fvf) {
    std::vector<D3DVERTEXELEMENT9> e;
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        e.push_back(D3DVERTEXELEMENT9{0, off, type, D3DDECLMETHOD_DEFAULT, usage, index});
        off = WORD(off + size);
    };
    switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZ: add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12); break;
        case D3DFVF_XYZRHW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16); break;
        case D3DFVF_XYZW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16); break;
        case D3DFVF_XYZB1: case D3DFVF_XYZB2: case D3DFVF_XYZB3: case D3DFVF_XYZB4: case D3DFVF_XYZB5: {
            add(D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0,12);
            unsigned betas=((fvf&D3DFVF_POSITION_MASK)-D3DFVF_XYZB1)/2+1;
            BYTE index_type=0xff;
            if(fvf&D3DFVF_LASTBETA_D3DCOLOR)index_type=D3DDECLTYPE_D3DCOLOR;
            else if(fvf&D3DFVF_LASTBETA_UBYTE4)index_type=D3DDECLTYPE_UBYTE4;
            else if((fvf&D3DFVF_POSITION_MASK)==D3DFVF_XYZB5)index_type=D3DDECLTYPE_FLOAT1;
            if(index_type!=0xff)--betas;
            if(betas)add(BYTE(D3DDECLTYPE_FLOAT1+betas-1),D3DDECLUSAGE_BLENDWEIGHT,0,WORD(4*betas));
            if(index_type!=0xff)add(index_type,D3DDECLUSAGE_BLENDINDICES,0,4);
            break;
        }
        default: break;
    }
    if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    const DWORD tex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (DWORD i = 0; i < tex; ++i) {
        static const BYTE types[] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
        static const WORD sizes[] = {8, 12, 16, 4};
        const DWORD fmt = (fvf >> (16 + i * 2)) & 3;
        add(types[fmt], D3DDECLUSAGE_TEXCOORD, BYTE(i), sizes[fmt]);
    }
    e.push_back(D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0});
    return e;
}

}  // namespace

template <typename T>
void Device::set_ref(T*& slot, T* obj) {
    ApiLock api_lock(__func__);
    if (slot == obj) return;
    if constexpr (std::is_same_v<T, IDirect3DBaseTexture9>) {
        if (obj) texture_add_private(obj);
        if (slot) texture_release_private(slot);
    } else {
        if (obj) obj->T::AddRefPrivate();
        if (slot) slot->T::ReleasePrivate();
    }
    slot = obj;
}

Device::Device(D3D9* parent, HWND window, DWORD flags, const D3DPRESENT_PARAMETERS& pp)
    : parent_(parent), window_(window), flags_(flags), pp_(pp) {
    parent_->AddRef();
    const char* rebind = std::getenv("DIRT2_DEKO_REBIND_STATE");
    rebind_state_ = rebind && std::strcmp(rebind, "0") != 0;
    if (rebind_state_) rt::log("deko3d: diagnostic state rebinding enabled; asynchronous submission unchanged");
    // D3D9's default render states.
    rs_[D3DRS_ZENABLE] = pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    rs_[D3DRS_FILLMODE] = D3DFILL_SOLID;
    rs_[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs_[D3DRS_ZWRITEENABLE] = TRUE;
    rs_[D3DRS_LASTPIXEL] = TRUE;
    rs_[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs_[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs_[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs_[D3DRS_STENCILFAIL] = rs_[D3DRS_STENCILZFAIL] = rs_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs_[D3DRS_STENCILMASK] = rs_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs_[D3DRS_CCW_STENCILFAIL] = rs_[D3DRS_CCW_STENCILZFAIL] = rs_[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs_[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs_[D3DRS_COLORWRITEENABLE] = rs_[D3DRS_COLORWRITEENABLE1] = rs_[D3DRS_COLORWRITEENABLE2] =
        rs_[D3DRS_COLORWRITEENABLE3] = 0xF;
    rs_[D3DRS_BLENDOP] = rs_[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    rs_[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs_[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    rs_[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    for (auto& matrix : transforms_)
        for (unsigned i=0;i<4;++i) matrix.m[i][i]=1.0f;
    material_.Diffuse=material_.Ambient=D3DCOLORVALUE{1,1,1,1};
    rs_[D3DRS_CLIPPING]=rs_[D3DRS_LIGHTING]=rs_[D3DRS_COLORVERTEX]=rs_[D3DRS_LOCALVIEWER]=TRUE;
    rs_[D3DRS_DIFFUSEMATERIALSOURCE]=D3DMCS_COLOR1;
    rs_[D3DRS_SPECULARMATERIALSOURCE]=D3DMCS_COLOR2;
    rs_[D3DRS_TEXTUREFACTOR]=rs_[D3DRS_MULTISAMPLEMASK]=0xffffffff;
    rs_[D3DRS_MULTISAMPLEANTIALIAS]=TRUE;
    rs_[D3DRS_FOGEND]=rs_[D3DRS_FOGDENSITY]=0x3f800000;
    rs_[D3DRS_POINTSIZE]=rs_[D3DRS_POINTSIZE_MIN]=rs_[D3DRS_POINTSCALE_A]=0x3f800000;
    rs_[D3DRS_POINTSIZE_MAX]=0x43800000; // same 256.0f as MaxPointSize caps
    rs_[D3DRS_POSITIONDEGREE]=D3DDEGREE_CUBIC;
    rs_[D3DRS_NORMALDEGREE]=D3DDEGREE_LINEAR;
    rs_[D3DRS_MINTESSELLATIONLEVEL]=rs_[D3DRS_MAXTESSELLATIONLEVEL]=rs_[D3DRS_ADAPTIVETESS_Z]=0x3f800000;
    for(unsigned stage=0;stage<8;++stage) {
        auto* t=tss_[stage];
        t[D3DTSS_COLOROP]=stage?D3DTOP_DISABLE:D3DTOP_MODULATE;
        t[D3DTSS_ALPHAOP]=stage?D3DTOP_DISABLE:D3DTOP_SELECTARG1;
        t[D3DTSS_COLORARG1]=t[D3DTSS_ALPHAARG1]=D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2]=t[D3DTSS_ALPHAARG2]=D3DTA_CURRENT;
        t[D3DTSS_COLORARG0]=t[D3DTSS_ALPHAARG0]=D3DTA_CURRENT;
        t[D3DTSS_RESULTARG]=D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX]=stage;
    }
    for (auto& s : ss_) {
        s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        s[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        s[D3DSAMP_MAXANISOTROPY] = 1;
    }
}

Device::~Device() {
    if(gpu().state_owner==this)gpu().state_owner=nullptr;
    fixed_function_.reset();
    if(vertex_state_)gpu().defer([a=vertex_state_]{gpu().buffers.free_now(a);});
    if(fragment_state_)gpu().defer([a=fragment_state_]{gpu().buffers.free_now(a);});
    if(swap_chain_)swap_chain_->ReleasePrivate();
    if (recording_) recording_->ReleasePrivate();
    if (gpu().queue && gpu().cmd) gpu().flush(true,"device destruction");
    else if (gpu().queue) dkQueueWaitIdle(gpu().queue);
    for (auto*& t : textures_) set_ref(t, static_cast<IDirect3DBaseTexture9*>(nullptr));
    for (auto& s : streams_) set_ref(s.vb, static_cast<VertexBuffer*>(nullptr));
    set_ref(indices_, static_cast<IndexBuffer*>(nullptr));
    set_ref(decl_, static_cast<VertexDeclaration*>(nullptr));
    set_ref(vs_, static_cast<VertexShader*>(nullptr));
    set_ref(ps_, static_cast<PixelShader*>(nullptr));
    for (auto*& t : targets_) set_ref(t, static_cast<Surface*>(nullptr));
    set_ref(depth_, static_cast<Surface*>(nullptr));
    for (auto* buffer : extra_back_buffers_) buffer->ReleasePrivate();
    extra_back_buffers_.clear();
    if (back_buffer_) back_buffer_->ReleasePrivate();
    if (auto_depth_) auto_depth_->ReleasePrivate();
    for (auto& entry : fvf_decls_) entry.second->ReleasePrivate();
    dummy_.destroy();dummy_cube_.destroy();dummy_volume_.destroy();
    front_buffer_.destroy();
    for(auto& image:feedback_images_)image.destroy();
    for (const auto& allocation : ubo_) {
        gpu().defer([allocation] { gpu().buffers.free_now(allocation); });
    }
    for (const auto& entry : samplers_) {
        const int id=entry.second;
        gpu().defer([id] { gpu().sampler_ids.give(id); });
    }
    // All recorded GPU work was submitted and completed before releasing these
    // resources. Reclaim deferred allocations even in API tests with no Present.
    for (auto& frame : gpu().frames) {
        auto completed=std::move(frame.on_done);
        frame.on_done.clear();
        for(auto& release:completed)release();
    }
    parent_->Release();
}

bool Device::init() {
    ApiLock api_lock(__func__);
    Gpu& g = gpu();
    if (!g.device && !g.init()) {
        rt::log("deko3d: cannot create the device");
        return false;
    }
    static bool pack_loaded = false;
    if (!pack_loaded) {
        const char* data = std::getenv("DIRT2_DATA_DIR");
        pack_loaded = load_shader_pack(std::string(data ? data : ".") + "/dirt2.dkshpack");
    }
    for (Alloc& u : ubo_) u = g.buffers.alloc(kUboSize, DK_UNIFORM_BUF_ALIGNMENT);
    for (auto& cache : constant_upload_) cache.reset();
    staging_.resize(kUboSize);
    dummy_.create(DkImageType_2D, DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, 1, DkImageFlags_Usage2DEngine|DkImageFlags_UsageRender);
    Alloc black = g.cur().transient.alloc(256, 256);
    std::memset(black.cpu(), 0, 256);black.cpu()[3]=255;
    upload(dummy_, 0, 0, black, 4, RECT{0, 0, 1, 1});
    dummy_cube_.create(DkImageType_Cubemap,DkImageFormat_RGBA8_Unorm,1,1,1,1,6,DkImageFlags_Usage2DEngine);
    dummy_volume_.create(DkImageType_3D,DkImageFormat_RGBA8_Unorm,1,1,1,1,1,0);
    for(unsigned face=0;face<6;++face)upload(dummy_cube_,face,0,black,4,RECT{0,0,1,1});
    upload(dummy_volume_,0,0,black,4,RECT{0,0,1,1});
    make_back_buffer();
    if (const char* d = std::getenv("DIRT2_DEKO_DUMP")) dump_frame_ = uint32_t(std::atoi(d));
    if (const char* d = std::getenv("DIRT2_FRAME_DUMP")) state_log_frame_ = uint32_t(std::atoi(d));
    if (const char* d = std::getenv("DIRT2_DEKO_UI_PROBE")) ui_probe_frame_ = uint32_t(std::atoi(d));
    if (const char* d = std::getenv("DIRT2_DEKO_MRT_PROBE")) mrt_probe_frame_ = uint32_t(std::atoi(d));
    if (std::getenv("DIRT2_DEKO_SELFTEST")) self_test();
    return back_buffer_ && back_buffer_->ok();
}

void Device::self_test() {
    ApiLock api_lock(__func__);
    Gpu& g = gpu();
    rt::log("deko3d: self-test: 180 frames of cleared swap images");
    auto t0 = std::chrono::steady_clock::now(), last = t0;
    for (int i = 1; i <= 180; ++i) {
        const int slot = g.acquire_swap_image();
        DkImageView v;
        dkImageViewDefaults(&v, &g.swap_images[slot]);
        dkCmdBufBindRenderTarget(g.cmd, &v, nullptr);
        const DkScissor sc{0, 0, g.swap_w, g.swap_h};
        dkCmdBufSetScissors(g.cmd, 0, &sc, 1);
        const float t = float(i % 60) / 60.0f;
        dkCmdBufClearColorFloat(g.cmd, 0, DkColorMask_RGBA, t, 0.5f * (1.0f - t), 1.0f - t, 1.0f);
        g.end_frame(slot);
        if (i % 30 == 0) {
            const auto now = std::chrono::steady_clock::now();
            rt::log("deko3d: self-test frame %d, %.1f ms/frame", i,
                    std::chrono::duration<double, std::milli>(now - last).count() / 30.0);
            last = now;
        }
    }
    rt::log("deko3d: self-test done in %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    dirty_ = kAll;
}

void Device::sync(const char* fmt, ...) {
    ApiLock api_lock(__func__);
    static const bool on = std::getenv("DIRT2_DEKO_SYNC") != nullptr;
    if (!on) { gpu().checkpoint(); return; }
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    gpu().flush(true, what);
}

void Device::sync_after_draw(const char* kind, uint32_t prims, uint32_t instances, int base, uint32_t start) {
    ApiLock api_lock(__func__);
    ++draw_total_;
    probe_surface_pixels(targets_[0], kind);
    probe_surface_pixels(depth_, "draw-depth");
    if (mrt_probe_frame_ && present_count_ + 1 == mrt_probe_frame_ && targets_[1]) {
        ++mrt_probe_draw_;
        rt::log("deko3d: MRT probe draw %u: %s vs %s ps %s, primitives %u instances %u base %d start %u",
                mrt_probe_draw_, kind, draw_vs_.meta->name.c_str(), draw_ps_.meta->name.c_str(), prims, instances, base, start);
        probe_mrt("draw", false);
    }
    static const bool sync = std::getenv("DIRT2_DEKO_SYNC") != nullptr;
    if (!sync) { gpu().checkpoint(); return; }
    char what[256];
    std::snprintf(what, sizeof what, "draw %llu (%s: vs %s ps %s, %u primitives, %u instances, base %d, start %u, frame %u)",
                  (unsigned long long)draw_total_, kind, draw_vs_.meta->name.c_str(), draw_ps_.meta->name.c_str(), prims,
                  instances, base, start, present_count_);
    gpu().flush(true, what);
}

void Device::make_back_buffer() {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    for (auto* buffer : extra_back_buffers_) buffer->ReleasePrivate();
    extra_back_buffers_.clear();
    if (back_buffer_) back_buffer_->ReleasePrivate();
    if (auto_depth_) auto_depth_->ReleasePrivate();
    back_buffer_ = auto_depth_ = nullptr;
    front_buffer_.destroy();
    const UINT w = pp_.BackBufferWidth ? pp_.BackBufferWidth : gpu().swap_w;
    const UINT h = pp_.BackBufferHeight ? pp_.BackBufferHeight : gpu().swap_h;
    pp_.BackBufferWidth = w;
    pp_.BackBufferHeight = h;
    if (pp_.BackBufferFormat == D3DFMT_UNKNOWN) pp_.BackBufferFormat = D3DFMT_X8R8G8B8;
    back_buffer_ = new Surface(this, w, h, pp_.BackBufferFormat, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, false, pp_.MultiSampleType, pp_.MultiSampleQuality);
    if (pp_.EnableAutoDepthStencil)
        auto_depth_ = new Surface(this, w, h, pp_.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, false, pp_.MultiSampleType, pp_.MultiSampleQuality);
    back_buffer_->AddRefPrivate();
    back_buffer_->Release();
    pp_.BackBufferCount = std::clamp<UINT>(pp_.BackBufferCount, 1, 3);
    for (UINT i = 1; i < pp_.BackBufferCount; ++i) {
        auto* buffer = new Surface(this, w, h, pp_.BackBufferFormat, D3DUSAGE_RENDERTARGET,
            D3DPOOL_DEFAULT, false, pp_.MultiSampleType, pp_.MultiSampleQuality);
        buffer->AddRefPrivate();buffer->Release();extra_back_buffers_.push_back(buffer);
    }
    if (auto_depth_) { auto_depth_->AddRefPrivate();auto_depth_->Release(); }
    for (auto*& t : targets_) set_ref(t, static_cast<Surface*>(nullptr));
    set_ref(targets_[0], back_buffer_);
    set_ref(depth_, auto_depth_);
    viewport_ = D3DVIEWPORT9{0, 0, w, h, 0.0f, 1.0f};
    scissor_ = RECT{0, 0, LONG(w), LONG(h)};
    dirty_ = kAll;
    rt::log("deko3d: back buffer %ux%u format %u, depth %u", w, h, uint32_t(pp_.BackBufferFormat),
            pp_.EnableAutoDepthStencil ? uint32_t(pp_.AutoDepthStencilFormat) : 0u);
}

// ---- device ----

HRESULT Device::GetDirect3D(IDirect3D9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    parent_->AddRef();
    *out = parent_;
    return D3D_OK;
}
HRESULT Device::GetDeviceCaps(D3DCAPS9* caps) {
    ApiLock api_lock(__func__);
    if (!caps) return D3DERR_INVALIDCALL;
    fill_caps(caps);
    return D3D_OK;
}
HRESULT Device::GetDisplayMode(UINT, D3DDISPLAYMODE* mode) {
    ApiLock api_lock(__func__); return parent_->GetAdapterDisplayMode(0, mode); }
HRESULT Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) {
    ApiLock api_lock(__func__);
    if (!p) return D3DERR_INVALIDCALL;
    p->AdapterOrdinal = 0;
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = window_;
    p->BehaviorFlags = flags_;
    return D3D_OK;
}
HRESULT Device::Reset(D3DPRESENT_PARAMETERS* pp) {
    ApiLock api_lock(__func__);
    if (!pp || !multisample_valid(pp->MultiSampleType,pp->MultiSampleQuality)) return D3DERR_INVALIDCALL;
    pp_ = *pp;
    in_scene_ = false;
    make_back_buffer();
    *pp = pp_;
    return D3D_OK;
}
HRESULT Device::GetBackBuffer(UINT swapchain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (swapchain || index > extra_back_buffers_.size() || type != D3DBACKBUFFER_TYPE_MONO) return D3DERR_INVALIDCALL;
    auto* buffer = index ? extra_back_buffers_[index - 1] : back_buffer_;
    buffer->AddRef();*out = buffer;
    return D3D_OK;
}

void Device::advance_back_buffers() {
    if (pp_.SwapEffect != D3DSWAPEFFECT_COPY && !extra_back_buffers_.empty()) {
        // D3D keeps each COM surface identity stable while rotating storage.
        Surface* previous = back_buffer_;
        for (auto* buffer : extra_back_buffers_) {
            std::swap(previous->own, buffer->own);
            previous = buffer;
        }
    }
}

HRESULT Device::Present(const RECT*, const RECT*, HWND, const RGNDATA*) {
    gpu().state_owner=nullptr;
    ApiLock api_lock(__func__);
    Gpu& g = gpu();
    if (dump_frame_ && present_count_ + 1 == dump_frame_) dump_targets();
    const int slot = g.acquire_swap_image();
    if (GpuImage* bb = back_buffer_->image()) {
        if(!front_buffer_.mem)front_buffer_.create(DkImageType_2D,bb->format,bb->width,bb->height,1,1,1,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
        const DkImageView original=bb->view(),front=front_buffer_.view();
        dkCmdBufBarrier(g.cmd,DkBarrier_Full,0);
        if(bb->ms_mode!=DkMsMode_1x)dkCmdBufResolveImage(g.cmd,&original,&front);
        else{const DkImageRect rect{0,0,0,bb->width,bb->height,1};dkCmdBufCopyImage(g.cmd,&original,&rect,&front,&rect,0);}
        DkImageView src = front;
        DkImageView dst;
        dkImageViewDefaults(&dst, &g.swap_images[slot]);
        const DkImageRect sr{0, 0, 0, bb->width, bb->height, 1};
        const DkImageRect dr{0, 0, 0, g.swap_w, g.swap_h, 1};
        dkCmdBufBarrier(g.cmd, DkBarrier_Full, 0);  // the frame's rendering done before the blit reads it
        dkCmdBufBlitImage(g.cmd, &src, &sr, &dst, &dr, DkBlitFlag_FilterLinear, 0);
        sync("Present blit (format %d %ux%u -> swap image %d)", int(bb->format), bb->width, bb->height, slot);
    }
    advance_back_buffers();
    g.end_frame(slot);
    dirty_ = kAll;
    // Per-frame cache maintenance: CPU writes to memory the GPU may hold in L2.
    dkCmdBufBarrier(g.cmd, DkBarrier_None, DkInvalidateFlags_L2Cache | DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
    ++present_count_;
    static const bool log_perf = std::getenv("DIRT2_DEKO_LOG_PERF") != nullptr;
    if (log_perf && present_count_ <= 5) rt::log("deko3d: Present %u (%u draws, %u skipped so far)", present_count_, draws_, skipped_draws_);
    // Frame rate every 300 presents or 5 s, whichever comes first (loading screens present rarely).
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!fps_t0_) fps_t0_ = now;
    const uint32_t frames = present_count_ - fps_presents_;
    if (frames >= 300 || now - fps_t0_ >= 5'000'000'000ull) {
        const double secs = double(now - fps_t0_) / 1e9;
        if (log_perf) {
            rt::log("deko3d: %u presents, %.1f fps, %u draws/frame, %u skipped", present_count_, frames / secs,
                    frames ? draws_ / frames : 0, frames ? skipped_draws_ / frames : 0);
            rt::log("deko3d: constants %llu/%llu bytes uploaded (%.1f%% saved)",
                    static_cast<unsigned long long>(constant_bytes_sent_),
                    static_cast<unsigned long long>(constant_bytes_full_),
                    constant_bytes_full_ ? 100.0 * (1.0 - double(constant_bytes_sent_) / constant_bytes_full_) : 0.0);
        }
        constant_bytes_full_ = constant_bytes_sent_ = 0;
        fps_t0_ = now;
        fps_presents_ = present_count_;
        draws_ = skipped_draws_ = 0;
    }
    return D3D_OK;
}

// Event queries are completion fences. A synchronous implementation is legal:
// Issue(END) submits and waits for all commands preceding the marker.
namespace {
class EventQuery final : public StubIDirect3DQuery9 {
public:
    explicit EventQuery(Device* device) : device_(device) { SetDeviceParent(device); }
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override {
        ApiLock lock(__func__); if (!out) return D3DERR_INVALIDCALL;
        *out=device_; device_->AddRef(); return D3D_OK;
    }
    STDMETHOD_(D3DQUERYTYPE, GetType)() override { return D3DQUERYTYPE_EVENT; }
    STDMETHOD_(DWORD, GetDataSize)() override { return sizeof(BOOL); }
    STDMETHOD(Issue)(DWORD flags) override {
        ApiLock lock(__func__);
        if (flags != D3DISSUE_END) return D3DERR_INVALIDCALL;
        gpu().flush(true, "event query completion"); return D3D_OK;
    }
    STDMETHOD(GetData)(void* data, DWORD size, DWORD flags) override {
        ApiLock lock(__func__);
        if ((flags & ~D3DGETDATA_FLUSH) || (data && size != sizeof(BOOL))) return D3DERR_INVALIDCALL;
        if (data) { const BOOL done=TRUE; std::memcpy(data,&done,sizeof(done)); }
        return D3D_OK;
    }
private:
    Device* device_;
};
}
HRESULT Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** out) {
    ApiLock lock(__func__);
    if (type != D3DQUERYTYPE_EVENT) return D3DERR_NOTAVAILABLE;
    if (out) *out=new EventQuery(this);
    return D3D_OK;
}

// ---- resources ----

namespace {
bool valid_texture_creation(UINT w, UINT h, DWORD usage, D3DFORMAT format, D3DPOOL pool) {
    if (!w || !h || !format_info(format) || (usage & D3DUSAGE_WRITEONLY)) return false;
    if ((format == D3DFMT_YUY2 || format == D3DFMT_UYVY) && (pool != D3DPOOL_SCRATCH || usage)) return false;
    if ((usage & D3DUSAGE_DYNAMIC) && (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SCRATCH)) return false;
    switch (format) {
        case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
            return !(w % 4 || h % 4);
        default: return true;
    }
}
}

HRESULT Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                              IDirect3DTexture9** out, HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    if(!valid_texture_creation(w,h,usage,f,pool))return D3DERR_INVALIDCALL;
    auto* t = new Texture;
    if (!t->init(this, w, h, levels, usage, f, pool)) {
        rt::log("deko3d: CreateTexture(%ux%u, %u levels, usage %X, format %u) failed", w, h, levels, usage, uint32_t(f));
        t->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
HRESULT Device::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                    IDirect3DVolumeTexture9** out, HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    if(!d || !valid_texture_creation(w,h,usage,f,pool))return D3DERR_INVALIDCALL;
    auto* t = new VolumeTexture;
    if (!t->init(this, w, h, d, levels, usage, f, pool)) {
        t->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
HRESULT Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                  IDirect3DCubeTexture9** out, HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    if(!valid_texture_creation(edge,edge,usage,f,pool))return D3DERR_INVALIDCALL;
    auto* t = new CubeTexture;
    if (!t->init(this, edge, levels, usage, f, pool)) {
        t->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
HRESULT Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out,
                                   HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    if(!length || pool==D3DPOOL_SCRATCH)return D3DERR_INVALIDCALL;
    auto* b = new VertexBuffer(this, length, usage, fvf, pool);
    if (!b->ok()) {
        b->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = b;
    return D3D_OK;
}
HRESULT Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9** out,
                                  HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    if(!length || pool==D3DPOOL_SCRATCH)return D3DERR_INVALIDCALL;
    auto* b = new IndexBuffer(this, length, usage, f, pool);
    if (!b->ok()) {
        b->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = b;
    return D3D_OK;
}
HRESULT Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL lockable,
                                   IDirect3DSurface9** out, HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    const auto* info=format_info(f);
    if(!w || !h || !info || !(!info->depth && info->block==1) || !multisample_valid(ms,quality) || (lockable && ms!=D3DMULTISAMPLE_NONE))return D3DERR_INVALIDCALL;
    auto* s = new Surface(this, w, h, f, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, false, ms, quality);
    if (!s->ok()) {
        s->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = s;
    return D3D_OK;
}
HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL,
                                          IDirect3DSurface9** out, HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    const auto* info=format_info(f);
    if(!w || !h || !info || !(info->depth) || !multisample_valid(ms,quality))return D3DERR_INVALIDCALL;
    auto* s = new Surface(this, w, h, f, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, false, ms, quality);
    if (!s->ok()) {
        s->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = s;
    return D3D_OK;
}
HRESULT Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL pool, IDirect3DSurface9** out,
                                            HANDLE*) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=nullptr;
    const auto* info=format_info(f);
    if(!info || pool==D3DPOOL_MANAGED || !valid_texture_creation(w,h,0,f,pool))return D3DERR_INVALIDCALL;
    auto* s = new Surface(this, w, h, f, 0, pool, pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH);
    if (!s->ok()) {
        s->Release();
        *out = nullptr;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = s;
    return D3D_OK;
}

void Device::clear_new_image(GpuImage& img, bool depth) {
    gpu().state_owner=nullptr;
    ApiLock api_lock(__func__);
    DkCmdBuf cmd = gpu().cmd;
    const uint32_t faces = img.type == DkImageType_Cubemap ? 6 : 1;
    for (uint32_t level = 0; level < img.levels; ++level)
        for (uint32_t f = 0; f < faces; ++f) {
            const DkImageView v = img.view(level, f, false);
            if (depth) dkCmdBufBindRenderTargets(cmd, nullptr, 0, &v);
            else dkCmdBufBindRenderTarget(cmd, &v, nullptr);
            const DkScissor sc{0, 0, mip(img.width, level), mip(img.height, level)};
            dkCmdBufSetScissors(cmd, 0, &sc, 1);
            if (depth) dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
            else dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    dirty_ |= kTargets | kViewport;
    copy_barrier_ = true;
}

void Device::read_image(GpuImage& img, uint32_t layer, uint32_t level, const Alloc& out, uint32_t pitch_bytes,
                        const RECT& r, uint32_t depth) {
    ApiLock api_lock(__func__);
    dkCmdBufBarrier(gpu().cmd, DkBarrier_Full, 0);  // the image's rendering must be done before the copy reads it
    const DkImageView v = img.view(level, layer, false);
    const DkImageRect rect{uint32_t(r.left), uint32_t(r.top), 0, uint32_t(r.right - r.left), uint32_t(r.bottom - r.top), depth};
    const DkCopyBuf dst{out.gpu(), pitch_bytes, 0};
    dkCmdBufCopyImageToBuffer(gpu().cmd, &v, &rect, &dst, 0);
    gpu().flush(true, "a render target read-back");
    dkMemBlockFlushCpuCache(out.block, out.offset, out.size);
}

// DkCopyBuf's rowLength and imageHeight are byte strides (deko3d dk_image.cpp),
// not pixel counts: a pixel pitch made the copy engine walk the wrong rows and
// hung the GPU on block-compressed uploads.
void Device::upload(GpuImage& img, uint32_t layer, uint32_t level, const Alloc& data, uint32_t pitch_bytes,
                    const RECT& r, uint32_t z, uint32_t depth, uint32_t slice_bytes) {
    ApiLock api_lock(__func__);
    Alloc pixels = data;
    if (img.expanded_bc) {
        // BC2/DXT3 and BC3/DXT5 share an opaque BC1 colour block.
        // Keep D3D's compressed lock layout; expand only at the GPU boundary.
        const uint32_t w = uint32_t(r.right - r.left), h = uint32_t(r.bottom - r.top);
        const uint32_t out_pitch = w * 4, out_slice = out_pitch * h;
        pixels = gpu().cur().transient.alloc(out_slice * depth, 256);
        if (!pixels) return;
        const uint32_t in_slice = slice_bytes ? slice_bytes : pitch_bytes * ((h + 3) / 4);
        for (uint32_t slice = 0; slice < depth; ++slice)
            for (uint32_t by = 0; by < (h + 3) / 4; ++by)
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx) {
                    const uint8_t* b = data.cpu() + slice * in_slice + by * pitch_bytes + bx * 16;
                    uint16_t endpoints[2];
                    uint32_t indices;
                    std::memcpy(endpoints, b + 8, 4);
                    std::memcpy(&indices, b + 12, 4);
                    uint8_t colours[4][3];
                    for (unsigned i = 0; i < 2; ++i) {
                        const uint16_t c = endpoints[i];
                        const unsigned red = c >> 11, green = (c >> 5) & 63, blue = c & 31;
                        colours[i][0] = uint8_t((red << 3) | (red >> 2));
                        colours[i][1] = uint8_t((green << 2) | (green >> 4));
                        colours[i][2] = uint8_t((blue << 3) | (blue >> 2));
                    }
                    for (unsigned c = 0; c < 3; ++c) {
                        colours[2][c] = uint8_t((2 * colours[0][c] + colours[1][c]) / 3);
                        colours[3][c] = uint8_t((colours[0][c] + 2 * colours[1][c]) / 3);
                    }
                    uint8_t alpha[8] = {b[0], b[1]};
                    uint64_t alpha_indices = 0;
                    if (img.expanded_bc == 3) {
                        std::memcpy(&alpha_indices, b + 2, 6);
                        if (alpha[0] > alpha[1]) {
                            for (unsigned i = 1; i <= 6; ++i)
                                alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
                        } else {
                            for (unsigned i = 1; i <= 4; ++i)
                                alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
                            alpha[6] = 0;
                            alpha[7] = 255;
                        }
                    }
                    for (unsigned i = 0; i < 16; ++i) {
                        const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                        if (x >= w || y >= h) continue;
                        uint8_t* out = pixels.cpu() + slice * out_slice + y * out_pitch + x * 4;
                        std::memcpy(out, colours[(indices >> (2 * i)) & 3], 3);
                        out[3] = img.expanded_bc == 3 ? alpha[(alpha_indices >> (3 * i)) & 7]
                                                    : uint8_t(((b[i / 2] >> (4 * (i % 2))) & 15) * 17);
                    }
                }
        pitch_bytes = out_pitch;
        slice_bytes = out_slice;
    }
    const DkImageView v = img.view(level, layer, false);
    const DkImageRect rect{uint32_t(r.left), uint32_t(r.top), z, uint32_t(r.right - r.left), uint32_t(r.bottom - r.top),
                           depth};
    const DkCopyBuf src{pixels.gpu(), pitch_bytes, slice_bytes};
    dkCmdBufBarrier(gpu().cmd, DkBarrier_Full, 0); // prior sampling/rendering must finish before overwrite
    dkCmdBufCopyBufferToImage(gpu().cmd, &src, &v, &rect, 0);
    copy_barrier_ = true;
    sync("upload (image format %d %ux%ux%u, %u levels, level %u layer %u, rect %ld,%ld %ldx%ld depth %u, pitch %u bytes)",
         int(img.format), img.width, img.height, img.depth, img.levels, level, layer, long(r.left), long(r.top),
         long(r.right - r.left), long(r.bottom - r.top), depth, pitch_bytes);
}

void Device::blit(Surface* src, const RECT* sr, Surface* dst, const RECT* dr, bool linear) {
    gpu().state_owner=nullptr;
    ApiLock api_lock(__func__);
    GpuImage* si = src->image();
    GpuImage* di = dst->image();
    if (!si || !di) {
        log_unimplemented("StretchRect with a CPU surface");
        return;
    }
    if(si->ms_mode!=DkMsMode_1x && src->fmt->depth){resolve_depth(src,dst,sr,dr);return;}
    if(si->ms_mode!=DkMsMode_1x && di->ms_mode==DkMsMode_1x) {
        GpuImage resolved;
        resolved.create(DkImageType_2D,si->format,src->width,src->height,1,1,1,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
        const auto multisampled=src->view(),single=resolved.view();
        dkCmdBufBarrier(gpu().cmd,DkBarrier_Full,0);dkCmdBufResolveImage(gpu().cmd,&multisampled,&single);
        const auto destination=dst->view();const RECT s=sr?*sr:RECT{0,0,LONG(src->width),LONG(src->height)},d=dr?*dr:RECT{0,0,LONG(dst->width),LONG(dst->height)};
        const DkImageRect srect{uint32_t(s.left),uint32_t(s.top),0,uint32_t(s.right-s.left),uint32_t(s.bottom-s.top),1},drect{uint32_t(d.left),uint32_t(d.top),0,uint32_t(d.right-d.left),uint32_t(d.bottom-d.top),1};
        dkCmdBufBlitImage(gpu().cmd,&single,&srect,&destination,&drect,linear?DkBlitFlag_FilterLinear:0,0);
        resolved.destroy();copy_barrier_=true;return;
    }
    const DkImageView sv = src->view(), dv = dst->view();
    const RECT s = sr ? *sr : RECT{0, 0, LONG(src->width), LONG(src->height)};
    const RECT d = dr ? *dr : RECT{0, 0, LONG(dst->width), LONG(dst->height)};
    const DkImageRect srect{uint32_t(s.left), uint32_t(s.top), 0, uint32_t(s.right - s.left), uint32_t(s.bottom - s.top), 1};
    const DkImageRect drect{uint32_t(d.left), uint32_t(d.top), 0, uint32_t(d.right - d.left), uint32_t(d.bottom - d.top), 1};
    dkCmdBufBarrier(gpu().cmd, DkBarrier_Full, 0);  // the source's rendering done before the copy reads it
    if (srect.width == drect.width && srect.height == drect.height && si->format == di->format && si->ms_mode==di->ms_mode)
        dkCmdBufCopyImage(gpu().cmd, &sv, &srect, &dv, &drect, 0);
    else
        dkCmdBufBlitImage(gpu().cmd, &sv, &srect, &dv, &drect, linear ? DkBlitFlag_FilterLinear : 0, 0);
    if (src->fmt->depth && si != di) {
        // DMA changes depth pixels without updating the 3D engine's cull
        // summary. Rebind through the source so deko's ConditionalZcullInvalidate
        // executes its full target-change reset when binding the destination.
        // The ordinary Zcull barrier alone does not perform that target reset.
        dkCmdBufBindRenderTargets(gpu().cmd, nullptr, 0, &sv);
        dkCmdBufBindRenderTargets(gpu().cmd, nullptr, 0, &dv);
    }
    probe_surface_pixels(src, "blit-source");
    probe_surface_pixels(dst, "blit-destination");
    copy_barrier_ = true;
    sync("blit (format %d %ux%u -> format %d %ux%u)", int(si->format), srect.width, srect.height, int(di->format),
         drect.width, drect.height);
}

HRESULT Device::StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                            D3DTEXTUREFILTERTYPE filter) {
    ApiLock api_lock(__func__);
    if (!src || !dst) return D3DERR_INVALIDCALL;
    auto* s=static_cast<Surface*>(src);auto* d=static_cast<Surface*>(dst);
    if (s->owner()!=this || d->owner()!=this || s->pool!=D3DPOOL_DEFAULT || d->pool!=D3DPOOL_DEFAULT ||
        !(filter==D3DTEXF_NONE || filter==D3DTEXF_POINT || filter==D3DTEXF_LINEAR)) return D3DERR_INVALIDCALL;
    if (d->tex && !(d->usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))) return D3DERR_INVALIDCALL;
    if (!(d->usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)) && (s->tex || (s->usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))) return D3DERR_INVALIDCALL;
    if (s->fmt->depth || d->fmt->depth) {
        const auto full_rect = [](const RECT* r, const Surface* surface) {
            return !r || (r->left == 0 && r->top == 0 && r->right == LONG(surface->width) && r->bottom == LONG(surface->height));
        };
        if (in_scene_ || s->d3dfmt != d->d3dfmt || s->width != d->width || s->height != d->height ||
            !full_rect(sr, s) || !full_rect(dr, d)) return D3DERR_INVALIDCALL;
    }
    blit(s, sr, d, dr, filter == D3DTEXF_LINEAR);
    return D3D_OK;
}

HRESULT Device::UpdateSurface(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const POINT* dp) {
    ApiLock api_lock(__func__);
    auto* s = static_cast<Surface*>(src);
    auto* d = static_cast<Surface*>(dst);
    if (!s || !d || s->multisample!=D3DMULTISAMPLE_NONE || d->multisample!=D3DMULTISAMPLE_NONE) return D3DERR_INVALIDCALL;
    const RECT r = sr ? *sr : RECT{0, 0, LONG(s->width), LONG(s->height)};
    const LONG x = dp ? dp->x : 0, y = dp ? dp->y : 0;
    const RECT dr{x, y, x + (r.right - r.left), y + (r.bottom - r.top)};
    if(d->tex)d->tex->autogen_dirty=true;
    if (s->cpu && d->image()) {
        // CPU surface -> GPU: through staging (the source may be rewritten before the GPU copies it).
        const uint32_t pitch = row_pitch(*s->fmt, uint32_t(r.right - r.left));
        const uint32_t rows = level_rows(*s->fmt, uint32_t(r.bottom - r.top));
        Alloc st = gpu().cur().transient.alloc(pitch * rows, 256);
        for (uint32_t row = 0; row < rows; ++row)
            std::memcpy(st.cpu() + row * pitch,
                        s->cpu.cpu() + (size_t(r.top) / s->fmt->block + row) * s->cpu_pitch +
                            size_t(r.left) / s->fmt->block * s->fmt->bytes,
                        pitch);
        upload(*d->image(), d->layer, d->level, st, pitch, dr);
        return D3D_OK;
    }
    blit(s, &r, d, &dr, false);
    return D3D_OK;
}

HRESULT Device::UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
    ApiLock api_lock(__func__);
    TextureCore* s = core_of(src);
    TextureCore* d = core_of(dst);
    if (!s || !d || s->dev!=this || d->dev!=this || s->pool!=D3DPOOL_SYSTEMMEM || d->pool!=D3DPOOL_DEFAULT || s->img.type!=d->img.type || s->d3dfmt!=d->d3dfmt) return D3DERR_INVALIDCALL;
    dkCmdBufBarrier(gpu().cmd, DkBarrier_Full, 0);
    const uint32_t layers = s->img.type == DkImageType_Cubemap ? 6 : 1;
    const uint32_t skip = s->img.levels > d->img.levels ? s->img.levels - d->img.levels : 0;
    for (uint32_t l = 0; l < layers; ++l)
        for (uint32_t m = 0; m < d->img.levels && m + skip < s->img.levels; ++m) {
            const DkImageView sv = s->img.view(m + skip, l, false), dv = d->img.view(m, l, false);
            const DkImageRect r{0, 0, 0, mip(d->img.width, m), mip(d->img.height, m), mip(d->img.depth, m)};
            dkCmdBufCopyImage(gpu().cmd, &sv, &r, &dv, &r, 0);
        }
    d->autogen_dirty=true;
    copy_barrier_ = true;
    return D3D_OK;
}

HRESULT Device::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) {
    ApiLock api_lock(__func__);
    auto* s = static_cast<Surface*>(rt);
    auto* d = static_cast<Surface*>(dst);
    if (!s || !d || !s->image() || s->multisample!=D3DMULTISAMPLE_NONE || !d->cpu) return D3DERR_INVALIDCALL;
    // This API is a same-format, same-extent readback, not a scaled/converting
    // blit. A mismatched destination pitch can otherwise overrun its storage.
    if (s->d3dfmt != d->d3dfmt || s->width != d->width || s->height != d->height)
        return D3DERR_INVALIDCALL;
    const DkImageView v = s->view();
    const DkImageRect r{0, 0, 0, s->width, s->height, 1};
    const DkCopyBuf out{d->cpu.gpu(), d->cpu_pitch, 0};  // byte pitch
    dkCmdBufBarrier(gpu().cmd, DkBarrier_Full, 0);  // rendering into rt done before the copy reads it
    dkCmdBufCopyImageToBuffer(gpu().cmd, &v, &r, &out, 0);
    // The game reads it right away: submit and wait (a stall; the luminance
    // read-back does this every frame, see docs/SWITCH.md).
    gpu().flush(true);
    return D3D_OK;
}

HRESULT Device::ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
    gpu().state_owner=nullptr;
    ApiLock api_lock(__func__);
    auto* s = static_cast<Surface*>(surface);
    if (!s || !s->image()) return D3DERR_INVALIDCALL;
    DkCmdBuf cmd = gpu().cmd;
    const DkImageView v = s->view();
    dkCmdBufBindRenderTarget(cmd, &v, nullptr);
    const RECT r = rect ? *rect : RECT{0, 0, LONG(s->width), LONG(s->height)};
    const DkScissor sc{uint32_t(r.left), uint32_t(r.top), uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    float c[4];
    argb_to_float(color, c);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
    dirty_ |= kTargets | kViewport;
    copy_barrier_ = true;
    sync("ColorFill (format %d %ux%u)", int(s->image()->format), s->width, s->height);
    return D3D_OK;
}

// ---- targets and viewport ----

HRESULT Device::SetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (index >= 4 || (index == 0 && !surface)) return D3DERR_INVALIDCALL;
    if(surface && (static_cast<Surface*>(surface)->owner()!=this || !(static_cast<Surface*>(surface)->usage&D3DUSAGE_RENDERTARGET)))return D3DERR_INVALIDCALL;
    if (mrt_probe_frame_ && present_count_ + 1 == mrt_probe_frame_ && targets_[1] &&
        targets_[index] != surface && (index == 0 || index == 1)) probe_mrt("unbind", true);
    if (targets_[index] != surface) copy_barrier_ = true;
    set_ref(targets_[index], static_cast<Surface*>(surface));
    if (index == 0 && surface) {  // D3D9 resets the viewport to the new target
        auto* s = static_cast<Surface*>(surface);
        viewport_ = D3DVIEWPORT9{0, 0, s->width, s->height, 0.0f, 1.0f};
        scissor_ = RECT{0, 0, LONG(s->width), LONG(s->height)};
        dirty_ |= kViewport;
    }
    // RT0's sample count determines whether alpha testing is replaced by
    // alpha-to-coverage. Re-emit the hardware alpha test with the new target.
    dirty_ |= kTargets | kBlend;
    return D3D_OK;
}
HRESULT Device::GetRenderTarget(DWORD index, IDirect3DSurface9** out) {
    ApiLock api_lock(__func__);
    if (index >= 4 || !out) return D3DERR_INVALIDCALL;
    *out = targets_[index];
    if (!*out) return D3DERR_NOTFOUND;
    (*out)->AddRef();
    return D3D_OK;
}
HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9* surface) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if(surface && (static_cast<Surface*>(surface)->owner()!=this || !(static_cast<Surface*>(surface)->usage&D3DUSAGE_DEPTHSTENCIL)))return D3DERR_INVALIDCALL;
    if (depth_ != surface) copy_barrier_ = true;
    set_ref(depth_, static_cast<Surface*>(surface));
    dirty_ |= kTargets | kDepthStencil | kRaster;
    return D3D_OK;
}
HRESULT Device::GetDepthStencilSurface(IDirect3DSurface9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out = depth_;
    if (!depth_) return D3DERR_NOTFOUND;
    depth_->AddRef();
    return D3D_OK;
}

HRESULT Device::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    ApiLock api_lock(__func__);
    Gpu& g = gpu();
    if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !depth_) return D3DERR_INVALIDCALL;
    if (list_seen_ != g.list_id || g.state_owner != this) {
        g.state_owner = this;
        list_seen_ = g.list_id;
        dirty_ = kAll;
    }
    if (dirty_ & kTargets) flush_targets();
    DkCmdBuf cmd = g.cmd;
    // D3D9 clear rectangles are clipped by both viewport and enabled scissor.
    RECT vp{LONG(viewport_.X), LONG(viewport_.Y), LONG(viewport_.X + viewport_.Width), LONG(viewport_.Y + viewport_.Height)};
    if(rs_[D3DRS_SCISSORTESTENABLE]) {
        vp.left=std::max(vp.left,scissor_.left);
        vp.top=std::max(vp.top,scissor_.top);
        vp.right=std::min(vp.right,scissor_.right);
        vp.bottom=std::min(vp.bottom,scissor_.bottom);
    }
    std::vector<RECT> areas;
    if (rects)
        for (DWORD i = 0; i < count; ++i)
            areas.push_back(RECT{std::max(vp.left, rects[i].x1), std::max(vp.top, rects[i].y1),
                                 std::min(vp.right, rects[i].x2), std::min(vp.bottom, rects[i].y2)});
    else
        areas.push_back(vp);
    float c[4];
    argb_to_float(color, c);
    for (const RECT& r : areas) {
        if (r.right <= r.left || r.bottom <= r.top) continue;
        // deko clips an MRT binding to the smallest attachment. D3D9 clears
        // each surface independently, including a larger depth surface.
        auto scissor_for=[&](Surface* target){
            const LONG left=std::max<LONG>(r.left,0),top=std::max<LONG>(r.top,0);
            const LONG right=std::min<LONG>(r.right,target->width),bottom=std::min<LONG>(r.bottom,target->height);
            if(right<=left || bottom<=top)return false;
            const DkScissor sc{uint32_t(left),uint32_t(top),uint32_t(right-left),uint32_t(bottom-top)};
            dkCmdBufSetScissors(cmd,0,&sc,1);return true;
        };
        if (flags & D3DCLEAR_TARGET)
            for (uint32_t i = 0; i < 4; ++i)
                if (targets_[i] && targets_[i]->image()) {
                    auto view=targets_[i]->view();
                    if(rs_[D3DRS_SRGBWRITEENABLE])view.format=srgb_format(targets_[i]->image()->format);
                    dkCmdBufBindRenderTarget(cmd,&view,nullptr);
                    if(scissor_for(targets_[i]))dkCmdBufClearColorFloat(cmd,0,DkColorMask_RGBA,c[0],c[1],c[2],c[3]);
                    if(targets_[i]->tex)targets_[i]->tex->autogen_dirty=true;
                }
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depth_ && depth_->image()) {
            const auto view=depth_->view();dkCmdBufBindRenderTargets(cmd,nullptr,0,&view);
            if(scissor_for(depth_))dkCmdBufClearDepthStencil(cmd,(flags&D3DCLEAR_ZBUFFER)!=0,z,(flags&D3DCLEAR_STENCIL)?0xFF:0,uint8_t(stencil));
        }
    }
    probe_surface_pixels(depth_, "clear-depth");
    probe_surface_pixels(targets_[0], "clear-color");
    dirty_ |= kTargets|kViewport;
    if (mrt_probe_frame_ && present_count_ + 1 == mrt_probe_frame_ && targets_[1]) {
        rt::log("deko3d: MRT probe clear flags %lX color %08X z %.9g stencil %lu viewport %u,%u %ux%u",
                (unsigned long)flags, unsigned(color), z, (unsigned long)stencil,
                viewport_.X, viewport_.Y, viewport_.Width, viewport_.Height);
        probe_mrt("clear", true);
    }
    sync("Clear (flags %lX, %lu rects, target format %d)", (unsigned long)flags, (unsigned long)count,
         targets_[0] && targets_[0]->image() ? int(targets_[0]->image()->format) : -1);
    return D3D_OK;
}

HRESULT Device::SetViewport(const D3DVIEWPORT9* vp) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (!vp) return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::Viewport, 0, *vp,
        [this] { return viewport_; }, [this](const auto& value) { SetViewport(&value); });
    viewport_ = *vp;
    dirty_ |= kViewport;
    return D3D_OK;
}
HRESULT Device::GetViewport(D3DVIEWPORT9* vp) {
    ApiLock api_lock(__func__);
    if (!vp) return D3DERR_INVALIDCALL;
    *vp = viewport_;
    return D3D_OK;
}
HRESULT Device::SetScissorRect(const RECT* r) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (!r) return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::Scissor, 0, *r,
        [this] { return scissor_; }, [this](const auto& value) { SetScissorRect(&value); });
    scissor_ = *r;
    dirty_ |= kViewport;
    return D3D_OK;
}
HRESULT Device::GetScissorRect(RECT* r) {
    ApiLock api_lock(__func__);
    if (!r) return D3DERR_INVALIDCALL;
    *r = scissor_;
    return D3D_OK;
}

// ---- state ----

HRESULT Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (uint32_t(state) >= 256) return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::Render, state, value,
        [this,state] { return rs_[state]; }, [this,state](const auto& value) { SetRenderState(state,value); });
    if(state==D3DRS_POINTSIZE && (value==0x5a534552u || value==0x7fa05000u)){ // RESZ fourcc / ATI magic float
        resolve_depth_command();
        if (value==0x7fa05000u) rs_[state]=value;
        return D3D_OK;
    }
    if (rs_[state] == value) return D3D_OK;
    rs_[state] = value;
    switch (state) {
        case D3DRS_ADAPTIVETESS_Y:
            dirty_ |= kRaster | kBlend;
            break;
        case D3DRS_MULTISAMPLEANTIALIAS: case D3DRS_MULTISAMPLEMASK:
        case D3DRS_FILLMODE: case D3DRS_CULLMODE: case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
            dirty_ |= kRaster;
            break;
        case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP:
        case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA:
        case D3DRS_BLENDOPALPHA: case D3DRS_COLORWRITEENABLE: case D3DRS_COLORWRITEENABLE1:
        case D3DRS_COLORWRITEENABLE2: case D3DRS_COLORWRITEENABLE3: case D3DRS_ALPHATESTENABLE:
        case D3DRS_ALPHAFUNC: case D3DRS_ALPHAREF: case D3DRS_BLENDFACTOR:
            dirty_ |= kBlend|kRaster;
            break;
        case D3DRS_ZENABLE: case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC: case D3DRS_STENCILENABLE:
        case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC:
        case D3DRS_STENCILREF: case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_TWOSIDEDSTENCILMODE:
        case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC:
            dirty_ |= kDepthStencil;
            break;
        case D3DRS_SCISSORTESTENABLE:
            dirty_ |= kViewport;
            break;
        case D3DRS_SRGBWRITEENABLE:
            dirty_ |= kTargets;
            break;
        default:
            break;
    }
    return D3D_OK;
}
HRESULT Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    ApiLock api_lock(__func__);
    if (!value || uint32_t(state) >= 256) return D3DERR_INVALIDCALL;
    *value = rs_[state];
    return D3D_OK;
}
HRESULT Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    const int s = sampler_index(sampler);
    if (s < 0 || s >= kSamplers || uint32_t(type) >= 14) return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::Sampler, s*14+type, value,
        [this,s,sampler,type] { return ss_[s][type]; }, [this,s,sampler,type](const auto& value) { SetSamplerState(sampler,type,value); });
    if (ss_[s][type] != value) {
        ss_[s][type] = value;
        dirty_ |= kTextures;

    }
    return D3D_OK;
}
HRESULT Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) {
    ApiLock api_lock(__func__);
    const int s = sampler_index(sampler);
    if (!value || s < 0 || s >= kSamplers || uint32_t(type) >= 14) return D3DERR_INVALIDCALL;
    *value = ss_[s][type];
    return D3D_OK;
}
HRESULT Device::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    const int s = sampler_index(stage);
    if (s < 0 || s >= kSamplers) return D3DERR_INVALIDCALL;
    if (texture && (!texture_core(texture) || texture_core(texture)->dev != this))
        return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::TextureBinding, s, state_resource(texture),
        [this,s,stage] { return state_resource(textures_[s]); }, [this,s,stage](const auto& value) { SetTexture(stage,value.get()); });
    if (textures_[s] != texture) {
        set_ref(textures_[s], texture);
        dirty_ |= kTextures;
    }
    return D3D_OK;
}
void Device::texture_lod_changed(const TextureCore* texture) {
    ApiLock api_lock(__func__);
    for (auto* bound : textures_) {
        if (core_of(bound) == texture) {
            dirty_ |= kTextures;
            return;
        }
    }
}
HRESULT Device::GetTexture(DWORD stage, IDirect3DBaseTexture9** out) {
    ApiLock api_lock(__func__);
    const int s = sampler_index(stage);
    if (!out || s < 0 || s >= kSamplers) return D3DERR_INVALIDCALL;
    *out = textures_[s];
    if (*out) texture_add_ref(*out);
    return D3D_OK;
}
UINT Device::GetAvailableTextureMem() {
    ApiLock api_lock(__func__);
    const uint64_t budget=1024ull<<20;
    const uint64_t used=gpu().images.allocated_bytes()+gpu().buffers.allocated_bytes();
    return UINT(budget-std::min(budget,used));
}
HRESULT Device::BeginScene() {
    ApiLock api_lock(__func__);if(in_scene_)return D3DERR_INVALIDCALL;
    in_scene_=true;return D3D_OK;
}
HRESULT Device::EndScene() {
    ApiLock api_lock(__func__);if(!in_scene_)return D3DERR_INVALIDCALL;
    in_scene_=false;return D3D_OK;
}
HRESULT Device::ValidateDevice(DWORD* passes) {
    ApiLock api_lock(__func__);if(passes)*passes=1;
    return D3D_OK;
}
HRESULT Device::SetPaletteEntries(UINT index,const PALETTEENTRY* entries) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;if(!entries)return D3DERR_INVALIDCALL;
    for(unsigned i=0;i<256;++i)if(entries[i].peFlags!=0xff)return D3DERR_INVALIDCALL;
    std::memcpy(palettes_[index].data(),entries,256*sizeof(PALETTEENTRY));return D3D_OK;
}
HRESULT Device::GetPaletteEntries(UINT index,PALETTEENTRY* entries) {
    ApiLock api_lock(__func__);auto it=palettes_.find(index);
    if(!entries || it==palettes_.end())return D3DERR_INVALIDCALL;
    std::memcpy(entries,it->second.data(),256*sizeof(PALETTEENTRY));return D3D_OK;
}
HRESULT Device::SetCurrentTexturePalette(UINT index) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;current_palette_=index;return D3D_OK;
}
HRESULT Device::GetCurrentTexturePalette(UINT* index) {
    ApiLock api_lock(__func__);if(!index)return D3DERR_INVALIDCALL;
    *index=current_palette_;return D3D_OK;
}
HRESULT Device::GetFVF(DWORD* value) {
    ApiLock api_lock(__func__);
    if(!value)return D3DERR_INVALIDCALL;
    *value=fvf_;return D3D_OK;
}
HRESULT Device::SetTextureStageState(DWORD stage,D3DTEXTURESTAGESTATETYPE type,DWORD value) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if(stage>=8 || UINT(type)>=33)return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::TextureStage, stage*33+type, value,
        [this,stage,type] { return tss_[stage][type]; }, [this,stage,type](const auto& value) { SetTextureStageState(stage,type,value); });
    tss_[stage][type]=value;dirty_|=kShaders|kTextures;return D3D_OK;
}
HRESULT Device::GetTextureStageState(DWORD stage,D3DTEXTURESTAGESTATETYPE type,DWORD* value) {
    ApiLock api_lock(__func__);
    if(!value || stage>=8 || UINT(type)>=33)return D3DERR_INVALIDCALL;
    *value=tss_[stage][type];return D3D_OK;
}
namespace {
bool valid_transform(D3DTRANSFORMSTATETYPE state) {
    return state==D3DTS_VIEW || state==D3DTS_PROJECTION || (state>=D3DTS_TEXTURE0 && state<=D3DTS_TEXTURE7) || (UINT(state)>=256 && UINT(state)<512);
}
}
HRESULT Device::SetTransform(D3DTRANSFORMSTATETYPE state,const D3DMATRIX* matrix) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if(!matrix || !valid_transform(state))return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::Transform, state, *matrix,
        [this,state] { return transforms_[state]; }, [this,state](const auto& value) { SetTransform(state,&value); });
    transforms_[state]=*matrix;dirty_|=kVsConst;return D3D_OK;
}
HRESULT Device::GetTransform(D3DTRANSFORMSTATETYPE state,D3DMATRIX* matrix) {
    ApiLock api_lock(__func__);
    if(!matrix || !valid_transform(state))return D3DERR_INVALIDCALL;
    *matrix=transforms_[state];return D3D_OK;
}
HRESULT Device::MultiplyTransform(D3DTRANSFORMSTATETYPE state,const D3DMATRIX* matrix) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if(!matrix || !valid_transform(state))return D3DERR_INVALIDCALL;
    D3DMATRIX result{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
        result.m[i][j]+=matrix->m[i][k]*transforms_[state].m[k][j];
    transforms_[state]=result;dirty_|=kVsConst;return D3D_OK;
}
HRESULT Device::SetMaterial(const D3DMATERIAL9* material) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;if(!material)return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::Material, 0, *material,
        [this] { return material_; }, [this](const auto& value) { SetMaterial(&value); });
    material_=*material;dirty_|=kVsConst;return D3D_OK;
}
HRESULT Device::GetMaterial(D3DMATERIAL9* material) {
    ApiLock api_lock(__func__);if(!material)return D3DERR_INVALIDCALL;
    *material=material_;return D3D_OK;
}
HRESULT Device::SetLight(DWORD index,const D3DLIGHT9* light) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if(!light || light->Type<D3DLIGHT_POINT || light->Type>D3DLIGHT_DIRECTIONAL)return D3DERR_INVALIDCALL;
    if(light->Type!=D3DLIGHT_DIRECTIONAL && (light->Attenuation0<0 || light->Attenuation1<0 || light->Attenuation2<0))return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::LightValue, index, *light,
        [this,index] { return lights_.count(index)?lights_.at(index).value:D3DLIGHT9{}; }, [this,index](const auto& value) { SetLight(index,&value); });
    lights_[index].value=*light;dirty_|=kVsConst;return D3D_OK;
}
HRESULT Device::GetLight(DWORD index,D3DLIGHT9* light) {
    ApiLock api_lock(__func__);auto it=lights_.find(index);
    if(!light || it==lights_.end())return D3DERR_INVALIDCALL;
    *light=it->second.value;return D3D_OK;
}
HRESULT Device::LightEnable(DWORD index,BOOL enabled) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (recording_) return recording_->record(StateBlock::LightEnabled, index, BOOL(enabled?128:0),
        [this,index] { return lights_.count(index)?lights_.at(index).enabled:FALSE; }, [this,index](const auto& value) { LightEnable(index,value); });
    if(!lights_.count(index)) {
        D3DLIGHT9 light{};light.Type=D3DLIGHT_DIRECTIONAL;
        light.Diffuse={1,1,1,0};light.Direction={0,0,1};
        lights_[index].value=light;
    }
    lights_[index].enabled=enabled?128:0;dirty_|=kVsConst;return D3D_OK;
}
HRESULT Device::GetLightEnable(DWORD index,BOOL* enabled) {
    ApiLock api_lock(__func__);auto it=lights_.find(index);
    if(!enabled || it==lights_.end())return D3DERR_INVALIDCALL;
    *enabled=it->second.enabled;return D3D_OK;
}
HRESULT Device::SetClipPlane(DWORD index,const float* plane) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;if(!plane)return D3DERR_INVALIDCALL;
    index=std::min<DWORD>(index,5);
    if (recording_) return recording_->record(StateBlock::ClipPlane, index, state_array<float,4>(plane),
        [this,index] { return state_array<float,4>(clip_planes_[index]); }, [this,index](const auto& value) { SetClipPlane(index,value.data()); });
    std::memcpy(clip_planes_[index],plane,16);dirty_|=kVsConst;return D3D_OK;
}
HRESULT Device::GetClipPlane(DWORD index,float* plane) {
    ApiLock api_lock(__func__);if(!plane)return D3DERR_INVALIDCALL;
    index=std::min<DWORD>(index,5);
    std::memcpy(plane,clip_planes_[index],16);return D3D_OK;
}
HRESULT Device::GetStreamSourceFreq(UINT stream,UINT* divider) {
    ApiLock api_lock(__func__);if(!divider || stream>=kStreams)return D3DERR_INVALIDCALL;
    *divider=streams_[stream].freq;return D3D_OK;
}
HRESULT Device::SetFVF(DWORD fvf) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (!fvf) return D3D_OK;
    VertexDeclaration*& d = fvf_decls_[fvf];
    if (!d) {
        d = new VertexDeclaration(this, fvf_elements(fvf).data());
        d->fvf=fvf;
        d->AddRefPrivate();
        d->Release();
    }
    return SetVertexDeclaration(d);
}
HRESULT Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) {
    ApiLock api_lock(__func__);
    if (!elements || !out) return D3DERR_INVALIDCALL;
    *out = new VertexDeclaration(this, elements);
    return D3D_OK;
}
HRESULT Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (recording_) return recording_->record(StateBlock::Declaration, 0, state_resource(static_cast<VertexDeclaration*>(decl)),
        [this] { return state_resource(decl_); }, [this](const auto& value) { if(value) SetVertexDeclaration(value.get()); });
    fvf_=decl?static_cast<VertexDeclaration*>(decl)->fvf:0;
    if (decl_ != decl) {
        set_ref(decl_, static_cast<VertexDeclaration*>(decl));
        dirty_ |= kVertexInput;
    }
    return D3D_OK;
}
HRESULT Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out = decl_;
    if (decl_) decl_->AddRef();
    return D3D_OK;
}
HRESULT Device::CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** out) {
    ApiLock api_lock(__func__);
    if (!code || !out) return D3DERR_INVALIDCALL;
    uint32_t bytes;
    const PackMeta* m;
    const DkShader* sh;
    find_shader(code, &bytes, &m, &sh);
    *out = new VertexShader(this, code, bytes, m, sh);
    return D3D_OK;
}
HRESULT Device::SetVertexShader(IDirect3DVertexShader9* shader) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (recording_) return recording_->record(StateBlock::VertexProgram, 0, state_resource(static_cast<VertexShader*>(shader)),
        [this] { return state_resource(vs_); }, [this](const auto& value) { SetVertexShader(value.get()); });
    if (vs_ != shader) {
        set_ref(vs_, static_cast<VertexShader*>(shader));
        dirty_ |= kShaders | kVertexInput | kTextures | kVsConst;
    }
    return D3D_OK;
}
HRESULT Device::GetVertexShader(IDirect3DVertexShader9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out = vs_;
    if (vs_) vs_->AddRef();
    return D3D_OK;
}
HRESULT Device::CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** out) {
    ApiLock api_lock(__func__);
    if (!code || !out) return D3DERR_INVALIDCALL;
    uint32_t bytes;
    const PackMeta* m;
    const DkShader* sh;
    find_shader(code, &bytes, &m, &sh);
    *out = new PixelShader(this, code, bytes, m, sh);
    return D3D_OK;
}
HRESULT Device::SetPixelShader(IDirect3DPixelShader9* shader) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (recording_) return recording_->record(StateBlock::PixelProgram, 0, state_resource(static_cast<PixelShader*>(shader)),
        [this] { return state_resource(ps_); }, [this](const auto& value) { SetPixelShader(value.get()); });
    if (ps_ != shader) {
        set_ref(ps_, static_cast<PixelShader*>(shader));
        dirty_ |= kShaders | kTextures | kPsConst | kBlend;  // kBlend: the colour write masks follow its outputs
    }
    return D3D_OK;
}
HRESULT Device::GetPixelShader(IDirect3DPixelShader9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out = ps_;
    if (ps_) ps_->AddRef();
    return D3D_OK;
}

#define CONSTANT_SETTER(Name, Array, Limit, Type, Width, Flag, Kind)                       \
    HRESULT Device::Name(UINT reg, const Type* data, UINT count) {                  \
        ApiLock api_lock(__func__);                                                 \
        if (!data || reg > Limit || count > Limit - reg) return D3DERR_INVALIDCALL;                \
        if (recording_) {                                                        \
            for (UINT i=0;i<count;++i) { const UINT r=reg+i;                       \
                recording_->record(StateBlock::Kind,r,state_array<Type,Width>(data+i*Width), \
                    [this,r] { return state_array<Type,Width>(reinterpret_cast<const Type*>(&Array[r])); }, \
                    [this,r](const auto& value) { Name(r,value.data(),1); });      \
            } return D3D_OK;                                                     \
        }                                                                        \
        const size_t bytes = sizeof(Type) * Width * count;                        \
        if (!bytes || std::memcmp(&Array[reg], data, bytes) == 0) return D3D_OK;    \
        std::memcpy(&Array[reg], data, bytes);                                     \
        dirty_ |= Flag;                                                             \
        return D3D_OK;                                                              \
    }
CONSTANT_SETTER(SetVertexShaderConstantF, vsf_, 256, float, 4, kVsConst, VertexFloat)
CONSTANT_SETTER(SetVertexShaderConstantI, vsi_, 16, int, 4, kVsConst, VertexInt)
CONSTANT_SETTER(SetVertexShaderConstantB, vsb_, 16, BOOL, 1, kVsConst, VertexBool)
CONSTANT_SETTER(SetPixelShaderConstantF, psf_, 224, float, 4, kPsConst, PixelFloat)
CONSTANT_SETTER(SetPixelShaderConstantI, psi_, 16, int, 4, kPsConst, PixelInt)
CONSTANT_SETTER(SetPixelShaderConstantB, psb_, 16, BOOL, 1, kPsConst, PixelBool)
#undef CONSTANT_SETTER

HRESULT Device::GetVertexShaderConstantF(UINT reg, float* data, UINT count) {
    ApiLock api_lock(__func__);
    if (!data || reg > 256 || count > 256 - reg) return D3DERR_INVALIDCALL;
    std::memcpy(data, &vsf_[reg], 16 * count);
    return D3D_OK;
}
HRESULT Device::GetPixelShaderConstantF(UINT reg, float* data, UINT count) {
    ApiLock api_lock(__func__);
    if (!data || reg > 224 || count > 224 - reg) return D3DERR_INVALIDCALL;
    std::memcpy(data, &psf_[reg], 16 * count);
    return D3D_OK;
}

HRESULT Device::GetVertexShaderConstantI(UINT reg,int* data,UINT count) {
    ApiLock api_lock(__func__);
    if(!data || reg>16 || count>16-reg)return D3DERR_INVALIDCALL;
    std::memcpy(data,&vsi_[reg],sizeof(int)*4*count);return D3D_OK;
}

HRESULT Device::GetVertexShaderConstantB(UINT reg,BOOL* data,UINT count) {
    ApiLock api_lock(__func__);
    if(!data || reg>16 || count>16-reg)return D3DERR_INVALIDCALL;
    std::memcpy(data,&vsb_[reg],sizeof(BOOL)*1*count);return D3D_OK;
}

HRESULT Device::GetPixelShaderConstantI(UINT reg,int* data,UINT count) {
    ApiLock api_lock(__func__);
    if(!data || reg>16 || count>16-reg)return D3DERR_INVALIDCALL;
    std::memcpy(data,&psi_[reg],sizeof(int)*4*count);return D3D_OK;
}

HRESULT Device::GetPixelShaderConstantB(UINT reg,BOOL* data,UINT count) {
    ApiLock api_lock(__func__);
    if(!data || reg>16 || count>16-reg)return D3DERR_INVALIDCALL;
    std::memcpy(data,&psb_[reg],sizeof(BOOL)*1*count);return D3D_OK;
}

HRESULT Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (stream >= kStreams) return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::StreamSource, stream,
        std::make_tuple(state_resource(static_cast<VertexBuffer*>(vb)),offset,stride),
        [this,stream,offset,capture_offset=recording_->capture_stream_offsets] {
            const auto& s=streams_[stream];
            return std::make_tuple(state_resource(s.vb),capture_offset?s.offset:offset,s.stride);
        },
        [this,stream](const auto& value) { SetStreamSource(stream,std::get<0>(value).get(),std::get<1>(value),std::get<2>(value)); });
    Stream& s = streams_[stream];
    set_ref(s.vb, static_cast<VertexBuffer*>(vb));
    if (s.stride != stride) dirty_ |= kVertexInput;
    if(vb) { s.offset = offset;s.stride = stride; }
    return D3D_OK;
}
HRESULT Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** vb, UINT* offset, UINT* stride) {
    ApiLock api_lock(__func__);
    if (stream >= kStreams || !vb || !offset || !stride) return D3DERR_INVALIDCALL;
    const Stream& s = streams_[stream];
    *vb = s.vb;
    if (s.vb) s.vb->AddRef();
    *offset = s.offset;
    *stride = s.stride;
    return D3D_OK;
}
HRESULT Device::SetStreamSourceFreq(UINT stream, UINT divider) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    const DWORD frequency_flags = divider & (D3DSTREAMSOURCE_INDEXEDDATA | D3DSTREAMSOURCE_INSTANCEDATA);
    if (stream >= kStreams || !divider ||
        frequency_flags == (D3DSTREAMSOURCE_INDEXEDDATA | D3DSTREAMSOURCE_INSTANCEDATA) ||
        (!stream && (frequency_flags & D3DSTREAMSOURCE_INSTANCEDATA))) return D3DERR_INVALIDCALL;
    if (recording_) return recording_->record(StateBlock::StreamFrequency, stream, divider,
        [this,stream] { return streams_[stream].freq; }, [this,stream](const auto& value) { SetStreamSourceFreq(stream,value); });
    if (streams_[stream].freq != divider) {
        streams_[stream].freq = divider;
        dirty_ |= kVertexInput;
    }
    return D3D_OK;
}
HRESULT Device::SetIndices(IDirect3DIndexBuffer9* ib) {
    ApiLock api_lock(__func__);
    ffp_dirty_=true;
    if (recording_) return recording_->record(StateBlock::IndexSource, 0, state_resource(static_cast<IndexBuffer*>(ib)),
        [this] { return state_resource(indices_); }, [this](const auto& value) { SetIndices(value.get()); });
    set_ref(indices_, static_cast<IndexBuffer*>(ib));
    return D3D_OK;
}
HRESULT Device::GetIndices(IDirect3DIndexBuffer9** out) {
    ApiLock api_lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out = indices_;
    if (indices_) indices_->AddRef();
    return D3D_OK;
}

// ---- state -> deko3d ----

void Device::flush_targets(bool publish) {
    ApiLock api_lock(__func__);
    // Render-to-texture: what was drawn into the previous targets must be done,
    // and the texture cache invalidated, before anything samples it. Without this
    // the game's blur passes read their inputs as freshly cleared (zeros).
    // A texture-cache invalidate does not publish dirty render data from L2.
    // Complete fragment writes before switching targets, then make their data
    // visible to texture fetches and DMA as well as subsequent rendering.
    if (publish)
        dkCmdBufBarrier(gpu().cmd, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    DkImageView views[4];
    DkImageView const* ptrs[4];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!targets_[i] || !targets_[i]->image()) break;  // targets are contiguous from 0
        views[i] = targets_[i]->view();
        if(rs_[D3DRS_SRGBWRITEENABLE])views[i].format=srgb_format(targets_[i]->image()->format);
        ptrs[i] = &views[i];
        n = i + 1;
    }
    DkImageView dv;
    const bool depth = depth_ && depth_->image();
    if (depth) dv = depth_->view();
    dkCmdBufBindRenderTargets(gpu().cmd, ptrs, n, depth ? &dv : nullptr);
    dirty_|=kRaster;
}

void Device::flush_viewport() {
    ApiLock api_lock(__func__);
    const D3DVIEWPORT9& vp = viewport_;
    const float zbias = 0.001f;  // as DXVK: no degenerate depth range
    // deko3d's upper-left origin is D3D's convention (it already maps NDC +Y to
    // the top); DXVK's Vulkan-style negative height would flip twice and wrap
    // the viewport rectangle. Only D3D9's half-pixel offset is added.
    const DkViewport v{float(vp.X) + 0.5f, float(vp.Y) + 0.5f, float(vp.Width), float(vp.Height),
                       std::clamp(vp.MinZ, 0.0f, 1.0f), std::clamp(std::max(vp.MaxZ, vp.MinZ + zbias), 0.0f, 1.0f)};
    dkCmdBufSetViewports(gpu().cmd, 0, &v, 1);
    RECT r{LONG(vp.X), LONG(vp.Y), LONG(vp.X + vp.Width), LONG(vp.Y + vp.Height)};
    if (rs_[D3DRS_SCISSORTESTENABLE]) {
        r.left = std::max(r.left, scissor_.left);
        r.top = std::max(r.top, scissor_.top);
        r.right = std::max(r.left, std::min(r.right, scissor_.right));
        r.bottom = std::max(r.top, std::min(r.bottom, scissor_.bottom));
    }
    const DkScissor s{uint32_t(std::max<LONG>(r.left, 0)), uint32_t(std::max<LONG>(r.top, 0)),
                      uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
    dkCmdBufSetScissors(gpu().cmd, 0, &s, 1);
}

void Device::flush_raster_state() {
    ApiLock api_lock(__func__);
    DkRasterizerState r;
    dkRasterizerStateDefaults(&r);
    const DWORD fill = rs_[D3DRS_FILLMODE];
    r.polygonModeFront = r.polygonModeBack =
        fill == D3DFILL_POINT ? DkPolygonMode_Point : fill == D3DFILL_WIREFRAME ? DkPolygonMode_Line : DkPolygonMode_Fill;
    r.frontFace = DkFrontFace_CW;
    r.provokingVertex = DkProvokingVertex_First;
    const DWORD cull = rs_[D3DRS_CULLMODE];
    r.cullMode = cull == D3DCULL_CW ? DkFace_Front : cull == D3DCULL_CCW ? DkFace_Back : DkFace_None;
    const float bias = as_float(rs_[D3DRS_DEPTHBIAS]), slope = as_float(rs_[D3DRS_SLOPESCALEDEPTHBIAS]);
    r.depthBiasEnableMask = (bias != 0.0f || slope != 0.0f) ? DkPolygonFlag_All : 0;
    dkCmdBufBindRasterizerState(gpu().cmd, &r);
    DkMultisampleState multisample;dkMultisampleStateDefaults(&multisample);
    multisample.mode=targets_[0]&&targets_[0]->image()?targets_[0]->image()->ms_mode:DkMsMode_1x;
    multisample.rasterizerMode=rs_[D3DRS_MULTISAMPLEANTIALIAS]?multisample.mode:DkMsMode_1x;
    multisample.alphaToCoverageEnable=alpha_to_coverage();
    dkMultisampleStateSetLocations(&multisample,nullptr,0);
    dkCmdBufBindMultisampleState(gpu().cmd,&multisample);
    // Maxwell's four sample-mask registers are 16-bit. D3D9 exposes a DWORD
    // and defaults to all 32 bits set; reserved upper bits cause a GR method
    // error even for a single-sample target (method 0x3ef).
    dkCmdBufSetSampleMask(gpu().cmd,rs_[D3DRS_MULTISAMPLEMASK]&0xffffu);
    if (r.depthBiasEnableMask) {
        // D3D9's bias is in depth units; deko3d's in the format's smallest step.
        const float scale = depth_ && depth_->fmt && depth_->fmt->dk == DkImageFormat_Z16 ? float(1 << 16) : float(1 << 24);
        dkCmdBufSetDepthBias(gpu().cmd, bias * scale, 0.0f, slope);
    }
}

void Device::flush_blend_state() {
    ApiLock api_lock(__func__);
    DkCmdBuf cmd = gpu().cmd;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = rs_[D3DRS_ALPHABLENDENABLE] ? 0xFF : 0;
    const bool shader_alpha = draw_ps_.meta &&
        (draw_ps_.meta->fixed_function || draw_ps_.meta->has_draw_state);
    cs.alphaCompareOp = rs_[D3DRS_ALPHATESTENABLE] && !shader_alpha && !alpha_to_coverage()
        ? DkCompareOp(rs_[D3DRS_ALPHAFUNC]) : DkCompareOp_Always;
    dkCmdBufBindColorState(cmd, &cs);
    if (rs_[D3DRS_ALPHATESTENABLE]) dkCmdBufSetAlphaRef(cmd, float(rs_[D3DRS_ALPHAREF] & 0xFF) / 255.0f);

    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    const D3DRENDERSTATETYPE masks[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
                                         D3DRS_COLORWRITEENABLE3};
    // Targets the pixel shader does not write keep their contents, as on D3D9:
    // Maxwell would write the output registers' leftovers (a game pass bound a
    // depth target it did not write and filled it with garbage).
    const uint32_t outputs = draw_ps_.meta ? draw_ps_.meta->color_outputs : 1;
    for (uint32_t i = 0; i < 4; ++i) dkColorWriteStateSetMask(&cw, i, (outputs >> i & 1) ? rs_[masks[i]] & 0xF : 0);
    dkCmdBufBindColorWriteState(cmd, &cw);

    if (rs_[D3DRS_ALPHABLENDENABLE]) {
        DkBlendState b;
        dkBlendStateDefaults(&b);
        DWORD src = rs_[D3DRS_SRCBLEND], dst = rs_[D3DRS_DESTBLEND];
        if (src == D3DBLEND_BOTHSRCALPHA) dst = D3DBLEND_INVSRCALPHA;
        if (src == D3DBLEND_BOTHINVSRCALPHA) dst = D3DBLEND_SRCALPHA;
        const bool sep = rs_[D3DRS_SEPARATEALPHABLENDENABLE];
        const DWORD asrc = sep ? rs_[D3DRS_SRCBLENDALPHA] : src, adst = sep ? rs_[D3DRS_DESTBLENDALPHA] : dst;
        dkBlendStateSetOps(&b, DkBlendOp(rs_[D3DRS_BLENDOP]), DkBlendOp(sep ? rs_[D3DRS_BLENDOPALPHA] : rs_[D3DRS_BLENDOP]));
        dkBlendStateSetFactors(&b, blend_factor(src), blend_factor(dst), blend_factor(asrc), blend_factor(adst));
        const DkBlendState all[4] = {b, b, b, b};
        dkCmdBufBindBlendStates(cmd, 0, all, 4);
        float c[4];
        argb_to_float(rs_[D3DRS_BLENDFACTOR], c);
        dkCmdBufSetBlendConst(cmd, c[0], c[1], c[2], c[3]);
    }
}

void Device::flush_depth_stencil() {
    ApiLock api_lock(__func__);
    DkDepthStencilState d;
    dkDepthStencilStateDefaults(&d);
    const bool has_depth = depth_ && depth_->image();
    d.depthTestEnable = has_depth && rs_[D3DRS_ZENABLE] != D3DZB_FALSE;
    d.depthWriteEnable = has_depth && rs_[D3DRS_ZWRITEENABLE];
    d.depthCompareOp = DkCompareOp(rs_[D3DRS_ZFUNC]);
    d.stencilTestEnable = has_depth && rs_[D3DRS_STENCILENABLE];
    // D3D9's stencil ops and compare functions are deko3d's values.
    d.stencilFrontFailOp = DkStencilOp(rs_[D3DRS_STENCILFAIL]);
    d.stencilFrontDepthFailOp = DkStencilOp(rs_[D3DRS_STENCILZFAIL]);
    d.stencilFrontPassOp = DkStencilOp(rs_[D3DRS_STENCILPASS]);
    d.stencilFrontCompareOp = DkCompareOp(rs_[D3DRS_STENCILFUNC]);
    const bool two = rs_[D3DRS_TWOSIDEDSTENCILMODE];
    d.stencilBackFailOp = DkStencilOp(rs_[two ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL]);
    d.stencilBackDepthFailOp = DkStencilOp(rs_[two ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL]);
    d.stencilBackPassOp = DkStencilOp(rs_[two ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS]);
    d.stencilBackCompareOp = DkCompareOp(rs_[two ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC]);
    dkCmdBufBindDepthStencilState(gpu().cmd, &d);
    if (d.stencilTestEnable)
        dkCmdBufSetStencil(gpu().cmd, DkFace_FrontAndBack, uint8_t(rs_[D3DRS_STENCILWRITEMASK]), uint8_t(rs_[D3DRS_STENCILREF]),
                           uint8_t(rs_[D3DRS_STENCILMASK]));
}

bool Device::flush_vertex_input(uint32_t* instances) {
    ApiLock api_lock(__func__);
    DkCmdBuf cmd = gpu().cmd;
    // Instancing: stream 0 says how many instances, streams with INSTANCEDATA step per instance.
    *instances = 1;
    const bool instanced = (streams_[0].freq & D3DSTREAMSOURCE_INDEXEDDATA) != 0;
    if (instanced) *instances = std::max<UINT>(streams_[0].freq & 0x3FFFFFFF, 1);
    uint32_t nbuf = up_.active ? 1 : 0;
    if (dirty_ & (kVertexInput | kShaders)) {
        if (!decl_) {
            log_unimplemented("draw without a vertex declaration");
            return false;
        }
        DkVtxAttribState attrs[16] = {};
        uint32_t nattr = 0;
        for (const PackMeta::Input& in : draw_vs_.meta->inputs) {
            if (in.reg >= 16) continue;
            DkVtxAttribState& a = attrs[in.reg];
            const D3DVERTEXELEMENT9* e = nullptr;
            for (const D3DVERTEXELEMENT9& x : decl_->elements)
                if (x.Usage == in.usage && x.UsageIndex == in.index) {
                    e = &x;
                    break;
                }
            if (e && attrib_format(e->Type, a)) {
                a.bufferId = e->Stream;
                a.offset = e->Offset;
            } else {
                a.isFixed = 1;  // the shader reads (0, 0, 0, 1)
                a.size = DkVtxAttribSize_4x32;
                a.type = DkVtxAttribType_Float;
            }
            nattr = std::max<uint32_t>(nattr, in.reg + 1u);
        }
        for (uint32_t i = 0; i < nattr; ++i)
            if (!attrs[i].size) {
                attrs[i].isFixed = 1;
                attrs[i].size = DkVtxAttribSize_4x32;
                attrs[i].type = DkVtxAttribType_Float;
            }
        DkVtxBufferState bufs[kStreams] = {};
        for (const D3DVERTEXELEMENT9& x : decl_->elements) nbuf = std::max<uint32_t>(nbuf, x.Stream + 1u);
        for (uint32_t i = 0; i < nbuf && i < kStreams; ++i) {
            bufs[i].stride = (up_.active && i == 0) ? up_.stride : streams_[i].stride;
            bufs[i].divisor = instanced && (streams_[i].freq & D3DSTREAMSOURCE_INSTANCEDATA)
                                  ? std::max<UINT>(streams_[i].freq & 0x3FFFFFFF, 1)
                                  : 0;
        }
        dkCmdBufBindVtxAttribState(cmd, attrs, nattr);
        dkCmdBufBindVtxBufferState(cmd, bufs, nbuf);
    } else {
        for (const D3DVERTEXELEMENT9& x : decl_->elements) nbuf = std::max<uint32_t>(nbuf, x.Stream + 1u);
    }
    // Buffer addresses change when a lock renames them: bind on every draw.
    for (uint32_t i = 0; i < nbuf && i < kStreams; ++i) {
        if (up_.active && i == 0) {
            dkCmdBufBindVtxBuffer(cmd, 0, up_.addr, up_.size);
            continue;
        }
        Stream& s = streams_[i];
        if (!s.vb) continue;
        dkCmdBufBindVtxBuffer(cmd, i, s.vb->store.addr() + s.offset, s.vb->store.size - std::min(s.offset, s.vb->store.size));
        s.vb->store.used();
    }
    return true;
}

int Device::sampler_id(int s,bool compare) {
    ApiLock api_lock(__func__);
    const DWORD* st = ss_[s];
    const TextureCore* t = core_of(textures_[s]);
    const DWORD resident_lod = t ? t->lod : 0;
    const DWORD minimum_lod = st[D3DSAMP_MIPFILTER] == D3DTEXF_NONE ? 0 :
        std::max(st[D3DSAMP_MAXMIPLEVEL], resident_lod) - resident_lod;
    const std::array<DWORD, 11> key = {st[D3DSAMP_ADDRESSU], st[D3DSAMP_ADDRESSV], st[D3DSAMP_ADDRESSW],
                                       st[D3DSAMP_BORDERCOLOR], st[D3DSAMP_MAGFILTER], st[D3DSAMP_MINFILTER],
                                       st[D3DSAMP_MIPFILTER], st[D3DSAMP_MIPMAPLODBIAS],
                                       minimum_lod, st[D3DSAMP_MAXANISOTROPY],DWORD(compare)};
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    DkSampler smp;
    dkSamplerDefaults(&smp);
    smp.compareEnable=compare;
    smp.compareOp=DkCompareOp_Lequal;
    auto filter = [](DWORD f) { return f == D3DTEXF_POINT || f == D3DTEXF_NONE ? DkFilter_Nearest : DkFilter_Linear; };
    smp.magFilter = filter(key[4]);
    smp.minFilter = filter(key[5]);
    smp.mipFilter = key[6] == D3DTEXF_NONE ? DkMipFilter_None : key[6] == D3DTEXF_POINT ? DkMipFilter_Nearest : DkMipFilter_Linear;
    smp.wrapMode[0] = wrap_mode(key[0]);
    smp.wrapMode[1] = wrap_mode(key[1]);
    smp.wrapMode[2] = wrap_mode(key[2]);
    smp.lodBias = as_float(key[7]);
    smp.lodClampMin = float(key[8]);
    if (key[4] == D3DTEXF_ANISOTROPIC || key[5] == D3DTEXF_ANISOTROPIC)
        smp.maxAnisotropy = float(std::clamp<DWORD>(key[9], 1, 16));
    float border[4];
    argb_to_float(key[3], border);
    for (int i = 0; i < 4; ++i) smp.borderColor[i].value_f = border[i];
    Gpu& g = gpu();
    const int id = g.sampler_ids.take();
    dkSamplerDescriptorInitialize(&g.sampler_desc()[id], &smp);
    g.descriptors_dirty = true;
    samplers_[key] = id;
    return id;
}

void Device::flush_textures() {
    ApiLock api_lock(__func__);
    DkCmdBuf cmd = gpu().cmd;
    auto bind_stage = [&](const PackMeta* m, DkStage stage, int base) {
        for (const PackMeta::Sampler& s : m->samplers) {
            const int idx = base + s.reg;
            if (idx >= kSamplers) continue;
            TextureCore* t = core_of(textures_[idx]);
            if (s.type == 3 && stage == DkStage_Fragment && s.reg < 6) {
                GpuImage* source = feedback_sources_[idx] == t && t ? &feedback_images_[idx] : t ? &t->img : nullptr;
                auto bind = [&](GpuImage* fallback, DkImageType type, unsigned binding, bool compare = false) {
                    GpuImage* image = source && source->mem && source->type == type ? source : fallback;
                    const int descriptor = image->descriptor((ss_[idx][D3DSAMP_SRGBTEXTURE] & 1) != 0,
                        t && image == source ? t->lod : 0);
                    const auto handle = dkMakeTextureHandle(uint32_t(descriptor), uint32_t(sampler_id(idx, compare)));
                    dkCmdBufBindTextures(cmd, stage, binding, &handle, 1);
                };
                bind(&dummy_, DkImageType_2D, s.reg);
                bind(&dummy_volume_, DkImageType_3D, 8 + s.reg);
                bind(&dummy_cube_, DkImageType_Cubemap, 16 + s.reg);
                bind(&dummy_, DkImageType_2D, 24 + s.reg, true);
                continue;
            }
            GpuImage* fallback=s.type==1?&dummy_cube_:s.type==2?&dummy_volume_:&dummy_;
            const DkImageType expected=s.type==1?DkImageType_Cubemap:s.type==2?DkImageType_3D:DkImageType_2D;
            GpuImage* sampled=feedback_sources_[idx]==t && t?&feedback_images_[idx]:t?&t->img:fallback;
            if(!sampled->mem || sampled->type!=expected)sampled=fallback;
            const int image = sampled->descriptor((ss_[idx][D3DSAMP_SRGBTEXTURE]&1)!=0, t && sampled != fallback ? t->lod : 0);
            const DkResHandle h = dkMakeTextureHandle(uint32_t(image), uint32_t(sampler_id(idx,m->fixed_function && s.binding>=24)));
            dkCmdBufBindTextures(cmd, stage, s.binding==0xff?s.reg:s.binding, &h, 1);
            if (!m->fixed_function && stage == DkStage_Fragment && s.type == 0 && s.reg < 16) {
                const auto shadow = dkMakeTextureHandle(uint32_t(image), uint32_t(sampler_id(idx, true)));
                dkCmdBufBindTextures(cmd, stage, 16 + s.reg, &shadow, 1);
            }
        }
    };
    bind_stage(draw_ps_.meta, DkStage_Fragment, 0);
    bind_stage(draw_vs_.meta, DkStage_Vertex, 16);
}

void Device::flush_constants() {
    ApiLock api_lock(__func__);
    DkCmdBuf cmd = gpu().cmd;
    auto push = [&](const PackMeta* m, int stage, const float (*f)[4], const int (*iv)[4], const BOOL* b) {
        if (!m->block_bytes || m->block_bytes > kUboSize) return;
        uint8_t* out = staging_.data();
        uint32_t off = 0;
        for (const PackMeta::Uniform& u : m->uniforms) {
            const uint32_t n = std::max<uint32_t>(u.count, 1);
            if (u.type == 0) std::memcpy(out + off, f[u.reg], 16 * n);
            else if (u.type == 1) std::memcpy(out + off, iv[u.reg], 16 * n);
            else
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t v = b[u.reg + j] ? 1 : 0;
                    // The remaining lanes are padding, not the previous shader's
                    // constants. Stable padding also avoids spurious uploads.
                    std::memset(out + off + j * 16, 0, 16);
                    std::memcpy(out + off + j * 16, &v, 4);
                }
            off += 16 * n;
        }
        // UBO allocations survive command-list and shader changes. Compare the
        // packed bytes, so a shader with a different register layout is safe too.
        if (rebind_state_) constant_upload_[stage].reset();
        const auto range = constant_upload_[stage].update(out, off);
        constant_bytes_full_ += off;
        constant_bytes_sent_ += range.size;
        if (range.size)
            dkCmdBufPushConstants(cmd, ubo_[stage].gpu(), kUboSize,
                range.offset, range.size, out + range.offset);
    };
    if (dirty_ & (kVsConst | kShaders)) push(draw_vs_.meta, 0, vsf_, vsi_, vsb_);
    if (dirty_ & (kPsConst | kShaders)) push(draw_ps_.meta, 1, psf_, psi_, psb_);
}

bool Device::alpha_to_coverage() const {
    return targets_[0] && targets_[0]->image() && targets_[0]->image()->ms_mode!=DkMsMode_1x &&
        rs_[D3DRS_ADAPTIVETESS_Y]==0x434f5441u && rs_[D3DRS_ALPHATESTENABLE];
}

void Device::resolve_depth_command() {
    if(auto* texture=core_of(textures_[0]))if(texture->fmt->depth){auto* target=texture->surface(textures_[0],0,0);resolve_depth(depth_,target);}
}

bool Device::resolve_depth(Surface* source,Surface* target,const RECT* sr,const RECT* dr) {
    if(!source || !target || !source->image() || !target->image() || !source->fmt->depth || !target->fmt->depth)return false;
    if(!depth_copy_loaded_){
        const char* root=std::getenv("DIRT2_DATA_DIR");const std::string directory=root?root:".";
        const char* names[]={"fullscreen.vert.dksh","depth_copy.frag.dksh","depth_resolve.frag.dksh"};
        for(unsigned i=0;i<3;++i){std::ifstream f(directory+"/"+names[i],std::ios::binary);std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(f),{});if(!load_shader_binary(bytes,&depth_copy_shaders_[i]))return false;}
        depth_copy_loaded_=true;
    }
    const RECT s=sr?*sr:RECT{0,0,LONG(source->width),LONG(source->height)},d=dr?*dr:RECT{0,0,LONG(target->width),LONG(target->height)};
    if(s.right<=s.left || s.bottom<=s.top || d.right<=d.left || d.bottom<=d.top)return false;
    const float scale_x=float(s.right-s.left)/float(d.right-d.left),scale_y=float(s.bottom-s.top)/float(d.bottom-d.top);
    const float params[4]={scale_x,scale_y,float(s.left)-float(d.left)*scale_x,float(s.top)-float(d.top)*scale_y};
    auto& g=gpu();const auto buffer=g.cur().transient.alloc(256,256);if(!buffer)return false;
    dkCmdBufPushConstants(g.cmd,buffer.gpu(),buffer.size,0,sizeof(params),params);
    dkCmdBufBarrier(g.cmd,DkBarrier_Fragments,DkInvalidateFlags_L2Cache);
    dkCmdBufBarrier(g.cmd,DkBarrier_Full,DkInvalidateFlags_Image);
    probe_surface_pixels(source,"resolve-source");
    const auto output=target->view();dkCmdBufBindRenderTargets(g.cmd,nullptr,0,&output);
    const DkViewport viewport{float(d.left),float(d.top),float(d.right-d.left),float(d.bottom-d.top),0,1};
    const DkScissor scissor{uint32_t(d.left),uint32_t(d.top),uint32_t(d.right-d.left),uint32_t(d.bottom-d.top)};
    dkCmdBufSetViewports(g.cmd,0,&viewport,1);dkCmdBufSetScissors(g.cmd,0,&scissor,1);
    DkRasterizerState raster;dkRasterizerStateDefaults(&raster);raster.cullMode=DkFace_None;dkCmdBufBindRasterizerState(g.cmd,&raster);
    DkColorState color;dkColorStateDefaults(&color);dkCmdBufBindColorState(g.cmd,&color);
    DkMultisampleState ms;dkMultisampleStateDefaults(&ms);ms.mode=ms.rasterizerMode=target->image()->ms_mode;dkMultisampleStateSetLocations(&ms,nullptr,0);dkCmdBufBindMultisampleState(g.cmd,&ms);dkCmdBufSetSampleMask(g.cmd,0xffffu);
    DkDepthStencilState depth;dkDepthStencilStateDefaults(&depth);depth.depthTestEnable=true;depth.depthWriteEnable=true;depth.depthCompareOp=DkCompareOp_Always;dkCmdBufBindDepthStencilState(g.cmd,&depth);
    const DkShader* shaders[]={&depth_copy_shaders_[0],&depth_copy_shaders_[source->image()->ms_mode==DkMsMode_1x?1:2]};dkCmdBufBindShaders(g.cmd,DkStageFlag_GraphicsMask,shaders,2);
    dkCmdBufBindUniformBuffer(g.cmd,DkStage_Fragment,0,buffer.gpu(),buffer.size);
    const auto handle=dkMakeTextureHandle(source->image()->descriptor(),sampler_id(0));dkCmdBufBindTextures(g.cmd,DkStage_Fragment,0,&handle,1);
    if(g.descriptors_dirty){dkCmdBufBarrier(g.cmd,DkBarrier_None,DkInvalidateFlags_Descriptors);g.descriptors_dirty=false;}
    dkCmdBufDraw(g.cmd,DkPrimitive_Triangles,3,1,0,0);
    dkCmdBufBarrier(g.cmd,DkBarrier_Full,DkInvalidateFlags_Image);
    probe_surface_pixels(target,"resolve-destination");
    g.state_owner=nullptr;dirty_=kAll;copy_barrier_=true;
    return true;
}

uint32_t Device::active_texture_mask() const {
    uint32_t mask = 0;
    auto add = [&](const PackMeta* meta, unsigned base, unsigned count) {
        if (meta) for (const auto& sampler : meta->samplers)
            if (sampler.reg < count) mask |= 1u << (base + sampler.reg);
    };
    add(draw_ps_.meta, 0, 16);
    add(draw_vs_.meta, 16, 4);
    return mask;
}

bool Device::prepare_feedback(uint32_t sampled_mask) {
    for(unsigned stage=0;stage<kSamplers;++stage){
        auto* texture=core_of(textures_[stage]);bool attached=false;
        if((sampled_mask & (1u << stage)) && texture && texture->img.mem){
            for(auto* target:targets_)attached|=target && target->tex==texture;
            attached|=depth_ && depth_->tex==texture;
        }
        auto*source=attached?texture:nullptr;
        if(feedback_sources_[stage]!=source){feedback_sources_[stage]=source;dirty_|=kTextures;}
        if(!attached)continue;
        auto& src=texture->img;auto& dst=feedback_images_[stage];
        if(!dst.mem || dst.type!=src.type || dst.format!=src.format || dst.width!=src.width || dst.height!=src.height || dst.depth!=src.depth || dst.levels!=src.levels || dst.layers!=src.layers){
            dst.destroy();std::copy(std::begin(src.swizzle),std::end(src.swizzle),std::begin(dst.swizzle));
            if(!dst.create(src.type,src.format,src.width,src.height,src.depth,src.levels,src.layers,DkImageFlags_Usage2DEngine))return false;
        }
        // Read the completed previous contents, never the concurrently bound
        // attachment. This also makes consecutive dependent draws deterministic.
        dkCmdBufBarrier(gpu().cmd,DkBarrier_Full,DkInvalidateFlags_Image);
        for(unsigned layer=0;layer<src.layers;++layer)for(unsigned level=0;level<src.levels;++level){
            const auto sv=src.view(level,layer),dv=dst.view(level,layer);
            const DkImageRect r{0,0,0,mip(src.width,level),mip(src.height,level),mip(src.depth,level)};
            dkCmdBufCopyImage(gpu().cmd,&sv,&r,&dv,&r,0);
        }
        dkCmdBufBarrier(gpu().cmd,DkBarrier_Full,DkInvalidateFlags_Image);
        dirty_|=kTextures;
    }
    return true;
}

void Device::bind_vertex_state() {
    std::array<float,24> data{};
    for(unsigned i=0;i<6;++i)if(rs_[D3DRS_CLIPPLANEENABLE]&(1u<<i))std::memcpy(data.data()+4*i,clip_planes_[i],16);
    const bool fresh=!vertex_state_;if(fresh)vertex_state_=gpu().buffers.alloc(256,256);
    if(!vertex_state_)return;
    if(fresh || data!=vertex_state_cache_){dkCmdBufPushConstants(gpu().cmd,vertex_state_.gpu(),vertex_state_.size,0,sizeof(data),data.data());vertex_state_cache_=data;}
    dkCmdBufBindUniformBuffer(gpu().cmd,DkStage_Vertex,6,vertex_state_.gpu(),vertex_state_.size);
}

void Device::bind_fragment_state() {
    std::array<uint32_t,12> data{};
    data[0]=rs_[D3DRS_ALPHATESTENABLE]&&!alpha_to_coverage()?rs_[D3DRS_ALPHAFUNC]:D3DCMP_ALWAYS;
    data[1]=rs_[D3DRS_ALPHAREF]&255;data[2]=rs_[D3DRS_FOGTABLEMODE];data[3]=!!rs_[D3DRS_FOGENABLE];
    if(rs_[D3DRS_SRGBWRITEENABLE] && targets_[0] && targets_[0]->image() && srgb_format(targets_[0]->image()->format)==targets_[0]->image()->format)data[3]|=16;
    const DWORD color=rs_[D3DRS_FOGCOLOR];
    const float rgba[4]={float((color>>16)&255)/255.f,float((color>>8)&255)/255.f,float(color&255)/255.f,1.f};
    std::memcpy(data.data()+4,rgba,16);
    const auto& projection=transforms_[D3DTS_PROJECTION];bool position_t=false;if(decl_)for(const auto&e:decl_->elements)position_t|=e.Usage==D3DDECLUSAGE_POSITIONT;
    if(!vs_ && !position_t && projection.m[0][3]==0 && projection.m[1][3]==0 && projection.m[2][3]==0 && projection.m[3][3]==1)data[3]|=2;
    for (unsigned stage = 0; stage < 16; ++stage) {
        const auto* texture = core_of(textures_[stage]);
        if (texture && texture->fmt->depth && texture->d3dfmt != D3DFORMAT(0x5a544e49) &&
            texture->d3dfmt != D3DFORMAT(0x36314644) && texture->d3dfmt != D3DFORMAT(0x34324644))
            data[3] |= 1u << (16 + stage);
    }
    data[8]=rs_[D3DRS_FOGSTART];data[9]=rs_[D3DRS_FOGEND];data[10]=rs_[D3DRS_FOGDENSITY];
    for (unsigned stage = 0; stage < 6; ++stage) {
        const auto* texture = core_of(textures_[stage]);
        const uint32_t type = !texture ? 0 : texture->img.type == DkImageType_3D ? 2 : texture->img.type == DkImageType_Cubemap ? 1 : 0;
        data[11] |= type << (2 * stage);
    }
    const bool fresh=!fragment_state_;
    if(fresh)fragment_state_=gpu().buffers.alloc(256,256);
    if(!fragment_state_)return;
    if(fresh || data!=fragment_state_cache_){
        dkCmdBufPushConstants(gpu().cmd,fragment_state_.gpu(),fragment_state_.size,0,sizeof(data),data.data());
        fragment_state_cache_=data;
    }
    dkCmdBufBindUniformBuffer(gpu().cmd,DkStage_Fragment,5,fragment_state_.gpu(),fragment_state_.size);
}

bool Device::prepare_draw(D3DPRIMITIVETYPE primitive, uint32_t* instances) {
    ApiLock api_lock(__func__);
    if(point_list_!=(primitive==D3DPT_POINTLIST)){point_list_=primitive==D3DPT_POINTLIST;ffp_dirty_=true;dirty_|=kRaster;}
    DrawShader vertex{vs_?vs_->meta:nullptr,vs_?vs_->shader:nullptr};
    DrawShader pixel{ps_?ps_->meta:nullptr,ps_?ps_->shader:nullptr};
    if (pixel.meta && pixel.meta->has_flat_shader && rs_[D3DRS_SHADEMODE] == D3DSHADE_FLAT)
        pixel.shader = &pixel.meta->flat_shader;
    bool position_t=false;
    if(decl_)for(const auto& e:decl_->elements)position_t|=e.Usage==D3DDECLUSAGE_POSITIONT;
    const bool fixed_vertex=!vs_ || position_t;
    if(fixed_vertex || !ps_) {
        if(!fixed_function_)fixed_function_=std::make_unique<FixedFunction>(this);
        if(!fixed_function_->prepare(fixed_vertex?&vertex:nullptr,!ps_?&pixel:nullptr))return false;
    }
    if(vertex.shader!=draw_vs_.shader || pixel.shader!=draw_ps_.shader)dirty_|=kShaders|kVertexInput|kBlend|kTextures|kVsConst|kPsConst;
    draw_vs_=vertex;draw_ps_=pixel;
    if(!draw_vs_.shader || !draw_ps_.shader){++skipped_draws_;return false;}
    // Capture before state emission: readback starts another command list and
    // prepare_draw must restore every binding before the actual draw.
    if (mrt_probe_frame_ && present_count_ + 1 == mrt_probe_frame_ && targets_[1] &&
        !(draw_ps_.meta->color_outputs & 2)) probe_mrt("before-masked", true);
    const bool manual_capture = shim::gpu_dump_active();
    if (manual_capture && manual_capture_seen_ != shim::gpu_dump_directory()) {
        manual_capture_seen_ = shim::gpu_dump_directory();
        ui_probe_textures_.clear();
        ui_probe_draw_ = 0;
        state_log_.clear();
    }
    if (manual_capture || (ui_probe_frame_ && present_count_ + 1 == ui_probe_frame_ &&
        rs_[D3DRS_ALPHABLENDENABLE])) probe_ui_inputs();
    // Bound-but-unused textures must not create attachment snapshots or mip
    // work. They may still be bound from a preceding pass using other shaders.
    const uint32_t sampled_mask = active_texture_mask();
    for (unsigned stage = 0; stage < kSamplers; ++stage)
        if (sampled_mask & (1u << stage))
            if (auto* core = core_of(textures_[stage])) core->generate_mips();
    if(!prepare_feedback(sampled_mask))return false;
    Gpu& g = gpu();
    DkCmdBuf cmd = g.cmd;
    // SYNC also starts a new list after each draw, forcing all state to be
    // emitted again. Separate that effect from its GPU/CPU waits when testing
    // transient live glitches which disappear during detailed captures.
    if (list_seen_ != g.list_id || g.state_owner != this) {
        g.state_owner = this;
        list_seen_ = g.list_id;
        dirty_ = kAll;
    }
    const bool publish_targets = (dirty_ & kTargets) != 0;
    if (rebind_state_) dirty_ = kAll;
    if (dirty_ & kShaders) {
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, ubo_[0].gpu(), kUboSize);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo_[1].gpu(), kUboSize);
    }
    if (copy_barrier_) {
        dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Zcull);
        copy_barrier_ = false;
    }
    // Extra diagnostic rebinding must not add a render-to-sample barrier on
    // otherwise unchanged targets; preserve the normal publication points.
    if (dirty_ & kTargets) flush_targets(publish_targets);
    if (dirty_ & kViewport) flush_viewport();
    if (dirty_ & kShaders) {
        DkShader const* shaders[2] = {draw_vs_.shader, draw_ps_.shader};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    }
    if (dirty_ & kRaster) flush_raster_state();
    if (dirty_ & kBlend) flush_blend_state();
    if (dirty_ & kDepthStencil) flush_depth_stencil();
    if (!flush_vertex_input(instances)) return false;
    if (dirty_ & (kTextures | kShaders)) flush_textures();
    if (dirty_ & (kVsConst | kPsConst | kShaders)) flush_constants();
    if(fixed_vertex || !ps_)fixed_function_->bind_constants(fixed_vertex,!ps_);
    if(ps_ && draw_ps_.meta->has_draw_state)bind_fragment_state();
    if(!fixed_vertex && draw_vs_.meta->has_draw_state)bind_vertex_state();
    if (g.descriptors_dirty) {
        dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        g.descriptors_dirty = false;
    }
    dirty_ = 0;
    for(auto* target:targets_)if(target && target->tex)target->tex->autogen_dirty=true;
    ++draws_;
    if (dump_frame_ && present_count_ + 1 == dump_frame_) dump_note_draw();
    if ((manual_capture || (state_log_frame_ && present_count_ + 1 == state_log_frame_)) && targets_[1]) log_mrt_state();
    return true;
}

void Device::probe_ui_inputs() {
    ApiLock api_lock(__func__);
    ++ui_probe_draw_;
    rt::log("deko3d: UI probe draw %u vs %s ps %s target %ux%u blend %u/%u op %u alpha %u/%u/%u z %u/%u/%u srgbwrite %u",
            ui_probe_draw_, draw_vs_.meta->name.c_str(), draw_ps_.meta->name.c_str(),
            targets_[0] ? targets_[0]->width : 0, targets_[0] ? targets_[0]->height : 0,
            rs_[D3DRS_SRCBLEND], rs_[D3DRS_DESTBLEND], rs_[D3DRS_BLENDOP],
            rs_[D3DRS_ALPHATESTENABLE], rs_[D3DRS_ALPHAFUNC], rs_[D3DRS_ALPHAREF],
            rs_[D3DRS_ZENABLE], rs_[D3DRS_ZWRITEENABLE], rs_[D3DRS_ZFUNC], rs_[D3DRS_SRGBWRITEENABLE]);
    const char* data = std::getenv("DIRT2_DATA_DIR");
    const std::string dir = shim::gpu_dump_active() ? shim::gpu_dump_directory() + "/textures"
                                                   : std::string(data ? data : ".") + "/ui-probe";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (shim::gpu_dump_active()) {
        if (ec) shim::gpu_dump_error("cannot create textures directory");
        char path[512];
        std::snprintf(path, sizeof path, "%s/draw%04u.constants.raw", dir.c_str(), ui_probe_draw_);
        if (std::FILE* f = std::fopen(path, "wb")) {
            std::fwrite(vsf_, 1, sizeof vsf_, f);
            std::fwrite(psf_, 1, sizeof psf_, f);
            if (std::ferror(f)) shim::gpu_dump_error("writing shader constants");
            std::fclose(f);
        } else shim::gpu_dump_error("opening shader constants file");
    }
    for (const auto& sampler : draw_ps_.meta->samplers) {
        const unsigned slot = sampler.reg;
        if (slot >= 16) continue;
        TextureCore* t = core_of(textures_[slot]);
        if (!t || !t->fmt || !t->img.mem || t->fmt->depth) continue;
        const DkGpuAddr identity = dkImageGetGpuAddr(&t->img.image);
        auto it = std::find(ui_probe_textures_.begin(), ui_probe_textures_.end(), identity);
        const unsigned id = unsigned(it - ui_probe_textures_.begin());
        rt::log("deko3d: UI probe draw %u s%u texture %u %ux%u fmt %u usage %u levels %u address %u/%u filter %u/%u/%u srgb %u lodBiasBits %08X maxMip %u",
                ui_probe_draw_, slot, id, t->img.width, t->img.height, unsigned(t->d3dfmt), unsigned(t->usage),
                t->img.levels, ss_[slot][D3DSAMP_ADDRESSU], ss_[slot][D3DSAMP_ADDRESSV],
                ss_[slot][D3DSAMP_MAGFILTER], ss_[slot][D3DSAMP_MINFILTER], ss_[slot][D3DSAMP_MIPFILTER],
                ss_[slot][D3DSAMP_SRGBTEXTURE], ss_[slot][D3DSAMP_MIPMAPLODBIAS], ss_[slot][D3DSAMP_MAXMIPLEVEL]);
        if (it != ui_probe_textures_.end()) continue;
        ui_probe_textures_.push_back(identity);
        const FormatInfo* storage = t->img.expanded_bc ? format_info(D3DFMT_A8B8G8R8) : t->fmt;
        struct Part { uint32_t level, layer, width, height, pitch, bytes, offset; };
        std::vector<Part> parts;
        uint32_t total = 0;
        const uint32_t layers = t->img.type == DkImageType_Cubemap ? 6u : t->img.layers;
        for (uint32_t layer = 0; layer < layers; ++layer) {
            for (uint32_t level = 0; level < t->img.levels; ++level) {
                const uint32_t w = mip(t->img.width, level), h = mip(t->img.height, level);
                const uint32_t pitch = row_pitch(*storage, w), bytes = pitch * level_rows(*storage, h);
                const uint32_t offset = (total + 255) & ~255u;
                if (!bytes || uint64_t(offset) + bytes > (16u << 20)) {
                    rt::log("deko3d: GPU probe texture %u mip %u layer %u omitted (16MiB budget)", id, level, layer);
                    continue;
                }
                parts.push_back({level, layer, w, h, pitch, bytes, offset});
                total = offset + bytes;
            }
        }
        if (parts.empty()) continue;
        Alloc a = gpu().readback.alloc(total, 256);
        if (!a) { shim::gpu_dump_error("allocating texture readback"); continue; }
        dkMemBlockFlushCpuCache(a.block, a.offset, a.size);
        dkCmdBufBarrier(gpu().cmd, DkBarrier_Full, 0);
        for (const Part& p : parts) {
            const DkImageView view = t->img.view(p.level, p.layer, false);
            const DkImageRect rect{0, 0, 0, p.width, p.height, 1};
            const DkCopyBuf dst{a.gpu() + p.offset, p.pitch, 0};
            dkCmdBufCopyImageToBuffer(gpu().cmd, &view, &rect, &dst, 0);
        }
        // Submit all mips/faces together: one GPU stall per texture, not per mip.
        gpu().flush(true, "sampled texture mip-chain capture");
        dkMemBlockFlushCpuCache(a.block, a.offset, a.size);
        for (const Part& p : parts) {
            char path[512];
            const unsigned fmt = unsigned(t->img.expanded_bc ? D3DFMT_A8B8G8R8 : t->d3dfmt);
            if (!p.level && !p.layer)
                std::snprintf(path, sizeof path, "%s/tex%03u_%ux%u_fmt%u.raw", dir.c_str(), id, p.width, p.height, fmt);
            else
                std::snprintf(path, sizeof path, "%s/tex%03u_mip%02u_layer%02u_%ux%u_fmt%u.raw", dir.c_str(), id,
                              p.level, p.layer, p.width, p.height, fmt);
            if (std::FILE* f = std::fopen(path, "wb")) {
                if (std::fwrite(a.cpu() + p.offset, 1, p.bytes, f) != p.bytes) shim::gpu_dump_error("writing sampled texture");
                if (std::fclose(f) != 0) shim::gpu_dump_error("closing sampled texture");
            } else shim::gpu_dump_error("opening sampled texture file");
        }
        gpu().readback.free_now(a);
    }
}

void Device::probe_mrt(const char* phase, bool full) {
    ApiLock api_lock(__func__);
    const char* data = std::getenv("DIRT2_DATA_DIR");
    const std::string dir = std::string(data ? data : ".") + "/mrt-probe";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Surface* surfaces[] = {targets_[0], targets_[1], depth_};
    const char* names[] = {"rt0", "rt1", "z"};
    for (unsigned i = 0; i < 3; ++i) {
        Surface* s = surfaces[i];
        if (!s || !s->image() || !s->fmt || s->fmt->block != 1) continue;
        const uint32_t x = full ? 0 : std::min(96u, s->width - 1);
        const uint32_t y = full ? 0 : std::min(96u, s->height - 1);
        const uint32_t w = full ? s->width : std::min(64u, s->width - x);
        const uint32_t h = full ? s->height : std::min(64u, s->height - y);
        const uint32_t pitch = w * s->fmt->bytes;
        Alloc a = gpu().readback.alloc(pitch * h, 256);
        if (!a) continue;
        read_image(*s->image(), s->layer, s->level, a, pitch,
                   RECT{LONG(x), LONG(y), LONG(x + w), LONG(y + h)});
        uint32_t zero = 0, nonfinite = 0, hash = 2166136261u;
        double sum = 0, low = INFINITY, high = -INFINITY;
        for (uint32_t j = 0; j < pitch * h; ++j) hash = (hash ^ a.cpu()[j]) * 16777619u;
        for (uint32_t j = 0; j < w * h; ++j) {
            const uint8_t* p = a.cpu() + j * s->fmt->bytes;
            bool is_zero = true;
            for (unsigned k = 0; k < s->fmt->bytes; ++k) is_zero &= p[k] == 0;
            zero += is_zero;
            if (s->d3dfmt == D3DFMT_R32F) {
                float f;
                std::memcpy(&f, p, 4);
                if (!std::isfinite(f)) ++nonfinite;
                else { sum += f; low = std::min(low, double(f)); high = std::max(high, double(f)); }
            }
        }
        if (s->d3dfmt != D3DFMT_R32F) low = high = 0;
        char path[512];
        std::snprintf(path, sizeof path, "%s/%04u_%s_%s_%ux%u_fmt%u.raw", dir.c_str(),
                      mrt_probe_draw_, phase, names[i], w, h, unsigned(s->d3dfmt));
        if (std::FILE* f = std::fopen(path, "wb")) {
            std::fwrite(a.cpu(), 1, pitch * h, f);
            std::fclose(f);
        }
        rt::log("deko3d: MRT probe %u %s %s surface %p image %llX block %p offset %u size %u "
                "fmt %u rect %u,%u %ux%u zero %u/%u hash %08X mean %.9g min %.9g max %.9g nonfinite %u",
                mrt_probe_draw_, phase, names[i], static_cast<void*>(s),
                (unsigned long long)dkImageGetGpuAddr(&s->image()->image),
                static_cast<void*>(s->image()->mem.block), s->image()->mem.offset, s->image()->mem.size,
                unsigned(s->d3dfmt), x, y, w, h, zero, w * h, hash, sum / (w * h), low, high, nonfinite);
        // read_image waits for completion; no queued command still uses this allocation.
        gpu().readback.free_now(a);
    }
}

void Device::log_mrt_state() {
    ApiLock api_lock(__func__);
    char s[320];
    std::snprintf(s, sizeof s,
                  "ps %s outputs %X; blend %u src %u dst %u op %u sep %u asrc %u adst %u aop %u; write %X %X; "
                  "alphatest %u func %u ref %u; z %u write %u func %u; formats %u %u",
                  draw_ps_.meta ? draw_ps_.meta->name.c_str() : "-", draw_ps_.meta ? draw_ps_.meta->color_outputs : 0,
                  rs_[D3DRS_ALPHABLENDENABLE], rs_[D3DRS_SRCBLEND], rs_[D3DRS_DESTBLEND], rs_[D3DRS_BLENDOP],
                  rs_[D3DRS_SEPARATEALPHABLENDENABLE], rs_[D3DRS_SRCBLENDALPHA], rs_[D3DRS_DESTBLENDALPHA],
                  rs_[D3DRS_BLENDOPALPHA], rs_[D3DRS_COLORWRITEENABLE], rs_[D3DRS_COLORWRITEENABLE1],
                  rs_[D3DRS_ALPHATESTENABLE], rs_[D3DRS_ALPHAFUNC], rs_[D3DRS_ALPHAREF], rs_[D3DRS_ZENABLE],
                  rs_[D3DRS_ZWRITEENABLE], rs_[D3DRS_ZFUNC], uint32_t(targets_[0] ? targets_[0]->d3dfmt : 0), uint32_t(targets_[1]->d3dfmt));
    if (std::find(state_log_.begin(), state_log_.end(), s) != state_log_.end()) return;
    state_log_.push_back(s);
    rt::log("deko3d: MRT draw state: %s", s);
}

void Device::dump_note_draw() {
    ApiLock api_lock(__func__);
    Surface* s = targets_[0];
    if (!s) return;
    auto it = std::find_if(dump_.begin(), dump_.end(), [&](const DumpTarget& t) { return t.surface == s; });
    if (it == dump_.end()) {
        s->AddRef();
        dump_.push_back({s});
        it = dump_.end() - 1;
    }
    ++it->draws;
    const std::string name = draw_vs_.meta->name + "/" + draw_ps_.meta->name;
    if (it->shaders.size() < 6 && std::find(it->shaders.begin(), it->shaders.end(), name) == it->shaders.end())
        it->shaders.push_back(name);
}

namespace {
float half_to_float(uint16_t v) {
    const uint32_t sgn = v >> 15, e = (v >> 10) & 31, m = v & 1023;
    float r = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
    return sgn ? -r : r;
}
}  // namespace

void Device::dump_targets() {
    ApiLock api_lock(__func__);
    const char* data = std::getenv("DIRT2_DATA_DIR");
    const std::string dir = std::string(data ? data : ".") + "/dump";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    rt::log("deko3d: dumping %zu render targets of frame %u to %s", dump_.size(), dump_frame_, dir.c_str());
    int index = 0;
    for (DumpTarget& t : dump_) {
        Surface* s = t.surface;
        GpuImage* img = s->image();
        ++index;
        if (!img) continue;
        const uint32_t w = s->width, h = s->height, bpp = s->fmt->bytes;
        const uint32_t pitch = w * bpp;
        Alloc a = gpu().readback.alloc(pitch * h, 256);
        if (!a) continue;
        read_image(*img, s->tex ? s->layer : 0, s->tex ? s->level : 0, a, pitch, RECT{0, 0, LONG(w), LONG(h)});
        // To 8-bit BGRA, with statistics of the stored values.
        std::vector<uint8_t> out(size_t(w) * h * 4);
        double sum[3] = {}, mn = 1e30, mx = -1e30;
        uint64_t nans = 0, infs = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = a.cpu() + size_t(y) * pitch + size_t(x) * bpp;
                float c[3] = {0, 0, 0};
                switch (s->fmt->dk) {
                    case DkImageFormat_BGRA8_Unorm: c[0] = p[2] / 255.f; c[1] = p[1] / 255.f; c[2] = p[0] / 255.f; break;
                    case DkImageFormat_RGBA8_Unorm: c[0] = p[0] / 255.f; c[1] = p[1] / 255.f; c[2] = p[2] / 255.f; break;
                    case DkImageFormat_RGB10A2_Unorm: {
                        uint32_t v;
                        std::memcpy(&v, p, 4);
                        c[0] = (v & 1023) / 1023.f; c[1] = ((v >> 10) & 1023) / 1023.f; c[2] = ((v >> 20) & 1023) / 1023.f;
                        break;
                    }
                    case DkImageFormat_RGBA16_Float: case DkImageFormat_RG16_Float: case DkImageFormat_R16_Float: {
                        const uint32_t n = s->fmt->dk == DkImageFormat_R16_Float ? 1 : s->fmt->dk == DkImageFormat_RG16_Float ? 2 : 3;
                        for (uint32_t k = 0; k < n; ++k) {
                            uint16_t hv;
                            std::memcpy(&hv, p + 2 * k, 2);
                            c[k] = half_to_float(hv);
                        }
                        break;
                    }
                    case DkImageFormat_RGBA16_Unorm:
                        for (int k = 0; k < 3; ++k) {
                            uint16_t v;
                            std::memcpy(&v, p + 2 * k, 2);
                            c[k] = v / 65535.f;
                        }
                        break;
                    case DkImageFormat_R32_Float: std::memcpy(&c[0], p, 4); c[1] = c[2] = c[0]; break;
                    default: break;
                }
                uint8_t* o = &out[(size_t(y) * w + x) * 4];
                for (int k = 0; k < 3; ++k) {
                    const float v = c[k];
                    if (std::isnan(v)) ++nans;
                    else if (std::isinf(v)) ++infs;
                    else {
                        sum[k] += v;
                        mn = std::min<double>(mn, v);
                        mx = std::max<double>(mx, v);
                    }
                    const float t8 = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : (std::isnan(v) ? 1.0f : 0.0f);
                    o[2 - k] = uint8_t(t8 * 255.0f + 0.5f);
                }
                o[3] = 255;
            }
        gpu().defer([a] { gpu().readback.free_now(a); });
        char path[512];
        std::snprintf(path, sizeof path, "%s/%02d_%ux%u_fmt%u.bmp", dir.c_str(), index, w, h, uint32_t(s->d3dfmt));
        if (std::FILE* f = std::fopen(path, "wb")) {
            const uint32_t size = w * 4 * h;
            uint8_t hdr[54] = {'B', 'M'};
            auto put32 = [&](int off, uint32_t v) { std::memcpy(hdr + off, &v, 4); };
            put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, w);
            put32(22, uint32_t(-int32_t(h)));
            hdr[26] = 1; hdr[28] = 32; put32(34, size);
            std::fwrite(hdr, 1, 54, f);
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
        const double n = double(w) * h;
        std::string shaders;
        for (auto& x : t.shaders) shaders += " " + x;
        rt::log("deko3d: dump %02d %ux%u fmt %u%s level %u: %u draws, mean %.3f %.3f %.3f, min %.3f max %.3f, NaN %llu Inf %llu; shaders%s",
                index, w, h, uint32_t(s->d3dfmt), s->tex ? " (texture)" : "", s->tex ? s->level : 0, t.draws, sum[0] / n,
                sum[1] / n, sum[2] / n, mn, mx, (unsigned long long)nans, (unsigned long long)infs, shaders.c_str());
        s->Release();
    }
    dump_.clear();
}

// ---- draws ----

void Device::capture_draw(D3DPRIMITIVETYPE type, int base, uint32_t start, uint32_t count, uint32_t instances,
                          bool indexed, const void* up_indices, D3DFORMAT index_format) {
    ApiLock api_lock(__func__);
    if (!shim::gpu_dump_active() || !decl_ || !ps_) return;
    // Capture every draw, including opaque UI and alpha-tested scenery. Keep the
    // full byte range and binding offsets; content-addressing deduplicates the
    // shared buffers without losing later DISCARD/NOOVERWRITE versions.
    const std::string& ps = draw_ps_.meta->name;
    const std::string dir = shim::gpu_dump_directory() + "/geometry";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) { shim::gpu_dump_error("creating geometry directory"); return; }
    auto blob = [&](const uint8_t* data, uint32_t bytes) {
        if (!data || !bytes) return std::string("empty");
        if (bytes > (8u << 20)) return std::string("omitted-over-8MiB");
        const std::string name = recomp::sha256_hex(data, bytes) + ".raw";
        if (!std::filesystem::exists(dir + "/" + name)) {
            std::FILE* f = std::fopen((dir + "/" + name).c_str(), "wb");
            if (!f) shim::gpu_dump_error("opening geometry buffer");
            else {
                if (std::fwrite(data, 1, bytes, f) != bytes) shim::gpu_dump_error("writing geometry buffer");
                if (std::fclose(f) != 0) shim::gpu_dump_error("closing geometry buffer");
            }
        }
        return name;
    };
    char name[64];
    std::snprintf(name, sizeof name, "/draw%04u.txt", ui_probe_draw_);
    std::ofstream out(dir + name);
    out << "vs " << draw_vs_.meta->name << " ps " << ps << " primitive " << unsigned(type)
        << " base " << base << " start " << start << " vertices " << vertex_count(type, count)
        << " instances " << instances << " indexed " << indexed << '\n';
    out << "viewport " << viewport_.X << ' ' << viewport_.Y << ' ' << viewport_.Width << ' ' << viewport_.Height
        << " depth " << viewport_.MinZ << ' ' << viewport_.MaxZ << " cull " << rs_[D3DRS_CULLMODE]
        << " scissor " << rs_[D3DRS_SCISSORTESTENABLE] << ' ' << scissor_.left << ' ' << scissor_.top
        << ' ' << scissor_.right << ' ' << scissor_.bottom << '\n';
    std::set<unsigned> streams;
    for (const D3DVERTEXELEMENT9& e : decl_->elements) {
        out << "element " << e.Stream << ' ' << e.Offset << ' ' << unsigned(e.Type)
            << ' ' << unsigned(e.Method) << ' ' << unsigned(e.Usage) << ' ' << unsigned(e.UsageIndex) << '\n';
        streams.insert(e.Stream);
    }
    for (unsigned i : streams) {
        if (i >= kStreams) continue;
        if (up_.active && i == 0) out << "stream 0 offset 0 stride " << up_.stride << " frequency 1 bytes "
            << up_.size << " file " << blob(up_.cpu, up_.size) << '\n';
        else if (VertexBuffer* v = streams_[i].vb) {
            const auto& s = streams_[i];
            out << "stream " << i << " offset " << s.offset << " stride " << s.stride << " frequency " << s.freq
                << " bytes " << v->store.size << " file "
                << blob(v->store.versions[v->store.current].mem.cpu(), v->store.size) << '\n';
        }
    }
    if (indexed) {
        if (up_indices) {
            const uint32_t bytes = vertex_count(type, count) * (index_format == D3DFMT_INDEX32 ? 4u : 2u);
            out << "indices format " << unsigned(index_format) << " bytes " << bytes << " file "
                << blob(static_cast<const uint8_t*>(up_indices), bytes) << '\n';
        } else if (indices_) out << "indices format " << unsigned(indices_->format) << " bytes " << indices_->store.size
            << " file " << blob(indices_->store.versions[indices_->store.current].mem.cpu(), indices_->store.size) << '\n';
    }
    out.flush();
    if (!out) shim::gpu_dump_error("writing geometry metadata");
}

HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
    ApiLock api_lock(__func__);
    uint32_t instances;
    if (!prepare_draw(type, &instances)) return D3D_OK;
    capture_draw(type, 0, start, count, 1, false);
    dkCmdBufDraw(gpu().cmd, primitive(type), vertex_count(type, count), 1, start, 0);
    sync_after_draw("DrawPrimitive", count, 1, 0, start);
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT, UINT, UINT start, UINT count) {
    ApiLock api_lock(__func__);
    if (!indices_) return D3DERR_INVALIDCALL;
    uint32_t instances;
    if (!prepare_draw(type, &instances)) return D3D_OK;
    capture_draw(type, base, start, count, instances, true);
    DkCmdBuf cmd = gpu().cmd;
    dkCmdBufBindIdxBuffer(cmd, indices_->format == D3DFMT_INDEX32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16,
                          indices_->store.addr());
    indices_->store.used();
    dkCmdBufDrawIndexed(cmd, primitive(type), vertex_count(type, count), instances, start, base, 0);
    sync_after_draw("DrawIndexedPrimitive", count, instances, base, start);
    return D3D_OK;
}

HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    ApiLock api_lock(__func__);
    if (!data) return D3DERR_INVALIDCALL;
    const uint32_t bytes = vertex_count(type, count) * stride;
    Alloc a = gpu().cur().transient.alloc(bytes, 16);
    std::memcpy(a.cpu(), data, bytes);
    up_ = {true, a.gpu(), bytes, stride, a.cpu()};
    dirty_ |= kVertexInput;
    uint32_t instances;
    if (prepare_draw(type, &instances)) {
        capture_draw(type, 0, 0, count, 1, false);
        dkCmdBufDraw(gpu().cmd, primitive(type), vertex_count(type, count), 1, 0, 0);
        sync_after_draw("DrawPrimitiveUP", count, 1, 0, 0);
    }
    up_.active = false;
    // D3D9 unbinds stream 0 after a UP draw.
    set_ref(streams_[0].vb, static_cast<VertexBuffer*>(nullptr));
    streams_[0].offset = streams_[0].stride = 0;
    dirty_ |= kVertexInput;
    return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count,
                                       const void* indices, D3DFORMAT index_format, const void* data, UINT stride) {
    ApiLock api_lock(__func__);
    if (!data || !indices) return D3DERR_INVALIDCALL;
    const uint32_t vbytes = (min_index + vertices) * stride;
    const uint32_t isize = index_format == D3DFMT_INDEX32 ? 4 : 2, n = vertex_count(type, count);
    Alloc v = gpu().cur().transient.alloc(vbytes, 16);
    Alloc ix = gpu().cur().transient.alloc(n * isize, 16);
    std::memcpy(v.cpu(), data, vbytes);
    std::memcpy(ix.cpu(), indices, n * isize);
    up_ = {true, v.gpu(), vbytes, stride, v.cpu()};
    dirty_ |= kVertexInput;
    uint32_t instances;
    if (prepare_draw(type, &instances)) {
        capture_draw(type, 0, 0, count, 1, true, indices, index_format);
        dkCmdBufBindIdxBuffer(gpu().cmd, isize == 4 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, ix.gpu());
        dkCmdBufDrawIndexed(gpu().cmd, primitive(type), n, 1, 0, 0, 0);
        sync_after_draw("DrawIndexedPrimitiveUP", count, 1, 0, 0);
    }
    up_.active = false;
    set_ref(streams_[0].vb, static_cast<VertexBuffer*>(nullptr));
    set_ref(indices_, static_cast<IndexBuffer*>(nullptr));
    streams_[0].offset = streams_[0].stride = 0;
    dirty_ |= kVertexInput;
    return D3D_OK;
}

}  // namespace deko
