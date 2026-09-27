#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BUILD=${NETDISPLAY_BUILD_DIR:-"$ROOT/build"}
SITE=${NETDISPLAY_SITE_DIR:-"$ROOT/dist"}
JOBS=${NETDISPLAY_JOBS:-2}
ARCH=$(uname -m)
case "$ARCH" in amd64) ARCH=x86_64 ;; arm64) ARCH=aarch64 ;; esac

# Reuse the build cache; explicitly reset options shared with install.sh.
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DNETDISPLAY_BUILD_SERVER=ON -DNETDISPLAY_BUILD_CLIENT=ON \
    "-DNETDISPLAY_STATIC_CLIENT_PREFIX=${NETDISPLAY_STATIC_CLIENT_PREFIX:-}"
cmake --build "$BUILD" --parallel "$JOBS"

mkdir -p "$SITE"

cp "$BUILD/netdisplay-client" "$SITE/netdisplay-client-$ARCH"
cp "$BUILD/netdisplay-server" "$SITE/netdisplay-server-$ARCH"
cp "$ROOT/site/index.html" "$SITE/index.html"
cp "$ROOT/config/client.conf.example" "$ROOT/config/server.conf.example" "$SITE/"

# The fallback archive is deliberately just the source needed to CMake-build.
# No release manifests, version folders, container files, or generated output.
tar -czf "$SITE/source.tar.gz" -C "$ROOT" CMakeLists.txt src tests kernel/netdisplay_power.h

printf '\nBuilt site directory: %s\n' "$SITE"
printf 'Serve %s as https://nd.myc.li/\n' "$SITE"
printf 'Receiver command: curl -fsSL https://nd.myc.li/ | sh\n'
printf 'Server command:   curl -fsSL https://nd.myc.li/ | sh -s -- --server\n'
