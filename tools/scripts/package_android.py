#!/usr/bin/env python3
"""Package a local debug APK using installed SDK tools (no Gradle downloads)."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

root = Path(__file__).resolve().parents[2]
sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Library/Android/sdk'))
ndk = Path(os.environ.get('ANDROID_NDK_HOME', sdk / 'ndk/29.0.14206865'))
java = Path(os.environ.get('JAVA_HOME', Path.home() / 'Applications/Android Studio.app/Contents/jbr/Contents/Home'))
bt = sdk / 'build-tools/35.0.1'
android = sdk / 'platforms/android-35/android.jar'
work = root / 'build/android-apk'
out = root / 'build/package/android'
work.mkdir(parents=True, exist_ok=True)
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, JAVA_HOME=str(java), PATH=str(java/'bin') + os.pathsep + os.environ['PATH'])
def run(*args):
    subprocess.run([str(a) for a in args], check=True, env=env)
classes = work / 'classes'
if classes.exists(): shutil.rmtree(classes)
classes.mkdir()
sources = list((root/'build/deps/src/SDL3-3.4.18/android-project/app/src/main/java').rglob('*.java'))
sources += list((root/'platform/android/java').rglob('*.java'))
run(java/'bin/javac', '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-classpath', android, '-d', classes, *sources)
run(java/'bin/jar', 'cf', work/'classes.jar', '-C', classes, '.')
run(bt/'d8', '--min-api', '28', '--lib', android, '--output', work, work/'classes.jar')
# Use the original game artwork, as on Apple; generated PNG stays in build/.
cache = (root/'build/android-arm64/CMakeCache.txt').read_text()
game_line = next(line for line in cache.splitlines() if line.startswith('DIRT2_GAME_DIR:PATH='))
icon = Path(game_line.split('=', 1)[1])/'dirt2.ico'
if not icon.is_file():
    raise SystemExit('Missing original launcher artwork: '+str(icon))
res = work/'res'
if res.exists(): shutil.rmtree(res)
shutil.copytree(root/'platform/android/res', res)
(res/'drawable-nodpi').mkdir()
run('sips', '-s', 'format', 'png', icon, '--out', res/'drawable-nodpi/game_logo.png')
run(bt/'aapt2', 'compile', '--dir', res, '-o', work/'resources.zip')
run(bt/'aapt2', 'link', '-o', work/'base.apk', '--manifest', root/'platform/android/AndroidManifest.xml', '-I', android, '-R', work/'resources.zip')
native = work/'lib/arm64-v8a'
native.mkdir(parents=True, exist_ok=True)
libs = {
    'libdirt2.so': root/'build/android-arm64/libdirt2.so',
    'libSDL3.so': root/'build/deps/android-arm64/lib/libSDL3.so',
    'libc++_shared.so': ndk/'toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so',
}
for name, source in libs.items():
    shutil.copyfile(source, native/name)
    run(ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip', '--strip-unneeded', native/name)
    segments = subprocess.check_output([str(ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf'), '-lW', str(native/name)], text=True)
    alignments = [int(line.split()[-1], 0) for line in segments.splitlines() if line.strip().startswith('LOAD ')]
    if not alignments or any(align < 16384 for align in alignments):
        raise SystemExit('Native library does not support16KB ELF page alignment: '+name)
with zipfile.ZipFile(work/'base.apk', 'a', zipfile.ZIP_STORED) as z:
    z.write(work/'classes.dex', 'classes.dex')
    for name in libs:
        z.write(native/name, 'lib/arm64-v8a/'+name)
    z.write(root/'third_party/SDL_GameControllerDB/gamecontrollerdb.txt', 'assets/gamecontrollerdb.txt')
    for name in ['LICENSE', 'COPYING']:
        file = root/'build/src/dxvk-android'/name
        if file.exists(): z.write(file, 'assets/DXVK-'+name)
run(bt/'zipalign', '-P', '16', '-f', '4', work/'base.apk', work/'aligned.apk')
key = work/'debug.keystore'
if not key.exists():
    run(java/'bin/keytool', '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android',
        '-alias', 'androiddebugkey', '-dname', 'CN=Android Debug,O=Android,C=US', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000')
apk = out/'DiRT2-android-arm64-debug.apk'
run(bt/'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, work/'aligned.apk')
run(bt/'apksigner', 'verify', '--verbose', apk)
run(bt/'zipalign', '-c', '-P', '16', '4', apk)
notes = subprocess.check_output([str(ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf'), '-n', str(libs['libdirt2.so'])], text=True)
build_id = notes.split('Build ID: ')[1].split()[0]
symbols = out/'symbols'/build_id
symbols.mkdir(parents=True, exist_ok=True)
shutil.copyfile(libs['libdirt2.so'], symbols/'libdirt2.so')
shutil.copyfile(libs['libSDL3.so'], symbols/'libSDL3.so')
with (symbols/'libdirt2.nm').open('w') as f:
    subprocess.run([str(ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm'), '-n', str(libs['libdirt2.so'])], stdout=f, check=True)
manifest = {
    'build_id': build_id,
    'apk': apk.name, 'sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
    'size': apk.stat().st_size, 'abi': 'arm64-v8a', 'min_api': 28,
    'renderer': 'Direct3D 11 -> DXVK native -> Android Vulkan',
    'debug_signed': True, 'hardware_tested': False,
    'assets': '/data/user/0/com.daniilvinogradov.dirt2/files/Dirt 2',
    'data': '/data/user/0/com.daniilvinogradov.dirt2/files/Dirt2Data',
}
(out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(apk)
