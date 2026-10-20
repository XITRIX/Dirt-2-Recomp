# Android ARM64

Android uses the recompiled game runtime with **D3D11 → native DXVK → Android
Vulkan**. It does not use Wine, MoltenVK, DXMT, NVK or the Switch driver.
The first target is the Xiaomi Pad 8 Pro. Hardware validation is still pending.
A USB/Bluetooth gamepad is required; touchscreen driving controls are not implemented.
The Android keyboard handles player names.

## Build on this Mac

Install CMake3.28+, Ninja, Meson, glslang, pkg-config, Android SDK platform35/build-tools35.0.1,
NDK29.0.14206865 and a Java JDK (Android Studio's bundled JDK works). The app is
ARM64 only, targets Android35, requires Android28+ and a Vulkan1.3-capable device.
DXVK's full required Vulkan feature checks still run on the device.

Generate `generated/sources.txt` using the normal host recompiler instructions in
README.md first. Then:

```sh
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/29.0.14206865"
export JAVA_HOME="$HOME/Applications/Android Studio.app/Contents/jbr/Contents/Home"
bash tools/scripts/build_android.sh
```

The build uses private `build/deps/android-arm64`, `build/src/dxvk-android`,
`build/dxvk/android-arm64` and `build/android-arm64` directories. Apple and Switch
libraries are not reused as Android binaries. Generated C is built with x86 TSO
ordering enabled and strict floating-point semantics. The NDK builds PIC code;
shared library segments and the APK are aligned for16KB pages.

Packaging uses installed SDK tools directly, without downloading Gradle plugins.
The result is `build/package/android/DiRT2-android-arm64-debug.apk`, signed with a
local development key in `build/android-apk/debug.keystore`. Keep that key for
future in-place updates; it is not committed. The launcher uses your game's `dirt2.ico`, fitted inside Android's adaptive icon
mask. The game executable and playable data are not included in the APK.

## Install and supply game data

Connect the tablet with USB debugging enabled and authorize this computer.
Use one authorized Android device at a time, or set `ANDROID_SERIAL`.

```sh
bash tools/scripts/install_android.sh "/absolute/path/to/Dirt 2"
```

The folder must contain your PCv1.1 `dirt2_game.exe` and its game data. The helper
installs the APK, copies the folder with ADB and launches the app. Omit the folder
argument for subsequent APK updates. Game-data copies resume by skipping completed
files. For Wi-Fi, pair and connect through Wireless debugging first, then set
`ANDROID_SERIAL` to the tablet's debugging IP:port; the pairing port is separate.
Use the SDK's current ADB via `ADB="$ANDROID_HOME/platform-tools/adb"` if another
ADB version is earlier in your search path. A first launch without assets displays the
required path instead of starting the guest.

Assets:
`/data/user/0/com.daniilvinogradov.dirt2/files/Dirt 2`

Settings, saves and logs:
`/data/user/0/com.daniilvinogradov.dirt2/files/Dirt2Data`

These are private app directories and need no broad storage permission. The
installer stages files in `/data/local/tmp`, then unpacks as the app UID through
`run-as`. This avoids scoped-storage permission failures with shell-created
external directories. Successful copies remove staging; interrupted copies can resume. Uninstalling
the app removes its app-scoped data; APK updates preserve it. Saves follow the
same `C/Users/Player/Documents/My Games/DiRT2/savegame` layout as the other ports,
so existing saves can be copied there. Android does not sync saves through iCloud.

## First hardware validation

Check launch/menu, controller input, player-name keyboard, resolution changes,
race loading/rendering/audio, background/resume and Quit. The FPS/compiler HUD is
shown. Fresh/async exposure handling from the working Switch DX11 path is enabled;
no Switch scheduler changes or instruction sampler are enabled.

Retrieve evidence:

```sh
adb exec-out run-as com.daniilvinogradov.dirt2 tar -cf - -C files Dirt2Data > build/android-device-logs.tar
adb logcat -b crash -d > build/android-crash.log
```

Matching unstripped game/SDL libraries and game symbols are archived under
`build/package/android/symbols/<build-id>/` to symbolize native crashes. Android's native crash handler is retained so tombstones are generated.
Building and APK signature verification do not establish gameplay correctness.

Platform references: [SDL3 Android integration](https://wiki.libsdl.org/SDL3/README-android),
[Android Vulkan](https://developer.android.com/ndk/guides/graphics),
[16KB page sizes](https://developer.android.com/guide/practices/page-sizes).
