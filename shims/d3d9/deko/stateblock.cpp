// State-block selection follows native D3D9 behavior, cross-checked against
// DXVK's CaptureType/Pixel/Vertex masks. Render/depth targets and palettes are
// deliberately outside state blocks. See tests/d3d9_wine/upstream/stateblock.c.
#include "device.h"

namespace deko {
StateBlock::StateBlock(Device* device) : device_(device) { SetDeviceParent(device); }
HRESULT StateBlock::GetDevice(IDirect3DDevice9** out) {
    ApiLock lock(__func__);
    if (!out) return D3DERR_INVALIDCALL;
    *out=device_; device_->AddRef();return D3D_OK;
}
HRESULT StateBlock::Capture() {
    ApiLock lock(__func__);
    if (device_->recording_) return D3DERR_INVALIDCALL;
    for (auto& item : entries_) item.second.capture();
    return D3D_OK;
}
HRESULT StateBlock::Apply() {
    ApiLock lock(__func__);
    if (device_->recording_) return D3DERR_INVALIDCALL;
    for (auto& item : entries_) item.second.apply();
    return D3D_OK;
}
HRESULT Device::BeginStateBlock() {
    ApiLock lock(__func__);
    if (recording_) return D3DERR_INVALIDCALL;
    recording_=new StateBlock(this);
    // During recording, ownership belongs solely to the device. Public parent
    // ownership is acquired on EndStateBlock, avoiding a device/block cycle.
    recording_->AddRefPrivate();recording_->Release();
    return D3D_OK;
}
HRESULT Device::EndStateBlock(IDirect3DStateBlock9** out) {
    ApiLock lock(__func__);
    if (!recording_ || !out) return D3DERR_INVALIDCALL;
    auto* block=recording_;recording_=nullptr;
    block->AddRef();block->ReleasePrivate();*out=block;
    return D3D_OK;
}
HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) {
    ApiLock lock(__func__);
    if (recording_ || !out || type<D3DSBT_ALL || type>D3DSBT_VERTEXSTATE) return D3DERR_INVALIDCALL;
    BeginStateBlock();
    // Native typed state-block Capture preserves its existing stream offsets;
    // explicit Begin/End recording captures offsets normally.
    recording_->capture_stream_offsets = false;
    const bool pixel=type==D3DSBT_ALL || type==D3DSBT_PIXELSTATE;
    const bool vertex=type==D3DSBT_ALL || type==D3DSBT_VERTEXSTATE;
    static const D3DRENDERSTATETYPE pixel_states[] = {
        D3DRS_ZENABLE,
        D3DRS_FILLMODE,
        D3DRS_SHADEMODE,
        D3DRS_ZWRITEENABLE,
        D3DRS_ALPHATESTENABLE,
        D3DRS_LASTPIXEL,
        D3DRS_SRCBLEND,
        D3DRS_DESTBLEND,
        D3DRS_ZFUNC,
        D3DRS_ALPHAREF,
        D3DRS_ALPHAFUNC,
        D3DRS_DITHERENABLE,
        D3DRS_FOGSTART,
        D3DRS_FOGEND,
        D3DRS_FOGDENSITY,
        D3DRS_ALPHABLENDENABLE,
        D3DRS_DEPTHBIAS,
        D3DRS_STENCILENABLE,
        D3DRS_STENCILFAIL,
        D3DRS_STENCILZFAIL,
        D3DRS_STENCILPASS,
        D3DRS_STENCILFUNC,
        D3DRS_STENCILREF,
        D3DRS_STENCILMASK,
        D3DRS_STENCILWRITEMASK,
        D3DRS_TEXTUREFACTOR,
        D3DRS_WRAP0,
        D3DRS_WRAP1,
        D3DRS_WRAP2,
        D3DRS_WRAP3,
        D3DRS_WRAP4,
        D3DRS_WRAP5,
        D3DRS_WRAP6,
        D3DRS_WRAP7,
        D3DRS_WRAP8,
        D3DRS_WRAP9,
        D3DRS_WRAP10,
        D3DRS_WRAP11,
        D3DRS_WRAP12,
        D3DRS_WRAP13,
        D3DRS_WRAP14,
        D3DRS_WRAP15,
        D3DRS_COLORWRITEENABLE,
        D3DRS_BLENDOP,
        D3DRS_SCISSORTESTENABLE,
        D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_ANTIALIASEDLINEENABLE,
        D3DRS_TWOSIDEDSTENCILMODE,
        D3DRS_CCW_STENCILFAIL,
        D3DRS_CCW_STENCILZFAIL,
        D3DRS_CCW_STENCILPASS,
        D3DRS_CCW_STENCILFUNC,
        D3DRS_COLORWRITEENABLE1,
        D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3,
        D3DRS_BLENDFACTOR,
        D3DRS_SRGBWRITEENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_SRCBLENDALPHA,
        D3DRS_DESTBLENDALPHA,
        D3DRS_BLENDOPALPHA,
    };
    static const D3DRENDERSTATETYPE vertex_states[] = {
        D3DRS_CULLMODE,
        D3DRS_FOGENABLE,
        D3DRS_FOGCOLOR,
        D3DRS_FOGTABLEMODE,
        D3DRS_FOGSTART,
        D3DRS_FOGEND,
        D3DRS_FOGDENSITY,
        D3DRS_RANGEFOGENABLE,
        D3DRS_AMBIENT,
        D3DRS_COLORVERTEX,
        D3DRS_FOGVERTEXMODE,
        D3DRS_CLIPPING,
        D3DRS_LIGHTING,
        D3DRS_LOCALVIEWER,
        D3DRS_EMISSIVEMATERIALSOURCE,
        D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_DIFFUSEMATERIALSOURCE,
        D3DRS_SPECULARMATERIALSOURCE,
        D3DRS_VERTEXBLEND,
        D3DRS_CLIPPLANEENABLE,
        D3DRS_POINTSIZE,
        D3DRS_POINTSIZE_MIN,
        D3DRS_POINTSPRITEENABLE,
        D3DRS_POINTSCALEENABLE,
        D3DRS_POINTSCALE_A,
        D3DRS_POINTSCALE_B,
        D3DRS_POINTSCALE_C,
        D3DRS_MULTISAMPLEANTIALIAS,
        D3DRS_MULTISAMPLEMASK,
        D3DRS_PATCHEDGESTYLE,
        D3DRS_POINTSIZE_MAX,
        D3DRS_INDEXEDVERTEXBLENDENABLE,
        D3DRS_TWEENFACTOR,
        D3DRS_POSITIONDEGREE,
        D3DRS_NORMALDEGREE,
        D3DRS_MINTESSELLATIONLEVEL,
        D3DRS_MAXTESSELLATIONLEVEL,
        D3DRS_ADAPTIVETESS_X,
        D3DRS_ADAPTIVETESS_Y,
        D3DRS_ADAPTIVETESS_Z,
        D3DRS_ADAPTIVETESS_W,
        D3DRS_ENABLEADAPTIVETESSELLATION,
        D3DRS_NORMALIZENORMALS,
        D3DRS_SPECULARENABLE,
        D3DRS_SHADEMODE,
    };
    if (pixel) {
        for (auto state:pixel_states) SetRenderState(state,rs_[state]);
        for (DWORD i=0;i<16;++i) for (UINT state=1;state<=12;++state)
            SetSamplerState(i,D3DSAMPLERSTATETYPE(state),ss_[i][state]);
        for (DWORD i=0;i<8;++i) for (UINT state=0;state<33;++state)
            SetTextureStageState(i,D3DTEXTURESTAGESTATETYPE(state),tss_[i][state]);
        SetPixelShader(ps_);
        SetPixelShaderConstantF(0,&psf_[0][0],224);
        SetPixelShaderConstantI(0,&psi_[0][0],16);
        SetPixelShaderConstantB(0,psb_,16);
    }
    if (vertex) {
        for (auto state:vertex_states) SetRenderState(state,rs_[state]);
        for (UINT i=16;i<kSamplers;++i)
            SetSamplerState(D3DVERTEXTEXTURESAMPLER0+i-16,D3DSAMP_DMAPOFFSET,ss_[i][D3DSAMP_DMAPOFFSET]);
        SetVertexDeclaration(decl_);SetVertexShader(vs_);
        SetVertexShaderConstantF(0,&vsf_[0][0],256);
        SetVertexShaderConstantI(0,&vsi_[0][0],16);
        SetVertexShaderConstantB(0,vsb_,16);
        for (UINT i=0;i<kStreams;++i) SetStreamSourceFreq(i,streams_[i].freq);
        for (const auto& item:lights_) { SetLight(item.first,&item.second.value);LightEnable(item.first,item.second.enabled); }
    }
    if (type==D3DSBT_ALL) {
        for (UINT i=0;i<kSamplers;++i) SetTexture(i<16?i:D3DVERTEXTEXTURESAMPLER0+i-16,textures_[i]);
        for (UINT i=0;i<kStreams;++i) SetStreamSource(i,streams_[i].vb,streams_[i].offset,streams_[i].stride);
        SetIndices(indices_);SetViewport(&viewport_);SetScissorRect(&scissor_);
        for (UINT i=0;i<6;++i) SetClipPlane(i,clip_planes_[i]);
        SetTransform(D3DTS_VIEW,&transforms_[D3DTS_VIEW]);
        SetTransform(D3DTS_PROJECTION,&transforms_[D3DTS_PROJECTION]);
        for (UINT i=16;i<24;++i) SetTransform(D3DTRANSFORMSTATETYPE(i),&transforms_[i]);
        for (UINT i=256;i<512;++i) SetTransform(D3DTRANSFORMSTATETYPE(i),&transforms_[i]);
        SetMaterial(&material_);
    }
    return EndStateBlock(out);
}
} // namespace deko
