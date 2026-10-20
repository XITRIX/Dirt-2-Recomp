#!/usr/bin/env python3
"""Build a read-only, deduplicated DXBC bundle for native Switch startup warm-up."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, struct, collections
ROOT=Path(__file__).resolve().parents[2]
def module(name):
 spec=importlib.util.spec_from_file_location(name,ROOT/'tools/scripts'/(name+'.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--input',type=Path,default=ROOT/'build/dxbc');ap.add_argument('--output',type=Path,default=ROOT/'build/shaders/dx11/dirt2.dx11-shaders');args=ap.parse_args()
 extract=module('extract_dxbc');immediate=module('dxbc_immediate');unique={};sources=0
 def add(blob,source):
  nonlocal sources
  info=extract.parse_shader(blob);stage=info['stage']
  # The bytecode header, rather than the filename, is authoritative.
  chunks=extract.chunks(blob);code=chunks.get('SHEX') or chunks.get('SHDR');token=struct.unpack_from('<I',code)[0];stage_id=token>>16
  if stage_id not in extract.STAGES:raise ValueError('Unsupported shader stage')
  digest=hashlib.sha256(blob).hexdigest();sources+=1
  unique.setdefault(digest,{'stage':stage_id,'blob':blob,'source':source})
 for p in sorted(args.input.rglob('*.dxbc')):add(p.read_bytes(),str(p.relative_to(args.input)))
 if not unique:raise SystemExit('No DXBC blobs found; run extract_dxbc.py first')
 for name,(vertex,ins,outs,glob,tex,body) in immediate.SHADERS.items():
  blob=immediate.container([(b'RDEF',immediate.rdef(vertex,glob,tex)),(b'ISGN',immediate.signature(ins)),(b'OSGN',immediate.signature(outs)),(b'SHEX',immediate.shex(vertex,body))]);add(blob,'immediate/'+name)
 data=bytearray(struct.pack('<4sII',b'D2SP',1,len(unique)));inventory=[]
 for digest,item in sorted(unique.items()):
  blob=item['blob'];data+=struct.pack('<II',item['stage'],len(blob))+blob
  inventory.append({'sha256':digest,'stage':extract.STAGES[item['stage']],'bytes':len(blob),'source':item['source']})
 args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_bytes(data)
 manifest={'format':1,'source_blobs':sources,'unique_shaders':len(unique),'stages':dict(collections.Counter(x['stage'] for x in inventory)),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'contents':'original DX11 bytecode; compiled into native NVK shader libraries at startup','shaders':inventory}
 args.output.with_suffix('.json').write_text(json.dumps(manifest,indent=2));print(json.dumps({k:v for k,v in manifest.items() if k!='shaders'},indent=2))
if __name__=='__main__':main()
