#!/usr/bin/env python3
"""Translate literal Wine test shader fixtures through the normal Switch pipeline."""
import argparse, concurrent.futures, hashlib, importlib.util, json, re, struct, subprocess, shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('census',ROOT/'tools/scripts/dx9_shader_census.py');c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
spec=importlib.util.spec_from_file_location('prepare',ROOT/'tools/scripts/prepare_wine_d3d9_tests.py');p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)

def collect():
    blobs={};unhandled=[]
    for source in sorted(p.UPSTREAM.glob('*.c')):
        text=source.read_text();clean=re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)
        for m in re.finditer(r'\bDWORD\s+(\w+)\s*(?:\[[^\]]*\])+\s*=\s*\{',clean):
            begin=m.end();depth=1;end=begin
            while depth:
                depth+=(clean[end]=='{')-(clean[end]=='}');end+=1
            body=clean[begin:end-1]
            numeric=r'\b(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b'
            residue=re.sub(numeric,'',body)
            if re.sub(r'[\s,{}]','',residue):
                unhandled.append(source.name+':'+m[1]);continue
            words=[int(re.sub(r'[uUlL]+$','',n),0) if not n.startswith('0') or n.lower().startswith('0x') else int(re.sub(r'[uUlL]+$','',n),10) for n in re.findall(numeric,body)]
            # Fixtures may be arrays of shaders. END bounds each candidate;
            # translation diagnoses deliberately invalid bytecode as well.
            for i,w in enumerate(words):
                if w>>16 not in (0xfffe,0xffff) or not 1<=(w>>8)&255<=3:continue
                try:j=words.index(0xffff,i+1)+1
                except ValueError:continue
                blob=struct.pack('<%dI'%(j-i),*words[i:j]);sha=hashlib.sha1(blob).hexdigest()
                entry=blobs.setdefault(sha,{'blob':blob,'origins':[]});entry['origins'].append(source.name+':'+m[1])
    # The NRM case assembles each bytecode from literal header/body/footer
    # arrays at runtime. Collect every body, including those after the first
    # assertion, rather than waiting for another missing-shader capture.
    text=(p.UPSTREAM/'visual.c').read_text()
    function=text[text.index('static void test_nrm_instruction(void)'):text.index('static void test_desktop_window(void)')]
    arrays={}
    for m in re.finditer(r'static const DWORD (\w+)\[\]\s*=\s*\{(.*?)\};',function,re.S):
        clean=re.sub(r'/\*.*?\*/','',m[2],flags=re.S)
        arrays[m[1]]=[int(word,16) for word in re.findall(r'0x([0-9a-fA-F]+)',clean)]
    for name in dict.fromkeys(re.findall(r'\{"[^"\n]+",\s*(nrm_\w+),\s*ARRAY_SIZE',function)):
        words=arrays['ps_header']+arrays[name]+arrays['ps_footer']
        blob=struct.pack('<%dI'%len(words),*words)
        entry=blobs.setdefault(hashlib.sha1(blob).hexdigest(),{'blob':blob,'origins':[]})
        entry['origins'].append('visual.c:test_nrm_instruction:'+name)
    return blobs,unhandled

def main():
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('--out',type=Path,default=ROOT/'build/shaders/wine-d3d9');a.add_argument('--jobs',type=int,default=6);a.add_argument('--extra-shaders',type=Path,help='Directory of captured runtime .d3dbc shaders');args=a.parse_args();out=args.out
    for d in ('src','glsl','dksh'):(out/d).mkdir(parents=True,exist_ok=True)
    blobs,unhandled=collect();srcs=[]
    if args.extra_shaders:
        for path in sorted(args.extra_shaders.glob('*.d3dbc')):
            blob=path.read_bytes()
            if len(blob)<8 or len(blob)%4 or struct.unpack_from('<I',blob)[0]>>16 not in (0xfffe,0xffff):
                raise ValueError('Invalid captured shader: '+str(path))
            entry=blobs.setdefault(hashlib.sha1(blob).hexdigest(),{'blob':blob,'origins':[]})
            entry['origins'].append('runtime:'+path.name)
    for sha,entry in blobs.items():
        # A previous successful output must never hide a new compiler failure.
        for suffix in ('.vert.glsl','.frag.glsl','.err','.json','.spv'):
            (out/'glsl'/(sha+suffix)).unlink(missing_ok=True)
        (out/'dksh'/(sha+'.dksh')).unlink(missing_ok=True)
        (out/'dksh'/(sha+'.dksh.flat')).unlink(missing_ok=True)
        path=out/'src'/(sha+'.d3dbc');path.write_bytes(entry['blob']);srcs.append(str(path))
    print('Literal shader candidates:',len(blobs),'nonliteral arrays:',len(unhandled),flush=True)
    translated=subprocess.run([c.DX9SHADER,'--out',str(out/'glsl')]+srcs,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if translated.returncode<0:raise RuntimeError("Shader translator crashed: "+str(translated.returncode))
    validator=shutil.which("spirv-val")
    results={};jobs=[]
    for sha,entry in blobs.items():
        stage='vert' if entry['blob'][2]==0xfe else 'frag';source=out/'glsl'/(sha+'.'+stage+'.glsl');err=out/'glsl'/(sha+'.err')
        if not source.exists():results[sha]={'status':'translation-failed','error':err.read_text() if err.exists() else translated.stderr.decode(errors='replace')[-2000:]};continue
        if validator:
            checked=subprocess.run([validator,str(out/'glsl'/(sha+'.spv'))],capture_output=True,text=True)
            if checked.returncode:
                results[sha]={'status':'spirv-invalid','error':checked.stdout+checked.stderr};continue
        jobs.append((sha,source,out/'dksh'/(sha+'.dksh')))
    def compile_one(job):
        sha,source,target=job;ok,error=c.run_uam(str(source),str(target));return sha,ok,error
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for sha,ok,error in pool.map(compile_one,jobs):results[sha]={'status':'compiled' if ok else 'uam-failed','error':error}
    entries=[]
    for sha,r in results.items():
        r['origins']=blobs[sha]['origins']
        if r['status']!='compiled':continue
        try:
            flat_path=out/'dksh'/(sha+'.dksh.flat')
            meta=c.shader_meta(json.loads((out/'glsl'/(sha+'.json')).read_text()), flat_path.read_bytes() if flat_path.exists() else b'')
            entries.append((hashlib.sha256(blobs[sha]['blob']).digest(),(out/'dksh'/(sha+'.dksh')).read_bytes(),meta))
        except Exception as e:r.update(status='metadata-failed',error=str(e))
    c.write_pack(str(out/'dirt2.dkshpack'),entries)
    (out/'report.json').write_text(json.dumps({'candidates':len(blobs),'compiled':len(entries),'spirv_validated':bool(validator),'nonliteral_arrays':unhandled,'shaders':results},indent=2)+'\n')
    print('Packed',len(entries),'shaders;',len(blobs)-len(entries),'candidates rejected (includes invalid-shader fixtures)',flush=True)

if __name__=='__main__':main()
