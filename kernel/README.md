# NetDisplay native power devices

This optional GPL-2.0 module mirrors receiver batteries and chargers through
Linux's `power_supply` class. It is separate from the video and input paths.
The server runs as its normal user; only module installation needs root.

From a NetDisplay checkout, install the server and opt into DKMS setup:

```sh
./install.sh --server --with-power-module
```

Install `dkms`, a C compiler, make, and the development headers matching the
running kernel first (for example `linux-headers` on Arch, or
`linux-headers-$(uname -r)` on Ubuntu). The helper does not install system
packages. It installs version 0.1.0 under `/usr/src/netdisplay-power-0.1.0`,
registers/builds/installs it with DKMS, loads it, configures boot loading and
udev permissions, and adds the invoking server user to `netdisplay-power`.
Log out and back in before restarting the server so it inherits that group.
Active local sessions can also receive access through udev's `uaccess` ACL.
Secure Boot systems may require enrolling DKMS's signing key according to the
distribution's instructions before the kernel will load the module.

For separate setup, run `sudo ./kernel/install.sh "$(id -un)"` from the checkout.
Do not run the whole user installer with sudo: that would install the server
for root. `--with-power-module` is rejected with `--client`.

Normal installation does not install or load this module. A missing or
inaccessible `/dev/netdisplay-power` produces a server warning; video continues.
Set `power_devices=0` on the server to disable mirroring, or `send_power=0` on a
client to disable reporting. Both default to 1; older peers skip reporting via
capability negotiation. The existing session authentication applies. Like the
other control messages, power telemetry is not encrypted by `frame_encryption`.

## Devices and properties

Each supply has a name such as:

```text
/sys/class/power_supply/netdisplay-192.0.2.10-1234-1-BAT0/
```

The address, server PID, session number, and remote supply name distinguish
clients, reconnects, and multiple batteries/chargers. Read standard properties
such as `capacity`, `status`, `energy_now`, `voltage_now`, `current_now`, `temp`,
`cycle_count`, `health`, `model_name`, `serial_number`, or charger `online` as
usual. `upower --enumerate` and `upower --dump` can inspect devices discovered
by UPower; individual desktops decide which devices and details to show.

`remote/` contains the original reported text attributes, including ones with
no corresponding mapping on the server's kernel, such as a vendor-specific
field or a list of supported USB charging types. This also retains the remote
`scope`. The native `scope` is always `Device`: these supplies power the remote
computer or peripheral, not the server. The module does not create thermal
zones, wakeup sources, writable charge controls, or hardware-management hooks.

The client reads available top-level text attributes in
`/sys/class/power_supply` every two seconds. It skips `uevent`, directories,
attribute symlinks, binary/multiline values, unreadable values, and already
mirrored `netdisplay-*` devices. Hardware that does not report a measurement
cannot provide it. Limits are 16 supplies per client, 128 properties per supply,
63-byte names/keys, and 383-byte values. Over-limit values are skipped; excessive
supply/property counts reject the sample with a warning rather than silently
publishing an incomplete device inventory.

Snapshots are validated and collected completely before publication. Device
updates are atomic per supply; different supplies are updated sequentially.
Removal closes the corresponding device descriptor. Disconnects and process
exit remove all devices owned by that connection. The kernel independently
removes devices after 15 seconds without a successful update, even if userspace
hangs. Reporting resumes by recreating them. Property-set or identity changes
also recreate a device; absent measurements are never synthesized as zero.

## Build and test

```sh
make -C kernel
# Or build for another installed kernel:
make -C kernel KERNEL_DIR=/lib/modules/VERSION/build
```

The normal CMake build needs only `netdisplay_power.h`, not kernel headers or
DKMS. Protocol/collector tests run through CTest. To load and test the actual
module in a disposable VM without changing the host kernel:

```sh
./kernel/tests/run-vm.sh
```

This requires x86-64 QEMU, cpio, gzip, a static C runtime, matching kernel headers,
and a readable kernel image. It uses software emulation and does not require
KVM or root. Override `KERNEL_RELEASE`, `KERNEL_DIR`, and `KERNEL_IMAGE` as needed
(for example `/boot/vmlinuz-VERSION` on Ubuntu). It checks native properties,
raw details, charging transitions, charger disconnects, multiple clients,
malformed writes, hot removal, stale expiry, recovery, and module unload. If
UPower and D-Bus are installed on the host, it also checks UPower discovery and
confirms the remote battery is classified as a peripheral in the guest.

## Removal

Stop NetDisplay servers using the module first, then:

```sh
sudo modprobe -r netdisplay_power
sudo dkms remove netdisplay-power/0.1.0 --all
sudo rm -f /etc/modules-load.d/netdisplay-power.conf
sudo rm -f /etc/udev/rules.d/70-netdisplay-power.rules
sudo rm -rf /usr/src/netdisplay-power-0.1.0
sudo udevadm control --reload-rules
```

Remove unneeded membership in the `netdisplay-power` group separately. To update
this development version with different source, remove its DKMS installation
and source directory first; the helper refuses to overwrite different sources.
