#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
command -v python3 >/dev/null || { echo 'Python 3 is required.' >&2; exit 1; }
# Xcode configurations and separate build directories share this install tree.
# Hold a portable advisory lock until extraction, compilation and install finish.
if [ "${NETDISPLAY_SODIUM_LOCKED:-}" != "$ROOT" ]; then
  exec python3 - "$ROOT" <<'PY'
import fcntl, os, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
work = root/'build/dependencies'
work.mkdir(parents=True, exist_ok=True)
with (work/'.libsodium.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    environment = os.environ.copy()
    environment['NETDISPLAY_SODIUM_LOCKED'] = str(root)
    raise SystemExit(subprocess.call(['/bin/bash', str(root/'scripts/prepare-deps.sh')], env=environment))
PY
fi
VERSION=1.0.22
SOURCE_SHA256=adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349
PUBLIC_KEY='RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3'
WORK="$ROOT/build/dependencies"
PREFIX="$ROOT/build/sodium/iphoneos-arm64"
MIN_OS="${IPHONEOS_DEPLOYMENT_TARGET:-17.0}"
command -v xcrun >/dev/null || { echo 'An iPhoneOS SDK and xcrun-compatible toolchain are required.' >&2; exit 1; }
mkdir -p "$WORK" "$PREFIX"
cd "$WORK"
ARCHIVE="libsodium-$VERSION.tar.gz"
for name in "$ARCHIVE" "$ARCHIVE.minisig"; do
  if [ ! -f "$name" ]; then
    temporary="$(mktemp "$WORK/download.XXXXXX")"
    if ! curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' --tlsv1.2 \
        "https://download.libsodium.org/libsodium/releases/$name" -o "$temporary"; then
      rm -f "$temporary"
      exit 1
    fi
    mv "$temporary" "$name"
  fi
done
# A release is pinned by digest as well as authenticated by its upstream key.
python3 - "$ARCHIVE" "$SOURCE_SHA256" <<'PY'
import hashlib, sys
from pathlib import Path
archive, expected = sys.argv[1:]
digest = hashlib.sha256(Path(archive).read_bytes()).hexdigest()
if digest != expected:
    raise SystemExit('libsodium source SHA-256 mismatch; refusing to extract or build it')
PY
if command -v minisign >/dev/null; then
  minisign -Vm "$ARCHIVE" -P "$PUBLIC_KEY"
else
  python3 "$ROOT/scripts/verify_minisign.py" "$ARCHIVE" "$ARCHIVE.minisig" "$PUBLIC_KEY"
fi
printf '%s  %s\n' "$SOURCE_SHA256" "$ARCHIVE" > "$ROOT/build/libsodium-source.sha256"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
SDK_VERSION="$(xcrun --sdk iphoneos --show-sdk-version)"
CC="$(xcrun --sdk iphoneos --find clang)"
AR="$(xcrun --sdk iphoneos --find ar)"
RANLIB="$(xcrun --sdk iphoneos --find ranlib)"
CC_VERSION="$("$CC" --version)"
BUILD_KEY="$(python3 - "$SOURCE_SHA256" "$SDK" "$SDK_VERSION" "$MIN_OS" "$CC_VERSION" "$ROOT/scripts/prepare-deps.sh" "$ROOT/scripts/verify_minisign.py" <<'PY'
import hashlib, json, sys
from pathlib import Path
values = sys.argv[1:6]
values += [hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in sys.argv[6:]]
print(hashlib.sha256(json.dumps(values).encode()).hexdigest())
PY
)"
# Check every installed header/archive, not just the existence of a stale file.
build_state() {
  python3 - "$PREFIX" "$BUILD_KEY" "$1" <<'PY'
import hashlib, json, sys
from pathlib import Path
prefix, key, mode = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
paths = [prefix/'lib/libsodium.a', prefix/'include/sodium.h', prefix/'LICENSE', prefix/'libsodium-LICENSE.txt']
paths += sorted((prefix/'include/sodium').glob('*.h'))
if not all(p.is_file() for p in paths) or len(paths) < 5:
    raise SystemExit(1)
state = {'build_key': key, 'files': {
    str(p.relative_to(prefix)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
stamp = prefix/'.build-state.json'
if mode == 'check':
    try:
        raise SystemExit(0 if json.loads(stamp.read_text()) == state else 1)
    except (OSError, ValueError):
        raise SystemExit(1)
temporary = stamp.with_suffix('.tmp')
temporary.write_text(json.dumps(state, sort_keys=True, indent=2)+'\n')
temporary.replace(stamp)
PY
}
if build_state check; then
  echo "Using verified cached libsodium $VERSION for iPhoneOS arm64"
  exit 0
fi
# The authenticated archive is unpacked only after both checks above succeed.
rm -rf "$WORK/source"
mkdir "$WORK/source"
tar -xzf "$ARCHIVE" -C "$WORK/source" --strip-components=1
cd "$WORK/source"
export CC AR RANLIB
# Quote SDK paths for configure/make, including Xcode installations with spaces.
SDK_QUOTED="$(python3 - "$SDK" <<'PY'
import shlex, sys
print(shlex.quote(sys.argv[1]))
PY
)"
export CFLAGS="-O2 -arch arm64 -isysroot $SDK_QUOTED -miphoneos-version-min=$MIN_OS"
export CPPFLAGS="-isysroot $SDK_QUOTED"
export LDFLAGS="-arch arm64 -isysroot $SDK_QUOTED -miphoneos-version-min=$MIN_OS"
./configure --host=aarch64-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static
JOBS="$(python3 -c 'import os; print(min(os.cpu_count() or 1, 8))')"
make -j "$JOBS"
make install
cp LICENSE "$PREFIX/LICENSE"
cp LICENSE "$PREFIX/libsodium-LICENSE.txt"
build_state write
echo "Built verified libsodium $VERSION for iPhoneOS arm64: $PREFIX"
