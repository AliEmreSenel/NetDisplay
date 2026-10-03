#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
command -v xcodebuild >/dev/null || { echo 'Xcode is required; use the GitHub Actions iOS workflow from Linux.' >&2; exit 1; }
python3 "$ROOT/scripts/generate_project.py"
bash "$ROOT/scripts/prepare-deps.sh"
mkdir -p "$ROOT/build"
xcodebuild -version
xcodebuild -project "$ROOT/NetDisplay.xcodeproj" -scheme NetDisplay \
  -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
  -derivedDataPath "$ROOT/build/xcode" CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY='' \
  build 2>&1 | tee "$ROOT/build/build.log"
APP="$ROOT/build/xcode/Build/Products/Release-iphoneos/NetDisplay.app"
test -d "$APP" || { echo 'No .app was produced' >&2; exit 1; }
rm -rf "$ROOT/build/package"
mkdir -p "$ROOT/build/package/Payload"
ditto "$APP" "$ROOT/build/package/Payload/NetDisplay.app"
cp "$ROOT/../LICENSE" "$ROOT/build/package/Payload/NetDisplay.app/NetDisplay-LICENSE.txt"
cp "$ROOT/build/sodium/iphoneos-arm64/LICENSE" "$ROOT/build/package/Payload/NetDisplay.app/libsodium-LICENSE.txt"
cd "$ROOT/build/package"
rm -f "$ROOT/build/NetDisplay-unsigned.ipa"
zip -qry "$ROOT/build/NetDisplay-unsigned.ipa" Payload
cd "$ROOT/build"
shasum -a 256 NetDisplay-unsigned.ipa > NetDisplay-unsigned.ipa.sha256
printf '\nUnsigned IPA: %s\nSign on your own device/account with iloader or SideStore.\n' "$ROOT/build/NetDisplay-unsigned.ipa"
