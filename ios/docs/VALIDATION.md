# Validation record

Package prepared on **2026-10-03** against the user's uploaded `NetDisplay-master(1)(1).zip`.

- Archive Git snapshot: `b4f2713b3adb24ba6e875cd9a46f908c3d03dfb2`.
- Input ZIP SHA-256: `425a8ef83723a53e1385c831a8a9d07d9f087ed7eada8bc3f44510ba52db4da9`.
- Control protocol: **v6**.
- Minimum iOS deployment target: **17.0**, real arm64 iPhones.
- All **58 original uploaded files** were compared byte-for-byte and left unchanged. The package adds iOS source, supporting Linux tools, documentation, and a separate GitHub workflow.

## Executed in the Linux package-generation environment

| Check | Result |
|---|---|
| Swift portable core compiled and executed | PASS, 26 assertions covering network byte order, message sizes, version/size rejection, stream validation, H.264/HEVC Annex-B parsing, quaternion transforms, and motion serialization |
| C reassembler plus original `src/crypto.c` compiled and executed | PASS, 52 assertions including fragmentation, out-of-order delivery, duplicates, replacement, session filtering, sequence wrap, encryption round-trip, and modified-tag rejection |
| Malformed UDP input trials | PASS, 20,000 generated malformed datagrams |
| AddressSanitizer + UndefinedBehaviorSanitizer on native wrapper/test | PASS with leak detection; no reported sanitizer failure |
| Independent Python crypto vectors | PASS, BLAKE2b client/server proof and stream-key vectors independently reproduced |
| Swift -> C -> Python motion wire interoperability | PASS, 96-byte Swift body + C HMAC -> Python validation of the 128-byte packet |
| UDP loopback/replay rejection | PASS using actual local UDP sockets |
| Python stereo renderer GUI | PASS under Xvfb: launched Tk window, accepted an authenticated motion sample, drew stereo geometry, and exited cleanly |
| iOS Swift source syntax parsing | PASS; **not** Apple SDK typechecking |
| Xcode project membership / shared scheme | PASS for source references and XML structure; no `xcodebuild` execution |
| Info.plist / privacy manifest / asset references | PASS structural checks |
| Shell / Python source | PASS shell syntax and Python byte-compilation checks |
| Original repository preservation | PASS, all 58 original files unchanged |

The C tests used the Linux environment's **libsodium 1.0.18 runtime** with temporary ABI declarations because its development headers were not installed. Those temporary declarations and test binaries are **not shipped**. This establishes exercised runtime behavior and independent wire/crypto compatibility, not verification of the new iPhoneOS dependency build. The GitHub job reruns tests against official Homebrew development headers, then builds signed upstream **libsodium 1.0.22** for iOS.

Sanitizers were used for tests of this wrapper, not as production iPhone cryptographic-library build flags. The test shell script in the repository uses ordinary build flags by default.

The root overlay manifest, `ios/OVERLAY-SHA256SUMS.txt`, records hashes of delivered files other than the manifest itself.

## Not executed or established here

**No Mac/Xcode or physical iPhone was available in the package-generation environment.** Consequently:

- The iOS Swift/C/Metal application has **not been compiled with Xcode** here. Apple SDK typechecking, linking, shader compilation, and asset-catalog compilation remain to be confirmed by the supplied workflow.
- No IPA was produced, signed, sideloaded, or launched on the user's iPhone here.
- GitHub Actions was not run against the user's account, and no branch, commit, or workflow was pushed remotely.
- The iPhoneOS libsodium source build was not run locally. The build-time download/signature path is implemented but awaits the macOS CI execution.
- Actual USB tethering connectivity between this iPhone app and the Linux host was not tested.
- Hardware VideoToolbox decoding, GPU presentation, Core Motion delivery rate, optical geometry/signs, and thermals were not measured on the phone.
- End-to-end video delay, head-motion-to-photon delay, achievable frame rate, and visual comfort are unknown until measured on the actual setup.
- No Cobra lens calibration, eye tracking, gaze-dependent rendering, or SteamVR/OpenXR compatibility has been validated or claimed.

This is a complete implementation source package with portable tests, **not a hardware-certified or already device-tested headset app**. The next build gate is a green `iOS unsigned IPA` workflow. A green build is still not a substitute for physical integration testing.

## First-device acceptance checklist

1. GitHub job completes its tests, Xcode build, and IPA-layout validator.
2. IPA is signed/installed locally, Developer Mode/trust completed, and the app launches.
3. Local Network and Motion permissions are granted.
4. Calibration grid renders in both orientations; local room responds correctly to yaw/pitch/roll; recenter works.
5. Video handshake succeeds against the v6 server with the intended password/key/encryption policy.
6. The local IP shown by the app is the cable-network peer; source content appears on the expected headless output.
7. H.264 at 1280x720@60 decodes in hardware; the stream remains responsive during desktop changes.
8. The Linux receiver accepts the copied motion token; invalid tokens are rejected.
9. The host stereo demo updates from the phone's pose and is captured on the new output.
10. Disconnect/reconnect, denied permissions, background/foreground, cable removal, and server restart recover without a stale stream or application crash.
11. Longer sessions are checked for temperature, packet loss, memory growth, and discomfort before increasing resolution.
12. Actual latency is measured with an external timing method or properly synchronized clocks; local HUD timings are not substituted for this measurement.

## Reproduce portable checks

With Swift, Python 3, a C compiler, pkg-config, and libsodium development files installed:

```bash
bash ios/scripts/test.sh
```

Optional Linux GUI test with tkinter and Xvfb:

```bash
xvfb-run -a python3 ios/Tests/demo_smoke.py
```

The normal macOS CI job runs portable tests and then the actual Apple-platform build. It does not run this Linux/Xvfb test or claim to exercise a connected iPhone.

## Touch Display and SteamVR acceptance checks

These physical-device checks remain pending:

- Build the app with Xcode / the iOS workflow, install on a real iPhone, select
  each viewer mode, reconnect and verify settings survive relaunch.
- With input disabled or an older server, verify Touch Display stays video-only
  with an explicit notice. Enable input and verify the negotiated direct device.
- On a multi-monitor host, verify corner/center taps reach the streamed output,
  including letterboxing, portrait-sized content and after phone rotation.
- Exercise ten simultaneous contacts, drag outside the image, lift fingers in
  different orders, exit the viewer, interrupt with system UI, background the
  app and disconnect the cable. No contact may remain stuck on Linux.
- Confirm taps, double taps and long presses reach the host in Touch Display,
  while the dedicated Exit toolbar remains usable.
- Register the SteamVR driver using its README, verify HMD detection, left/right
  eye separation and compositor placement on the streamed output. Launch a
  seated application, verify rotation/recenter, and confirm tracking becomes
  invalid within 250 ms after motion stops.
- Verify wrong-token/wrong-peer motion is rejected; restart/reconnect both sides,
  wake from standby and ensure tracking resumes without reviving stale poses.
- Check target SteamVR/Hyprland extended-display compatibility, physical optics
  and actual end-to-end latency before treating this as a usable headset.
