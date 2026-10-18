#!/bin/bash
# Signs the iOS app bundle and packs it.
#   package_ios.sh sim      ad-hoc signs build/ios-sim/DiRT2.app (simulator)
#   package_ios.sh unsigned writes build/package/iphoneos/DiRT2-iOS-unsigned.ipa from
#                           build/ios/dirt2.app with no signature, for signing elsewhere
#                           (CI: .github/workflows/build.yml). Whoever signs it needs a
#                           profile with the entitlements in platform/apple/dirt2.entitlements.
#   package_ios.sh device   signs build/ios/DiRT2.app for the device with the development
#                           certificate and a provisioning profile that carries the memory
#                           entitlements, and writes build/package/iphoneos/DiRT2-iOS.ipa
# Env: DIRT2_TEAM (default D59DHVRS87), DIRT2_BUNDLE_ID (default com.daniilvinogradov.dirt2recomp).
# If no matching profile is installed, Xcode creates one through a stub project
# (automatic signing, -allowProvisioningUpdates); the device must be registered to the team.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
MODE=${1:-device}
TEAM=${DIRT2_TEAM:-D59DHVRS87}
BUNDLE_ID=${DIRT2_BUNDLE_ID:-com.daniilvinogradov.dirt2recomp}
ENTS=$ROOT/platform/apple/dirt2.entitlements
PROFILES="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"

if [ "$MODE" = sim ]; then
    APP=$ROOT/build/ios-sim/dirt2.app
    /usr/libexec/PlistBuddy -c "Set :CFBundleSupportedPlatforms:0 iPhoneSimulator" "$APP/Info.plist"
    # The simulator refuses ad-hoc apps with restricted entitlements; they matter on devices only.
    for fw in "$APP"/Frameworks/*.framework; do [ -d "$fw" ] && codesign --force --sign - "$fw"; done
    codesign --force --sign - "$APP"
    echo "signed (ad hoc): $APP"
    exit 0
fi

APP=$ROOT/build/ios/dirt2.app
[ -d "$APP" ] || { echo "no $APP: build the ios preset first" >&2; exit 1; }

if [ "$MODE" = unsigned ]; then
    PKG=$ROOT/build/package/iphoneos
    WORK=$(mktemp -d)
    mkdir -p "$PKG" "$WORK/Payload"
    cp -R "$APP" "$WORK/Payload/DiRT2.app"
    # No signature or profile from an earlier device build.
    find "$WORK/Payload" -name _CodeSignature -type d -prune -exec rm -rf {} +
    rm -f "$WORK/Payload/DiRT2.app/embedded.mobileprovision"
    (cd "$WORK" && rm -f "$PKG/DiRT2-iOS-unsigned.ipa" && zip -qry "$PKG/DiRT2-iOS-unsigned.ipa" Payload)
    rm -rf "$WORK"
    ls -la "$PKG/DiRT2-iOS-unsigned.ipa"
    exit 0
fi

# A profile for this app ID that grants extended virtual addressing.
find_profile() {
    for p in "$PROFILES"/*.mobileprovision; do
        [ -f "$p" ] || continue
        plist=$(security cms -D -i "$p" 2>/dev/null) || continue
        echo "$plist" | grep -q "<string>$TEAM.$BUNDLE_ID</string>" || continue
        # Every entitlement we sign with must be granted by the profile.
        missing=
        for key in $(/usr/libexec/PlistBuddy -c Print "$ENTS" | awk '/=/{print $1}'); do
            echo "$plist" | grep -q "<key>$key</key>" || missing=1
        done
        [ -z "$missing" ] || continue
        echo "$p"
        return 0
    done
    return 1
}

if ! PROFILE=$(find_profile); then
    echo "no provisioning profile for $TEAM.$BUNDLE_ID with all entitlements in $ENTS: asking Xcode for one"
    STUB=$ROOT/build/ios-profile-stub
    rm -rf "$STUB" && mkdir -p "$STUB/src"
    cat > "$STUB/src/main.m" <<'EOF'
int main(void) { return 0; }
EOF
    cat > "$STUB/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(ProfileStub C)
add_executable(dirt2 MACOSX_BUNDLE src/main.m)
set_target_properties(dirt2 PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER "$BUNDLE_ID"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "$BUNDLE_ID"
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "$TEAM"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
  XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "$ENTS")
EOF
    cmake -S "$STUB" -B "$STUB/build" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 > /dev/null
    xcodebuild -project "$STUB/build/ProfileStub.xcodeproj" -scheme dirt2 -destination "generic/platform=iOS" \
        -derivedDataPath "${DIRT2_XCODE_DD:-$HOME/Library/Caches/Dirt2Recomp/xcode}/profile-stub" \
        -allowProvisioningUpdates -allowProvisioningDeviceRegistration -quiet build
    PROFILE=$(find_profile) || { echo "Xcode did not produce a matching profile" >&2; exit 1; }
fi
echo "profile: $PROFILE"

# Entitlements: the profile's (application-identifier, team, get-task-allow) plus ours.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
security cms -D -i "$PROFILE" > "$WORK/profile.plist"
# Signing entitlements, as Xcode builds them: ours (dirt2.entitlements) plus the
# identity the profile assigns. The profile's own list holds wildcards
# (icloud-services "*", keychain groups) and environment arrays that are not
# valid in a signature.
python3 - "$WORK/profile.plist" "$ENTS" "$WORK/ents.plist" <<'PY'
import plistlib, sys
profile = plistlib.load(open(sys.argv[1], "rb"))["Entitlements"]
ents = plistlib.load(open(sys.argv[2], "rb"))
for key in ("application-identifier", "com.apple.developer.team-identifier", "get-task-allow"):
    if key in profile:
        ents[key] = profile[key]
if any(k.startswith("com.apple.developer.icloud") for k in ents):
    ents["com.apple.developer.icloud-container-environment"] = "Development" if profile.get("get-task-allow") else "Production"
plistlib.dump(ents, open(sys.argv[3], "wb"))
PY
# Sign with the profile certificate whose private key is in the keychain.
KEYCHAIN_IDS=$(security find-identity -v -p codesigning | awk '{print $2}')
IDENTITY=
for sha in $(python3 -c 'import hashlib, plistlib, sys
for c in plistlib.load(open(sys.argv[1], "rb"))["DeveloperCertificates"]:
    print(hashlib.sha1(c).hexdigest().upper())' "$WORK/profile.plist"); do
    if echo "$KEYCHAIN_IDS" | grep -qi "^$sha$"; then IDENTITY=$sha; break; fi
done
[ -n "$IDENTITY" ] || { echo "no certificate of the profile has its key in this keychain" >&2; exit 1; }

cp "$PROFILE" "$APP/embedded.mobileprovision"
# Embedded frameworks (dxmt.framework) are signed first, without entitlements.
for fw in "$APP"/Frameworks/*.framework; do
    [ -d "$fw" ] && codesign --force --sign "$IDENTITY" --timestamp=none "$fw"
done
codesign --force --sign "$IDENTITY" --timestamp=none --entitlements "$WORK/ents.plist" "$APP"
codesign --verify --deep --strict "$APP"

PKG=$ROOT/build/package/iphoneos
mkdir -p "$PKG" "$WORK/Payload"
cp -R "$APP" "$WORK/Payload/DiRT2.app"
(cd "$WORK" && rm -f "$PKG/DiRT2-iOS.ipa" && zip -qry "$PKG/DiRT2-iOS.ipa" Payload)
ls -la "$PKG/DiRT2-iOS.ipa"
