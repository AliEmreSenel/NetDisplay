# NetDisplay - simple build/deploy

Two binaries built under `build/`:

- `netdisplay-server` - Hyprland/NVENC source
- `netdisplay-client` - DRM/KMS + VAAPI receiver

One server accepts multiple client computers, and each client advertises every
connected DRM display. Each display gets an independent resolution, refresh
rate, virtual Wayland output, H.264 encoder/decoder pipeline, and UDP port.
The receiver cannot send arbitrary commands. Source lifecycle commands still
come only from the source's local config.

## Connection network test

New peers automatically run a roughly 6.5-second UDP test per display before
starting video. Older peers skip it through capability negotiation. Each
direction sends random 4, 16, 64, 256, and 1024 KiB frames for half a second
per size, at the display's refresh rate (capped at 240 Hz), followed by a
150 ms allowance for replies. Tests run sequentially across displays; they
measure each path separately, not the aggregate capacity of simultaneous streams.

The probe and video share the ND01 packet header, 1472-byte UDP payload limit,
batched nonblocking sender, fragmentation, latest-frame reassembly, socket
buffer sizes, and optional frame encryption. A newer frame abandons an
unfinished older frame; completed probe frames use a single replaceable slot
before hashing. Video and probes share that mailbox implementation, with a
dedicated receive thread independent of the consumer. The test substitutes
random data and a BLAKE2b-256 hasher for
the encoder and decoder. Hash replies also use ND01 framing over UDP. Probe
data and replies in each direction have separate derived encryption keys,
so probe sequence numbers never reuse video encryption nonces.
Probe frames also use a separate session ID, and the video receiver accepts
only its negotiated video session, so late probe packets cannot enter decoding
or advance the video's frame sequence.

Both logs show each direction's per-size results:

- `offered`: generated payload bitrate during the sending window.
- `verified`: useful payload bitrate for frames whose returned hashes match.
  Its observation window includes late verified replies, up to the allowance.
  `UDP` counts transmitted ND01 headers and encrypted payload, excluding IP,
  UDP and Ethernet overhead. Neither number is a link-speed claim.
- `complete`, `lost-or-late`, `aborted`, `producer-skipped`, and `corrupt`:
  completion consistency, missed replies, stale transmissions, missed
  generation slots, and incorrect hashes. A missing reply can mean a dropped
  frame, an overwritten hasher slot, a lost ACK, or a reply past the allowance.
- `RTT-p50/p95`: local monotonic time from frame submission through hash reply.
  `transport-RTT` subtracts the peer's time from complete frame reception to
  hash reply preparation, including decryption, hasher wait, and hashing.
  It still includes serialization, local socket/scheduling delays, and both
  network directions. `variation-p95-p50` describes latency spread.
- `receive-assembly-p95`: peer time from the first received fragment to the
  complete frame. Live video also reports useful receive bitrate and mean
  receive assembly time every second.

These are software timing measurements. No subtraction of timestamps from
different hosts is used. Exact one-way source-to-receiver latency and
capture-to-display latency require additional clock synchronization and
capture/presentation instrumentation; receive assembly time is neither of
those. The probe excludes codec and display costs and does not automatically
change encoder settings. UDP loss is reported without blocking video startup;
setup or control-protocol failure disconnects and follows the normal retry path.
The test server uses a temporary UDP source port, with the same negotiated
destination port that video will use on the client.

## Build

Install CMake 3.20 or newer, a C compiler, pkg-config, and the development
dependencies listed below. The server also needs `wayland-client`,
`wayland-scanner`, and `wlr-protocols`.

```sh
make          # configure and build both programs and tests
make test     # build and run the hardware-independent tests
```

The equivalent CMake commands are:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

Use `NETDISPLAY_BUILD_SERVER=OFF` or `NETDISPLAY_BUILD_CLIENT=OFF` as CMake
options to build only the other program. Set both to `OFF` to build just the
hardware-independent tests with libsodium. Debug builds use CMake's normal
Debug flags; optimizations are selected by the build type.

`make` accepts `BUILD_DIR` and `JOBS`. The build and installer scripts accept
`NETDISPLAY_BUILD_DIR` and `NETDISPLAY_JOBS` (default: 2).

GitHub Actions builds both binaries with GCC and Clang, runs the crypto, UDP
transport, bidirectional probe, and control session tests, and checks the
hardware-independent tests with AddressSanitizer and UndefinedBehaviorSanitizer.
It also checks shell scripts and builds both client-only and server-only
fallbacks from the packaged source archive. These checks require no GPU,
compositor, or display. The workflow can also be run manually.

## Source layout

- `src/`: C sources and internal headers; shared transport/authentication code
  is compiled once and linked into the binaries and tests.
- `tests/`: hardware-independent tests and their CMake targets.
- `config/`: example client and server configurations.
- `packaging/`: systemd user units and the uinput udev rule.
- `site/`: downloadable shell launcher.
- `build/` and `dist/`: ignored local build and website output.

## Package the website

```sh
./build-site.sh
```

`./build-website.sh`, `make site`, and `make website` use the same entry point.
Builds are incremental; `NETDISPLAY_SITE_DIR` overrides the default `dist/`
output directory.

`./build-site.sh` runs CMake, builds both programs, and creates only:

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

## Install

Install for the current user (without replacing an existing configuration):

```sh
./install.sh --server     # source computer
./install.sh --client     # receiver computer
```

The installer requires an explicit target and installs only the binary and
editable configuration by default. It never installs, enables, or starts a
systemd unit unless you explicitly add `--enable`:

```sh
./install.sh --server --enable
./install.sh --client --enable
```

The client acquires DRM master, so do not use `--enable` while a compositor is
using the same DRM device. `--no-start` remains accepted for compatibility and
has no effect because non-starting installation is the default.

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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DNETDISPLAY_BUILD_SERVER=OFF
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
cp config/client.conf.example \
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
