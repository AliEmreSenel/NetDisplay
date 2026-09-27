#!/bin/sh
# Compatibility-friendly public entry point for creating dist/.
exec "$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/build-site.sh" "$@"
