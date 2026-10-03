# Native NetDisplay iOS receiver

**Target:** real arm64 iPhones, iOS 17 or newer; designed around the user's iPhone 14. No simulator target. iOS 17 is the minimum because the public hardware-decoder requirement/reporting keys are available there. Native SwiftUI shell, VideoToolbox decoding, Metal rendering, Core Motion orientation, and direct BSD sockets. No browser, WebRTC relay, subscription, camera access, or paid Apple capability is required by the app code.

Start with [the repository quickstart](../QUICKSTART-IOS.md). Read [validation](docs/VALIDATION.md) before treating this as tested headset software.

## Included implementation

| Area | Implementation |
|---|---|
| Control | NetDisplay v6 HELLO/CHALLENGE/AUTH/DISPLAY/WELCOME/STREAM/READY, heartbeat, strict lengths and version checks |
| Authentication | Existing repository password-derived key and PSK proof functions; mutual server/client proof |
| Video protection | Optional original XChaCha20-Poly1305 frame encryption, no silent downgrade |
| Video network | Manual IPv4; UDP bound to the iPhone address used by TCP; validates remote IP/session/fragment sizes |
| Decoding | H.264 first; optional hardware HEVC, no AV1 advertisement; Annex-B conversion to VideoToolbox samples |
| Queues | One assembling frame, one pending compressed frame, one synchronous decode, one latest decoded image, one GPU command buffer in flight |
| Viewer modes | Touch Display with direct multitouch forwarding, or VR / SteamVR |
| Display | One stream; flat, duplicated mono, or side-by-side stereo; manual optical controls and eye swap |
| Test scenes | Calibration grid and a local orientation-driven room |
| Head motion | Relative quaternion, raw Core Motion quaternion, angular velocity, user acceleration, timestamps, sequence, session, recenter generation |
| SteamVR | Linux OpenVR HMD driver, stereo display geometry, authenticated 3DoF poses; see [setup](../steamvr/README.md) |
| Motion transport | Independent HMAC-authenticated UDP; companion Python receiver and Linux stereo test renderer |
| Diagnostics | Local pipeline timings and counters, observed motion rate, thermal state, explicit user-shareable report |
| CI | Standard GitHub macOS runner builds an unsigned IPA without Apple secrets |

The application is fully implemented source, not just view stubs. However, the **Apple-platform compilation and physical-device integration remain unverified** until the supplied CI and on-device checks have been run.

## Architecture

```text
Linux renderer -> Hyprland headless output -> existing NetDisplay encoder
   -> existing UDP framing (optionally encrypted) -> USB IP network
   -> native reassembly -> one-slot decoder mailbox
   -> VideoToolbox hardware decoder -> NV12 CVPixelBuffer
   -> CVMetalTextureCache -> Metal eye splitting / manual lens correction

Core Motion -> relative orientation + gyro + timestamps -> HMAC UDP
   -> tools/ios/pose_receiver.py or a renderer using motion_protocol.py
```

The app uses **the existing `../src/crypto.c` unchanged**. It does not duplicate or reinterpret the authentication KDF/proof implementation in Swift. The native bridge and v6 structs are compiled together. The historical `NetDisplay-v5-*` crypto domain labels are retained because that is what the v6 server uses; changing the labels would break interoperability.

### Deliberate low-latency boundaries

The current sender produces intra frames, so stale complete compressed frames can be replaced without maintaining a prediction chain. If the host later switches to ordinary P/B-frame inter prediction, this policy must be redesigned around decoder reference dependencies. Do not assume arbitrary frame dropping is safe for an inter-frame stream.

Decoding is synchronous on a dedicated queue, with temporal-processing/asynchronous flags unset. Pending input is replaceable and there is no AVPlayer playback buffer. A decoded pixel buffer is mapped into Metal without a CPU RGB image conversion. Texture/pixel-buffer lifetimes extend through GPU completion. The renderer uses two drawable buffers and one submitted command buffer at a time. These are implementation choices, not a demonstrated latency guarantee.

The source still performs its existing capture/encode work. This package does not add direct renderer-to-NVENC zero-copy capture, predict headset pose, or perform asynchronous timewarp. Streaming a head-tracked source without synchronized source-pose metadata is not sufficient to implement correct client-side reprojection.

### Display and optics

One source frame is decoded. In side-by-side mode its left half is the left eye, its right half the right eye; eye swap is optional. Flat and duplicated-mono modes preserve aspect ratio. The lens warp is a manually adjustable radial sampling correction, not a calibrated Cobra or certified Cardboard profile. There is no chromatic-aberration model, automatic IPD detection, viewer-QR import, or claim of distortion accuracy.

The local room's illustrative stereo separation is 64 mm. The Linux demo has `--ipd-mm` and `--fov`; tune them to the viewer/user. The image-center adjustment in the app does not alter the renderer's stereo camera baseline.

### Lifecycle

Network and motion work run off the UI thread. UI state and Metal-view lifecycle are main-actor isolated. Disconnect cancels worker sockets safely; frame generations prevent an old decoder callback from repopulating a new session. Backgrounding stops video/motion instead of attempting unsupported background VR. No automatic reconnect is enabled. Reconnect explicitly after changing the cable, address, permissions, or stream settings.

Motion starts locally for viewer tests; packets are sent only once the authenticated video control connection is established. The UDP motion token is separate from the NetDisplay server password. Normal settings are stored in UserDefaults, the motion token in a local device-only Keychain item, and passwords are kept only for the running app session.

## Build

The checked-in Xcode project is generated deterministically by Python, with no XcodeGen, CocoaPods, or third-party Swift packages:

```bash
# Regenerate the project after adding/removing Swift/C/Metal source files.
python3 ios/scripts/generate_project.py

# On macOS with Xcode, Python 3 and Homebrew:
brew install libsodium minisign pkgconf
bash ios/scripts/test.sh
bash ios/scripts/build-ipa.sh
python3 ios/scripts/validate_ipa.py ios/build/NetDisplay-unsigned.ipa
```

On Linux, use `.github/workflows/ios.yml` for the Apple build. Merely having the Swift compiler on Linux is not enough to compile Apple's UIKit/Metal/VideoToolbox frameworks.

The workflow compiles for `generic/platform=iOS`, arm64, with signing disabled. It outputs `ios/build/NetDisplay-unsigned.ipa`. Signing is performed afterward by the user's local sideloading tool/account. No certificates or credentials are embedded in the workflow.

The project references `ios/build/sodium/iphoneos-arm64/lib/libsodium.a`. `prepare-deps.sh` downloads **libsodium 1.0.22**, checks its official Minisign signature with the pinned upstream public key before extracting it, and builds an iPhoneOS static library. Release source and binaries are not vendored in this ZIP. The signed upstream source, network access, and Xcode toolchain are required at build time. Source SHA-256 is recorded as an artifact. Do not disable signature verification to work around a network failure.

The app's minimum deployment target is 17.0, but use Xcode 16 or newer for the supplied source/CI settings. No App Store/TestFlight submission workflow is included. Free-account sideloading limitations are documented in the quickstart.

## Portable tests

```bash
# Linux: install Swift plus your distro's C compiler, Python 3,
# pkg-config and libsodium development headers first.
bash ios/scripts/test.sh
```

Tests cover v6 packing, field validation, AVC/HEVC Annex-B parsing, quaternion coordinate changes, encrypted fragmentation/reassembly, tamper rejection, and real Swift -> C HMAC -> Python motion interoperability over a loopback UDP socket. The script's final Swift pass parses iOS source syntax only; it does not typecheck against the Apple SDK on Linux. The full `xcodebuild` step is separate. An optional Linux GUI smoke test is available as `xvfb-run -a python3 ios/Tests/demo_smoke.py` when tkinter and Xvfb are installed.

## Files

```text
ios/App/                         SwiftUI, sockets, VT decoder, Metal, Core Motion
ios/Core/                        portable wire, NAL and quaternion code
ios/Native/                      reassembler / crypto bridge
ios/Tests/                       Swift + C + Python interoperability tests
ios/scripts/                     project generation, deps, build and checks
ios/NetDisplay.xcodeproj/         shared build scheme
ios/docs/                        setup caveats, protocol and validation
../tools/ios/                    host motion receiver and stereo demo
../.github/workflows/ios.yml      unsigned IPA build
```

## Security and privacy boundaries

No analytics, cloud telemetry, microphone, camera, eye tracking, or file-upload service is added. Video and motion stay on the peer IP selected by the user. A diagnostic report is shared only when the user explicitly invokes the share sheet; it includes local IPs and diagnostic status but excludes passwords and tokens. Review it before posting publicly.

Video encryption is optional to preserve server compatibility and defaults off. Authentication is not the same as encrypting all traffic: the existing control protocol and motion fields are not encrypted. Use trusted networks and restrict interface/firewall exposure. The independent motion protocol authenticates messages with HMAC and rejects stale sequence numbers within active sessions; it is a development protocol, not a separately audited remote-control security product.

The source requests only Local Network and Motion usage permissions. It avoids multicast discovery and privileged entitlements. The privacy manifest describes local UserDefaults and uptime use. Device/Apple policies and third-party signing-tool compatibility can change independently of the source.

## Touch Display

Select **Touch Display** before connecting. It presents the full desktop with
aspect-fit coordinates and forwards up to ten simultaneous contacts, contact
size, available pressure, movement, releases and cancellations through the
existing authenticated TCP control connection. Gestures in the video area are
left to the host; viewer controls occupy a separate toolbar. Letterbox taps are
ignored and drags leaving the image clamp to its edge. iOS-reserved system
gestures cannot be captured by an application; pressure is sent when UIKit
provides it. This does not add a software keyboard, mouse emulation or Pencil
hover/tilt forwarding.

Set `input_enabled=1` on the server and allow its service user to open
`/dev/uinput`. The optional v6 `TOUCHSCREEN` capability (bit 6, alongside input
bit 0) must be acknowledged; older hosts remain video-only and show a notice.
Input device 3 is a direct type-B touchscreen with ten slots and 0…65535 X/Y.
Each touch frame ends with SYN_REPORT; negative tracking ID ends a contact.
The server destroys the virtual device at disconnect to release all touches.

Update your server's `connect_cmd` from `config/server.conf.example`. It maps
`ND_TOUCH_DEVICE` to `ND_OUTPUT` with Hyprland's per-device configuration. For an
existing custom hook, add `hl.device({ name = '$ND_TOUCH_DEVICE', output =
'$ND_OUTPUT' })` when the variable is nonempty. The device name is unique per
session (`netdisplay-touch-N`), and is configured before uinput creation. This
mapping is necessary on hosts with multiple displays. Existing keyboard and
mouse forwarding remains available to desktop clients.

## Scope exclusions

No eye/gaze tracking, foveated encoding, six-degree-of-freedom position, hand tracking, controllers, audio streaming, OpenXR runtime, app-store distribution, measured motion-to-photon performance, or calibrated Cobra optical model is included. The Linux demo exercises motion independently. The SteamVR driver uses an extended desktop compositor; live game compatibility remains unverified.

## References

- [Apple: free developer account / Personal Team](https://developer.apple.com/help/account/basics/about-your-developer-account)
- [Apple: VideoToolbox](https://developer.apple.com/documentation/videotoolbox)
- [Apple: Core Motion](https://developer.apple.com/documentation/coremotion)
- [Apple: Local Network usage description](https://developer.apple.com/documentation/bundleresources/information-property-list/nslocalnetworkusagedescription)
- [GitHub: hosted runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [iloader: official distribution](https://iloader.app/)
- [SideStore: current prerequisites](https://docs.sidestore.io/docs/installation/prerequisites)
- [libsodium: installation and signature verification](https://doc.libsodium.org/installation)

## License

New source follows this repository's **GPL-2.0-only** license; see `ios/LICENSE` and `../LICENSE`. The app build bundles that license and libsodium's upstream permissive license. Apple frameworks are system dependencies, not redistributed SDK files. This archive contains no Apple certificates, profiles, private keys, or font files.
