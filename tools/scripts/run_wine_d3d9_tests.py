#!/usr/bin/env python3
"""Run every native Wine D3D9 case in a separate process, with resumable results."""
import argparse, collections, json, os, subprocess, time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('runner',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--timeout',type=float,default=90);a=p.parse_args()
    a.runner=a.runner.resolve();a.out.mkdir(parents=True,exist_ok=True);a.out=a.out.resolve()
    listing=subprocess.check_output([str(a.runner),'--list'],text=True)
    cases=[(int(line.split('\t')[0]),line.split('\t')[1]) for line in listing.splitlines()]
    report=a.out/'results.jsonl';done={}
    if report.exists():
        for line in report.read_text().splitlines():
            try:r=json.loads(line);done[r['index']]=r
            except (ValueError,KeyError):pass
    for index,name in cases:
        if index in done:continue
        start=time.monotonic();env=os.environ.copy();env.update(MVK_CONFIG_LOG_LEVEL='0',DXVK_LOG_LEVEL='error',DXVK_LOG_PATH=str(a.out))
        error=None
        with (a.out/f'{index:03d}-{name}.log').open('w') as log:
            try:
                child=subprocess.run([str(a.runner),'--case',str(index),str(report)],stdout=log,stderr=subprocess.STDOUT,env=env,timeout=a.timeout)
                if child.returncode not in (0,1):error={'status':'crash','returncode':child.returncode}
            except subprocess.TimeoutExpired:error={'status':'timeout','seconds':a.timeout}
        if error:
            error.update(index=index,name=name)
            with report.open('a') as f:f.write('\n'+json.dumps(error)+'\n')
        latest=None
        for line in report.read_text().splitlines() if report.exists() else []:
            try:
                r=json.loads(line)
                if r['index']==index:latest=r
            except (ValueError,KeyError):pass
        if not latest:
            latest=dict(index=index,name=name,status='runner-error',message='child produced no report')
            with report.open('a') as f:f.write(json.dumps(latest)+'\n')
        done[index]=latest
        counts=collections.Counter(r['status'] for r in done.values())
        (a.out/'summary.json').write_text(json.dumps({'total':len(cases),'finished':len(done),'counts':dict(counts),'cases':[done[i] for i in sorted(done)]},indent=2)+'\n')
        if latest['status']!='pass' or index%10==0:print(f'{index+1}/{len(cases)} {name}: {latest["status"]} ({time.monotonic()-start:.1f}s)',flush=True)
    print('Completed:',dict(collections.Counter(r['status'] for r in done.values())),flush=True)

if __name__=='__main__':main()
