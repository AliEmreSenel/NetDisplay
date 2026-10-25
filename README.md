# NetDisplay

For the native iOS receiver and dedicated SteamVR backend, see [VR tracking and latency fixes](VR-TRACKING-FIX.md).

I built NetDisplay to use another Linux computer as a low-latency network
display for my Hyprland desktop. One source can drive multiple receivers, and
each receiver can expose multiple physical displays. Video, input, DPMS,
brightness, batteries, and chargers can all be forwarded.

The original desktop server/client are Linux-specific. The desktop source requires Hyprland and a supported
FFmpeg hardware encoder; the receiver uses DRM/KMS and VAAPI and normally runs
from a dedicated VT. Video is negotiated per session in AV1 -> HEVC/H.265 ->
H.264 order, using only codecs both endpoints advertise as hardware-capable.

## Use it

I host a public launcher and artifact mirror at `https://nd.myc.li`. On the
receiver, switch to a VT and run:

```sh
curl -fsSL https://nd.myc.li/ | sh
```

On the Hyprland source computer, run:

```sh
curl -fsSL https://nd.myc.li/ | sh -s -- --server
```

The launcher tries a prebuilt binary and falls back to compiling the published
source bundle when necessary. It runs from a temporary directory and creates
an editable config under
`${XDG_CONFIG_HOME:-$HOME/.config}/netdisplay/`. A new server config gets a
random password; the launcher prints it once and stores only its derived key.

You can use my hosted service or [deploy the same bundle yourself](#self-hosting).

## Install locally

On Debian or Ubuntu, install the common build tools and the dependencies for
the component you need:

```sh
sudo apt install build-essential cmake pkg-config libsodium-dev

# Source
sudo apt install libwayland-dev libwayland-bin libavcodec-dev libavutil-dev libswscale-dev

# Receiver
sudo apt install libdrm-dev libavcodec-dev libavutil-dev libswscale-dev libva-dev
```

Then run the guided installer:

```sh
./install.sh
```

It asks which component to install, whether to enable its systemd user service,
and whether to configure optional privileged features. Existing configs are
preserved. For scripts or unattended setup:

```sh
./install.sh server
./install.sh client
```

Add `--enable` to install and start the matching user service. The receiver
must acquire DRM master, so do not start its service while another compositor
is using the same DRM device. Run `./install.sh --help` for every option.

## Configure it

The default files are:

```text
~/.config/netdisplay/server.conf
~/.config/netdisplay/client.conf
```

The examples in `config/` document every setting. Discovery works
automatically on a normal LAN. To connect directly, set the source address in
`client.conf`:

```ini
host=192.168.1.20
```

On the source, `video_codec=auto` prefers AV1, then HEVC, then H.264. Set it to
`av1`, `hevc` (or `h265`), or `h264` to force that codec; a forced codec rejects
incompatible receivers instead of silently downgrading. Encoder backends are
probed at runtime (for example NVENC, QSV, AMF, or VAAPI), and the capture conversion
uses the pixel format selected by the backend rather than a fixed NV12 wire
assumption.

Two encoder rate-control modes are available in `server.conf`. The default
preserves fixed QP; lower QP gives higher quality and larger frames:

```ini
rate_control=qp
qp=24
```

To let the encoder vary QP with scene complexity within a bandwidth budget:

```ini
rate_control=bandwidth
bitrate_kbps=20000
```

`bitrate_kbps` is the peak encoded-video rate in decimal kbit/s **per stream**
(20000 = 20 Mbit/s). Capped VBR targets 80% of that peak and requests a VBV
reservoir of two frame periods. `qp` is ignored in bandwidth mode. Resolution,
frame rate, all-intra encoding, and latest-frame replacement are preserved.
QP scales differ by codec/backend; the supported configuration range is 0–51.
The budget excludes UDP/IP, fragmentation and encryption overhead; frame bursts
and encoder overshoot can exceed it briefly. This mode does not add packet
pacing or adapt the budget to network feedback. Restart the server to switch
modes. A backend that cannot open the requested mode is skipped during probing
rather than silently using another rate-control mode.

The dedicated VR backend supports the same choice without re-pairing:

```sh
python3 build-vr/netdisplay-vr/netdisplay-vr tune --rate-control bandwidth --bitrate-kbps 20000
python3 build-vr/netdisplay-vr/netdisplay-vr tune --rate-control qp --qp 28
```

Run either command with SteamVR stopped, then restart the VR backend and SteamVR.
The iOS receiver needs no changes.

With no `display=` entries, the receiver uses every connected display at its
preferred mode. Repeat the option to select displays or modes:

```ini
display=eDP-1,1920x1080@60
display=HDMI-A-1,2560x1440@144
```

The default server config creates and removes Hyprland headless outputs with
local `hyprctl` hooks. Before creating an output it removes only the exact
session-derived name, which also cleans up stale headless outputs left after an
unclean daemon restart. Receivers cannot send arbitrary commands.

For interactive authentication, generate a key and put it in
`password_key=` on the server:

```sh
netdisplay-server --derive-password-key
```

The client prompts for the password; plaintext is never sent or stored. For an
unattended client, put the same 32-byte PSK on both machines and configure
`psk_file` instead. Frame encryption can then be allowed or required by the
server.

## Optional features

### Input forwarding

Set `input_enabled=1` on the server and `want_input=1` on the client. Allow
the active source user to create virtual input devices with:

```sh
./install.sh server --with-input-access
```

The receiver needs access to `/dev/input/event*`. Press and release
**Escape five times** to stop it and restore the terminal. Input devices can be
connected and removed while it is running.

### Batteries and chargers

The optional GPLv2 DKMS module exposes receiver batteries and chargers as
native power-supply devices on the source:

```sh
./install.sh server --with-power-module
```

Install DKMS and matching kernel headers first. Video still works without the
module. See [kernel/README.md](kernel/README.md) for Secure Boot, permissions,
testing, and removal.

### Display state and network probe

The source mirrors DPMS and normalized backlight brightness to receivers when
the required interfaces are available. Missing DPMS or backlight support does
not stop video.

Before streaming, peers run a short bidirectional UDP probe using the same
transport and encryption path as video. It reports verified throughput, loss,
corruption, RTT, and receive-assembly time without changing encoder settings
or blocking startup on packet loss.

## Self-hosting

Build the same static bundle served by `nd.myc.li`, embedding your public URL:

```sh
NETDISPLAY_PUBLIC_URL=https://netdisplay.example.com ./build-site.sh
```

Serve `dist/` as the document root and return `index.html` for `/`. The
bundle contains the launcher, source fallback, GPL license, example configs,
and binaries for the build machine's architecture.

Use `NETDISPLAY_SITE_DIR` to change the output directory. During testing, a
launcher can use another artifact origin without being rebuilt:

```sh
curl -fsSL https://staging.example.com/ | \
  NETDISPLAY_BASE_URL=https://staging.example.com sh
```

To build a mostly-static receiver from an existing static-library prefix:

```sh
NETDISPLAY_STATIC_CLIENT_PREFIX=/path/to/prefix ./build-site.sh
```

libva, glibc, and the vendor VA driver remain dynamic and come from the
receiver.

## Build and test

```sh
make
make test
```

Or use CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
```

Set `NETDISPLAY_BUILD_SERVER=OFF` or `NETDISPLAY_BUILD_CLIENT=OFF` to build
only the other component.

The root build also supports `NETDISPLAY_BUILD_VR=ON` for the dedicated VR
backend/capture layer and `NETDISPLAY_BUILD_STEAMVR=ON` for the standalone HMD
driver. Both require `OPENVR_INCLUDE_DIR`; the VR backend also needs Vulkan
headers, libdrm, X11/XRandR headers, and FFmpeg. All enabled components reuse
the same compiled protocol, transport, and encoder libraries.

With Ninja installed, presets provide separate incremental build directories
for `desktop`, `core` (hardware-independent tests), `vr`, and `all`:

```sh
cmake --preset desktop
cmake --build --preset desktop --parallel
ctest --preset desktop

# Build desktop, dedicated VR, and the standalone driver together:
OPENVR_INCLUDE_DIR=/path/to/openvr/headers cmake --preset all
cmake --build --preset all --parallel
ctest --preset all
```

Add `-DNETDISPLAY_ENABLE_IPO=ON` at configure time to enable release link-time
optimization after checking compiler support. Compiler caches can be enabled
with standard CMake options such as `-DCMAKE_C_COMPILER_LAUNCHER=ccache` and
`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`. `make` detects the CPU count; set `JOBS`
to limit concurrency and `CMAKE_ARGS` to pass additional configure options.

The existing `cmake -S vr` and `cmake -S steamvr` entry points remain supported.
They use the same shared target definitions as the root build. iOS continues
to build through Xcode.

## Shared streaming core

Desktop and VR use the same v6 control protocol, authentication proofs,
stream-key derivation, and fragmented UDP video format. Capability flags
negotiate optional features; VR requires authenticated, encrypted video and a
matching stereo mode. Capture, device control, rendering, and head tracking
remain in their platform-specific components.

`src/session.*` implements the native authentication exchange.
`src/encoder.*` owns hardware encoder selection and low-latency configuration.
`src/video_tx.*` owns the single-slot transmit worker: newer independent
frames replace pending frames and a send expires after one frame interval.
`src/frame_transport.*` sends datagrams; `src/frame_receiver.*` validates,
reassembles, and decrypts them on both Linux and iOS. The iOS native bridge
only adapts that shared receiver to Swift.

`src/control_proto.h` and `src/proto.h` are the wire definitions. After changing
protocol constants, run `python3 tools/generate-wire.py` to update the checked-in
Swift constants, then `python3 ios/scripts/generate_project.py` if adding
sources. Tests check generated constants, mutual authentication and encryption
policy, transmit cancellation/replacement, and real encoded video passing
through both receiver interfaces. Wire compatibility stays at v6.

Default ports are UDP 5000 onward for video, UDP 5001 for discovery, and TCP
5001 for control, authentication, and optional input.

## License

NetDisplay is licensed under the [GNU General Public License version 2
only](LICENSE). The Wayland protocol definitions under `protocols/` retain
their included MIT licenses.

### iOS viewer modes

The iOS viewer offers **Touch Display** (direct multitouch returned over the
control connection) and **VR / SteamVR** (stereo video with authenticated head
rotation). See [iOS setup](ios/README.md#touch-display) and the
[Linux SteamVR HMD driver](steamvr/README.md) for build/pairing instructions and
current hardware-validation limits.
