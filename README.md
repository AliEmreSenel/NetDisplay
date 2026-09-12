# NetDisplay - simple build/deploy

Two binaries in the source tree:

- `netdisplay-server` - Hyprland/NVENC source
- `netdisplay-client` - DRM/KMS + VAAPI receiver

The receiver cannot send arbitrary commands. Discovery/control/input use the
fixed NetDisplay protocol; source lifecycle commands come only from the
source's local config.

## Build

Install the normal development dependencies for the source machine, then:

```sh
./build-site.sh
```

`./build-website.sh` is an equivalent, explicit website-build entry point.

## Install

Install for the current user (without replacing an existing configuration):

```sh
./install.sh --server     # source computer
./install.sh --client     # receiver computer
```

Pass `--no-start` to install without enabling or starting the user service.

Display state synchronization is built into the daemons and protocol. The
server watches the selected physical Wayland output's DPMS events and host
backlight changes, then immediately pushes them over the existing TCP control
connection. The receiver applies DPMS through DRM and maps the source
brightness percentage onto its own panel backlight. It writes the backlight
sysfs control directly when allowed and otherwise uses the active session's
systemd-logind brightness API, so synchronization does not require running the
client as root. GammaStep also sees the
dynamically-created `netdisplay` Wayland output and applies its gamma changes
to that output normally.

This runs CMake, builds both programs, and creates only:

```text
dist/
├── index.html                 # shell script served at /
├── netdisplay-client-x86_64  # or the current architecture
└── source.tar.gz              # C/CMake fallback
```

There are no versions, manifests, release directories, Docker files, or
release metadata.

If you already have a prefix containing static `libavcodec.a`, `libavutil.a`
and `libdrm.a`, build the receiver mostly-static with:

```sh
NETDISPLAY_STATIC_CLIENT_PREFIX=/path/to/prefix ./build-site.sh
```

`libva`/`libva-drm`, glibc and the vendor VA driver stay dynamic and come from
the receiver laptop.

Without `NETDISPLAY_STATIC_CLIENT_PREFIX`, the prebuilt client uses the normal
libraries from the build machine. The source fallback still makes the setup
usable when that ELF is incompatible.

## Serve

Serve `dist/` as the document root of `https://nd.myc.li` and configure your
web server to return `index.html` for `/` (the normal static-site behavior).
The file contains shell, despite its name; `curl | sh` does not care about the
HTTP content type.

On a receiver VT:

```sh
curl -fsSL https://nd.myc.li/ | sh
```

The script first downloads:

```text
https://nd.myc.li/netdisplay-client-x86_64
```

and runs `--version`. If the loader/ABI rejects it, it downloads:

```text
https://nd.myc.li/source.tar.gz
```

and performs the plain fallback:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNETDISPLAY_BUILD_SERVER=OFF
cmake --build build -j
```

then directly executes the resulting client from `/tmp`. It does not install a
service, change packages, create release state, or copy anything permanently.

## Receiver source-build dependencies

The fallback requires a C compiler, CMake, pkg-config and development headers
for:

- libdrm
- libavcodec
- libavutil
- libva + libva-drm

The receiver still expects a usable VAAPI H.264 implementation and, for now,
a VT where it can acquire DRM master.

The source's configured `width`, `height`, and `refresh_hz` select the stream
mode at runtime. The receiver uses the negotiated dimensions and scales the
stream to the available panel mode when necessary.

## Existing networking behavior

- UDP 5000: H.264 video
- UDP 5001: discovery to `255.255.255.255`
- TCP 5001: control + optional evdev input
- TCP display-state pushes: DPMS and normalized panel brightness
- discovery sockets use `SO_BINDTODEVICE`
- video remains all-IDR/latest-only so stale frames do not queue indefinitely
