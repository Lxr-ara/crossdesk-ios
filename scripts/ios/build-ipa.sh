#!/usr/bin/env bash
# Build an unsigned iOS IPA of the CrossDesk controller app.
#
# Must run on macOS with Xcode 16+ (xcodebuild) and Xmake 3.1.1+ on PATH.
# Windows and Linux cannot produce an .ipa: the binary has to be linked by the
# Apple toolchain for arm64-apple-ios.
#
# Usage:
#   scripts/ios/build-ipa.sh                 # Debug, generic iOS device
#   CONFIGURATION=Release scripts/ios/build-ipa.sh
#   BUILD_NUMBER=20261120 scripts/ios/build-ipa.sh
#
# Output: apps/ios/build/CrossDeskMobile-unsigned.ipa
# The IPA is unsigned: sign it with your own Apple ID before installing
# (Xcode free provisioning, Sideloadly, AltStore or TrollStore on supported
# iOS versions).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="$ROOT/apps/ios/CrossDeskMobile.xcodeproj"
SCHEME="CrossDeskMobile"
CONFIGURATION="${CONFIGURATION:-Debug}"
BUILD_NUMBER="${BUILD_NUMBER:-$(date +%Y%m%d%H%M)}"
DERIVED="$ROOT/apps/ios/build"
PRODUCTS="$DERIVED/Build/Products/$CONFIGURATION-iphoneos"
APP="$PRODUCTS/CrossDeskMobile.app"
IPA="$DERIVED/CrossDeskMobile-unsigned.ipa"

command -v xcodebuild >/dev/null || { echo "xcodebuild not found: run this on macOS with Xcode 16+" >&2; exit 1; }
command -v xmake >/dev/null || echo "warning: xmake not on PATH; the first build phase needs it" >&2

mkdir -p "$DERIVED"
cd "$ROOT"

# The project's first build phase compiles MiniRTC, the wire library and their
# dependencies for arm64-apple-ios into Vendor/. Signing is disabled here: an
# unsigned .app packages into an .ipa that a sideloading tool signs locally.
xcodebuild \
  -project "$PROJECT" \
  -scheme "$SCHEME" \
  -configuration "$CONFIGURATION" \
  -destination 'generic/platform=iOS' \
  -derivedDataPath "$DERIVED" \
  CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
  CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGN_IDENTITY="" \
  build

[ -d "$APP" ] || { echo "build finished without $APP" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"
rm -f "$IPA"
( cd "$STAGE" && zip -qry "$IPA" Payload )

echo
echo "ipa:   $IPA"
echo "size:  $(du -h "$IPA" | cut -f1)"
echo "app:   $APP"
echo "bundle: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Info.plist")"
echo
echo "Install (unsigned -> signed on the device):"
echo "  1. Xcode free provisioning: open the project, pick your team, Run on the device."
echo "  2. Sideloadly / AltStore: drop the .ipa in, sign with your Apple ID, install."
echo "  3. TrollStore (only on supported iOS versions): install the .ipa directly."
