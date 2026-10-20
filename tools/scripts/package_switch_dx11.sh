#!/bin/bash
# Package the opt-in DX11/NVK prototype alongside the normal Switch build.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - "$@" <<'PY'
from pathlib import Path
import hashlib,json,shutil,subprocess,zipfile,sys,argparse
root=Path.cwd();build=root/'build/switch-dxvk'
parser=argparse.ArgumentParser()
for flag in ['standalone-preflight','startup-watchdog','no-disk-shader-cache','exposure-trace','preflight','shader-preload','fresh-exposure','async-exposure','primary','performance-trace','indirect-profile']:
 parser.add_argument('--'+flag,action='store_true')
parser.add_argument('--output-dir',type=Path,default=root/'build/package/switch-dx11')
args=parser.parse_args()
out=args.output_dir;sd=out/'sd/switch/dirt2-dx11';sd.mkdir(parents=True,exist_ok=True)
standalone=args.standalone_preflight;watchdog=args.startup_watchdog
no_disk_cache=args.no_disk_shader_cache;exposure_trace=args.exposure_trace
if (args.shader_preload or args.fresh_exposure or args.async_exposure) and (standalone or args.preflight):
 raise SystemExit('Game options require game mode')
if watchdog and (standalone or args.preflight):
 raise SystemExit('Startup watchdog requires game mode')
binary = 'dirt2_dx11_probe' if standalone else 'dirt2'
nro=sd/'dirt2-dx11.nro';shutil.copyfile(build/(binary+'.nro'),nro)
(sd/'env.txt').write_text('DIRT2_RENDERER=dx11\nDXVK_WSI_DRIVER=SDL3\nDXVK_LOG_LEVEL=info\nDXVK_HUD=fps,compiler\n')
(sd/'README.txt').write_text('DiRT 2 for Switch: Direct3D 11 -> DXVK -> native NVK on Horizon.\nStart through title takeover. Uses existing assets in sdmc:/switch/dirt2/Dirt 2.\nLogs and settings are separate in sdmc:/switch/dirt2-dx11.\nSaves are shared with the original game in sdmc:/switch/dirt2/C/Users/Player/Documents/My Games/DiRT2/savegame.\nThe first run compiles shaders; rendering correctness/performance need hardware testing.\n')
preflight = standalone or args.preflight
if preflight:
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_DX11_PREFLIGHT_ONLY=1\n')
 with (sd/'README.txt').open('a') as f:f.write('This diagnostic package runs isolated audio/GPU checks, shows a blue frame on success, then returns to homebrew. It does not launch the game.\n')
if watchdog:
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_DX11_STARTUP_WATCHDOG=1\nDXVK_PIPELINE_WARMUP=\nDXVK_PIPELINE_RECORD=\nDXVK_SHADER_CACHE_NAME=game-startup\n')
 with (sd/'README.txt').open('a') as f:f.write('Runs the full game with a 120-second startup stall watchdog. Shader warm-up progress extends its deadline. A CPU startup stall requests an Atmosphere report. A complete system hang can prevent the watchdog from running.\nThe game closes the title when its guest entry returns, avoiding reuse of live GPU transfer memory by the launcher.\n')
if no_disk_cache:
 with (sd/'env.txt').open('a') as f:f.write('DXVK_SHADER_CACHE_PATH=\nMESA_SHADER_CACHE_DISABLE=true\n')
 with (sd/'README.txt').open('a') as f:f.write('Diagnostic configuration: disk shader caches are disabled to isolate concurrent SD writes seen in the startup watchdog report. Rendering is unchanged; shaders compile in memory.\n')
if exposure_trace:
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_LOG_EXPOSURE=1\nDIRT2_EXPOSURE_TRACE_SECONDS=600\n')
 with (sd/'README.txt').open('a') as f:f.write('Records small float texture readbacks and frame timing in exposure.csv for ten minutes. Writes are batched; tracing adds no extra GPU readbacks or waits.\n')
if args.indirect_profile:
 if not args.performance_trace: raise SystemExit('--indirect-profile requires --performance-trace')
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_PROFILE_INDIRECT=1\n')
 with (sd/'README.txt').open('a') as f:f.write('Samples active indirect guest calls at100Hz; reports inclusive wall-context for hot threads. No thread suspension. Adds two atomic stores per indirect call while enabled.\n')
if args.performance_trace:
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_PROFILE=1\nDIRT2_DX11_PERF=1\n')
 with (sd/'README.txt').open('a') as f:f.write('Performance trace: guest CPU usage every ten seconds, DXVK counters and Present timing every five seconds in dirt2.log. Does not pause threads or add GPU timestamp waits.\n')
if args.shader_preload:
 bundle=root/'build/shaders/dx11/dirt2.dx11-shaders'
 shutil.copyfile(bundle,sd/bundle.name)
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_DX11_SHADER_PRELOAD=sdmc:/switch/dirt2-dx11/dirt2.dx11-shaders\n')
 with (sd/'README.txt').open('a') as f:f.write('Opt-in startup shader-library warm-up: compiles the bundled original DX11 shaders with native NVK each launch, before gameplay. Disk shader caches remain disabled. Some full pipeline variants can still compile during play.\n')
else:
 (sd/'dirt2.dx11-shaders').unlink(missing_ok=True)
if args.async_exposure and not args.fresh_exposure:
 raise SystemExit('--async-exposure requires --fresh-exposure')
if args.async_exposure:
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_DX11_ASYNC_EXPOSURE=1\n')
 with (sd/'README.txt').open('a') as f:f.write('Exposure performance comparison: consumes the newest completed exposure pixel up to two frames old, then falls back to waiting for the latest pixel. Preserves bounded feedback delay while allowing GPU overlap.\n')
if args.fresh_exposure:
 with (sd/'env.txt').open('a') as f:f.write('DIRT2_DX11_FRESH_EXPOSURE=1\n')
 with (sd/'README.txt').open('a') as f:f.write('Exposure latency comparison: the known DiRT2 exposure readback consumes the newest queued pixel instead of an older ring entry. Can reduce GPU overlap. Other resource maps retain normal behavior.\n')
for name in ['LICENSE']:
 shutil.copyfile(root/'build/src/dxvk-switch'/name,sd/('DXVK-'+name))
licenses=root/'build/nvk-sdk/root/opt/devkitpro/portlibs/switch/share/licenses'
if licenses.exists():shutil.copytree(licenses,sd/'sdk-licenses',dirs_exist_ok=True)
build_id=subprocess.check_output(['/opt/devkitpro/devkitA64/bin/aarch64-none-elf-readelf','-n',str(build/(binary+'.elf'))],text=True).split('Build ID: ')[1].split()[0]
metadata=json.loads((root/'build/nvk-sdk/root/opt/devkitpro/portlibs/switch/share/switch-dev.json').read_text())
manifest={'build_id':build_id,'nro_sha256':hashlib.sha256(nro.read_bytes()).hexdigest(),'nro_size':nro.stat().st_size,'renderer':'D3D11 -> DXVK native -> NVK','sdk':metadata,'installed':False,'hardware_result':'not tested','game_assets':'sdmc:/switch/dirt2/Dirt 2','data_directory':'sdmc:/switch/dirt2-dx11','normal_deko_build_changed':False}
manifest['mode'] = 'standalone graphics preflight' if standalone else ('isolated graphics preflight' if preflight else ('game startup watchdog' if watchdog else 'game'))
manifest['binary'] = binary
# Record the limited CPU archive experiment so console comparisons are traceable.
cache=(build/'CMakeCache.txt').read_text()
archive_line=next((line for line in cache.splitlines() if line.startswith('DIRT2_SWITCH_GENERATED_ARCHIVE:FILEPATH=')), '')
if archive_line:
 archive=Path(archive_line.split('=',1)[1])
 variant=archive.parent/'manifest.json'
 if archive.is_file() and variant.is_file():
  cpu=json.loads(variant.read_text())
  if cpu.get('inline_guest_memory'):
   with archive.open('rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
   if digest != cpu['archive_sha256']: raise SystemExit('CPU variant archive digest mismatch')
   manifest['cpu_variant']={'inline_memory_chunks':[Path(item['source']).stem for item in cpu['sources']], 'tso':cpu['tso'], 'baseline_sha256':cpu['baseline_sha256'], 'archive_sha256':digest}
   shutil.copyfile(variant,out/'cpu-variant.json')
manifest['exposure_trace'] = exposure_trace
manifest['performance_trace'] = args.performance_trace
manifest['indirect_profile'] = args.indirect_profile
manifest['shader_preload'] = args.shader_preload
manifest['fresh_exposure'] = args.fresh_exposure
manifest['async_exposure'] = args.async_exposure
if args.shader_preload:
 manifest['shader_bundle'] = {k:v for k,v in json.loads((root/'build/shaders/dx11/dirt2.json').read_text()).items() if k != 'shaders'}
manifest['save_directory'] = None if standalone else 'sdmc:/switch/dirt2/C/Users/Player/Documents/My Games/DiRT2/savegame'
if no_disk_cache:
 manifest['configuration'] = 'disk shader caches disabled; native startup watchdog enabled' if watchdog else 'disk shader caches disabled'
if standalone:
 manifest['game_assets'] = None
 (sd/'README.txt').write_text('Standalone DiRT 2 DX11 graphics diagnostic.\nLaunch through title takeover; no game files are required.\nChecks audio, image readback and presentation, then returns to homebrew.\nLogs and image data are saved in sdmc:/switch/dirt2-dx11.\n')
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
manifest['primary'] = args.primary
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
archive=out/('DiRT2-switch.zip' if args.primary else 'DiRT2-switch-dx11-prototype.zip')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,6) as z:
 for file in (out/'sd').rglob('*'):
  if file.is_file():z.write(file,file.relative_to(out/'sd'))
print(archive);print(json.dumps(manifest,indent=2))
PY
