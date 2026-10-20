#!/usr/bin/env python3
"""Compile real bytecode and check output linking, shadow reference and DSY ABI."""
from pathlib import Path
import importlib.util,json,struct,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('census',root/'tools/scripts/dx9_shader_census.py')
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
fixtures={
 'legacy': [0xffff0101,0x00000001,0x800f0000,0x90e40000,0x0000ffff],
 'default_diffuse': [0xfffe0101,0x0000001f,0x80000000,0x900f0000,0x00000001,0xc00f0000,0x90e40000,0x0000ffff],
 'shadow': [0xffff0200,0x0200001f,0x90000000,0xa00f0800,0x0200001f,0x80000000,0xb00f0000,0x03010042,0x800f0000,0xb0e40000,0xa0e40800,0x02000001,0x800f0800,0x80e40000,0x0000ffff],
 'dsy': [0xffff0300,0x0200001f,0x8000000a,0x900f0000,0x0200005c,0x800f0800,0x90e40000,0x0000ffff],
}
# Include real upstream mixed-addressing and TEXDEPTH fixtures as well as a
# ps1.4 coordinate at t5, beyond the old four-entry implicit-input array.
spec=importlib.util.spec_from_file_location('wine_shader_builder',root/'tools/scripts/build_wine_d3d9_shaders.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
blobs,_=builder.collect()
for name,origin in [('address','visual.c:mov_test')]:
 blob=next(item['blob'] for item in blobs.values() if origin in item['origins'])
 fixtures[name]=list(struct.unpack('<%dI'%(len(blob)//4),blob))
fixtures['legacy_texture']=[0xffff0101,0x00000042,0xb00f0000,0x00000001,0x800f0000,0xb0e40000,0x0000ffff]
fixtures['legacy_coord5']=[0xffff0104,0x00000040,0x800f0000,0xb0e40005,0x0000ffff]
fixtures['legacy_depth']=[0xffff0104,0x00000001,0x800f0005,0xa0e40000,0x0000fffd,0x00000057,0x800f0005,0x00000001,0x800f0000,0xa0e40001,0x0000ffff]
fixtures['coissue']=[0xffff0101,0x00000051,0xa00f0000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000001,0x80080000,0xa0ff0000,0x40000050,0x80070000,0x80ff0000,0xa0e40001,0xa0e40002,0x0000ffff]
fixtures['kill2']=[0xffff0200,0x0200001f,0x80000000,0xb00f0000,0x01000041,0xb00f0000,0x02000001,0x800f0800,0xa0e40000,0x0000ffff]
with tempfile.TemporaryDirectory(prefix='deko-shader-interface-') as directory:
 p=Path(directory);inputs=[]
 for name,words in fixtures.items():
  path=p/(name+'.d3dbc');path.write_bytes(struct.pack('<%dI'%len(words),*words));inputs.append(str(path))
 subprocess.run([c.DX9SHADER,'--out',str(p)]+inputs,check=True)
 # The translator keeps the input basename for each output.
 sources={}
 for name,path in zip(fixtures,inputs):
  sha=name;stage='vert' if name in ('default_diffuse','address') else 'frag'
  source=p/(sha+'.'+stage+'.glsl');metadata=json.loads((p/(sha+'.json')).read_text());text=source.read_text();sources[name]=text
  subprocess.run(['spirv-val',str(p/(sha+'.spv'))],check=True)
  ok,error=c.run_uam(str(source),str(p/(name+'.dksh')));assert ok,error
  if name=='legacy':
   assert metadata['outputs']==[{'usage':'COLOR','index':0}]
   assert 'layout(location = 0) out vec4 ps_r0;' in text and 'if (!pass) discard;' in text
   assert 'ps_r0 = clamp(ps_r0, 0.0, 1.0);' in text
   flat=p/(name+'.dksh.flat');assert flat.is_file()
   record=c.shader_meta(metadata,flat.read_bytes());assert struct.unpack_from('<I',record,8+4*len(metadata['inputs']))[0]==1
  if name=='default_diffuse':assert 'd3d9DefaultDiffuse = vec4(1.0);' in text
  if name=='shadow':assert 'sampler2DShadow d3d9Shadow0' in text and '.xyz' in text and '.wwww' in text
  if name=='address':assert '_m0[7]' in text and 'floor(' in text
  if name=='legacy_texture':
   assert metadata['samplers']==[{'register':0,'type':'legacy'}]
   assert 'sampler3D d3d9Volume0' in text and 'samplerCube d3d9Cube0' in text
   assert 'layout(binding = 24) uniform sampler2DShadow' in text
  if name=='legacy_coord5':assert 'layout(location = 5) in vec4' in text
  if name=='legacy_depth':
   assert {'usage':'COLOR','index':0} in metadata['outputs'] and {'usage':'DEPTH','index':0} in metadata['outputs']
   assert 'gl_FragDepth =' in text
  if name=='coissue':
   body=text.split('void d3d9_original_main()')[1].split('void main()')[0]
   assert 'mix(' not in body and 'lessThan' not in body
  if name=='kill2':assert 'lessThan(ps_PointCoordOrTexCoord0, vec4(0.0))' in text or 'lessThan(ps_t0, vec4(0.0))' in text
  if name=='dsy':
   target=p/'dsy.tgsi';subprocess.run([c.UAM,'-s','frag','-t',str(target),'-o',str(p/'dsy.dksh'),str(source)],check=True)
   tgsi=target.read_text();assert 'DDY ' in tgsi and 'CONST[0]' not in tgsi
print('Real shader pipeline: implicit output/target mask, alpha epilogue, default diffuse, flat variant, projected shadow reference and derivative without unbound driver UBO passed')
