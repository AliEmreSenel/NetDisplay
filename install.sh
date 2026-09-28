#!/bin/sh
# Build and install NetDisplay for the current user. DKMS setup is opt-in.
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-"$HOME/.local"}
CONFIG_HOME=${XDG_CONFIG_HOME:-"$HOME/.config"}
SYSTEMD_USER_DIR=${XDG_CONFIG_HOME:-"$HOME/.config"}/systemd/user
MODE=
ENABLE=0
POWER_MODULE=0
INPUT_ACCESS=0

usage() {
    cat <<EOF
usage: $0 (server|client|all) [options]
       $0                       interactive setup

options:
  --prefix PATH         install under PATH (default: \$HOME/.local)
  --enable              install and start the systemd user service
  --with-input-access   allow the active local user to open /dev/uinput
  --with-power-module   install the optional server DKMS module
  -h, --help            show this help
EOF
}

ask_yes_no() {
    prompt=$1
    while :; do
        printf '%s [y/N] ' "$prompt" >&2
        IFS= read -r answer || exit 1
        case "$answer" in
            y|Y|yes|YES|Yes) return 0 ;;
            ''|n|N|no|NO|No) return 1 ;;
            *) printf '%s\n' 'Please answer yes or no.' >&2 ;;
        esac
    done
}

if [ "$#" -eq 0 ]; then
    [ -t 0 ] || { usage >&2; exit 2; }
    printf '%s\n' 'NetDisplay guided setup' >&2
    while [ -z "$MODE" ]; do
        printf '%s' 'Install server or client? [server/client] ' >&2
        IFS= read -r answer || exit 1
        case "$answer" in
            server|client) MODE=$answer ;;
            *) printf '%s\n' 'Please enter server or client.' >&2 ;;
        esac
    done
    if [ "$MODE" = client ]; then
        service_prompt='Start the client at login? (use only on a dedicated VT)'
    else
        service_prompt='Start the server with your graphical session?'
    fi
    if ask_yes_no "$service_prompt"; then ENABLE=1; fi
    if [ "$MODE" = server ]; then
        if ask_yes_no 'Set up /dev/uinput access for input forwarding? (needs admin access)'; then
            INPUT_ACCESS=1
        fi
        if ask_yes_no 'Install the optional battery/charger DKMS module? (needs admin access)'; then
            POWER_MODULE=1
        fi
    fi
    printf '\n' >&2
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        server|client|all) MODE=$1 ;;
        --server|--client|--all) MODE=${1#--} ;;
        --prefix) shift; [ "$#" -gt 0 ] || { usage >&2; exit 2; }; PREFIX=$1 ;;
        --enable) ENABLE=1 ;;
        --with-input-access) INPUT_ACCESS=1 ;;
        --with-power-module) POWER_MODULE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

[ -n "$MODE" ] || { usage >&2; exit 2; }

for tool in cmake install; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'Missing required command: %s\n' "$tool" >&2
        exit 1
    }
done

if [ "$ENABLE" -eq 1 ] && [ "$PREFIX" != "$HOME/.local" ]; then
    printf '%s\n' 'A custom --prefix cannot use --enable (the bundled user units use %h/.local).' >&2
    exit 2
fi

if [ "$ENABLE" -eq 1 ] && ! command -v systemctl >/dev/null 2>&1; then
    printf '%s\n' '--enable requires systemd and systemctl.' >&2
    exit 1
fi

if { [ "$POWER_MODULE" -eq 1 ] || [ "$INPUT_ACCESS" -eq 1 ]; } && [ "$MODE" = client ]; then
    printf '%s\n' '--with-input-access and --with-power-module require server or all mode.' >&2
    exit 2
fi

run_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    elif command -v doas >/dev/null 2>&1; then
        doas "$@"
    else
        printf '%s\n' 'This option requires root access through sudo or doas.' >&2
        exit 1
    fi
}

BUILD=${NETDISPLAY_BUILD_DIR:-"$ROOT/build"}
build_server=OFF
build_client=OFF
case "$MODE" in
    server) build_server=ON ;;
    client) build_client=ON ;;
    all) build_server=ON; build_client=ON ;;
esac
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DNETDISPLAY_BUILD_SERVER="$build_server" \
    -DNETDISPLAY_BUILD_CLIENT="$build_client" \
    -DNETDISPLAY_STATIC_CLIENT_PREFIX=
cmake --build "$BUILD" --parallel "${NETDISPLAY_JOBS:-2}"

install -d "$PREFIX/bin" "$CONFIG_HOME/netdisplay"
if [ "$MODE" = all ] || [ "$MODE" = server ]; then
    install -m 0755 "$BUILD/netdisplay-server" "$PREFIX/bin/netdisplay-server"
    if [ "$ENABLE" -eq 1 ]; then
        install -d "$SYSTEMD_USER_DIR"
        install -m 0644 "$ROOT/packaging/systemd/netdisplay-server.service" "$SYSTEMD_USER_DIR/netdisplay-server.service"
    fi
    [ -e "$CONFIG_HOME/netdisplay/server.conf" ] || \
        install -m 0600 "$ROOT/config/server.conf.example" "$CONFIG_HOME/netdisplay/server.conf"
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

if [ "$POWER_MODULE" -eq 1 ]; then
    ACCOUNT=$(id -un)
    run_as_root "$ROOT/kernel/install.sh" "$ACCOUNT"
fi

if [ "$INPUT_ACCESS" -eq 1 ]; then
    for tool in modprobe udevadm; do
        command -v "$tool" >/dev/null 2>&1 || {
            printf 'Missing required command for input setup: %s\n' "$tool" >&2
            exit 1
        }
    done
    run_as_root install -d -m 0755 /etc/udev/rules.d
    run_as_root install -m 0644 "$ROOT/packaging/udev/99-netdisplay-uinput.rules" \
        /etc/udev/rules.d/99-netdisplay-uinput.rules
    run_as_root modprobe uinput
    run_as_root udevadm control --reload-rules
    run_as_root udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput
    run_as_root udevadm settle
fi

if [ "$ENABLE" -eq 1 ]; then
    systemctl --user daemon-reload
    if [ "$MODE" = all ] || [ "$MODE" = server ]; then
        systemctl --user enable --now netdisplay-server.service
    fi
    if [ "$MODE" = all ] || [ "$MODE" = client ]; then
        systemctl --user enable --now netdisplay-client.service
    fi
fi
printf '\nInstalled NetDisplay under %s\n' "$PREFIX"
case "$MODE" in
    server|client) printf 'Configuration: %s/netdisplay/%s.conf\n' "$CONFIG_HOME" "$MODE" ;;
    all) printf 'Configuration: %s/netdisplay/{server,client}.conf\n' "$CONFIG_HOME" ;;
esac
if [ "$ENABLE" -eq 0 ]; then
    case "$MODE" in
        server|client)
            printf 'Run: %s/bin/netdisplay-%s\n' "$PREFIX" "$MODE"
            ;;
        all)
            printf 'Run: %s/bin/netdisplay-server or %s/bin/netdisplay-client\n' "$PREFIX" "$PREFIX"
            ;;
    esac
else
    printf '%s\n' 'The systemd user service is enabled and running.'
fi
