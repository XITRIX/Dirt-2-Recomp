#!/usr/bin/env python3
"""Prepare pinned Wine D3D9 C sources for a native runner; never edit upstream."""
import argparse, hashlib, json, re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
UPSTREAM=ROOT/'tests/d3d9_wine/upstream'

def mask(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: ''.join('\n' if c=='\n' else ' ' for c in m[0]),source,flags=re.S)

def functions(source):
    clean=mask(source); result={}
    pattern=r'(?m)^(?:(?:static|inline|const|unsigned|signed|struct|WINAPI|CALLBACK)\s+|[A-Za-z_]\w*[ \t*]+)+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{'
    for m in re.finditer(pattern,clean):
        start=m.end()-1;depth=1;end=start+1
        while depth:
            if clean[end]=='{':depth+=1
            if clean[end]=='}':depth-=1
            end+=1
        result[m[1]]=(m.start(),start,end,clean[start:end])
    return result

SAFE={'AdjustWindowRect','CreateWindowA','DestroyWindow','Direct3DCreate9','SetRect','PtInRect',
      'GetDesktopWindow','GetDC','ZeroMemory','EqualRect','QueryPerformanceCounter','QueryPerformanceFrequency','GetTickCount',
      'GetCurrentThreadId','InterlockedIncrement','Sleep','IsEqualGUID'}
EXPLICIT={'test_fpu_setup':'x86 FPU control-word behavior',
          'test_device_vtbl_initialization_assembly_hook':'Windows/x86 assembly hook',
          'init_d3d9on12_modules':'Windows D3D9On12/D3D12',
          'create_d3d9on12_device_':'Windows D3D9On12/D3D12',
          'test_d3d9on12':'Windows D3D9On12/D3D12'}

def prepare(out):
    out.mkdir(parents=True,exist_ok=True); inventory=[]
    for module in ('visual','device','stateblock','d3d9ex'):
        path=UPSTREAM/(module+'.c');s=path.read_text();clean=mask(s)
        entry=re.search(r'START_TEST\('+module+r'\)\s*\{',clean)
        calls=re.findall(r'^    ([a-z]\w*)\(\);\s*$',clean[entry.end():],re.M)
        if not calls:raise ValueError('no cases in '+module)
        fn=functions(s);blocked={}; deps={}
        for name,(_,_,_,body) in fn.items():
            deps[name]={n for n in fn if re.search(r'\b'+re.escape(n)+r'\b',body)}-{name}
            if name in EXPLICIT:blocked[name]=EXPLICIT[name];continue
            unknown=sorted({n for n in re.findall(r'\b([A-Z]\w*)\s*\(',body)
                if n not in SAFE and n not in fn and not n.isupper() and not n.startswith(('IDirect','IUnknown_','D3D','IDXGI','ID3D'))})
            if unknown:blocked[name]='Win32 platform API: '+', '.join(unknown)
        changed=True
        while changed:
            changed=False
            for name,needed in deps.items():
                if name not in blocked and needed & blocked.keys():
                    other=sorted(needed & blocked.keys())[0];blocked[name]=blocked[other];changed=True
        edits=[]
        if module=='device':
            # No fake Windows desktop/module initialization: only the dispatched
            # tests whose dependencies are supported are allowed to run.
            for inc in ('d3d9on12.h','dxgi1_4.h','psapi.h'):
                s=s.replace('#include <'+inc+'>','/* native: '+inc+' is unavailable */')
            fn=functions(s);clean=mask(s);entry=re.search(r'START_TEST\(device\)\s*\{',clean)
        if module=='d3d9ex':
            generated='#include "wine/test.h"\nSTART_TEST(d3d9ex) {}\n'
        else:
            for name,(start,brace,end,body) in fn.items():
                if name in blocked:
                    signature=s[start:brace]
                    ret='return;' if re.search(r'\bvoid\s+'+name+r'\s*\(',signature) else 'return 0;'
                    edits.append((brace,end,'{ wine_platform_skip('+json.dumps(blocked[name])+'); '+ret+' }'))
            # Preserve visual module setup; device's setup is Windows-specific.
            if module=='device':
                edits.append((entry.end(),len(s),'\n'+''.join('    wine_run_case('+json.dumps(module+'.'+n)+', '+n+');\n' for n in calls)+'}\n'))
            else:
                for m in re.finditer(r'^    ([a-z]\w*)\(\);\s*$',clean[entry.end():],re.M):
                    name=m[1];a=entry.end()+m.start();b=entry.end()+m.end()
                    edits.append((a,b,'    wine_run_case('+json.dumps(module+'.'+name)+', '+name+');\n'))
            generated=s
            for a,b,value in sorted(edits,reverse=True):
                value += '\n'*max(0,generated[a:b].count('\n')-value.count('\n'))
                generated=generated[:a]+value+generated[b:]
            generated='#line 1 '+json.dumps(str(path))+'\n'+generated
        for name in calls:
            reason='D3D9Ex is not implemented by the Switch backend' if module=='d3d9ex' else blocked.get(name,'')
            inventory.append(dict(name=module+'.'+name,module=module,function=name,platform_block=reason))
        (out/(module+'.c')).write_text(generated)
    (out/'inventory.json').write_text(json.dumps({'revision':(UPSTREAM/'revision.txt').read_text().strip(),'cases':inventory},indent=2)+'\n')
    lines=['/* Generated from upstream START_TEST calls. */','static const struct WineCase wine_cases[] = {']
    for t in inventory:lines.append(' {'+','.join(json.dumps(t[k]) for k in ('name','module','platform_block'))+'},')
    (out/'inventory.inc').write_text('\n'.join(lines+['};','']))
    print(len(inventory),'upstream cases;',sum(bool(t['platform_block']) for t in inventory),'platform-dependent')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);prepare(p.parse_args().out)
