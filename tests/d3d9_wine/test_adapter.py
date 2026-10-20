#!/usr/bin/env python3
"""Check original-case preservation and reporting without a GPU."""
import importlib.util,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('prepare',ROOT/'tools/scripts/prepare_wine_d3d9_tests.py');p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
with tempfile.TemporaryDirectory(prefix='wine-d3d9-adapter-') as directory:
    out=Path(directory);p.prepare(out)
    inventory=json.loads((out/'inventory.json').read_text());verified=0
    for module in ('visual','device','stateblock'):
        old=(p.UPSTREAM/(module+'.c')).read_text();new=(out/(module+'.c')).read_text();of=p.functions(old);nf=p.functions(new)
        for case in inventory['cases']:
            if case['module']!=module or case['platform_block']:continue
            name=case['function'];a=of[name];b=nf[name]
            assert old[a[1]:a[2]]==new[b[1]:b[2]],name
            verified+=1
    here=ROOT/'tests/d3d9_wine';native=ROOT/'build/src/dxvk/include/native'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-w','-include',str(here/'compat/platform_compat.h'),
        '-I'+str(here/'compat'),'-I'+str(here),'-I'+str(out),'-I'+str(native/'directx'),'-I'+str(native/'windows'),
        str(here/'runner.c'),str(here/'adapter_test.c'),'-o',str(out/'test')],check=True)
    expected={'pass':('pass',2,0,0),'fail':('fail',1,1,0),'todo':('fail',1,1,1),'skip':('partial-skip',2,0,0)}
    for mode,values in expected.items():
        report=out/(mode+'.json');subprocess.run([str(out/'test'),mode,str(report)],check=True,stderr=subprocess.DEVNULL)
        r=json.loads(report.read_text());assert (r['status'],r['passed_assertions'],r['failed_assertions'],r['todo_failures'])==values,r
    print(f'{verified} runnable upstream case bodies preserved exactly; reporting/fail-fast/TODO/skip fixtures passed')
