#!/usr/bin/env python3
"""Package only newly built Switch 2 NROs, without game data or SDK binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', action='store_true')
    parser.add_argument('--devkitpro', type=Path, default=None)
    args = parser.parse_args()
    build = ROOT / 'build/switch2'
    name = 'dirt2_dx11_probe.nro' if args.probe else 'dirt2.nro'
    source = build / name
    cache = (build / 'CMakeCache.txt').read_text()
    toolchain = next((line.split('=', 1)[1] for line in cache.splitlines()
        if line.startswith('CMAKE_TOOLCHAIN_FILE:') and '=' in line), '')
    if not toolchain.replace('\\', '/').endswith('/libounce/cmake/Switch2.cmake'):
        raise SystemExit('Refusing to package a build without the Switch 2 toolchain')
    data = source.read_bytes()
    if len(data) < 0x80 or data[0x10:0x14] != b'NRO0':
        raise SystemExit('Build output is not an NRO')
    out = ROOT / 'build/package/switch2'
    out.mkdir(parents=True, exist_ok=True)
    app = 'dirt2-switch2-gpu-test.nro' if args.probe else 'Dirt2.nro'
    manifest = {'target':'Switch 2', 'renderer':'D3D11 -> DXVK -> OunceVK',
        'mode':'graphics diagnostic' if args.probe else 'game',
        'nro_sha256':hashlib.sha256(data).hexdigest(), 'nro_bytes':len(data),
        'hardware_tested':False, 'game_data_included':False}
    archive_path = next((line.split('=', 1)[1] for line in cache.splitlines()
        if line.startswith('DIRT2_SWITCH_GENERATED_ARCHIVE:') and '=' in line), '')
    if archive_path and not args.probe:
        cpu_archive = Path(archive_path)
        cpu_record = json.loads((cpu_archive.parent / 'manifest.json').read_text())
        with cpu_archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != cpu_record['archive_sha256']:
            raise SystemExit('CPU archive hash does not match its build manifest')
        manifest['cpu_variant'] = {
            'archive_sha256':digest, 'baseline_sha256':cpu_record['baseline_sha256'],
            'compiler':cpu_record['compiler_version'], 'tso':cpu_record['tso'],
            'inline_guest_memory':cpu_record['inline_guest_memory'],
            'chunks':[Path(item['source']).stem for item in cpu_record['sources']]}
    text = ('DiRT 2 Switch 2 '+manifest['mode']+'\n'
        'Launch from HBMenu under HEN title takeover.\n'
        'Logs/settings: sdmc:/switch/dirt2\n'
        'Diagnostic: tests audio, DX11 readback/presentation and returns to HBMenu.\n'
        if args.probe else
        'DiRT 2 Switch 2 experimental build\n'
        'Requires your PC v1.1 data at sdmc:/switch/dirt2/Dirt 2\n'
        'Logs/settings: sdmc:/switch/dirt2; saves shared with switch/dirt2.\n'
        'Rendering, guest-memory permissions and gameplay require hardware testing.\n')
    (out / app).write_bytes(data)
    archive = out / ('DiRT2-switch2-gpu-test.zip' if args.probe else 'DiRT2-switch2.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        base = 'switch/dirt2/'
        z.writestr(base+app, data)
        z.writestr(base+'README.txt', text)
        license_file = ROOT / 'build/src/dxvk-switch2/LICENSE'
        if license_file.is_file():
            z.write(license_file, base+'licenses/DXVK-LICENSE')
        for license_file in (ROOT / 'build/deps/switch2/share/licenses').rglob('*'):
            if license_file.is_file():
                z.write(license_file, base+'licenses/deps/'+str(license_file.relative_to(ROOT / 'build/deps/switch2/share/licenses')))
        if args.devkitpro:
            for folder in ['libounce/licenses', 'portlibs/switch2/share/licenses']:
                for license_file in (args.devkitpro / folder).rglob('*'):
                    if license_file.is_file():
                        z.write(license_file, base+'licenses/sdk/'+str(license_file.relative_to(args.devkitpro)))
        # User configuration is kept outside the ZIP to avoid overwriting it.
        z.writestr(base+'build-manifest.json', json.dumps(manifest, indent=2)+'\n')
    print(archive)

if __name__ == '__main__':
    main()
