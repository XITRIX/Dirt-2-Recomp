#!/usr/bin/env python3
"""Validate the actual generated Switch XML and the shipped parallel task graph."""
import pathlib,subprocess,sys,xml.etree.ElementTree as ET
xml=ET.fromstring(subprocess.check_output([sys.argv[1]],text=True))
threads={t.attrib['name']:t for t in xml.findall('workerThread')}
assert len(threads)==len(xml.findall('workerThread'))==5
assert all(int(t.attrib['core'])<3 for t in threads.values())
groups={s.attrib['name']:{w.attrib['name'] for w in s.findall('worker')} for s in xml.findall('workerSet')}
assert len(groups)==len(xml.findall('workerSet'))
assert all(workers and workers<=threads.keys() for workers in groups.values())
assert groups['solver'].isdisjoint(groups['softbody'])
assert len(groups['vehicleSingleThreaded'])==3
reference=ET.parse(sys.argv[2]).getroot()
roles={'core_0':'switch_update',**{f'core_{i}':'switch_solver' for i in range(1,5)},
       **{f'core_{i}':'switch_softbody' for i in range(5,8)},
       **{f'gc_{i:02}':f'switch_gc_{1+(i-1)%2}' for i in range(1,9)}}
expected={s.attrib['name']:{roles[w.attrib['name']] for w in s.findall('worker')} for s in reference.findall('workerSet')}
assert groups==expected,(groups.keys(),expected.keys())
print('PASS: valid XML,5 workers,exact mapped8-core task groups,disjoint solver/softbody')
