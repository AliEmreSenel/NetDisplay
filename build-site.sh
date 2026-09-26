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
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF $EXTRA
cmake --build "$BUILD" -j"$JOBS"

cp "$BUILD/netdisplay-client" "$SITE/netdisplay-client-$ARCH"
cp "$BUILD/netdisplay-server" "$SITE/netdisplay-server-$ARCH"
cp "$ROOT/site/index.html" "$SITE/index.html"
cp "$ROOT/client.conf.example" "$ROOT/server.conf.example" "$SITE/"

# The fallback archive is deliberately just the source needed to CMake-build.
# No release manifests, version folders, container files, or generated output.
tar -czf "$SITE/source.tar.gz" -C "$ROOT" \
    CMakeLists.txt \
    client.c video_receiver.c video_receiver.h \
    server.c display_state.c display_state.h video_sender.c video_sender.h \
    crypto.c crypto.h frame_transport.c frame_transport.h network_test.c network_test.h \
    tests/crypto_test.c tests/control_test.c tests/transport_test.c tests/network_test.c \
    proto.h control_proto.h common.h

printf '\nBuilt site directory: %s\n' "$SITE"
printf 'Serve %s as https://nd.myc.li/\n' "$SITE"
printf 'Receiver command: curl -fsSL https://nd.myc.li/ | sh\n'
printf 'Server command:   curl -fsSL https://nd.myc.li/ | sh -s -- --server\n'
