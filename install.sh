#!/bin/sh
# Install NetDisplay for the current user.  No root access is required.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-"$HOME/.local"}
CONFIG_HOME=${XDG_CONFIG_HOME:-"$HOME/.config"}
SYSTEMD_USER_DIR=${XDG_CONFIG_HOME:-"$HOME/.config"}/systemd/user
MODE=all
START=1

usage() {
    printf '%s\n' "usage: $0 [--server|--client|--all] [--prefix PATH] [--no-start]"
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --server|--client|--all) MODE=${1#--} ;;
        --prefix) shift; [ "$#" -gt 0 ] || { usage >&2; exit 2; }; PREFIX=$1 ;;
        --no-start) START=0 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

if [ "$START" -eq 1 ] && [ "$PREFIX" != "$HOME/.local" ]; then
    printf '%s\n' 'A custom --prefix requires --no-start (the bundled user units use %h/.local).' >&2
    exit 2
fi

BUILD=${NETDISPLAY_BUILD_DIR:-"$ROOT/build"}
build_server=OFF
build_client=OFF
[ "$MODE" = all ] || [ "$MODE" = server ] && build_server=ON
[ "$MODE" = all ] || [ "$MODE" = client ] && build_client=ON
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DNETDISPLAY_BUILD_SERVER="$build_server" \
    -DNETDISPLAY_BUILD_CLIENT="$build_client"
cmake --build "$BUILD" -j"${NETDISPLAY_JOBS:-2}"

install -d "$PREFIX/bin" "$CONFIG_HOME/netdisplay" "$SYSTEMD_USER_DIR"
if [ "$MODE" = all ] || [ "$MODE" = server ]; then
    install -m 0755 "$BUILD/netdisplay-server" "$PREFIX/bin/netdisplay-server"
    install -m 0644 "$ROOT/netdisplay-server.service" "$SYSTEMD_USER_DIR/netdisplay-server.service"
    [ -e "$CONFIG_HOME/netdisplay/server.conf" ] || \
        install -m 0644 "$ROOT/server.conf.example" "$CONFIG_HOME/netdisplay/server.conf"
fi
if [ "$MODE" = all ] || [ "$MODE" = client ]; then
    install -m 0755 "$BUILD/netdisplay-client" "$PREFIX/bin/netdisplay-client"
    install -m 0644 "$ROOT/netdisplay-client.service" "$SYSTEMD_USER_DIR/netdisplay-client.service"
    [ -e "$CONFIG_HOME/netdisplay/client.conf" ] || \
        install -m 0644 "$ROOT/client.conf.example" "$CONFIG_HOME/netdisplay/client.conf"
fi

if [ "$START" -eq 1 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload
    if [ "$MODE" = all ] || [ "$MODE" = server ]; then
        systemctl --user enable --now netdisplay-server.service
    fi
    if [ "$MODE" = all ] || [ "$MODE" = client ]; then
        systemctl --user enable --now netdisplay-client.service
    fi
fi
printf 'Installed NetDisplay under %s\n' "$PREFIX"
