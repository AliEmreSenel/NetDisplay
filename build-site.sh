#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SITE=${NETDISPLAY_SITE_DIR:-"$ROOT/dist"}
PUBLIC_URL=${NETDISPLAY_PUBLIC_URL:-https://nd.myc.li}

case "$PUBLIC_URL" in
    http://*|https://*) PUBLIC_URL=${PUBLIC_URL%/} ;;
    *) printf '%s\n' 'NETDISPLAY_PUBLIC_URL must be an http(s) URL.' >&2; exit 2 ;;
esac
if ! printf '%s\n' "$PUBLIC_URL" | grep -Eq '^https?://[A-Za-z0-9._~:/-]+$'; then
    printf '%s\n' 'NETDISPLAY_PUBLIC_URL contains unsupported characters.' >&2
    exit 2
fi

mkdir -p "$SITE"
# Binaries are no longer published; drop any left over from older bundles.
rm -f "$SITE"/netdisplay-client-* "$SITE"/netdisplay-server-*

escaped_url=$(printf '%s' "$PUBLIC_URL" | sed 's/[\\&|]/\\&/g')
sed "s|https://nd.myc.li|$escaped_url|g" "$ROOT/site/index.html" > "$SITE/index.html"
cp "$ROOT/LICENSE" "$SITE/LICENSE"
cp "$ROOT/config/client.conf.example" "$ROOT/config/server.conf.example" "$SITE/"

# The launcher builds from this archive, so keep it to the release build inputs.
tar -czf "$SITE/source.tar.gz" -C "$ROOT" LICENSE CMakeLists.txt cmake src protocols kernel/netdisplay_power.h

printf '\nBuilt site directory: %s\n' "$SITE"
printf 'Configured artifact URL: %s\n' "$PUBLIC_URL"
printf 'Receiver command: curl -fsSL %s/ | sh\n' "$PUBLIC_URL"
printf 'Server command:   curl -fsSL %s/ | sh -s -- --server\n' "$PUBLIC_URL"
