#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=1.0.22
PUBLIC_KEY='RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3'
WORK="$ROOT/build/dependencies"
PREFIX="$ROOT/build/sodium/iphoneos-arm64"
command -v xcrun >/dev/null || { echo 'Run this script on macOS with Xcode (the GitHub workflow does this).' >&2; exit 1; }
command -v minisign >/dev/null || { echo 'Install minisign: brew install minisign' >&2; exit 1; }
mkdir -p "$WORK" "$PREFIX"
cd "$WORK"
ARCHIVE="libsodium-$VERSION.tar.gz"
URL="https://download.libsodium.org/libsodium/releases/$ARCHIVE"
if [ ! -f "$ARCHIVE" ]; then curl --fail --location --retry 3 --proto '=https' --tlsv1.2 "$URL" -o "$ARCHIVE"; fi
if [ ! -f "$ARCHIVE.minisig" ]; then curl --fail --location --retry 3 --proto '=https' --tlsv1.2 "$URL.minisig" -o "$ARCHIVE.minisig"; fi
# Verify before extracting/building. Never silently fall back to unsigned source.
minisign -Vm "$ARCHIVE" -P "$PUBLIC_KEY"
shasum -a 256 "$ARCHIVE" > "$ROOT/build/libsodium-source.sha256"
if [ -f "$PREFIX/lib/libsodium.a" ]; then echo 'Using already built iPhoneOS library'; exit 0; fi
rm -rf "$WORK/source"
mkdir "$WORK/source"
tar -xzf "$ARCHIVE" -C "$WORK/source" --strip-components=1
cd "$WORK/source"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
export CC="$(xcrun --sdk iphoneos --find clang)"
export AR="$(xcrun --sdk iphoneos --find ar)"
export RANLIB="$(xcrun --sdk iphoneos --find ranlib)"
export CFLAGS="-O2 -arch arm64 -isysroot $SDK -miphoneos-version-min=17.0"
export LDFLAGS="-arch arm64 -isysroot $SDK -miphoneos-version-min=17.0"
./configure --host=aarch64-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static
make -j "$(sysctl -n hw.ncpu)"
make install
cp LICENSE "$PREFIX/LICENSE"
echo "Built signed upstream libsodium source for iPhoneOS arm64: $PREFIX"
