#!/bin/sh
set -eu
package=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
original="$(dirname -- "$0")/vrcompositor.netdisplay-original"
export NETDISPLAY_VR_SESSION_JSON="$package/session.json"
export VK_LAYER_PATH="$package/capture"
export VK_INSTANCE_LAYERS=VK_LAYER_NETDISPLAY_capture
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_NETDISPLAY_capture
export VK_LOADER_LAYERS_DISABLE='*'
export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
export LD_PRELOAD="$package/capture/libnetdisplay_drm_shim.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$original" "$@"
