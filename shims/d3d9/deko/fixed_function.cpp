// State ABI for DXVK's fixed-function algorithms adapted by
// tools/scripts/build_deko_ffp_shaders.py (third_party/deko_ffp/LICENSE).
#include "fixed_function.h"
#include "device.h"
#include "recomp/host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace deko {
namespace {
template<class T>void put(std::vector<uint8_t>& bytes,size_t offset,const T& value){
    std::memcpy(bytes.data()+offset,&value,sizeof(value));
}
float floating(DWORD value){float out;std::memcpy(&out,&value,4);return out;}
D3DMATRIX multiply(const D3DMATRIX&a,const D3DMATRIX&b){
    D3DMATRIX out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)out.m[r][c]+=a.m[r][k]*b.m[k][c];return out;
}
D3DMATRIX inverse(const D3DMATRIX& value){
    double a[4][8]{};for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)a[r][c]=value.m[r][c];a[r][r+4]=1;}
    for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
        if(std::abs(a[pivot][c])<1e-30){D3DMATRIX identity{};for(unsigned i=0;i<4;++i)identity.m[i][i]=1;return identity;}
        for(unsigned k=0;k<8;++k)std::swap(a[pivot][k],a[c][k]);
        const double scale=a[c][c];for(double&x:a[c])x/=scale;
        for(unsigned r=0;r<4;++r)if(r!=c){const double f=a[r][c];for(unsigned k=0;k<8;++k)a[r][k]-=f*a[c][k];}
    }
    D3DMATRIX out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out.m[r][c]=float(a[r][c+4]);return out;
}
std::array<float,4> transform(const D3DVECTOR&v,const D3DMATRIX&m,float w){
    const float input[4]={v.x,v.y,v.z,w};std::array<float,4>out{};
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)out[c]+=input[r]*m.m[r][c];return out;
}
uint32_t argument_mask(DWORD op){switch(op){
    case D3DTOP_DISABLE:case D3DTOP_BUMPENVMAP:case D3DTOP_BUMPENVMAPLUMINANCE:return 0;
    case D3DTOP_SELECTARG1:case D3DTOP_PREMODULATE:return 2;
    case D3DTOP_SELECTARG2:return 4;
    case D3DTOP_MULTIPLYADD:case D3DTOP_LERP:return 7;
    default:return 6;
}}
uint32_t argument(DWORD arg){return(arg&7)|((arg&0x30)>>1);}
uint32_t components(BYTE type){switch(type){case D3DDECLTYPE_FLOAT1:return 1;case D3DDECLTYPE_FLOAT2:case D3DDECLTYPE_SHORT2:case D3DDECLTYPE_SHORT2N:case D3DDECLTYPE_USHORT2N:case D3DDECLTYPE_FLOAT16_2:return 2;case D3DDECLTYPE_FLOAT3:case D3DDECLTYPE_UDEC3:case D3DDECLTYPE_DEC3N:return 3;default:return 4;}}
}

FixedFunction::~FixedFunction(){
    for(auto& stage:buffers_)for(const auto& buffer:stage)if(buffer)gpu().defer([buffer]{gpu().buffers.free_now(buffer);});
}
bool FixedFunction::load(){
    if(loaded_)return true;
    const char* root=std::getenv("DIRT2_DATA_DIR");const std::string directory=root?root:".";
    const char* names[]={"fixed_function.vert.dksh","fixed_function.frag.dksh","fixed_function.flat.frag.dksh"};
    // Standalone diagnostic only; normal game/test rendering keeps both FFP
    // stages. Compare each against a minimal stage without changing its state.
    const char* diagnostic=std::getenv("DIRT2_DEKO_FFP_DIAGNOSTIC");
    const unsigned variant=diagnostic?unsigned(std::strtoul(diagnostic,nullptr,10)):0;
    if(variant&1)names[0]="diagnostic.vert.dksh";
    if(variant&2)names[1]=names[2]="diagnostic.frag.dksh";
    for(unsigned i=0;i<3;++i){std::ifstream stream(directory+"/"+names[i],std::ios::binary);
        binaries_[i].assign(std::istreambuf_iterator<char>(stream),{});
        if(!load_shader_binary(binaries_[i],&shaders_[i])){rt::log("deko3d: cannot load fixed-function shader %s",names[i]);return false;}}
    vertex_meta_.name="fixed-function-vs";vertex_meta_.fixed_function=true;
    pixel_meta_.name="fixed-function-ps";pixel_meta_.fixed_function=true;pixel_meta_.pixel=true;pixel_meta_.color_outputs=1;
    for(unsigned i=0;i<8;++i){
        pixel_meta_.samplers.push_back({uint8_t(i),0,uint8_t(i)});
        pixel_meta_.samplers.push_back({uint8_t(i),1,uint8_t(i+8)});
        pixel_meta_.samplers.push_back({uint8_t(i),2,uint8_t(i+16)});
        pixel_meta_.samplers.push_back({uint8_t(i),0,uint8_t(i+24)});
    }
    loaded_=true;return true;
}
bool FixedFunction::prepare(DrawShader* vertex,DrawShader* pixel){
    if(!load())return false;
    if(device_->ffp_dirty_ || vertex_data_.empty()){
        build_state();device_->ffp_dirty_=false;
        device_->dirty_|=Device::kVertexInput|Device::kTextures;
    }
    if(vertex)*vertex={&vertex_meta_,&shaders_[0]};
    if(pixel)*pixel={&pixel_meta_,&shaders_[device_->rs_[D3DRS_SHADEMODE]==D3DSHADE_FLAT?2:1]};
    return true;
}
void FixedFunction::build_state(){
    const auto& d=*device_;const auto*rs=d.rs_;
    vertex_data_.assign(2048,0);pixel_data_.assign(768,0);spec_data_.assign(80,0);render_data_.assign(64,0);clip_data_.assign(96,0);
    const auto& view=d.transforms_[D3DTS_VIEW];const auto& projection=d.transforms_[D3DTS_PROJECTION];
    const auto world_view=multiply(d.transforms_[D3DTS_WORLD],view);
    put(vertex_data_,0,world_view);put(vertex_data_,64,inverse(world_view));
    const auto inverse_view=inverse(view);D3DMATRIX inverse_view_transpose{};
    for(unsigned row=0;row<4;++row)for(unsigned column=0;column<4;++column)inverse_view_transpose.m[row][column]=inverse_view.m[column][row];
    put(vertex_data_,128,inverse_view_transpose);
    put(vertex_data_,192,projection);put(vertex_data_,256,multiply(world_view,projection));
    for(unsigned i=0;i<8;++i)put(vertex_data_,320+i*64,d.transforms_[D3DTS_TEXTURE0+i]);
    const auto&vp=d.viewport_;
    const std::array<float,4> extent={2.f/std::max(vp.Width,1u),-2.f/std::max(vp.Height,1u),rs[D3DRS_ZENABLE]?1.f:0.f,1.f};
    const std::array<float,4> offset={-1.f-float(vp.X)*extent[0],1.f-float(vp.Y)*extent[1],0,0};
    put(vertex_data_,832,offset);put(vertex_data_,848,extent);
    uint32_t texcoord_mask=0;bool position_t=false,color0=false,color1=false,pointsize=false,fog=false;
    if(d.decl_)for(const auto&e:d.decl_->elements){
        position_t|=e.Usage==D3DDECLUSAGE_POSITIONT;
        color0|=e.Usage==D3DDECLUSAGE_COLOR&&e.UsageIndex==0;color1|=e.Usage==D3DDECLUSAGE_COLOR&&e.UsageIndex==1;
        pointsize|=e.Usage==D3DDECLUSAGE_PSIZE;fog|=e.Usage==D3DDECLUSAGE_FOG;
        if(e.Usage==D3DDECLUSAGE_TEXCOORD&&e.UsageIndex<8)texcoord_mask|=components(e.Type)<<(e.UsageIndex*3);
    }
    const uint8_t blend=rs[D3DRS_VERTEXBLEND]==D3DVBF_TWEENING?2:rs[D3DRS_VERTEXBLEND]!=D3DVBF_DISABLE?1:0;
    vertex_meta_.inputs={{uint8_t(position_t?D3DDECLUSAGE_POSITIONT:D3DDECLUSAGE_POSITION),0,0},{D3DDECLUSAGE_NORMAL,0,1},
        {uint8_t(blend==1?D3DDECLUSAGE_BLENDWEIGHT:D3DDECLUSAGE_POSITION),uint8_t(blend==1?0:1),2},
        {uint8_t(blend==1?D3DDECLUSAGE_BLENDINDICES:D3DDECLUSAGE_NORMAL),uint8_t(blend==1?0:1),3}};
    for(unsigned i=0;i<8;++i)vertex_meta_.inputs.push_back({D3DDECLUSAGE_TEXCOORD,uint8_t(i),uint8_t(4+i)});
    vertex_meta_.inputs.insert(vertex_meta_.inputs.end(),{{D3DDECLUSAGE_COLOR,0,12},{D3DDECLUSAGE_COLOR,1,13},{D3DDECLUSAGE_FOG,0,14},{D3DDECLUSAGE_PSIZE,0,15}});
    const bool lighting=rs[D3DRS_LIGHTING]&&!position_t;
    unsigned light_count=0;
    for(const auto&item:d.lights_)if(item.second.enabled&&light_count<8){const auto&l=item.second.value;if(l.Type<D3DLIGHT_POINT||l.Type>D3DLIGHT_DIRECTIONAL)continue;
        const size_t o=864+112*light_count++;put(vertex_data_,o,l.Diffuse);put(vertex_data_,o+16,l.Specular);put(vertex_data_,o+32,l.Ambient);
        put(vertex_data_,o+48,transform(l.Position,view,1));
        auto direction=transform(l.Direction,view,0);const float length=std::sqrt(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);
        if(length>0)for(auto&component:direction)component/=length;
        put(vertex_data_,o+64,direction);put(vertex_data_,o+80,uint32_t(l.Type));
        put(vertex_data_,o+84,l.Range);put(vertex_data_,o+88,l.Falloff);put(vertex_data_,o+92,l.Attenuation0);put(vertex_data_,o+96,l.Attenuation1);put(vertex_data_,o+100,l.Attenuation2);put(vertex_data_,o+104,std::cos(l.Theta*.5f));put(vertex_data_,o+108,std::cos(l.Phi*.5f));
    }
    put(vertex_data_,1760,d.material_);put(vertex_data_,1840,rs[D3DRS_AMBIENT]);put(vertex_data_,1844,rs[D3DRS_TWEENFACTOR]);
    uint8_t primitive[48]{};uint32_t project_mask=0;
    for(unsigned i=0;i<8;++i){primitive[i]=d.tss_[i][D3DTSS_TEXCOORDINDEX]&7;primitive[8+i]=(d.tss_[i][D3DTSS_TEXCOORDINDEX]>>16)&7;primitive[16+i]=d.tss_[i][D3DTSS_TEXTURETRANSFORMFLAGS]&7;
        if(d.tss_[i][D3DTSS_TEXTURETRANSFORMFLAGS]&D3DTTFF_PROJECTED)project_mask|=1u<<i;}
    std::memcpy(primitive+24,&texcoord_mask,4);primitive[28]=position_t;primitive[29]=color0;primitive[30]=color1;primitive[31]=pointsize;primitive[32]=fog;
    primitive[33]=blend;primitive[34]=!!rs[D3DRS_INDEXEDVERTEXBLENDENABLE];primitive[35]=rs[D3DRS_VERTEXBLEND]&255;
    primitive[36]=!!rs[D3DRS_CLIPPLANEENABLE];primitive[37]=!!rs[D3DRS_NORMALIZENORMALS];primitive[38]=lighting&&rs[D3DRS_LOCALVIEWER];primitive[39]=!!rs[D3DRS_RANGEFOGENABLE];primitive[40]=lighting;primitive[41]=lighting?light_count:0;
    const uint32_t color_mask=lighting&&rs[D3DRS_COLORVERTEX]?uint32_t(color0?D3DMCS_COLOR1:0)|uint32_t(color1?D3DMCS_COLOR2:0):0;
    primitive[42]=rs[D3DRS_DIFFUSEMATERIALSOURCE]&color_mask;primitive[43]=rs[D3DRS_AMBIENTMATERIALSOURCE]&color_mask;primitive[44]=rs[D3DRS_SPECULARMATERIALSOURCE]&color_mask;primitive[45]=rs[D3DRS_EMISSIVEMATERIALSOURCE]&color_mask;
    for(unsigned i=0;i<12;++i){uint32_t word;std::memcpy(&word,primitive+4*i,4);put(vertex_data_,1856+i*16,word);}
    unsigned planes=0;for(unsigned i=0;i<6;++i)if(rs[D3DRS_CLIPPLANEENABLE]&(1u<<i))std::memcpy(clip_data_.data()+16*planes++,d.clip_planes_[i],16);
    const uint32_t compare=rs[D3DRS_ALPHATESTENABLE]&&!d.alpha_to_coverage()?std::clamp<DWORD>(rs[D3DRS_ALPHAFUNC],1,8)-1:7;
    put(spec_data_,0,compare|(8u<<4)|(uint32_t(bool(d.point_list_ && rs[D3DRS_POINTSPRITEENABLE]))<<8)|(planes<<16));
    const bool use_z=!d.vs_&&!position_t&&projection.m[0][3]==0&&projection.m[1][3]==0&&projection.m[2][3]==0&&projection.m[3][3]==1;
    put(spec_data_,4,uint32_t(!!rs[D3DRS_FOGENABLE])|((rs[D3DRS_FOGVERTEXMODE]&3)<<8)|((rs[D3DRS_FOGTABLEMODE]&3)<<16)|(uint32_t(use_z)<<24));
    put(spec_data_,8,project_mask|(uint32_t(!!rs[D3DRS_SPECULARENABLE])<<8)|(uint32_t(bool(d.point_list_ && rs[D3DRS_POINTSCALEENABLE]))<<16)|(uint32_t(d.ps_ && d.ps_->major_version() >= 3)<<24));
    unsigned active=0;
    for(;active<8;++active){const auto*t=d.tss_[active];const auto op=t[D3DTSS_COLOROP];if(op==D3DTOP_DISABLE)break;
        bool missing=false;const auto mask=argument_mask(op);
        if(!d.textures_[active])for(unsigned j=0;j<3;++j)if((mask&(1u<<j))&&((t[j==0?D3DTSS_COLORARG0:j==1?D3DTSS_COLORARG1:D3DTSS_COLORARG2]&D3DTA_SELECTMASK)==D3DTA_TEXTURE))missing=true;
        if(missing)break;
    }
    uint32_t sampler_types=0,sampler_modes=0,ops[4]{};bool disabled=false;
    for(unsigned i=0;i<8;++i){const DWORD*t=d.tss_[i];auto*texture=texture_core(d.textures_[i]);uint32_t type=texture?(texture->img.type==DkImageType_Cubemap?2:texture->img.type==DkImageType_3D?1:0):3;sampler_types|=type<<(2*i);
        if(texture && texture->fmt->depth && texture->d3dfmt!=D3DFORMAT(0x5a544e49) && texture->d3dfmt!=D3DFORMAT(0x36314644) && texture->d3dfmt!=D3DFORMAT(0x34324644))sampler_modes|=2u<<(2*i);
        DWORD color=t[D3DTSS_COLOROP],alpha=t[D3DTSS_ALPHAOP];const auto mask=argument_mask(color);
        if(!texture)for(unsigned j=0;j<3;++j)if((mask&(1u<<j))&&((t[j==0?D3DTSS_COLORARG0:j==1?D3DTSS_COLORARG1:D3DTSS_COLORARG2]&D3DTA_SELECTMASK)==D3DTA_TEXTURE))disabled=true;
        disabled|=i>=active || color==D3DTOP_DISABLE;if(disabled)color=alpha=D3DTOP_DISABLE;
        bool last=i+1>=active;
        if(!i && t[D3DTSS_RESULTARG]==D3DTA_TEMP && color!=D3DTOP_DISABLE && alpha==D3DTOP_DISABLE)alpha=D3DTOP_SELECTARG1;
        const uint32_t op=(color&31)|((alpha&31)<<8)|uint32_t(!last&&t[D3DTSS_RESULTARG]==D3DTA_TEMP)<<15;
        ops[i/2]|=op<<(16*(i&1));
        const uint32_t args=argument(t[D3DTSS_COLORARG0])|(argument(t[D3DTSS_COLORARG1])<<5)|(argument(t[D3DTSS_COLORARG2])<<10)|(argument(t[D3DTSS_ALPHAARG0])<<16)|(argument(!i && t[D3DTSS_RESULTARG]==D3DTA_TEMP && t[D3DTSS_ALPHAOP]==D3DTOP_DISABLE?D3DTA_DIFFUSE:t[D3DTSS_ALPHAARG1])<<21)|(argument(t[D3DTSS_ALPHAARG2])<<26);
        put(spec_data_,48+4*i,args);const size_t o=i*96;put(pixel_data_,o,t[D3DTSS_CONSTANT]);put(pixel_data_,o+16,t[D3DTSS_BUMPENVMAT00]);put(pixel_data_,o+32,t[D3DTSS_BUMPENVMAT01]);put(pixel_data_,o+48,t[D3DTSS_BUMPENVMAT10]);put(pixel_data_,o+64,t[D3DTSS_BUMPENVMAT11]);put(pixel_data_,o+80,t[D3DTSS_BUMPENVLSCALE]);put(pixel_data_,o+84,t[D3DTSS_BUMPENVLOFFSET]);
    }
    put(spec_data_,28,uint32_t(rs[D3DRS_SRGBWRITEENABLE] && d.targets_[0] && d.targets_[0]->image() && srgb_format(d.targets_[0]->image()->format)==d.targets_[0]->image()->format));
    put(spec_data_,16,sampler_types);put(spec_data_,20,sampler_modes);for(unsigned i=0;i<4;++i)put(spec_data_,32+4*i,ops[i]);
    put(render_data_,0,(rs[D3DRS_FOGCOLOR]&0xffffff)|((rs[D3DRS_ALPHAREF]&255)<<24));
    const float start=floating(rs[D3DRS_FOGSTART]),end=floating(rs[D3DRS_FOGEND]);put(render_data_,4,end!=start?1.f/(end-start):std::numeric_limits<float>::max());put(render_data_,8,end);put(render_data_,12,rs[D3DRS_FOGDENSITY]);
    auto point=[](DWORD bits){return uint32_t(std::clamp(floating(bits)*8.f,0.f,65535.f));};
    put(render_data_,32,point(rs[D3DRS_POINTSIZE])<<16);put(render_data_,36,point(rs[D3DRS_POINTSIZE_MIN])|(point(rs[D3DRS_POINTSIZE_MAX])<<16));
    const float point_scale=1.f/(float(std::max(vp.Height,1u))*float(std::max(vp.Height,1u)));
    put(render_data_,48,point_scale*floating(rs[D3DRS_POINTSCALE_A]));put(render_data_,52,point_scale*floating(rs[D3DRS_POINTSCALE_B]));put(render_data_,56,point_scale*floating(rs[D3DRS_POINTSCALE_C]));
    if(blend==1){blend_data_.resize(256*64);for(unsigned i=0;i<256;++i)put(blend_data_,64*i,multiply(d.transforms_[256+i],view));}
    // The generic VS declares all 256 matrices even when blending is disabled.
    // Keep the bound range valid for the entire declared uniform block.
    else blend_data_.assign(256*64,0);
}
void FixedFunction::bind(unsigned stage,unsigned binding,const std::vector<uint8_t>& data){
    auto&buffer=buffers_[stage][binding];if(!buffer || buffer.size<data.size()){
        if(buffer)gpu().defer([old=buffer]{gpu().buffers.free_now(old);});buffer=gpu().buffers.alloc(std::max<size_t>(data.size(),256),256);cached_[stage][binding].clear();
    }
    if(!buffer)return;
    if(cached_[stage][binding]!=data){dkCmdBufPushConstants(gpu().cmd,buffer.gpu(),buffer.size,0,data.size(),data.data());cached_[stage][binding]=data;}
    dkCmdBufBindUniformBuffer(gpu().cmd,stage?DkStage_Fragment:DkStage_Vertex,binding,buffer.gpu(),buffer.size);
}
void FixedFunction::bind_constants(bool vertex,bool pixel){
    if(vertex){bind(0,0,vertex_data_);bind(0,1,spec_data_);bind(0,2,clip_data_);bind(0,3,blend_data_);bind(0,4,render_data_);}
    if(pixel){bind(1,0,pixel_data_);bind(1,1,spec_data_);auto render=render_data_;render.resize(48);put(render,32,device_->rs_[D3DRS_TEXTUREFACTOR]);bind(1,4,render);}
}
} // namespace deko
