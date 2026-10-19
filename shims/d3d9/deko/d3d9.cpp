// IDirect3D9 on deko3d: adapter queries, capabilities, CreateDevice, and the
// Direct3DCreate9 the D3D9 bridge calls (shims/d3d9/bridge.cpp).
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../../../platform/platform.h"
#include "device.h"
#include "recomp/host.h"

namespace deko {

namespace {
// Bring-up: the first few calls of each IDirect3D9 method.
void trace(const char* what) {
    static std::atomic<int> n{0};
    if (n++ < 64) rt::log("deko3d: IDirect3D9::%s", what);
}
}  // namespace

HRESULT D3D9::GetAdapterIdentifier(UINT adapter, DWORD, D3DADAPTER_IDENTIFIER9* id) {
    trace("GetAdapterIdentifier");
    if (adapter || !id) return D3DERR_INVALIDCALL;
    // Up to WHQLLevel only: the host struct is 4 bytes longer than the game's
    // (DXVK's headers align it to 8 for DriverVersion; D3D9's pack it to 4),
    // and clearing the padding overwrote the field after it in the game's object.
    std::memset(id, 0, offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof id->WHQLLevel);
    std::snprintf(id->Driver, sizeof id->Driver, "deko3d");
    std::snprintf(id->Description, sizeof id->Description, "NVIDIA Tegra X1 (deko3d)");
    std::snprintf(id->DeviceName, sizeof id->DeviceName, "\\\\.\\DISPLAY1");
    id->VendorId = 0x10DE;
    id->DeviceId = 0x1189;
    id->WHQLLevel = 1;
    return D3D_OK;
}

UINT D3D9::GetAdapterModeCount(UINT adapter, D3DFORMAT) {
    trace("GetAdapterModeCount");
    return adapter ? 0 : UINT(platform::display_modes().size());
}

HRESULT D3D9::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) {
    trace("EnumAdapterModes");
    const auto& modes = platform::display_modes();
    if (adapter || !out || mode >= modes.size()) return D3DERR_INVALIDCALL;
    *out = D3DDISPLAYMODE{modes[mode].width, modes[mode].height, modes[mode].refresh, format};
    return D3D_OK;
}

HRESULT D3D9::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) {
    trace("GetAdapterDisplayMode");
    if (adapter || !mode) return D3DERR_INVALIDCALL;
    int w = 1280, h = 720;
    platform::display_size(&w, &h);
    *mode = D3DDISPLAYMODE{UINT(w), UINT(h), 60, D3DFMT_X8R8G8B8};
    return D3D_OK;
}

HRESULT D3D9::CheckDeviceFormat(UINT adapter, D3DDEVTYPE, D3DFORMAT, DWORD usage, D3DRESOURCETYPE, D3DFORMAT format) {
    trace("CheckDeviceFormat");
    if (adapter) return D3DERR_INVALIDCALL;
    const FormatInfo* f = format_info(format);
    if (!f) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !f->depth) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_RENDERTARGET) && (f->depth || f->block > 1)) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT D3D9::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE ms, DWORD* quality) {
    if (quality) *quality = 1;
    // Multisampling comes later (resolves on Present/StretchRect).
    return adapter == 0 && ms == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT D3D9::GetDeviceCaps(UINT adapter, D3DDEVTYPE, D3DCAPS9* caps) {
    trace("GetDeviceCaps");
    if (adapter || !caps) return D3DERR_INVALIDCALL;
    fill_caps(caps);
    return D3D_OK;
}

HRESULT D3D9::CreateDevice(UINT adapter, D3DDEVTYPE, HWND window, DWORD flags, D3DPRESENT_PARAMETERS* params,
                           IDirect3DDevice9** out) {
    trace("CreateDevice");
    if (adapter || !params || !out) return D3DERR_INVALIDCALL;
    auto* d = new Device(this, window, flags, *params);
    if (!d->init()) {
        d->Release();
        *out = nullptr;
        return D3DERR_NOTAVAILABLE;
    }
    *out = d;
    return D3D_OK;
}

void fill_caps(D3DCAPS9* c) {
    std::memset(c, 0, sizeof *c);
    c->DeviceType = D3DDEVTYPE_HAL;
    c->Caps2 = D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_FULLSCREENGAMMA;
    c->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION |
               D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    c->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    c->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                 D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                 D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
                 D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                 D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
    c->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                           D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_TSSARGTEMP |
                           D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_FOGANDSPECULARALPHA |
                           D3DPMISCCAPS_SEPARATEALPHABLEND | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS |
                           D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING | D3DPMISCCAPS_POSTBLENDSRGBCONVERT;
    c->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_ANISOTROPY |
                    D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS |
                    D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    c->ZCmpCaps = c->AlphaCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                                    D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL |
                                    D3DPCMPCAPS_ALWAYS;
    c->SrcBlendCaps = c->DestBlendCaps =
        D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
        D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA |
        D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
    c->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                   D3DPSHADECAPS_FOGGOURAUD;
    c->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP |
                     D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP | D3DPTEXTURECAPS_CUBEMAP |
                     D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_PROJECTED;
    c->TextureFilterCaps = c->CubeTextureFilterCaps = c->VolumeTextureFilterCaps =
        D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC |
        D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR |
        D3DPTFILTERCAPS_MAGFANISOTROPIC;
    c->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                               D3DPTFILTERCAPS_MAGFLINEAR;
    c->TextureAddressCaps = c->VolumeTextureAddressCaps =
        D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER |
        D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    c->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
    c->MaxTextureWidth = c->MaxTextureHeight = 8192;
    c->MaxVolumeExtent = 2048;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 8192;
    c->MaxAnisotropy = 16;
    c->MaxVertexW = 1e10f;
    c->GuardBandLeft = c->GuardBandTop = -32768.0f;
    c->GuardBandRight = c->GuardBandBottom = 32768.0f;
    c->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                     D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                     D3DSTENCILCAPS_TWOSIDED;
    c->FVFCaps = 8;
    c->MaxTextureBlendStages = 8;
    c->MaxSimultaneousTextures = 8;
    c->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                              D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    c->MaxActiveLights = 8;
    c->MaxUserClipPlanes = 6;
    c->MaxVertexBlendMatrices = 4;
    c->MaxPointSize = 256.0f;
    c->MaxPrimitiveCount = 0xFFFFFF;
    c->MaxVertexIndex = 0xFFFFFF;
    c->MaxStreams = 16;
    c->MaxStreamStride = 508;
    c->VertexShaderVersion = D3DVS_VERSION(3, 0);
    c->MaxVertexShaderConst = 256;
    c->PixelShaderVersion = D3DPS_VERSION(3, 0);
    c->PixelShader1xMaxValue = 65504.0f;
    c->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET |
                  D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;
    c->NumSimultaneousRTs = 4;
    c->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N |
                   D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    c->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    c->VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    c->VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
    c->VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
    c->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION |
                       D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    c->PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    c->PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
    c->PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
    c->PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
    c->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT;
    c->MaxVShaderInstructionsExecuted = 0xFFFFFFFF;
    c->MaxPShaderInstructionsExecuted = 0xFFFFFFFF;
    c->MaxVertexShader30InstructionSlots = 32768;
    c->MaxPixelShader30InstructionSlots = 32768;
}

}  // namespace deko

// The D3D9 bridge's host entry point (DXVK's on the Apple platforms).
extern "C" IDirect3D9* Direct3DCreate9(UINT sdk_version) {
    rt::log("deko3d: Direct3DCreate9(%u)", sdk_version);
    return new deko::D3D9;
}
