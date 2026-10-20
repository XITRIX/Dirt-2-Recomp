#!/usr/bin/env python3
"""Summarize a resumed Wine run and compare first failures with DXVK."""
import argparse,collections,json,re,struct,subprocess
from pathlib import Path

def read(path):
    result={};invalid=0
    for line in path.read_text().splitlines():
        if not line.strip():continue
        try:r=json.loads(line);result[r['index']]=r
        except (ValueError,KeyError):invalid+=1
    return result,invalid

def faults(directory,inventory):
    """Import raw fault evidence even before the next console launch."""
    rows={}
    for path in sorted(directory.glob('fault-*.bin')):
        data=path.read_bytes()
        if len(data)!=336:continue
        magic,version,index,phase,main,pc,lr,far,sp,esr,error,build=struct.unpack_from('<4I5Q2I32s',data)
        if magic!=0x57435231 or version!=1 or index>=len(inventory['cases']):continue
        signed=lambda x:(x+(1<<63))%(1<<64)-(1<<63)
        rows[index]=dict(index=index,name=inventory['cases'][index]['name'],status='timeout' if error==0xffffffff else 'crash',
            phase='cleanup' if phase==2 else 'test',build_id=build.hex(),exception=error,esr=esr,
            pc_main_offset=signed(pc-main),lr_main_offset=signed(lr-main),fault_address=hex(far),
            message='native CPU fault' if error!=0xffffffff else '90 second watchdog expired',fault_record=path.name)
    return rows

def symbolize(row,symbols):
    if row.get('status')!='crash' or 'pc_main_offset' not in row or not symbols:return
    build=row.get('build_id','').lower()
    choices=[p for p in symbols.glob('*.elf') if build.startswith(p.stem.lower())]
    if len(choices)!=1:return
    elf=choices[0];prefix='/opt/devkitpro/devkitA64/bin/aarch64-none-elf-'
    nm=subprocess.check_output([prefix+'nm','-n',str(elf)],text=True)
    main=next((int(line.split()[0],16) for line in nm.splitlines() if line.endswith(' T main')),None)
    if main is None:return
    for key in ('pc','lr'):
        address=main+row[key+'_main_offset']
        if address<0 or address>=1<<64:continue
        row[key+'_symbol']=subprocess.check_output([prefix+'addr2line','-Cfipe',str(elf),hex(address)],text=True).strip()

def signature(row):
    message=row.get('message','').strip()
    if row['status'] in ('crash','interrupted','timeout'):
        return row['status']+': '+row.get('pc_symbol',message)+' ['+row.get('phase','unknown phase')+']'
    if re.search(r'refcount|references? left',message,re.I):return 'reference counts / resource lifetime'
    # Similar signatures guide batching; they do not prove a common root cause.
    return re.sub(r'\b(?:0x[0-9a-f]+|[0-9]+)\b','#',message,flags=re.I)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('inventory',type=Path);p.add_argument('results',type=Path);p.add_argument('--reference',type=Path);p.add_argument('--symbols',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    inventory=json.loads(a.inventory.read_text());got,invalid=read(a.results);ref,_=read(a.reference) if a.reference else ({},0);rows=[]
    for index,fault in faults(a.results.parent,inventory).items():
        previous=got.get(index)
        if previous and previous.get('status') not in ('crash','timeout','interrupted'):fault['test_result_before_crash']=previous
        got[index]=fault
    for i,test in enumerate(inventory['cases']):
        row=dict(got.get(i,dict(index=i,name=test['name'],status='not-run')));baseline=ref.get(i)
        if row['name']!=test['name']:raise ValueError('inventory/result mismatch at index '+str(i))
        symbolize(row,a.symbols)
        if baseline:
            current=row['status'];previous=baseline['status'];row['reference_status']=previous
            if current==previous=='pass':category='pass-both'
            elif current=='fail' and previous=='pass':category='switch-only-failure'
            elif current==previous=='fail':category='same-first-failure' if row.get('line')==baseline.get('line') else 'different-first-failure'
            elif current=='pass' and previous=='fail':category='reference-only-failure'
            elif current in ('crash','interrupted','timeout'):category='candidate-interrupted'
            elif current==previous:category='matching-'+current
            else:category='different-outcome'
            row['comparison']=category
        rows.append(row)
    groups={}
    for row in rows:
        if row['status'] not in ('fail','crash','interrupted','timeout'):continue
        key=signature(row)
        group=groups.setdefault(key,dict(signature=key,cases=[],switch_only=0))
        group['cases'].append(row['index'])
        group['switch_only']+=row.get('reference_status')=='pass'
    report={'revision':inventory['revision'],'total':len(rows),'finished':len(got),'invalid_partial_lines':invalid,
            'counts':dict(collections.Counter(r['status'] for r in rows)),
            'comparison_counts':dict(collections.Counter(r.get('comparison','no-reference') for r in rows)),'failure_signature_groups':sorted(groups.values(),key=lambda g:(-g['switch_only'],-len(g['cases']))),'cases':rows}
    targets=[row for row in rows if row.get('reference_status')=='pass']
    report['mac_passing_targets']={'total':len(targets),'passing':sum(row['status']=='pass' for row in targets),
        'counts':dict(collections.Counter(row['status'] for row in targets)),
        'remaining':[row['index'] for row in targets if row['status']!='pass']}
    a.out.write_text(json.dumps(report,indent=2)+'\n');print(report['counts']);print(report['comparison_counts'])

if __name__=='__main__':main()
