#!/bin/sh
# Build and test in a disposable QEMU guest; never loads a module on the host.
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
RELEASE=${KERNEL_RELEASE:-$(uname -r)}
IMAGE=${KERNEL_IMAGE:-/lib/modules/$RELEASE/vmlinuz}
HEADERS=${KERNEL_DIR:-/lib/modules/$RELEASE/build}
WORK=${NETDISPLAY_VM_DIR:-"$ROOT/build/power-vm"}
mkdir -p "$WORK/root/proc" "$WORK/root/sys" "$WORK/root/dev" "$WORK/module"
cp "$ROOT/kernel/netdisplay_power.c" "$ROOT/kernel/netdisplay_power.h" "$ROOT/kernel/Makefile" "$WORK/module/"
make -C "$WORK/module" KERNEL_DIR="$HEADERS" -j2
cc -static -O2 -Wall -Wextra -I"$ROOT/src" "$ROOT/kernel/tests/vm_init.c" \
    "$ROOT/src/power_native.c" "$ROOT/src/power_state.c" -o "$WORK/root/init"
cp "$WORK/module/netdisplay_power.ko" "$WORK/root/"
# If UPower is installed, verify desktop-service discovery in the guest too.
DAEMON=
for candidate in /usr/lib/upowerd /usr/libexec/upowerd; do
    if [ -x "$candidate" ]; then DAEMON=$candidate; break; fi
done
if [ -n "$DAEMON" ] && command -v upower >/dev/null && command -v dbus-daemon >/dev/null; then
    mkdir -p "$WORK/root/usr/bin" "$WORK/root/run/dbus" "$WORK/root/var/lib/upower" "$WORK/root/etc" "$WORK/root/tmp"
    for executable in "$DAEMON" "$(command -v upower)" "$(command -v dbus-daemon)"; do
        cp -L "$executable" "$WORK/root/usr/bin/$(basename "$executable")"
        ldd "$executable" | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}' > "$WORK/libraries"
        while IFS= read -r library; do cp --parents -L "$library" "$WORK/root/"; done < "$WORK/libraries"
    done
    printf '%s\n' 'root:x:0:0:root:/:/bin/sh' 'upower:x:82:82:upower:/:/bin/sh' 'messagebus:x:81:81:dbus:/:/bin/sh' > "$WORK/root/etc/passwd"
    printf '%s\n' 'root:x:0:' 'upower:x:82:' 'messagebus:x:81:' > "$WORK/root/etc/group"
    printf '%s\n' '12345678901234567890123456789012' > "$WORK/root/etc/machine-id"
    cat > "$WORK/root/dbus.conf" <<'DBUS'
<busconfig>
  <type>system</type>
  <listen>unix:path=/run/dbus/system_bus_socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/><allow own="*"/>
    <allow send_destination="*"/><allow receive_sender="*"/>
  </policy>
</busconfig>
DBUS
fi
(cd "$WORK/root" && find . -print0 | cpio --null -o --format=newc) | gzip > "$WORK/initramfs.gz"
timeout 120 qemu-system-x86_64 -accel tcg -cpu max -m 512 -smp 2 \
    -nodefaults -nographic -serial stdio -monitor none -no-reboot \
    -kernel "$IMAGE" -initrd "$WORK/initramfs.gz" \
    -append 'console=ttyS0 panic=-1 quiet' > "$WORK/console.log" 2>&1 || {
        cat "$WORK/console.log"
        exit 1
    }
cat "$WORK/console.log"
grep -q 'NETDISPLAY POWER VM PASS' "$WORK/console.log"
! grep -Eq 'BUG:|WARNING:|KASAN:|Oops:' "$WORK/console.log"
