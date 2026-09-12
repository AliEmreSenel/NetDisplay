# NetDisplay - simple build/deploy

Two binaries in the source tree:

- `netdisplay-server` - Hyprland/NVENC source
- `netdisplay-client` - DRM/KMS + VAAPI receiver

One server accepts multiple client computers, and each client advertises every
connected DRM display. Each display gets an independent resolution, refresh
rate, virtual Wayland output, H.264 encoder/decoder pipeline, and UDP port.
The receiver cannot send arbitrary commands. Source lifecycle commands still
come only from the source's local config.

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
dynamically-created `netdisplay-*` Wayland outputs and applies its gamma changes
to that output normally.

This runs CMake, builds both programs, and creates only:

```text
dist/
├── index.html                  # shell script served at /
├── client.conf.example
├── server.conf.example
├── netdisplay-client-x86_64   # or the current architecture
├── netdisplay-server-x86_64
└── source.tar.gz               # C/CMake fallback for either mode
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

On the Hyprland source computer:

```sh
curl -fsSL https://nd.myc.li/ | sh -s -- --server
```

The script detects the CPU architecture and Linux distribution. It tries the
requested prebuilt program first. Only if that binary is unavailable or
incompatible does it check the source-build dependencies and offer the detected
system package manager (`apt`, `dnf`, `yum`, `zypper`, `pacman`, `apk`, or
`xbps`) the matching packages. It runs the package manager directly as root,
otherwise through `sudo` or `doas`; no elevation is requested when nothing is
missing.

In client mode the script first downloads:

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

then directly executes the resulting program from `/tmp`. It does not install a
service or create release state. It creates the appropriate editable config
from the published example when the default config path does not exist, and it
may install missing fallback build packages as described above.

## Receiver source-build dependencies

The fallback requires a C compiler, CMake, pkg-config and development headers
for:

- libdrm
- libavcodec
- libavutil
- libva + libva-drm
- libsodium

The receiver still expects a usable VAAPI H.264 implementation and, for now,
a VT where it can acquire DRM master.

By default the client advertises all connected connectors at their preferred
modes. Repeated configuration entries select explicit per-display modes:

```ini
display=eDP-1,1920x1080@60
display=HDMI-A-1,2560x1440@144
```

The client opens DRM once and passes the same DRM-master file description to
independent receiver workers with preassigned connector/CRTC pairs. Thus each
stream retains the direct socket-to-decoder-to-plane path; there is no shared
frame router or additional plaintext packet header.

Set `host=192.168.1.20` to bypass discovery and connect to a specific IPv4
server. `host=auto` retains broadcast discovery.

## Client configuration

With no config argument, the website launcher creates (if needed) and uses
`${XDG_CONFIG_HOME:-$HOME/.config}/netdisplay/client.conf`. The client itself reads
`${XDG_CONFIG_HOME:-$HOME/.config}/netdisplay/client.conf` when that file
exists, and otherwise uses its built-in defaults. Start with the example:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/netdisplay"
cp client.conf.example \
  "${XDG_CONFIG_HOME:-$HOME/.config}/netdisplay/client.conf"
```

For the website launcher, the same default path is used. You can also pass an
explicit file to the streamed script:

```sh
curl -fsSL https://nd.myc.li/ | sh -s -- /path/to/client.conf
```

Usually only `host` needs attention: leave it as `auto` for discovery, or use
the source computer's IPv4 address. Leave all `display=` lines commented to use
every connected display at its preferred mode. Add one line per connector only
when selecting particular outputs or modes. `want_input=1` requests input;
`grab_input=1` additionally grabs the receiver's input devices exclusively.
If the server uses `password_key`, the interactive prompt needs no client setting.
For unattended authentication, `psk_file` must point to the same key material
configured on the server.

## Passwords, PSKs, and frame encryption

Set `password_key=` in the server config to require interactive authentication.
Generate it without exposing the password in the process list:

```sh
netdisplay-server --derive-password-key
```

The command prompts twice and prints the 64-character derived key to paste
after `password_key=`. After connecting, the client prompts on its controlling
terminal with input hidden. It derives the same Argon2id key locally and proves
possession through the challenge-response handshake; the plaintext password is
never sent or stored in either config. The website launcher generates a random
password when it creates a default server config, prints it once, and stores
only its derived key.

For an unattended client service, generate a 32-byte PSK and copy it to both
machines, readable only by the service users. Leave `password_key` blank and
configure `psk_file` on both sides instead. Configure only one server
authentication method. Either method authenticates the connection even when
video encryption is disabled.

The server policy is `frame_encryption=off`, `allowed`, or `required`; the
client selects `frame_encryption=0` or `1`. Authentication must be configured
before encryption can be enabled. In encrypted mode each complete
encoded frame is protected with XChaCha20-Poly1305 before fragmentation. In
plaintext mode the original 24-byte UDP header, packetization, and direct hot
path are unchanged.

## Existing networking behavior

- UDP 5000 onward: one H.264 stream port per client display
- UDP 5001: discovery to `255.255.255.255`
- TCP 5001: control + optional evdev input
- TCP display-state pushes: DPMS and normalized panel brightness
- discovery sockets use `SO_BINDTODEVICE`
- every video stream remains all-IDR/latest-only so stale frames do not queue
  indefinitely
