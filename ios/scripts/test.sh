#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/ios/build/tests"
mkdir -p "$OUT"
exec > >(tee "$OUT/test.log") 2>&1
command -v swiftc >/dev/null || { echo 'Swift compiler is required (included on the macOS CI runner).' >&2; exit 1; }
if [ -z "${SODIUM_TEST_CFLAGS:-}" ]; then
  command -v pkg-config >/dev/null || { echo 'Install pkg-config and libsodium development headers.' >&2; exit 1; }
  SODIUM_TEST_CFLAGS="$(pkg-config --cflags libsodium)"
  SODIUM_TEST_LIBS="$(pkg-config --libs libsodium)"
fi
swiftc -swift-version 5 "$ROOT"/ios/Core/*.swift "$ROOT/ios/Tests/main.swift" -o "$OUT/swift-tests"
"$OUT/swift-tests" "$OUT/motion-body.bin"
# Deliberate word splitting of pkg-config flags; paths must not contain spaces.
# shellcheck disable=SC2086
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra \
  -I"$ROOT/src" -I"$ROOT/ios/Native" $SODIUM_TEST_CFLAGS \
  "$ROOT/ios/Native/NDNative.c" "$ROOT/src/crypto.c" "$ROOT/ios/Tests/wire_test.c" \
  $SODIUM_TEST_LIBS -o "$OUT/wire-tests"
"$OUT/wire-tests" "$OUT"
python3 "$ROOT/ios/Tests/test_cross_language.py" "$OUT"
python3 -m py_compile "$ROOT"/tools/ios/*.py "$ROOT"/ios/scripts/*.py
swiftc -frontend -parse -swift-version 5 "$ROOT"/ios/App/*.swift "$ROOT"/ios/Core/*.swift
python3 "$ROOT/ios/scripts/check_source.py"
echo 'All portable checks passed. These checks do not replace xcodebuild or iPhone testing.'
