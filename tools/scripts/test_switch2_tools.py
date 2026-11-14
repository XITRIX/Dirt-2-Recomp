#!/usr/bin/env python3
"""Host regressions for Windows source preparation and Switch 2 packaging."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SCRIPTS=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('package_switch2',SCRIPTS/'package_switch2.py')
package=importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class Switch2Tools(unittest.TestCase):
    def test_crlf_patch_applies_without_weakening_context_checks(self):
        with tempfile.TemporaryDirectory(prefix='dirt2 patch ') as tmp:
            root=Path(tmp)
            subprocess.run(['git','init','-q',str(root)],check=True)
            source=root/'file.txt'; source.write_bytes(b'one\ntwo\nthree\n')
            diff=root/'change.patch'
            diff.write_bytes(b'diff --git a/file.txt b/file.txt\r\n--- a/file.txt\r\n+++ b/file.txt\r\n@@ -1,3 +1,3 @@\r\n one\r\n-two\r\n+changed\r\n three\r\n')
            command=[sys.executable,str(SCRIPTS/'apply_source_patch.py'),str(root),str(diff)]
            subprocess.run(command,check=True,capture_output=True)
            self.assertEqual(source.read_bytes(),b'one\nchanged\nthree\n')
            subprocess.run(command+['--reverse-check'],check=True,capture_output=True)
            source.write_bytes(b'unrelated\nchanged\nthree\n')
            result=subprocess.run(command+['--reverse-check'],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(source.read_bytes(),b'unrelated\nchanged\nthree\n')

    def test_packages_preserve_the_requested_root_and_exclude_user_data(self):
        with tempfile.TemporaryDirectory(prefix='dirt2 package ') as tmp:
            root=Path(tmp);build=root/'build/switch2';build.mkdir(parents=True)
            (build/'CMakeCache.txt').write_text('CMAKE_TOOLCHAIN_FILE:FILEPATH=/sdk/libounce/cmake/Switch2.cmake\n')
            data=bytearray(128);data[16:20]=b'NRO0'
            (build/'dirt2.nro').write_bytes(data)
            (build/'dirt2_dx11_probe.nro').write_bytes(data)
            for probe in [False,True]:
                with patch.object(package,'ROOT',root),patch.object(sys,'argv',['package']+(['--probe'] if probe else [])),contextlib.redirect_stdout(io.StringIO()):
                    package.main()
                archive=root/'build/package/switch2'/('DiRT2-switch2-gpu-test.zip' if probe else 'DiRT2-switch2.zip')
                with zipfile.ZipFile(archive) as z:
                    names=z.namelist()
                    self.assertTrue(all(n.startswith('switch/dirt2/') for n in names))
                    self.assertFalse(any('env.txt' in n or 'savegame' in n or 'dirt2_game.exe' in n for n in names))
                    manifest=json.loads(z.read('switch/dirt2/build-manifest.json'))
                    self.assertFalse(manifest['hardware_tested'])
                    self.assertEqual(manifest['nro_bytes'],128)

    def test_cpu_archive_identity_is_recorded_and_mismatch_rejected(self):
        with tempfile.TemporaryDirectory(prefix='dirt2 cpu package ') as tmp:
            root=Path(tmp);build=root/'build/switch2';build.mkdir(parents=True)
            cpu=root/'cpu';cpu.mkdir();archive=cpu/'libdirt2_gen.a'
            archive.write_bytes(b'cpu variant archive')
            digest=hashlib.sha256(archive.read_bytes()).hexdigest()
            (cpu/'manifest.json').write_text(json.dumps({
                'archive_sha256':digest, 'baseline_sha256':'baseline',
                'compiler_version':'test compiler', 'tso':True,
                'inline_guest_memory':True, 'sources':[{'source':'generated/src/gen_0232.c'}]}))
            (build/'CMakeCache.txt').write_text(
                'CMAKE_TOOLCHAIN_FILE:FILEPATH=/sdk/libounce/cmake/Switch2.cmake\n'
                +'DIRT2_SWITCH_GENERATED_ARCHIVE:FILEPATH='+str(archive)+'\n')
            data=bytearray(128);data[16:20]=b'NRO0'
            (build/'dirt2.nro').write_bytes(data)
            with patch.object(package,'ROOT',root),patch.object(sys,'argv',['package']),contextlib.redirect_stdout(io.StringIO()):
                package.main()
                with zipfile.ZipFile(root/'build/package/switch2/DiRT2-switch2.zip') as z:
                    manifest=json.loads(z.read('switch/dirt2/build-manifest.json'))
                    self.assertEqual(manifest['cpu_variant']['archive_sha256'],digest)
                    self.assertEqual(manifest['cpu_variant']['chunks'],['gen_0232'])
                    self.assertTrue(manifest['cpu_variant']['tso'])
                archive.write_bytes(b'changed after validation')
                with self.assertRaisesRegex(SystemExit,'CPU archive hash'):
                    package.main()

    def test_wrong_toolchain_and_invalid_output_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);build=root/'build/switch2';build.mkdir(parents=True)
            cache=build/'CMakeCache.txt';cache.write_text('CMAKE_TOOLCHAIN_FILE:FILEPATH=/sdk/cmake/Switch.cmake\n')
            (build/'dirt2.nro').write_bytes(b'invalid')
            with patch.object(package,'ROOT',root),patch.object(sys,'argv',['package']):
                with self.assertRaisesRegex(SystemExit,'Switch 2 toolchain'):package.main()
                cache.write_text('CMAKE_TOOLCHAIN_FILE:FILEPATH=/sdk/libounce/cmake/Switch2.cmake\n')
                with self.assertRaisesRegex(SystemExit,'not an NRO'):package.main()

if __name__=='__main__':unittest.main()
