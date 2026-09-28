#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
BUILD=${NETDISPLAY_BUILD_DIR:-"$ROOT/build"}
SITE=${NETDISPLAY_SITE_DIR:-"$ROOT/dist"}
JOBS=${NETDISPLAY_JOBS:-2}
PUBLIC_URL=${NETDISPLAY_PUBLIC_URL:-https://nd.myc.li}
ARCH=$(uname -m)
case "$ARCH" in amd64) ARCH=x86_64 ;; arm64) ARCH=aarch64 ;; esac

case "$PUBLIC_URL" in
    http://*|https://*) PUBLIC_URL=${PUBLIC_URL%/} ;;
    *) printf '%s\n' 'NETDISPLAY_PUBLIC_URL must be an http(s) URL.' >&2; exit 2 ;;
esac
if ! printf '%s\n' "$PUBLIC_URL" | grep -Eq '^https?://[A-Za-z0-9._~:/-]+$'; then
    printf '%s\n' 'NETDISPLAY_PUBLIC_URL contains unsupported characters.' >&2
    exit 2
fi

# Reuse the build cache; explicitly reset options shared with install.sh.
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DNETDISPLAY_BUILD_SERVER=ON -DNETDISPLAY_BUILD_CLIENT=ON \
    "-DNETDISPLAY_STATIC_CLIENT_PREFIX=${NETDISPLAY_STATIC_CLIENT_PREFIX:-}"
cmake --build "$BUILD" --parallel "$JOBS"

mkdir -p "$SITE"

cp "$BUILD/netdisplay-client" "$SITE/netdisplay-client-$ARCH"
cp "$BUILD/netdisplay-server" "$SITE/netdisplay-server-$ARCH"
escaped_url=$(printf '%s' "$PUBLIC_URL" | sed 's/[\\&|]/\\&/g')
sed "s|https://nd.myc.li|$escaped_url|g" "$ROOT/site/index.html" > "$SITE/index.html"
cp "$ROOT/LICENSE" "$SITE/LICENSE"
cp "$ROOT/config/client.conf.example" "$ROOT/config/server.conf.example" "$SITE/"

# Keep the fallback archive to the files required for a release build.
tar -czf "$SITE/source.tar.gz" -C "$ROOT" LICENSE CMakeLists.txt src protocols kernel/netdisplay_power.h

printf '\nBuilt site directory: %s\n' "$SITE"
printf 'Configured artifact URL: %s\n' "$PUBLIC_URL"
printf 'Receiver command: curl -fsSL %s/ | sh\n' "$PUBLIC_URL"
printf 'Server command:   curl -fsSL %s/ | sh -s -- --server\n' "$PUBLIC_URL"
