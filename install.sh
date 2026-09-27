#!/bin/sh
# Install NetDisplay for the current user.  No root access is required.
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-"$HOME/.local"}
CONFIG_HOME=${XDG_CONFIG_HOME:-"$HOME/.config"}
SYSTEMD_USER_DIR=${XDG_CONFIG_HOME:-"$HOME/.config"}/systemd/user
MODE=
ENABLE=0

usage() {
    printf '%s\n' "usage: $0 (--server|--client|--all) [--prefix PATH] [--enable]"
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --server|--client|--all) MODE=${1#--} ;;
        --prefix) shift; [ "$#" -gt 0 ] || { usage >&2; exit 2; }; PREFIX=$1 ;;
        --enable) ENABLE=1 ;;
        # Kept harmless for scripts written for older installers.  Services
        # are opt-in now, so this is already the default.
        --no-start) ENABLE=0 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

[ -n "$MODE" ] || { usage >&2; exit 2; }

if [ "$ENABLE" -eq 1 ] && [ "$PREFIX" != "$HOME/.local" ]; then
    printf '%s\n' 'A custom --prefix cannot use --enable (the bundled user units use %h/.local).' >&2
    exit 2
fi

BUILD=${NETDISPLAY_BUILD_DIR:-"$ROOT/build"}
build_server=OFF
build_client=OFF
[ "$MODE" = all ] || [ "$MODE" = server ] && build_server=ON
[ "$MODE" = all ] || [ "$MODE" = client ] && build_client=ON
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DNETDISPLAY_BUILD_SERVER="$build_server" \
    -DNETDISPLAY_BUILD_CLIENT="$build_client"
cmake --build "$BUILD" --parallel "${NETDISPLAY_JOBS:-2}"

install -d "$PREFIX/bin" "$CONFIG_HOME/netdisplay"
if [ "$MODE" = all ] || [ "$MODE" = server ]; then
    install -m 0755 "$BUILD/netdisplay-server" "$PREFIX/bin/netdisplay-server"
    if [ "$ENABLE" -eq 1 ]; then
        install -d "$SYSTEMD_USER_DIR"
        install -m 0644 "$ROOT/packaging/systemd/netdisplay-server.service" "$SYSTEMD_USER_DIR/netdisplay-server.service"
    fi
    [ -e "$CONFIG_HOME/netdisplay/server.conf" ] || \
        install -m 0644 "$ROOT/config/server.conf.example" "$CONFIG_HOME/netdisplay/server.conf"
fi
if [ "$MODE" = all ] || [ "$MODE" = client ]; then
    install -m 0755 "$BUILD/netdisplay-client" "$PREFIX/bin/netdisplay-client"
    if [ "$ENABLE" -eq 1 ]; then
        install -d "$SYSTEMD_USER_DIR"
        install -m 0644 "$ROOT/packaging/systemd/netdisplay-client.service" "$SYSTEMD_USER_DIR/netdisplay-client.service"
    fi
    [ -e "$CONFIG_HOME/netdisplay/client.conf" ] || \
        install -m 0644 "$ROOT/config/client.conf.example" "$CONFIG_HOME/netdisplay/client.conf"
fi

if [ "$ENABLE" -eq 1 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload
    if [ "$MODE" = all ] || [ "$MODE" = server ]; then
        systemctl --user enable --now netdisplay-server.service
    fi
    if [ "$MODE" = all ] || [ "$MODE" = client ]; then
        systemctl --user enable --now netdisplay-client.service
    fi
fi
printf 'Installed NetDisplay under %s\n' "$PREFIX"
if [ "$ENABLE" -eq 0 ]; then
    printf '%s\n' 'No systemd service was installed, enabled, or started.'
fi
