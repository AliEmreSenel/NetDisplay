# Troubleshooting

## GitHub does not show the workflow

The actual `.github/workflows/ios.yml` must exist in the repository, not inside an uploaded ZIP or an extra `NetDisplay-iOS/` directory. The manual workflow button normally needs the workflow on the default branch. Enable Actions and allow `actions/checkout` and `actions/upload-artifact` in repository policy. Existing workflows do not need to be removed.

## Apple build fails

Open the failing step in Actions. The Linux portable tests do not prove Apple SDK compilation. Download `NetDisplay-iOS-logs` and inspect `build.log` and `tests/test.log`. The app needs a modern Xcode compiler, iPhoneOS SDK, and the signed libsodium source download. A signature/download failure is not permission to bypass verification. The workflow contains no signing step: an Apple-ID or certificate error during `xcodebuild` indicates its unsigned settings have been changed or overridden.

## Cannot install the IPA

Extract the workflow artifact ZIP and select the inner `NetDisplay-unsigned.ipa`, not the outer ZIP. Use the official current iloader/SideStore instructions for the installed iOS version. The unsigned IPA cannot simply be opened from Files and run. A free-account device installation still requires account-based provisioning, device trust, and Developer Mode where applicable. Check the seven-day validity and three-app limit. Do not delete apps blindly when troubleshooting provisioning.

## Linux sees the iPhone but no USB network

USB pairing and USB tethering are different. Unlock the phone, trust the host, enable Personal Hotspot, and check a data-capable cable. Linux normally needs the `ipheth` kernel driver and pairing support; package names depend on the distribution. Use your distro's iPhone tethering instructions. Check `ip -br addr`, `ip route`, and your network manager. Carrier/iOS hotspot policy can affect availability.

The app does not emulate a network adapter or turn Lightning into HDMI input. If USB networking does not exist, no app-side UDP implementation can route over it. Use the same protocol on a local Wi-Fi link as a diagnostic alternative.

## TCP connection times out

Enter the Linux-side tether-interface address, not the phone gateway. Grant NetDisplay Local Network permission in Settings, then reconnect. Check that the server is listening on the chosen IPv4/interface and TCP control port, normally 5001. Check local firewall rules and route selection. Temporarily turning off Linux Wi-Fi can isolate whether traffic is actually using the cable; restore it afterward if needed.

## Protocol v5 error

The app is explicitly protocol v6. Update the server to the supplied source snapshot. An older binary from a public launcher or old systemd service may still be running even if the repository contains newer sources. Check which binary/service you started and its log. Do not remove protocol checks to make an old server appear compatible.

## Authentication or encryption rejected

Password mode takes the original password; the server's `password_key` is already derived. The 32-byte-key mode takes exactly 64 hexadecimal characters. An unauthenticated server requires explicitly choosing No authentication. Match the server's `frame_encryption` policy. The app does not silently weaken the connection if authentication/encryption fails.

## Connected, but blank image

First verify that content exists on the **new headless Hyprland output** named by the app. The receiver is not automatically mirroring the host's existing monitor. Open the viewer after connecting. Use Flat screen when debugging a desktop.

If no complete UDP frames arrive, check the source sender log, the negotiated video port and firewalls. The sender's UDP source port is ephemeral; firewall rules that only allow packets *from* port 5000 are not sufficient. The app binds the destination on the iPhone IP used by TCP.

If complete frames arrive but decoding fails, start with H.264 at 1280x720@60 and inspect the decoder's last error. HEVC is opt-in and AV1 is not advertised. Verify the actual source dimensions and intra-frame bitstream behavior.

If decode succeeds but draw rate stays zero, open the full-screen viewer. Telemetry outside the viewer does not force an invisible Metal view to draw. Black borders in Flat/Mono modes can be normal aspect-ratio fitting.

## Frames drop or phone gets hot

Start at 1280x720. Reduce the source encoder's bandwidth demand using its existing configuration, or reduce frame rate for diagnosis. This app does not add a remote bitrate control. All-intra streams can be much larger than an ordinary inter-frame stream. USB nominal link speed is not measured payload throughput. Latest-frame replacement prevents an application FIFO from growing, but the network/driver/decoder still has costs.

Do not benchmark with unnecessary VPNs, recording, overlays, or background downloads. Charging while decoding/rendering can contribute to heating. The app shows thermal state; it does not promise to avoid throttling. Stop and let the phone cool if needed.

## Motion exists locally, but Linux receives nothing

The local room works without networking. External motion sends begin only after a successful NetDisplay video control connection and only when Send motion is enabled. Both sides need the same separate token and UDP port, normally 5010. Use the iPhone peer IP shown by the app when setting `--peer`. Keep the token file private. Do not run the console receiver and demo on the same port at once.

The receiver ignores malformed/HMAC-invalid/replayed packets. A replacement app session may need a brief one-second quiet interval. There is no automatic motion driver in the stock server; run the companion tool or integrate the protocol into your renderer.

## View rotates incorrectly or looks uncomfortable

Set the camera-left/right option to match the actual mount, stop/restart motion or reconnect, and recenter facing forward. Check yaw, pitch, and roll independently in the local room before streaming. The default camera basis is implemented consistently in both clients but still needs on-device validation.

Use Side-by-side stereo only for a real two-eye image. Select Mono in both eyes for an ordinary desktop. A lens-center slider does not correct a wrong renderer IPD. No measured Cobra lens profile is included; start with correction off and use the grid. Incorrect FOV, distortion, delay, and tracking can all cause discomfort. Do not compensate for an unmeasured problem by increasing a warp slider indefinitely.

## Viewer cannot exit while mounted

Single tap reveals controls. Long press exits. Double tap recenters. Use the three-second recenter button before putting the viewer on. If the viewer prevents all touchscreen interaction, remove the phone to change settings; no controller or headset-button integration is implemented.

## Latency number seems implausibly low

The app reports local stage intervals. First-packet-to-GPU-submit excludes Linux rendering/capture/encode, any time before the first packet, and physical display scanout. TCP RTT does not measure video or motion one-way delay. A real end-to-end experiment needs synchronized clocks with error bounds or an external optical/high-speed-camera method. No earlier conversational latency estimate is a measured result for this app.
