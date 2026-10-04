# SteamVR tracking/setup and latency patch

Applies to the supplied `NetDisplay-upload.tar(1).gz` tree, not the earlier iOS-only overlay.

## What was found

The dedicated VR sender is `vr/server.c`. It is not the desktop sender in
`src/video_sender.c`. The VR encoder sets `zerolatency=1` but omits FFmpeg's
independent NVENC `delay=0` option. This permits an output-surface backlog even
with the ultra-low-latency tuning preset. The VR sender also requests a 4 MiB
UDP send buffer and sends entire frames synchronously on the encoder loop,
without cancelling an obsolete frame. Those are concrete latency problems.
The requested socket size is not proof of the effective kernel buffer size or
of a particular measured delay.

The HMD driver already sends valid orientation and `Running_OK` for fresh motion;
3DoF alone does not explain the symptom. However, it declares tracking universe
20036 without supplying standing/seated origins or driver-provided chaperone.
It also omits its tracking-system name and conflates stale tracking with hardware
disconnection. Valve's driver API calls for a driver that supplies tracking to
supply its chaperone setup. The patch completes that integration and provides a
stationary origin rather than requiring the legacy room-setup wizard.

**The exact failed condition on the user's "Establish tracking" page is not
proven without a live runtime diagnostic.** This source review found a likely
integration blocker, not a captured reproduction of that UI. The diagnostic
below distinguishes a pose problem from a space/calibration or wizard problem.

## Patch contents

* Driver-provided stationary chaperone JSON, matching universe ID, configurable
  head height, tracking-system properties, separate connected/pose-valid flags,
  and a dedicated motion receive/publish thread instead of `RunFrame` polling.
* NVENC `delay=0` verified after initialization, explicit zero lookahead, all-IDR
  preservation, a bounded latest-frame TX worker with obsolete/deadline checks,
  a 128 KiB requested socket buffer, removal of one full-frame CPU copy, and
  local-stage timing/drop counters. No compression/encryption downgrade.
* Opt-in rear-camera ARKit position + orientation, authenticated version-2 pose
  packets, position/velocity consumption by the Linux HMD driver, and local
  demo translation. Existing 128-byte 3DoF packets remain supported.
* Read-only live diagnostics, configuration tuning without re-pairing, tests,
  and a CI build for the dedicated `vr/` backend (not just `steamvr/`).

No SteamVR installation or GitHub repository has been changed remotely. These
are source changes to build and test. Xcode/SteamVR/NVENC/iPhone runtime testing
was not available in the preparation environment; see `VR-VALIDATION.md`.

## First: apply the Linux fix with AR disabled

Back up or commit your current source changes. Extract the overlay at the
repository root, preserving its paths and hidden `.github` directory. Alternatively
use the complete patched source ZIP. Do not upload just the ZIP to GitHub and
expect it to be extracted.

Exit SteamVR and stop your running NetDisplay VR backend before rebuilding.
**Keep the same source location and VR build directory used by your installed
wrapper.** Its registration and private pairing files reference that directory.
Do not delete `session.json`, `installation.json`, or the package's `private/`
directory. Do not run `configure` again just to change resolution.

The example below assumes your existing build directory is `build-vr` and the
OpenVR SDK used by the original instructions is still in `/tmp/netdisplay-openvr-sdk`.
Substitute your existing paths when different:

```bash
# From the repository root, with SteamVR and the backend stopped:
cmake -S vr -B build-vr -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DOPENVR_INCLUDE_DIR=/tmp/netdisplay-openvr-sdk/headers
cmake --build build-vr --parallel 2
ctest --test-dir build-vr --output-on-failure

# Change 1.65 to your eye height above the real floor, in metres, in your
# intended starting posture. For a seated test, use seated eye height.
python3 build-vr/netdisplay-vr/netdisplay-vr tune \
  --head-height 1.65 --encoder h264_nvenc \
  --width 1280 --height 720 --fps 60 --qp 30

python3 build-vr/netdisplay-vr/netdisplay-vr run
```

The dependency set for building `vr/` on Debian/Ubuntu includes a C/C++ toolchain,
CMake, pkg-config, libsodium development files, FFmpeg avcodec/avutil/swscale
development files, Vulkan development files, libdrm development files and X11/XRandR
headers. The CI job lists the concrete package names. This is the dedicated VR
build, not `cmake -S steamvr` and not the normal desktop server.

Use the existing installed wrapper; an in-place rebuild does not require
`install` again. If you intentionally move the package, restore the old wrapper
using the old launcher before registering the new package. The installation
manager's backup/restore logic is unchanged.

In the iPhone app choose VR, 1280x720, 60 fps, and the existing VR control port
(default 5021), existing video credentials and motion token/port. Enable encrypted
video as required by the dedicated VR server. Leave **Rear-camera position** off.
Keep your previous USB IP addresses, authentication and connection settings.
Start SteamVR, connect the app, open the streaming viewer and recenter looking
straight ahead. This first test can use your existing iOS build; its NDM1 motion
packets remain compatible with the patched Linux driver.

Use the driver-provided stationary origin rather than trying to finish room-scale
calibration. It declares a 1 m x 1 m nominal play area but **no measured collision
bounds**. This is neither a surveyed safe space nor a guardian system. Test seated,
keep the cable clear and stop when uncomfortable.

## Read the actual runtime state

With SteamVR running and phone motion streaming:

```bash
python3 build-vr/netdisplay-vr/netdisplay-vr status
python3 build-vr/netdisplay-vr/netdisplay-vr diagnose \
  --openvr-library /tmp/netdisplay-openvr-sdk/lib/linux64/libopenvr_api.so
```

The diagnostic is read-only. It uses the OpenVR client API and does **not** bind
UDP 5010, so it can run alongside the driver. Do not simultaneously launch the
standalone Python pose receiver on the driver's motion port.

For fresh 3DoF motion, expected results are:

```text
trackingSystem=netdisplay_vr
universe=20036
providedChaperoneBytes=<nonzero> propertyError=0
raw connected=1 valid=1 result=200 ...
standing connected=1 valid=1 result=200 ...
seated connected=1 valid=1 result=200 ...
```

These are expected patterns, not outputs measured on your system. `result=200`
is OpenVR `Running_OK`. Raw and standing Y should be near configured head height;
seated Y should be near zero before additional runtime recentering.

| Observed result | Interpretation / next check |
|---|---|
| Wrong tracking system, universe, or no chaperone property | Old/wrong driver loaded, registration path, blocked add-on, or global settings overriding driver defaults. Rebuild the actual installed package and restart SteamVR. |
| Raw pose invalid or `NDVR ... fresh=0 valid=0` | Motion is absent/stale, wrong peer/token/port, app in background, another process owns the UDP port, or tracking explicitly failed. |
| Raw valid but standing/seated invalid | Investigate runtime origin/chaperone integration and reported calibration state. |
| All three valid, result 200, but wizard still waits | Motion is not absent. The remaining issue is wizard/runtime setup behavior; keep the supplied stationary origin and retain the diagnostic output. |
| AR mode: packets arrive but `ar=1 valid=0` | Camera tracking is limited; expose the cameras, improve lighting/features, then explicitly recenter. Do not turn invalid tracking into a fabricated position. |

`tune` warns about conflicting `driver_netdisplay_vr` values in the global
SteamVR settings file. It deliberately does not erase global configuration or
unrelated calibration data.

## Understand the latency counters

At backend startup, expect `VR NVENC verified delay=0 ...`. Every few seconds the
backend logs submission fps, UDP throughput, GPU readback/fence time, raw mailbox
age, conversion/encoding/TX durations, and completed/replaced/aborted frames.
The socket size log reports the **actual** `SO_SNDBUF` returned by the kernel.

The iPhone's diagnostics report assembly, parse/decode, first-packet-to-GPU-submit,
decoder replacement/error counters and thermal state. Compare 3DoF with AR off
first. Raise resolution only after the baseline is stable.

* Growing TX aborted/replaced counts: encoding output is outrunning transport or
  system scheduling. Reduce resolution or increase QP (lower visual quality).
  All-IDR is intentionally retained so dropping a frame does not corrupt a
  prediction chain. The sender has no automatic bitrate adaptation yet.
* High readback/convert/encode times: the remaining GPU-to-CPU readback and CPU
  color conversion path needs attention. The patch is not zero-copy NVENC.
* Low host/network times but a slow streamed head response: no client-side
  rotational timewarp is implemented, and display/presentation timing remains.
* A new frame cannot remove bytes already handed to the kernel/USB driver. The
  bounded application queue is not a guarantee of bounded end-to-end delay.

Do not add unsynchronized phone/Linux timestamps to claim one-way or
motion-to-photon latency. The patch measures local stages only. No numerical
end-to-end latency result is claimed for this hardware.

## Optional rear-camera 6DoF

Build the updated iOS app using the existing GitHub macOS/unsigned-IPA workflow
and install it with your existing free-signing procedure. The bundle identity and
Keychain service are unchanged; an in-place installation should preserve pairing,
but re-check the displayed token if the installer changes the signing identity.
The camera path uses normal iOS ARKit APIs and no new paid capability or entitlement.

In **Viewer & motion -> Motion to Linux**, enable **Rear-camera position
(experimental)** while disconnected, then allow Camera permission. First test
**Motion room** offline: expose the rear lenses to a well-lit, textured environment,
wait for **AR normal**, and recenter while looking straight ahead. Shift slightly
left/right/up/down and confirm local parallax and the position readout. Then test
streamed SteamVR. In normal tracking the driver reports `mode=6dof ar=2 valid=1`.

The rear cameras must look outward through the Cobra viewer. A solid cover over
the camera area prevents usable visual tracking. The app does not require a
LiDAR depth stream and does not enable plane detection, meshing or camera video
transmission. Frames stay local; only pose data is sent.

The initial normal pose/recenter anchors relative position at your configured
head height; floor height is **manual**, not automatically measured from AR planes.
Gravity is preserved by yaw-only AR recentering. Optional camera-to-head offsets
are in metres: +X screen right, +Y screen up, +Z toward the wearer. A zero offset
tracks the camera rather than the physical eye midpoint. Calibrate that offset
before treating close-range translation as accurate.

Camera tracking adds compute/thermal load, uses camera-cadence pose updates rather
than the Core Motion-only rate, can drift and can lose its map. Limited tracking,
interruptions and detected jumps produce invalid poses. After tracking recovers,
**explicit recentering is required** instead of silently moving the origin. There
is no automatic switch to fake 6DoF from integrated accelerometer data.

This remains an experimental stationary/limited-translation viewer, not a
calibrated room-scale headset or safety-boundary system. No controllers,
gaze tracking, foveation, camera passthrough UI or iPhone late timewarp are added.

## Reference specifications

* Valve driver API, chaperone and pose conventions:
  https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md
* FFmpeg NVENC `delay` versus `zerolatency` options and output queue:
  https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/nvenc_h264.c
  https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/nvenc.c
* Apple AR world tracking and tracking-quality lifecycle:
  https://developer.apple.com/documentation/arkit/arworldtrackingconfiguration
  https://developer.apple.com/documentation/arkit/managing-session-life-cycle-and-tracking-quality
