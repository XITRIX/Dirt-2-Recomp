# iOS (and iPadOS) delivery

## Build and sign

- Use the same static sources as macOS, with per-platform outputs (`iphoneos`, `iphonesimulator`) and CMake presets (`ios`, `ios-sim`). The result is one executable in `dirt2.app`.
- **Entitlements:**
  - `com.apple.developer.kernel.extended-virtual-addressing` for the 4 GB guest space, and `com.apple.developer.kernel.increased-memory-limit`;
  - `com.apple.developer.sustained-execution`;
  - iCloud with a CloudKit container (`icloud-container-identifiers`, `icloud-services` = CloudKit, plus `icloud-container-environment` taken from the profile).
- **Signing script:**
  - find an installed provisioning profile that carries *all* the entitlement keys;
  - if there is none, have Xcode create one through a stub project with automatic signing (`-allowProvisioningUpdates -allowProvisioningDeviceRegistration`); the device must be registered to the team;
  - sign with our entitlements merged with the profile's identity keys.
- **Simulator:** it refuses ad-hoc apps that carry restricted entitlements, so sign the simulator build without them (`codesign --force --sign -`). Its GPU can't run DXVK 3 anyway (apple-graphics.md).
- **Info.plist:**
  - `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`, so the Documents folder shows in Finder and the Files app;
  - `LSApplicationCategoryType` games, `LSSupportsGameMode` and `GCSupportsGameMode`, `GCSupportedGameControllers`, `UIRequiresFullScreen` and landscape orientations;
  - `CADisableMinimumFrameDurationOnPhone` = true, to allow 120 Hz on ProMotion.
- **App icon:** generate the asset catalog from one PNG with `ios_app_icon.sh` (`sips` sizes plus `actool`). On macOS, set the icon at run time.

## Devices (devicectl)

```bash
xcrun devicectl list devices
xcrun devicectl device install app --device <UDID> build/ios/dirt2.app
xcrun devicectl device process launch --device <UDID> <bundle id>
xcrun devicectl device copy from --device <UDID> --domain-type appDataContainer \
  --domain-identifier <bundle id> --source "Library/Application Support/<App>/dirt2.log" --destination ./dirt2.log
```

- Logs live in the container's `Library/Application Support/<App>/`, not in Documents.
- Pull them yourself after the user reports a crash, and keep the previous run's log (`dirt2.prev.log`) so a relaunch doesn't erase the crash.
- Don't copy game data to the device for the user unless asked; they put it in through the Files app.

## Game data on device

- The user copies the game folder into the app's Documents (Finder or Files). Accept either the folder itself or its contents.
- Create a placeholder file (`put your game here.txt`) at first launch, so the app shows up in Files even before the data is there.
- `SIMCTL_CHILD_DIRT2_GAME_DIR=<mac path>` points the simulator at the Mac's copy.

## Windowing and lifecycle

- **One window:** iOS has a single window per screen, so every top-level game window (splash, then main) shares one SDL window.
- **Points vs pixels:** see d3d9-bridge.md, display modes.
- **Backgrounding:**
  - submitting GPU work in the background is fatal on iOS;
  - park the render thread at Present while the app is backgrounded or paused (`g_background`, `g_paused` with a condition variable, an SDL lifecycle event watch);
  - failed Locks during the transition must get scratch memory, not stale pointers.
- **Pause for dialogs:** `platform::set_game_paused(true)` holds the render thread while a native alert is up, for example a save conflict.
- **Text input:** use `UIAlertController` with a text field instead of the game's own on-screen keyboard. On macOS, an `NSAlert` with a helper alert window (without the folder icon on the alert).
- **Controllers on iOS:** GameController (MFi) is the only route, so enable SDL's MFi driver there, while it stays off on macOS.

## Saves sync between devices (CloudKit)

iCloud Drive was the first attempt and was rejected: on a fresh device it reported "no file" until the download happened, and it can't express "don't let an older device overwrite newer progress".

- **CloudKit private database:**
  - one record `snapshot` with fields time, device, and a data asset;
  - the asset is a pack of the whole save folder (`DIRT2SAVE1` header);
  - the local state file (`savegame.cloudkit`) records what was last synced, so the code can tell whether local or cloud moved since.
- **At launch, before the game reads saves:** fetch, with three outcomes: Found, Missing or Unknown. Unknown means a network or account error, and the game must not upload over the cloud then.
  - Cloud newer than local, and local unchanged since the last sync: pull.
  - Local changed and cloud changed: **conflict prompt**, Steam-style (cloud or local, showing times and device names), with the game paused until it's answered.
- **After each save:** a watcher packs and uploads with `CKRecordSaveIfServerRecordUnchanged`. A server-changed error means another device won: refetch and prompt.
- Tell the user the order on first rollout: run the up-to-date device first so it uploads, then the other one pulls.
- **API pitfalls:**
  - `modifyRecordsResultBlock` was unavailable for the deployment target, so use `modifyRecordsCompletionBlock`;
  - an ARC reference parameter needs `CKRecord* __strong&`;
  - don't capture `__block` variables in C++ lambdas.

## Performance notes

- Races ran at up to 120 fps on iPhone 15 Pro Max and iPad Pro M4.
- The frontend measured 106–120 fps on the iPad.
