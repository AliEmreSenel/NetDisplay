#!/bin/sh
# Explicit administrator-only setup for the optional power-supply module.
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ACCOUNT=${1:-}
[ "$(id -u)" -eq 0 ] || { printf '%s\n' 'Run this helper as root with the server account name.' >&2; exit 2; }
if [ -z "$ACCOUNT" ] || ! id "$ACCOUNT" >/dev/null 2>&1; then
    printf '%s\n' 'A valid server account is required.' >&2
    exit 2
fi
for tool in dkms make cc modprobe udevadm getent groupadd usermod install; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'Missing prerequisite: %s\n' "$tool" >&2; exit 1; }
done
[ -d "/lib/modules/$(uname -r)/build" ] || { printf '%s\n' 'Install development headers for the running kernel first.' >&2; exit 1; }
NAME=netdisplay-power
VERSION=0.1.0
DEST="/usr/src/$NAME-$VERSION"
# Refuse to replace an existing source installation with different files.
if [ -d "$DEST" ]; then
    for file in netdisplay_power.c netdisplay_power.h Makefile dkms.conf; do
        cmp -s "$ROOT/$file" "$DEST/$file" || {
            printf 'Existing %s differs; remove it with dkms remove %s/%s --all and remove its source directory before reinstalling.\n' "$DEST" "$NAME" "$VERSION" >&2
            exit 1
        }
    done
else
    install -d -m 0755 "$DEST"
    install -m 0644 "$ROOT/netdisplay_power.c" "$ROOT/netdisplay_power.h" "$ROOT/Makefile" "$ROOT/dkms.conf" "$DEST/"
fi
if ! dkms status -m "$NAME" -v "$VERSION" | grep -q .; then
    dkms add -m "$NAME" -v "$VERSION"
fi
dkms install -m "$NAME" -v "$VERSION" -k "$(uname -r)"
getent group netdisplay-power >/dev/null || groupadd --system netdisplay-power
usermod -a -G netdisplay-power "$ACCOUNT"
install -d -m 0755 /etc/udev/rules.d /etc/modules-load.d
install -m 0644 "$ROOT/70-netdisplay-power.rules" /etc/udev/rules.d/70-netdisplay-power.rules
printf '%s\n' netdisplay_power > /etc/modules-load.d/netdisplay-power.conf
udevadm control --reload-rules
modprobe netdisplay_power
udevadm trigger --action=change --subsystem-match=misc --sysname-match=netdisplay-power
udevadm settle
printf 'Installed native power devices. Log out and back in as %s for group membership, then restart the server.\n' "$ACCOUNT"
