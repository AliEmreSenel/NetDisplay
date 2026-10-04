# NetDisplay SteamVR HMD driver (Linux)

This is a loadable OpenVR server driver, with HMD registration, stereo display
viewports/projection/IPD, pose updates, standby, disconnect handling and the
same authenticated motion protocol as the iOS app. It emulates a **3DoF HMD**;
position is fixed at `(0, 1.6, 0)` metres. It does not emulate tracked controllers.

The display path is SteamVR's **extended desktop compositor window** → the
NetDisplay Hyprland headless output → the existing encoder → iPhone VR viewer.
The driver reports a virtual desktop display and identity distortion; optional
lens correction is applied on the phone. This is not a direct-mode GPU driver,
OpenXR runtime, or asynchronous timewarp implementation. SteamVR supplies the
runtime and game compositor. Applications requiring positional tracking or VR
controllers need other input hardware/drivers.

The driver builds and its automated checks run on Linux. A live SteamVR /
Hyprland / iPhone session has **not** been validated. Extended-display support
and window placement must be verified on the target SteamVR/compositor versions;
a successful driver build alone does not establish game compatibility.

**Confirmed limitation (2026-10-04):** SteamVR 2.17.10 on the tested Hyprland
Wayland session loads this driver and selects `netdisplay-ios-hmd`, but its
compositor rejects the headless display with `VR requires direct mode` and
`VRInitError_Compositor_CannotDRMLeaseDisplay`. Both `steamvr.displayDebug` and
the driver's `Prop_DisplayDebugMode_Bool` were tested and did not bypass this
requirement. This configuration cannot render VR through the extended-desktop
path above. It needs a different compositor integration (such as a driver that
receives rendered frames directly), not additional phone pairing or room setup.

## Build

Install a C++17 compiler, CMake, pkg-config and libsodium development files.
Use Valve's SDK at the tested revision (SDK files are not vendored):

```sh
git clone https://github.com/ValveSoftware/openvr.git /tmp/netdisplay-openvr-sdk
git -C /tmp/netdisplay-openvr-sdk checkout 0924064316de3effbcd1acf1e309182a2deb1c05
cmake -S steamvr -B build-steamvr \
  -DOPENVR_INCLUDE_DIR=/tmp/netdisplay-openvr-sdk/headers
cmake --build build-steamvr --parallel
ctest --test-dir build-steamvr --output-on-failure
```

The complete driver package is `build-steamvr/netdisplay`, including
`bin/linux64/driver_netdisplay.so`, its bundled libsodium dependency, manifest and
default settings. Keep the complete package at a stable absolute path when
registering it. Rebuilding preserves the build package's existing settings.
The bundled library must be compatible with SteamVR's runtime; a host-only
successful load does not establish runtime compatibility.

## Pair and configure

1. On iOS select **Viewer → VR / SteamVR**, **Side-by-side stereo**, enable motion
   UDP, and copy the motion pairing token. Save the 64 hex characters to a file
   on Linux with permissions `0600`. The token is separate from the video password.
2. Connect iOS to NetDisplay at **1920×1080, 60 fps** initially. Leave this
   connection running so the named Hyprland output exists. Obtain its position
   with `hyprctl -j monitors all`. Use output scale 1 and no rotation.
3. Edit `build-steamvr/netdisplay/resources/settings/default.vrsettings`:
   - `bindAddress`: Linux USB/hotspot IPv4 address.
   - `peerAddress`: iPhone IPv4 address shown under Route and output.
   - `tokenFile`: absolute path to the private token file (no `~` expansion).
   - `motionPort`: match iOS, normally 5010. Stop `pose_demo.py` and
     `pose_receiver.py` first; only one receiver can bind this port.
   - `width`, `height`, `frequency`: match the negotiated video stream.
   - `windowX`, `windowY`: desktop position of that headless output.
   - `ipd`: eye separation in metres; `verticalFov`: per-eye vertical degrees.
     These are manual parameters, not a calibrated optical profile.
4. With SteamVR stopped, register the absolute package path:
   ```sh
   "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/vrpathreg" \
     adddriver "$(realpath build-steamvr/netdisplay)"
   ```
   Adjust the Steam library path if necessary. Enable NetDisplay in SteamVR's
   add-on settings. If another HMD wins selection, set `steamvr.forcedDriver` to
   `netdisplay` in your SteamVR settings and restart. Existing `driver_netdisplay`
   overrides in `steamvr.vrsettings` take precedence over package defaults.
5. Start SteamVR. Its extended compositor window must fill the streamed output
   with **both eyes**, not a single-eye desktop mirror. Move/fullscreen it on that
   output if the compositor does not honor the configured window position.
   Open the iOS viewer and recenter before wearing the headset. Complete SteamVR's
   seated setup as needed. Use a desktop keyboard/gamepad or separately tracked
   controllers for applications that require input.

If tracking works but video does not, inspect the desktop compositor window and
SteamVR logs before changing iOS decoding. If SteamVR cannot create an extended
compositor window on the target system, this display path is unavailable there;
this driver does not silently substitute a flat desktop or claim direct-mode
support. Reconnect creates a new output; update its bounds and restart SteamVR.

To unregister, stop SteamVR and run `vrpathreg removedriver ABSOLUTE_PACKAGE_PATH`.
Remove any `forcedDriver` override you added. This leaves other drivers intact.

## Protocol and lifecycle

Only exact 128-byte NDM1 packets from the configured peer are accepted. HMAC,
header, finite values, quaternion norm, sequence and timestamps are checked.
A new session is accepted after one second of quiet; recently retired sessions
are rejected. Replay memory is bounded to 64 retired sessions and resets when
the driver restarts. Pair over a trusted local network.

The driver publishes invalid/disconnected tracking after 250 ms without a valid
sample, and does not predict using stale angular velocity. Phone timestamps are
not assumed synchronized with Linux: pose age uses local receipt time, excluding
unknown network delay. Core Motion local angular velocity is rotated into the
OpenVR driver coordinate frame. No acceleration is integrated into fake position.

Tests cover authentication/tampering, malformed data, quaternion validation,
replay/session transitions, tracking freshness and the exported driver factory.
Apple SDK compilation, physical touch routing and SteamVR rendering still need
the checks in [iOS validation](../ios/docs/VALIDATION.md).

Interface reference: [Valve's driver documentation](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/docs/Driver_API_Documentation.md).
