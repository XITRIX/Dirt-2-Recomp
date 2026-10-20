#!/usr/bin/env python3
"""Adapt DXVK's fixed-function algorithms to deko's combined samplers/std140 ABI.

Original sources and zlib license are retained in third_party/deko_ffp. Generated
files are explicitly marked altered; no Wine test or expected image is an input.
"""
from pathlib import Path
import argparse,subprocess,re,hashlib,json
ROOT=Path(__file__).resolve().parents[2]
def common():
    s=(ROOT/'third_party/deko_ffp/d3d9_fixed_function_common.glsl').read_text()
    s=re.sub(r'^spirv_instruction[^\n]+\n','',s,flags=re.M)
    s='#define spvNMin min\n#define spvNMax max\n#define spvNClamp clamp\n'+s
    s=s.replace('layout(set = SPEC_DATA_SET, binding = 0, scalar)','layout(std140,binding=1)')
    return s

def adapt(stage):
    name='d3d9_fixed_function_vert.vert' if stage=='vert' else 'd3d9_fixed_function_frag.glsl'
    s=(ROOT/'third_party/deko_ffp'/name).read_text()
    s=re.sub(r'^#(?:version|extension)[^\n]*\n','',s,flags=re.M)
    s=s.replace('#include "d3d9_fixed_function_common.glsl"',common())
    s=s.replace('[[unroll]]','').replace('demote;','discard;')
    s=re.sub(r'precise (float|vec[234]) (\w+\()',r'\1 \2',s)
    s=s.replace('layout(push_constant, scalar, row_major)','layout(std140,binding=4,row_major)')
    # Link FFP and programmable stages by the same D3D semantic locations.
    remap={0:20,**{n+1:n for n in range(8)},9:16,10:17,11:24}
    direction='out' if stage=='vert' else 'in'
    s=re.sub(r'layout\(location = (\d+)\)(\s+(?:INTERP_MODE\s+)?'+direction+r'\s+)',lambda m:f'layout(location = {remap.get(int(m[1]),int(m[1]))})'+m[2],s)
    if stage=='vert':
        # Tween inputs and matrix-blend weights/indices are mutually exclusive.
        # Share those locations to stay within UAM's 16-input GL frontend.
        s=s.replace('layout(location = 16) in vec4 in_BlendWeight;', '#define in_BlendWeight in_Position1').replace('layout(location = 17) in vec4 in_BlendIndices;', '#define in_BlendIndices in_Normal1')
        s=s.replace('layout(set = CBV_SET, binding = CBV_VS_FIXED_FUNCTION, scalar, row_major)','layout(std140,binding=0,row_major)')
        s=s.replace('layout(set = CBV_SET, binding = CBV_VS_VERTEX_BLEND, std140, row_major)\nreadonly buffer','layout(std140,binding=3,row_major)\nuniform')
        s=s.replace('mat4 WorldViewArray[]','mat4 WorldViewArray[256]')
        s=s.replace('layout(set = CBV_SET, binding = CBV_VS_CLIP_PLANES, std140)','layout(std140,binding=2)')
    else:
        s=s.replace('layout(set = CBV_SET, binding = CBV_PS_SHARED, scalar, row_major)','layout(std140,binding=0,row_major)')
        start=s.index('layout(set = SRV_SET, binding = SRV_PS_BASE)');end=s.index('vec3 calculateFog(',start)
        samplers=''
        for typ,base in [('2D',0),('Cube',8),('3D',16),('2DShadow',24)]:
            for i in range(8):samplers+=f'layout(binding={base+i}) uniform sampler{typ} ff_{typ}_{i};\n'
            coord='vec2' if typ=='2D' else 'vec3'
            samplers+=f'vec4 ff_sample_{typ}(uint stage,{coord} coord){{switch(stage){{\n'
            for i in range(8):samplers+=f'case {i}u:return '+('vec4(' if typ.endswith('Shadow') else '')+f'texture(ff_{typ}_{i},coord)'+(')' if typ.endswith('Shadow') else '')+';\n'
            samplers+='}return vec4(0.0);}\n'
        s=s[:start]+samplers+s[end:]
        start=s.index('uint loadSamplerHeapIndex(');end=s.index('float adjustDref(',start);s=s[:start]+s[end:]
        for typ,coords in [('2D','xy'),('Cube','xyz'),('3D','xyz')]:
            s=s.replace(f'texture(sampler{typ}(t'+{'2D':'2d','Cube':'cube','3D':'3d'}[typ]+'[stage], sampler_heap[loadSamplerHeapIndex(stage)]), texcoord.'+coords+')',f'ff_sample_{typ}(stage,texcoord.{coords})')
        s=s.replace('texture(sampler2DShadow(t2d[stage], sampler_heap[loadSamplerHeapIndex(stage)]), texcoord.xyz).xxxx','ff_sample_2DShadow(stage,texcoord.xyz)')
        s=s.replace('texture(samplerCubeShadow(tcube[stage], sampler_heap[loadSamplerHeapIndex(stage)]), texcoord).xxxx','vec4(float(ff_sample_Cube(stage,texcoord.xyz).r >= texcoord.w))')
        s=s.replace('    uint packedSamplerIndices[TextureStageCount / 2u];','')
        s=s.replace('    out_Color0 = state.current;', '''    out_Color0 = state.current;
    if (unused_spec7 != 0u) {
        vec3 c = clamp(out_Color0.rgb, 0.0, 1.0);
        out_Color0.rgb = mix(1.055 * pow(c, vec3(1.0/2.4)) - 0.055, 12.92 * c, lessThanEqual(c, vec3(0.0031308)));
    }''')
    return '#version 460\n// Altered DXVK fixed-function shader for deko3d; see third_party/deko_ffp/LICENSE.\n'+s

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,default=ROOT/'build/shaders/deko-ffp');a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    manifest={}
    for stage in ['vert','frag']:
        source=a.out/('fixed_function.'+stage+'.glsl');source.write_text(adapt(stage))
        target=a.out/('fixed_function.'+stage+'.dksh')
        subprocess.run([str(ROOT/'build/tools/uam/uam'),'-s',stage,'-o',str(target),str(source)],check=True)
        manifest[stage]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'bytes':target.stat().st_size}
    flat=a.out/'fixed_function.flat.frag.glsl'
    flat.write_text(adapt('frag').replace('INTERP_MODE in vec4 in_Color','flat in vec4 in_Color'))
    subprocess.run([str(ROOT/'build/tools/uam/uam'),'-s','frag','-o',str(a.out/'fixed_function.flat.frag.dksh'),str(flat)],check=True)
    utility = {
        'diagnostic.vert': '#version 460\nlayout(location=0) in vec4 position;layout(location=4) in vec4 texcoord;layout(location=0) out vec4 uv;layout(location=16) out vec4 color;void main(){gl_Position=position;uv=texcoord;color=vec4(1.0);}\n',
        'diagnostic.frag': '#version 460\nlayout(location=0) out vec4 color;void main(){color=vec4(0.0,1.0,0.0,1.0);}\n',
        'fullscreen.vert': '#version 460\nvoid main(){ vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0); }\n',
        'depth_resolve.frag': '#version 460\nlayout(binding=0) uniform sampler2DMS sourceDepth;layout(std140,binding=0) uniform Params{vec4 scaleOffset;};void main(){ivec2 p=ivec2(floor(gl_FragCoord.xy*scaleOffset.xy+scaleOffset.zw));gl_FragDepth=texelFetch(sourceDepth,p,0).r;}\n',
        'depth_copy.frag': '#version 460\nlayout(binding=0) uniform sampler2D sourceDepth;layout(std140,binding=0) uniform Params{vec4 scaleOffset;};void main(){ivec2 p=ivec2(floor(gl_FragCoord.xy*scaleOffset.xy+scaleOffset.zw));gl_FragDepth=texelFetch(sourceDepth,p,0).r;}\n',
    }
    for name,code in utility.items():
        source=a.out/(name+'.glsl');source.write_text(code)
        subprocess.run([str(ROOT/'build/tools/uam/uam'),'-s',name.rsplit('.',1)[1],'-o',str(a.out/(name+'.dksh')),str(source)],check=True)
    (a.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
