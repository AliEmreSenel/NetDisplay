#!/bin/sh
# Real evdev hotplug tests in a disposable VM; no host devices are created.
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RELEASE=${KERNEL_RELEASE:-$(uname -r)}
IMAGE=${KERNEL_IMAGE:-/lib/modules/$RELEASE/vmlinuz}
WORK=${NETDISPLAY_INPUT_VM_DIR:-"$ROOT/build/input-vm"}
mkdir -p "$WORK/root/proc" "$WORK/root/sys" "$WORK/root/dev" "$WORK/root/modules"
cc -static -pthread -O2 -Wall -Wextra -I"$ROOT/src" "$ROOT/tests/input_vm_init.c" \
    "$ROOT/src/input_receiver.c" -o "$WORK/root/init"
(modprobe --show-depends -S "$RELEASE" evdev; modprobe --show-depends -S "$RELEASE" uinput) \
    | awk '$1 == "insmod" && !seen[$2]++ {print $2}' > "$WORK/dependencies"
: > "$WORK/root/modules.list"
index=0
while IFS= read -r module; do
    destination="$WORK/root/modules/$index.ko"
    case "$module" in
        *.zst) zstd -dc "$module" > "$destination" ;;
        *.xz) xz -dc "$module" > "$destination" ;;
        *.gz) gzip -dc "$module" > "$destination" ;;
        *) cp "$module" "$destination" ;;
    esac
    printf '/modules/%s.ko\n' "$index" >> "$WORK/root/modules.list"
    index=$((index + 1))
done < "$WORK/dependencies"
(cd "$WORK/root" && find . -print0 | cpio --null -o --format=newc) | gzip > "$WORK/initramfs.gz"
timeout 90 qemu-system-x86_64 -accel tcg -cpu max -m 512 -smp 2 \
    -nodefaults -nographic -serial stdio -monitor none -no-reboot \
    -kernel "$IMAGE" -initrd "$WORK/initramfs.gz" \
    -append 'console=ttyS0 panic=-1 quiet' > "$WORK/console.log" 2>&1 || {
        cat "$WORK/console.log"
        exit 1
    }
cat "$WORK/console.log"
grep -q 'NETDISPLAY INPUT VM PASS' "$WORK/console.log"
! grep -Eq 'BUG:|WARNING:|KASAN:|Oops:' "$WORK/console.log"
