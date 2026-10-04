# Validation of the tracking/latency/optional-AR patch

## Provenance and scope

Baseline: the uploaded `NetDisplay-upload.tar(1).gz`. This patch modifies the
actual dedicated `vr/` pipeline and shared `steamvr/` HMD driver in that archive,
plus the existing native iOS app. It is not a claim that the earlier overlay
already supported the new VR backend.

Source locations in the **unmodified uploaded tree**:

| Finding | Evidence |
|---|---|
| VR NVENC output delay not explicitly disabled | `vr/server.c:64-96`, especially option block 84-89 |
| Large requested UDP send buffer | `vr/server.c:260-264` |
| Synchronous whole-frame sends | `vr/server.c:327-338`; its obsolete callback only checks shutdown |
| Extra CPU frame copy | `vr/server.c:294-301`; first copy in capture worker 46-50 |
| Universe ID without provided chaperone/origins or tracking-system name | `steamvr/driver.cpp:66-89` |
| Existing valid quaternion / fixed-height pose / Running_OK | `steamvr/driver.cpp:98-119` |
| Motion receive/publication on runtime frame callback | `steamvr/driver.cpp:122-150` and Provider::RunFrame |
| Streamed client not using head rotation for timewarp | `ios/App/Viewer.metal` uses orientation only in the local room; `vr/capture.cpp` does not forward the render pose with video |

The missing NVENC option is a definite configuration regression relative to the
low-latency desktop path. Its exact delay on the user's installed FFmpeg/NVIDIA
combination was not measured. Missing driver-origin setup is a concrete incomplete
integration, but the exact room-setup wizard predicate was not observed. The
included runtime probe is necessary to distinguish possible causes on that machine.

## Executed in the preparation environment

* **45 Swift checks passed**: control/Annex-B/touch compatibility, original NDM1,
  NDM2 layout, metre-coordinate mapping, camera offset, and yaw-only AR recenter.
  These compile real portable Swift source on Linux.
* **12 Python spatial-wire checks passed**: a body emitted by the real Swift
  implementation is authenticated and parsed in Python; malformed position,
  quality, reserved fields, sizes and HMAC are rejected.
* **C++ motion validation passed**, including both real Swift-generated NDM1 and
  NDM2 fixtures, HMAC verification, replay/session handling, invalid tracking,
  age/connection separation and malformed values. AddressSanitizer and
  UndefinedBehaviorSanitizer were enabled; no errors were reported.
* **TX-worker policy tests passed**: production `vr/video_tx.c` ran in a thread
  with a deterministic transport test double; checks cover packet ownership,
  latest replacement, obsolete/deadline cancellation, errors and shutdown.
  Local tests used a small AVPacket memory-model test double because FFmpeg
  development headers were unavailable. They do NOT validate FFmpeg ABI,
  codecs, real packet transmission, USB throughput or GPU performance. The CI
  target uses real FFmpeg AVPackets with the same transport test double.
* **Stationary-space test passed**: invalid heights rejected; emitted chaperone
  parsed as JSON with expected universe, height and origins. This is not a
  SteamVR runtime acceptance test.
* **Launcher configuration tests passed** in temporary directories: matching HMD
  and capture mode, preservation of pairing paths, invalid mode rejection and
  private file permissions. No actual SteamVR installation was altered.
* Full iOS Swift source **syntax parsing** passed; Xcode project membership,
  permissions, scheme, privacy manifest and credential-free workflow structure
  checks passed. Python files compiled successfully.

The real installed libsodium shared library was used for the C++ HMAC tests.
Only the function declarations were supplied by a test-local header because
libsodium development headers were absent. The test header and packet memory
model are not included in the distributed application source or used by CI.

## NOT executed here

* Full Linux driver/VR backend compilation against OpenVR, Vulkan and FFmpeg
  development SDKs. The container lacked those headers and external downloads
  were unavailable. A CI job has been added to compile/test this exact path.
* Xcode compilation/linking, IPA installation, code signing, or iOS execution.
  Syntax checks do not catch all Apple API/type/linking errors. Run the supplied
  macOS workflow before treating the updated iOS app as build-verified.
* SteamVR room setup, driver activation, chaperone acceptance, live diagnostic,
  GPU capture, NVENC output timing, physical USB transport or iPhone rendering.
* AR tracking accuracy, tracking recovery, camera/viewer geometry, thermal
  endurance, camera-to-eye offset or physical motion-to-photon latency.
* The entire repository's legacy C/VM/hardware suites. Existing earlier reports
  describe earlier versions and are not validation of this patch.

## Release gates on the user's hardware

1. Linux CI and macOS IPA build must pass. Run `ctest` in the actual VR build.
2. Existing iOS app + patched host, AR disabled: confirm fresh Running_OK poses in
   raw, standing and seated spaces; chaperone property succeeds; correct driver
   registration and universe are reported.
3. Confirm startup verifies NVENC delay=0; compare host stage counters and iOS
   diagnostics before increasing resolution or enabling rear-camera tracking.
4. Updated app: local AR room test, permission denial, camera occlusion, recovery
   requiring recenter, sign/axis checks in both landscape orientations.
5. Streamed AR: small seated translations, yaw/pitch/roll, recenter, reconnect,
   tracking loss and thermal behavior. Do not test walking without a safe,
   externally managed space: this patch supplies no measured collision boundary.

No end-to-end latency number or claim that the SteamVR wizard now succeeds has
been measured. The patch is a testable implementation and diagnosis, not a
hardware-certified build.
