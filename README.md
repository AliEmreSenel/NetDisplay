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

The launcher compiles the published source bundle, installing missing build
packages through the distro package manager if needed. It runs from a
temporary directory and creates
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
frame rate and the bounded low-latency pipeline are preserved.
QP scales differ by codec/backend; the supported configuration range is 0–51.
The budget excludes UDP/IP, fragmentation and encryption overhead; frame bursts
and encoder overshoot can exceed it briefly. This mode does not add packet
pacing or adapt the budget to network feedback. Restart the server to switch
modes. A backend that cannot open the requested mode is skipped during probing
rather than silently using another rate-control mode.

`keyframe_interval=3` is the default: one independent keyframe followed by two
P-frames (`I P P`). Set it to `1` for all-intra or `2` for `I P`; the supported
range is 1–60 frames. Longer intervals reduce keyframe overhead but increase
recovery time after packet loss. At 60 fps, interval 3 provides a recovery
opportunity every 50 ms; losing that keyframe can extend the wait. There are no
B-frames or lookahead. Older receivers that do not advertise prediction support
automatically receive the original all-intra format. Restart the server after
changing the interval.

The dedicated VR backend supports the same choice without re-pairing:

```sh
python3 build-vr/netdisplay-vr/netdisplay-vr tune --rate-control bandwidth --bitrate-kbps 20000
python3 build-vr/netdisplay-vr/netdisplay-vr tune --rate-control qp --qp 28
python3 build-vr/netdisplay-vr/netdisplay-vr tune --keyframe-interval 3
```

Run these commands with SteamVR stopped, then restart the VR backend and SteamVR.
The iOS receiver needs no changes.

With no `display=` entries, the receiver uses every connected display at its
preferred mode. Repeat the option to select displays or modes:

```ini
display=eDP-1,1920x1080@60
display=HDMI-A-1,2560x1440@144
```

The default server config creates and removes Hyprland headless outputs with
local `hyprctl` hooks. Receivers cannot send arbitrary commands. On `SIGTERM`
(`systemctl --user stop` or `restart`) the server ends every session and runs
its `disconnect_cmd` before exiting. At startup it runs `disconnect_cmd`, with
`ND_STALE=1`, for any output that still carries its `output=` prefix, since
only a server that crashed can have left one behind.

The server exits when its Wayland compositor goes away, and refuses to start
while `HYPRLAND_INSTANCE_SIGNATURE` names a Hyprland instance that is not
running. The service then restarts it until the new session has exported its
environment, instead of leaving it attached to a dead compositor.

For interactive authentication, generate a key and put it in
`password_key=` on the server:

```sh
netdisplay-server --derive-password-key
```

The output is a random per-server salt and the Argon2id key derived from it.
Keys from older versions have no salt; they keep working, but the server warns
until you regenerate them. The client prompts for the password; plaintext is
never sent or stored. For an unattended client, put the same 32-byte PSK on
both machines and configure `psk_file` instead. Frame encryption can then be
allowed or required by the server.

With a password or PSK, the two sides authenticate each other with CPace, a
password-authenticated key exchange, so a captured or impersonated handshake
gives an attacker nothing to test password guesses against offline. Every
control message after the handshake, including forwarded keystrokes, is
encrypted and authenticated with keys unique to that session. Video keys come
from the same exchange. Without a password or PSK the control channel is
plaintext.

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

Stream statistics are logged once a minute, or every second when stderr is a
terminal. Set `NETDISPLAY_STATS_INTERVAL` to a number of seconds to change
that, or to 0 to turn them off.

To measure the link during initial setup, run this on the receiver with the
source running:

```sh
netdisplay-client --network-test
```

Pass a config path after `--network-test` to use a specific source or interface.
The command runs one bidirectional UDP test over the configured link and exits.
It uses the same authentication and encryption settings as streaming and
reports verified throughput, loss, corruption, RTT, and receive-assembly time.
Both peers use ephemeral UDP ports for this test. Regular streaming connections
skip the test, including reconnects and multi-display sessions. The standalone
command requires the updated source and receiver.

For the hosted launcher, run:

```sh
curl -fsSL https://nd.myc.li/ | sh -s -- --network-test
```

## Self-hosting

Build the same static bundle served by `nd.myc.li`, embedding your public URL:

```sh
NETDISPLAY_PUBLIC_URL=https://netdisplay.example.com ./build-site.sh
```

Serve `dist/` as the document root and return `index.html` for `/`. The
bundle contains the launcher, source archive, GPL license, and example configs.

Use `NETDISPLAY_SITE_DIR` to change the output directory. During testing, a
launcher can use another artifact origin without being rebuilt:

```sh
curl -fsSL https://staging.example.com/ | \
  NETDISPLAY_BASE_URL=https://staging.example.com sh
```

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

Desktop and VR use the same v7 control protocol, password-authenticated key
exchange, sealed control channel, stream-key derivation, and fragmented UDP
video format. Capability flags
negotiate optional features; VR requires authenticated, encrypted video and a
matching stereo mode. Capture, device control, rendering, and head tracking
remain in their platform-specific components.

`src/crypto.*` implements the key exchange and message sealing on every
platform, including iOS; `src/session.*` runs them over a control socket.
`src/encoder.*` owns hardware encoder selection and low-latency configuration.
`src/video_tx.*` owns the bounded transmit worker: up to three dependent access
units preserve decode order, while a newer keyframe supersedes queued work.
A send expires one frame period after publication, including queue residence.
Local drops/errors abandon the affected chain and request an early keyframe.
Receivers also bound pending compressed video to three frames, decode required
references, and skip broken chains until the next keyframe. Rendering can skip
decoded images independently. During predicted video, the receiver can keep
one incomplete predecessor for up to 4 ms after the next frame starts arriving.
It releases decodable frames immediately, and a complete newer keyframe
supersedes the older chain. Network-loss recovery uses the periodic keyframes.
`src/frame_transport.*` sends datagrams; `src/frame_receiver.*` validates,
reassembles, and decrypts them on both Linux and iOS. The iOS native bridge
only adapts that shared receiver to Swift. Negotiated predicted video uses an
ND02 envelope with keyframe/dependency metadata inside video encryption; legacy
all-intra video and network probes retain ND01 framing.

`src/control_proto.h` and `src/proto.h` are the wire definitions. After changing
protocol constants, run `python3 tools/generate-wire.py` to update the checked-in
Swift constants, then `python3 ios/scripts/generate_project.py` if adding
sources. Tests check generated constants, mutual authentication and encryption
policy, transmit cancellation/replacement, and real encoded video passing
through both receiver interfaces. Protocol v7 does not interoperate with v6:
update the source and every receiver, including the iOS app, together.

The integrated video tests compare intervals 1, 2 and 3 on a textured background
with a moving window, inject lost keyframes/P-frames/fragments, and compare
recovered decoded pixels with an uninterrupted decoder. To exercise an available
H.264 hardware backend too, run e.g.
`ND_TEST_ENCODER=h264_nvenc build/tests/netdisplay-stream-test` or substitute
`h264_vaapi`. These synthetic encoded-byte measurements do not measure screen
capture, physical network throughput, display latency, or iPhone decoding.

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
