#!/usr/bin/env python3
"""Attribute Switch profiler PC/LR snapshots with the exact build's nm -n file."""
import argparse
import bisect
import collections
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log', type=Path)
parser.add_argument('symbols', type=Path, help='Matching ELF nm -n output; never use another build')
args = parser.parse_args()
symbols = []
main = None
for line in args.symbols.read_text().splitlines():
    m = re.match(r'^([0-9a-fA-F]+)\s+([tTwW])\s+(.+)$', line)
    if m:
        address, name = int(m[1], 16), m[3]
        symbols.append((address, name))
        if name == 'main':
            main = address
if main is None:
    parser.error('No main symbol in nm file')
symbols.sort()
addresses = [address for address, _ in symbols]
counts = [collections.Counter(), collections.Counter()]
samples = 0
for line in args.log.read_text(errors='replace').splitlines():
    m = re.search(r'prof-pc: tid (\d+) pc main([+-]\d+) lr main([+-]\d+) samples (\d+)/(\d+)', line)
    if not m:
        continue
    count = int(m[4])
    samples += count
    for index in range(2):
        address = main + int(m[index + 2])
        pos = bisect.bisect_right(addresses, address) - 1
        name = symbols[pos][1] if pos >= 0 else '<outside text>'
        counts[index][name] += count
print(json.dumps({
    'samples': samples,
    'warning': 'Wall snapshots, not CPU-weighted. Nearest text symbol; inspect disassembly for boundaries. Use only matching build symbols.',
    'pc_functions': counts[0].most_common(40),
    'lr_functions': counts[1].most_common(40),
}, indent=2))
