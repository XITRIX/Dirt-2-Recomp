#!/usr/bin/env python3
"""Keep Mesa's bundled C11 thread ABI private when linking native libsysbase users."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[2]
source=root/'build/nvk-sdk/root/opt/devkitpro/portlibs/switch/lib/libvulkan.a'
out=root/'build/nvk-sdk/libvulkan_dirt2.a'
tools=Path('/opt/devkitpro/devkitA64/bin')
raw=subprocess.check_output([str(tools/'aarch64-none-elf-nm'),'-A','--defined-only',str(source)],text=True)
names=[]
for line in raw.splitlines():
 if ':threads_posix.c.o:' in line:
  fields=line.split()
  if len(fields)>=3 and fields[-2] in ('T','W'):
   names.append(fields[-1])
assert len(names)==25 and all(n=='call_once' or n.startswith(('cnd_','mtx_','thrd_','tss_')) for n in names),names
# The SDK also exports its own libc compatibility stub; keep it private too.
assert ' T __xpg_strerror_r' in raw
names.append('__xpg_strerror_r')
mapping=out.with_suffix('.symbols')
mapping.write_text(''.join(f'{n} dirt2_mesa_{n}\n' for n in sorted(names)))
digest=hashlib.sha256(source.read_bytes()+mapping.read_bytes()).hexdigest()
lock=out.with_suffix('.json')
if not out.exists() or not lock.exists() or json.loads(lock.read_text())['source_and_map_sha256']!=digest:
 subprocess.run([str(tools/'aarch64-none-elf-objcopy'),'--redefine-syms='+str(mapping),str(source),str(out)],check=True)
 lock.write_text(json.dumps({'source_and_map_sha256':digest,'renamed':sorted(names)},indent=2)+'\n')
checked=subprocess.check_output([str(tools/'aarch64-none-elf-nm'),'-A',str(out)],text=True)
private=set()
for line in checked.splitlines():
 fields=line.split()
 if not fields: continue
 assert fields[-1] not in names, 'Unrenamed Mesa C11/libc reference: '+line
 if len(fields)>2 and fields[-2] in ('T','W') and fields[-1].startswith('dirt2_mesa_'):
  private.add(fields[-1][len('dirt2_mesa_'):])
assert private==set(names), (private,names)
print('Private Mesa C11 thread symbols:',out)
