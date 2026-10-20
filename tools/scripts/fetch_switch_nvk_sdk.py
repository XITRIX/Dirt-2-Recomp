#!/usr/bin/env python3
"""Fetch a pinned public NVK SDK into build/, without installing Docker or changing /opt.

The image is autorunhq/switch-dev (arm64). Only SDK files are extracted; none of
its executables are run. devkitA64 and host tools come from the existing SDK.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/nvk-sdk'
IMAGE = 'sha256:c130418a889d8f9afc3d80c5bda699dfcc83741a22d2f15f800ca5afba17fca1'
BASE = 'https://ghcr.io/v2/autorunhq/switch-dev'
ACCEPT = 'application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    lock = OUT / 'source-lock.json'
    sdk = OUT / 'root/opt/devkitpro'
    if lock.exists() and json.loads(lock.read_text())['image'] == IMAGE and (sdk / 'portlibs/switch/lib/libvulkan.a').exists():
        print('Pinned NVK SDK already available:', sdk)
        return
    token_url = 'https://ghcr.io/token?scope=repository:autorunhq/switch-dev:pull&service=ghcr.io'
    token = json.load(urllib.request.urlopen(token_url))['token']

    def read(path):
        request = urllib.request.Request(BASE + path, headers={'Authorization': 'Bearer ' + token, 'Accept': ACCEPT})
        return urllib.request.urlopen(request, timeout=120)

    with read('/manifests/' + IMAGE) as response:
        raw = response.read()
    assert 'sha256:' + hashlib.sha256(raw).hexdigest() == IMAGE
    manifest = json.loads(raw)
    (OUT / 'oci-manifest.json').write_text(json.dumps(manifest, indent=2))
    # Pinned image history: portlibs, updated libnx, Mesa/NVK, revision metadata.
    for index in [3, 10, 12, 19]:
        layer = manifest['layers'][index]
        archive = OUT / f'layer-{index}.tar.gz'
        if not archive.exists() or archive.stat().st_size != layer['size']:
            temporary = archive.with_suffix('.partial')
            with read('/blobs/' + layer['digest']) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
            temporary.replace(archive)
        assert archive.stat().st_size == layer['size']
        with archive.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        assert 'sha256:' + digest == layer['digest'], 'SDK layer hash mismatch'
        with tarfile.open(archive) as source:
            for member in source:
                name = PurePosixPath(member.name)
                if name.is_absolute() or '..' in name.parts or any(p.startswith('.wh.') for p in name.parts):
                    continue
                if not name.as_posix().startswith(('opt/devkitpro/libnx/', 'opt/devkitpro/portlibs/switch/')):
                    continue
                target = OUT / 'root' / name
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as contents, target.open('wb') as output:
                        shutil.copyfileobj(contents, output)
                    target.chmod(member.mode & 0o777)
        print('Verified and extracted SDK layer', index, flush=True)
    for name in ['cmake', 'devkitA64', 'tools']:
        target = sdk / name
        if not target.exists():
            target.symlink_to('/opt/devkitpro/' + name, target_is_directory=True)
    metadata = json.loads((sdk / 'portlibs/switch/share/switch-dev.json').read_text())
    lock.write_text(json.dumps({'image': IMAGE, 'repository': 'https://github.com/autorunhq/switch-dev', 'components': metadata}, indent=2))


if __name__ == '__main__':
    main()
