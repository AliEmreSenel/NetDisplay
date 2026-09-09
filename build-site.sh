#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD=${NETDISPLAY_BUILD_DIR:-"$ROOT/build"}
SITE=${NETDISPLAY_SITE_DIR:-"$ROOT/dist"}
JOBS=${NETDISPLAY_JOBS:-2}
ARCH=$(uname -m)
case "$ARCH" in amd64) ARCH=x86_64 ;; arm64) ARCH=aarch64 ;; esac

rm -rf "$BUILD" "$SITE"
mkdir -p "$SITE"

EXTRA=""
if [ -n "${NETDISPLAY_STATIC_CLIENT_PREFIX:-}" ]; then
    EXTRA="-DNETDISPLAY_STATIC_CLIENT_PREFIX=$NETDISPLAY_STATIC_CLIENT_PREFIX"
fi

# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release $EXTRA
cmake --build "$BUILD" -j"$JOBS"

cp "$BUILD/netdisplay-client" "$SITE/netdisplay-client-$ARCH"
cp "$ROOT/site/index.html" "$SITE/index.html"

# The fallback archive is deliberately just the source needed to CMake-build.
# No release manifests, version folders, container files, or generated output.
tar -czf "$SITE/source.tar.gz" -C "$ROOT" \
    CMakeLists.txt \
    client.c video_receiver.c video_receiver.h \
    proto.h control_proto.h common.h

printf '\nBuilt site directory: %s\n' "$SITE"
printf 'Serve %s as https://nd.myc.li/\n' "$SITE"
printf 'Receiver command: curl -fsSL https://nd.myc.li/ | sh\n'
