# DiRT 2 Recompiled

Static recompilation of Colin McRae: DiRT 2 (PC, v1.1, `dirt2_game.exe`) from 32-bit x86 to portable C, compiled natively for ARM64 Apple platforms (iOS first, then macOS, tvOS, visionOS) and anything else with a C compiler.

The game's own code is lifted mechanically. Windows, DirectX, OpenAL, Bink and Games for Windows Live imports are replaced with host implementations:

| Original | Replacement |
|---|---|
| Direct3D 11 (the default on iOS) | DXMT → Metal |
| Direct3D 9 (the default on macOS for now) | DXVK (d3d9) → Vulkan → MoltenVK → Metal |
| OpenAL / Rapture3D | OpenAL Soft |
| XInput / DirectInput | SDL3 gamepad (GameController on Apple) |
| Win32 window and messages | SDL3 |
| Bink 1 video | FFmpeg `bink` / `binkaudio` |
| GFWL (`xlive.dll`) | Offline stub |
| Winsock | BSD sockets |

No game data or executable is included. You need your own copy of the game.

## Layout

```
tools/recomp/   host-side recompiler (PE loader, analysis, C code generator)
runtime/        guest CPU context, memory, dispatch, SEH, threads
shims/          per-DLL import implementations
platform/       SDL3 host and per-platform glue (platform/apple)
generated/      recompiler output (not committed)
```

## Build and install (macOS and iOS)

You need:

- **A Mac with Apple silicon**, and **Xcode 26 or later** (with the iOS SDK for iOS builds). Run Xcode once so it installs its components.
- **Build tools from Homebrew:**

  ```bash
  brew install cmake ninja meson glslang pkg-config
  ```

  These are build tools only: every library the game uses is built from pinned sources by the scripts below. Python 3 comes with macOS.
- **Your own copy of the game:** Colin McRae: DiRT 2, PC version 1.1 (the Steam release), meaning the folder that contains `dirt2_game.exe`. It is never copied into the repository.
- **For iOS:** an Apple developer account (team), and an iPhone or iPad with Developer Mode on, connected by cable.

### 1. Get the sources

```bash
git clone --recurse-submodules <repository url> Dirt2
cd Dirt2
```

The folder name `Dirt2` is used in the commands below.

### 2. Build the libraries (once; about 30–60 minutes)

Each script builds for the platforms you list, into its own folder under `build/`:

```bash
tools/scripts/build_deps.sh macos iphoneos       # SDL3, OpenAL Soft, Ogg/Vorbis, FFmpeg (Bink)
tools/scripts/build_graphics.sh macos iphoneos   # MoltenVK and DXVK (the DX9 path)
tools/scripts/build_llvm.sh macos iphoneos       # LLVM 15 for DXMT's shader translator
tools/scripts/build_dxmt.sh macos iphoneos       # DXMT (the DX11 path)
```

Leave out `iphoneos` if you only want the Mac build.

### 3. Recompile the game (about 5 minutes)

```bash
cmake --preset macos-arm64
cmake --build build/macos-arm64 --target recomp
./build/macos-arm64/tools/recomp/recomp lift "<game folder>/dirt2_game.exe" --out generated \
    --entries tools/recomp/config/entries.txt --hooks tools/recomp/config/hooks.txt --include-dead
```

This writes the game's code as C into `generated/`, which is never committed. Rerun it only when the recompiler or its config changes.

### 4. macOS: build and play

```bash
cmake --preset macos-arm64
cmake --build build/macos-arm64
./build/macos-arm64/dirt2 --game "<game folder>"
```

- **Renderer:** DX9 is the default on macOS. Run with `DIRT2_RENDERER=dx11` to use the DX11 path.
- **Saves and settings:** these go to `~/Library/Application Support/Dirt2Recomp`.
- **A shareable build:** `tools/scripts/package_macos.sh` writes `build/package/macos/DiRT2-macOS.zip` with `DiRT2.app`, without game data. Without `--game`, the app looks for a **Dirt 2** folder next to itself, then the folder chosen last time, then asks for it.

### 5. iOS: build, sign, install

```bash
cmake --preset ios -DDIRT2_GAME_DIR="<game folder>"   # the app icon comes from the game's dirt2.ico
cmake --build build/ios
DIRT2_TEAM=<your team id> DIRT2_BUNDLE_ID=<your bundle id> tools/scripts/package_ios.sh device
# or: tools/scripts/package_ios.sh unsigned   -> build/package/iphoneos/DiRT2-iOS-unsigned.ipa
```

- **Signing:** `package_ios.sh` signs the app with a provisioning profile that grants the entitlements the game needs: the 4 GB address space, the increased memory limit, and iCloud for save sync. If no such profile is installed, it asks Xcode to create one; the device must be registered to your team.
- **Using your own bundle id:** change the iCloud container from `iCloud.com.daniilvinogradov.dirt2recomp` to `iCloud.<your bundle id>` in two places, `platform/apple/dirt2.entitlements` and `platform/apple/cloud_saves.mm`.

Install it on the connected device:

```bash
xcrun devicectl list devices
xcrun devicectl device install app --device <device id> build/ios/dirt2.app
```

**Copy the game data once.** In Finder, select the device, open the **Files** tab, and drag the game folder (the one with `dirt2_game.exe`) onto **DiRT 2**. On the device itself, the Files app shows the same folder under **On My iPhone** / **On My iPad**. After that, launch DiRT 2 from the home screen. A controller is recommended.

### Prebuilt binaries (GitHub Actions)

`.github/workflows/build.yml` builds both on every push to `main`, on `v*` tags, and on demand (**Actions → Build → Run workflow**):
- `DiRT2-macOS.zip`: the app, ad hoc signed. Right-click it and choose **Open** the first time.
- `DiRT2-iOS-unsigned.ipa`: unsigned. Sign it with your own team, using a profile that grants the entitlements in `platform/apple/dirt2.entitlements`: the 4 GB address space and the increased memory limit (iCloud is optional).

The game files aren't in this repository. CI takes `dirt2_game.exe` (v1.1; the job checks its SHA-256) and `dirt2.ico` from a **private repository** of your own:

1. Create a private repository containing just those two files (any folder layout). The workflow defaults to `XITRIX/Dirt2-_recomp_private_repo`; for another name set the variable `DIRT2_GAME_REPO` (step 4).
2. Create a key pair:

   ```bash
   ssh-keygen -t ed25519 -N "" -f dirt2_game_key
   ```

3. In the private repository, open **Settings → Deploy keys**, add `dirt2_game_key.pub`, and leave **Allow write access** off.
4. In this repository, open **Settings → Secrets and variables → Actions**:
   - add the secret `DIRT2_GAME_DEPLOY_KEY` with the contents of `dirt2_game_key` (the private half);
   - if your private repository has another name, add the variable `DIRT2_GAME_REPO` (`owner/name`).

Alternatively, set the secret `DIRT2_GAME_URL` to a downloadable zip with the same two files.

The first run builds the libraries, which takes about an hour; later runs use the cache. Shader warm-up data (`build/pipelines`) is game-derived and not in CI, so CI builds compile shaders on first sight.

### Troubleshooting

- **"Device is not able to fulfill the requested usage assertion" during install:** the device is locked or asleep. Unlock it, keep it connected, and retry.
- **Logs:**
  - macOS: `~/Library/Application Support/Dirt2Recomp/dirt2.log`.
  - iOS: copy the log off the device with

    ```bash
    xcrun devicectl device copy from --device <device id> --domain-type appDataContainer --domain-identifier <bundle id> --source "Library/Application Support/Dirt2Recomp/dirt2.log" --destination dirt2.log
    ```
  - The previous run's log is kept as `dirt2.prev.log`.
- **More detail:** [HANDOVER.md](HANDOVER.md) covers the build system, shader warm-up data, pitfalls and the debugging switches.

## Nintendo Switch (homebrew, in progress)

Not playable yet. The current build is the Gate 0 headless boot: the game starts, runs its engine start-up and stops where it asks for Direct3D, logging how long that took. It measures whether the Switch's CPU can run the recompiled game before any graphics work. The plan is in [docs/SWITCH.md](docs/SWITCH.md).

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev` (devkitA64, libnx, deko3d, uam) in `/opt/devkitpro`, a Switch running Atmosphère, and the lifter output from step 3 above (`generated/`, built on the Mac).

```bash
tools/scripts/build_deps.sh switch
cmake --preset switch
cmake --build build/switch
tools/scripts/package_switch.sh
```

`build_deps.sh switch` builds SDL3 with the Switch port (devkitPro's, see `third_party/patches/README.md`), OpenAL Soft, Vorbis and FFmpeg with devkitA64. Compiling the lifted code with GCC takes over an hour.

To install, unzip `build/package/switch/DiRT2-switch.zip` onto the root of the SD card, then copy the game's `Dirt 2` folder into `sdmc:/switch/dirt2/`. Start it by **title takeover**: hold **R** while launching any installed game, then pick DiRT 2 in the Homebrew Menu. Started from the Album, it gets about 400 MB and refuses to run. The log is `sdmc:/switch/dirt2/dirt2.log`; Gate 0 ends with a `gate 0: Direct3DCreate9(...) reached ... s after start` line, after which the app exits. Crashes log the faulting address as an offset into `build/switch/dirt2.elf`:

```bash
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e build/switch/dirt2.elf <offset>
```

## The recompiler on its own

```bash
./build/macos-arm64/tools/recomp/recomp analyze "<game folder>/dirt2_game.exe" --out generated
```

`generated/analysis.md` summarises function discovery, coverage, jump tables, the instruction mix and the import call sites. `generated/functions.csv` lists every function.

Debugging aids:

- `recomp disasm <exe> <hex addr> [count]` prints decoded instructions and their flow classification.
- `RECOMP_PROBE=addr,addr` prints the blocks around given addresses after analysis.
- `RECOMP_NO_GAPSCAN=1` restricts discovery to code that is actually referenced.

## Status

See [docs/STATUS.md](docs/STATUS.md).
