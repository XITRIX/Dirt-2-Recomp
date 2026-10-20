#!/usr/bin/env python3
"""Stage the standalone suite, matching symbols and reproducible source bundle."""
from pathlib import Path
import hashlib,json,re,shutil,subprocess,zipfile
root=Path(__file__).resolve().parents[2]
out=root/'build/package/switch-wine';stage=out/'sd/switch/d3d9-wine';stage.mkdir(parents=True,exist_ok=True)
binary=root/'build/switch/tests/d3d9_wine/d3d9_wine'
shutil.copy2(binary.with_suffix('.nro'),stage/'d3d9_wine.nro')
shutil.copy2(root/'tests/d3d9_wine/README.md',stage/'README.md')
subprocess.run(['python3',str(root/'tools/scripts/build_deko_ffp_shaders.py')],check=True)
for p in (root/'build/shaders/deko-ffp').glob('*.dksh'):shutil.copy2(p,stage/p.name)
shutil.copy2(root/'third_party/deko_ffp/LICENSE',stage/'DXVK-FFP-LICENSE')
with zipfile.ZipFile(stage/'upstream-tests-and-adapter.zip','w',zipfile.ZIP_DEFLATED) as z:
    files=list((root/'tests/d3d9_wine').rglob('*'))
    files += [root/'tools/scripts'/name for name in ('prepare_wine_d3d9_tests.py','run_wine_d3d9_tests.py','build_wine_d3d9_shaders.py','summarize_wine_d3d9_tests.py','package_wine_d3d9_switch.py')]
    files += list((root/'shims/d3d9/deko').glob('*'))
    files += list((root/'third_party/patches/mojoshader').glob('*.patch'))
    files += list((root/'third_party/patches/uam').glob('*.patch'))
    files += [root/'tools/scripts/build_uam_tools.sh']
    files += list((root/'third_party/deko_ffp').glob('*'))
    files += [root/'tools/scripts/build_deko_ffp_shaders.py',root/'tools/scripts/dx9_shader_census.py',root/'tools/scripts/build_shader_tools.sh']
    files += list((root/'tools/dx9shader').glob('*'))
    for p in sorted(set(files)):
        if p.is_file() and '__pycache__' not in p.parts:z.write(p,p.relative_to(root))
readelf='/opt/devkitpro/devkitA64/bin/aarch64-none-elf-readelf'
nm='/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'
buildid=re.search(r'Build ID: (\w+)',subprocess.check_output([readelf,'-n',str(binary.with_suffix('.elf'))],text=True))[1]
symbols=out/'symbols';symbols.mkdir(exist_ok=True)
shutil.copy2(binary.with_suffix('.elf'),symbols/(buildid+'.elf'))
with (symbols/(buildid+'.nm')).open('w') as f:subprocess.run([nm,'-nC',str(binary.with_suffix('.elf'))],stdout=f,check=True)
manifest={'build_id':buildid,'files':[{'name':p.name,'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(stage.iterdir()) if p.is_file()]}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
with zipfile.ZipFile(out/'D3D9-Wine-Tests-Switch.zip','w',zipfile.ZIP_DEFLATED) as z:
    for p in sorted((out/'sd').rglob('*')):
        if p.is_file():z.write(p,p.relative_to(out/'sd'))
print(json.dumps({'build_id':buildid,'package':str(out/'D3D9-Wine-Tests-Switch.zip')}))
