# NetDisplay iOS motion UDP v1

This is an **additional companion protocol**, not a message type added to the existing NetDisplay control stream. The stock Linux server does not read it. Use `tools/ios/motion_protocol.py`, `pose_receiver.py`, or `pose_demo.py`, or implement the same packet in your renderer.

Default destination: the configured Linux IPv4 address, UDP **5010**. Motion is sent independently of video from a socket bound to the iPhone IP used for the control connection. Motion transmission starts after the video handshake succeeds. There is no response/acknowledgment channel.

## Packet layout

Exactly **128 bytes**, no C-struct padding. Integers and IEEE-754 binary32 values use network (big-endian) order. Python body format: `!4sHHIQQQ14fI`; body size 96, HMAC 32.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `NDM1` |
| 4 | 2 | version = 1 |
| 6 | 2 | flags; bit 0 means phone camera on left; all other bits zero |
| 8 | 4 | wrapping sequence number |
| 12 | 8 | random nonzero motion session ID |
| 20 | 8 | Core Motion sample timestamp, seconds-since-boot converted to ns |
| 28 | 8 | iPhone `CLOCK_MONOTONIC` send timestamp, ns |
| 36 | 16 | relative headset quaternion x, y, z, w |
| 52 | 12 | headset-frame angular velocity x, y, z, rad/s |
| 64 | 12 | headset-frame user acceleration x, y, z, m/s^2; gravity removed |
| 76 | 16 | original Core Motion attitude quaternion x, y, z, w |
| 92 | 4 | recenter generation counter |
| 96 | 32 | HMAC-SHA256 over bytes 0..95, with the 32-byte pairing token |

The token is generated using the iPhone's secure random source and stored in its local Keychain. The app exposes a deliberate copy action. Store its 64-hex-character representation in a mode-0600 file on Linux. It is not the same as the NetDisplay server password/key. HMAC authenticates the pose but **does not hide its contents**.

## Orientation convention

Core Motion runs in `.xArbitraryZVertical`. This avoids assuming magnetic north. A reference attitude is captured at start and on recenter. The request interval is 1/120 second; actual callback frequency is measured rather than presumed.

Headset camera axes are:

- +X: right across the landscape screen.
- +Y: up across the landscape screen.
- +Z: out of the screen toward the wearer.
- Viewing direction: -Z.

Quaternions are **xyzw**, normalized, Hamilton products. Let `q` be raw Core Motion attitude, `q0` the reference, and `B` the landscape-headset-to-portrait-device basis quaternion:

```text
camera on left:  B = rotation about portrait +Z by -pi/2
camera on right: B = rotation about portrait +Z by +pi/2
q_head = inverse(B) * inverse(q0) * q * B
omega_head = rotate(inverse(B), omega_device)
a_head = rotate(inverse(B), 9.80665 * userAcceleration_device)
```

At recenter, `q_head` is identity. Use it as a camera-to-reference orientation; a view matrix uses its inverse. The local Metal room and the Python demo use this convention. Physical sign/orientation tests on the actual phone are still required; the portable tests establish algebraic consistency, not sensor-to-display calibration.

This supplies **3DoF rotation**, not 6DoF. Do not integrate phone acceleration twice and call it usable headset positional tracking. Yaw can drift because no absolute position/orientation reference is supplied.

## Time and freshness

The sample and send timestamp fields come from different iOS APIs. They are useful for ordering and future clock characterization, but the implementation does not establish their exact cross-API offset under suspend/resume. Do not subtract them without verification, and do not subtract either from the Linux monotonic clock to claim one-way latency. There is no clock synchronization exchange in v1.

The Python receiver timestamps arrival with Linux `time.monotonic()` and displays only time since the most recent accepted local arrival. That is freshness, not network transport latency or motion-to-photon latency.

Sequence numbers increment per sample. A new random session is created when motion capture restarts. Recenter changes the orientation origin and increments the generation; renderers should not interpolate across that discontinuity. Serial comparisons use the uint32 half-range rule so sequence wrap works.

The sample receiver accepts a new session after one second of quiet from the active session and remembers up to 64 retired sessions. Within a session it rejects duplicate/reordered sequence numbers and a changed source IP. `--peer` additionally restricts the expected iPhone address. Replay history is not persisted across receiver restarts; for more demanding threat models, extend this protocol with a negotiated challenge and explicit session expiry.

## Host use

```bash
python3 tools/ios/pose_receiver.py --token-file ~/.config/netdisplay/ios-motion.key
python3 tools/ios/pose_receiver.py --token-file ~/.config/netdisplay/ios-motion.key --json

# Bind only to the actual USB IPv4 and restrict the iPhone peer when practical.
python3 tools/ios/pose_receiver.py --bind LINUX_USB_IP --peer IPHONE_USB_IP \
  --token-file ~/.config/netdisplay/ios-motion.key
```

Substitute real addresses for the capitalized placeholders. Run one receiver per listening port. The JSON stream contains only samples accepted by the receiver; warnings are written to stderr.

For the head-tracked source demo:

```bash
python3 tools/ios/pose_demo.py --token-file ~/.config/netdisplay/ios-motion.key \
  --width 1280 --height 720 --ipd-mm 64 --fov 90
```

The demo renders world-space geometry twice from separate eye positions, then draws a side-by-side window. Move that window to the NetDisplay headless output and use F11 for fullscreen. It is Python/Tk software drawing, not an optimized benchmark. Its rendering cost and scheduling are part of the observed streaming behavior; do not use it to claim NVENC or iPhone minimum latency.
