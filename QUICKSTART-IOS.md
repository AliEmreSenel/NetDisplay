# NetDisplay on iPhone: start here

Native iPhone receiver for the **protocol-v6 source in this repository**, plus head-motion streaming and a Linux stereo demonstration. Requires **iOS 17 or newer**. Designed for iPhone 14, a Lightning USB Personal Hotspot connection, and a manually calibrated phone VR viewer.

**Build status:** the portable Swift/C/Python tests have been run. Xcode compilation, installation on a physical iPhone, USB-hotspot routing, and the Cobra optics have **not** been verified in the package-generation environment. The included GitHub workflow performs the real Apple-platform build; a successful run is the next validation gate. This ZIP contains source, not a prebuilt or signed IPA. See [validation](ios/docs/VALIDATION.md).

## 1. Put these files in your existing repository

Extract the overlay ZIP into the **root of NetDisplay**, alongside the existing `src/`, `CMakeLists.txt`, and `README.md`. Its layout is:

```text
.github/workflows/ios.yml
QUICKSTART-IOS.md
ios/...
tools/ios/...
```

Do not upload the ZIP itself as one file: GitHub does not extract it automatically. Commit the extracted files, including the normally hidden `.github/` directory. No existing Linux source or original CI workflow needs to be replaced.

From a local clone, after extraction:

```bash
git add ios tools/ios .github/workflows/ios.yml QUICKSTART-IOS.md
git commit -m "Add native iOS VR receiver, motion telemetry and unsigned IPA workflow"
git push
```

The app is matched to the uploaded source snapshot `b4f2713b3adb24ba6e875cd9a46f908c3d03dfb2` / control version 6. An older v5 server is not compatible. It deliberately rejects protocol-version mismatches rather than guessing.

## 2. Build without owning a Mac

In GitHub, open **Actions -> iOS unsigned IPA -> Run workflow**. A push affecting the iOS files also triggers the workflow. If the manual button is missing, put the workflow on the repository's default branch and enable Actions for the repository.

The workflow uses a standard `macos-15` runner, verifies a signed libsodium source release, builds the native app with Xcode, and packages an unsigned IPA. It needs **no Apple ID, developer certificate, provisioning profile, or GitHub secret**.

When the build is green, download the **NetDisplay-iOS-unsigned** artifact and extract `NetDisplay-unsigned.ipa`. The artifact ZIP is not the IPA: open the artifact first. Logs are saved in **NetDisplay-iOS-logs** if a step fails.

Standard hosted runners are free for public repositories. Private-repository usage has plan-dependent allowances and possible charges. See the [GitHub runner documentation](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).

## 3. Install from Linux using your free Apple Account

The simplest initial route is **iloader directly**, not a mandatory SideStore installation:

1. Get the Linux build from the [official iloader site](https://iloader.app/). Do not use lookalike download sites.
2. Connect and unlock the iPhone with a data-capable Lightning cable. Accept **Trust This Computer**. Follow iloader's Linux USB/pairing prerequisites if the phone is not detected.
3. In iloader, use its custom/any-IPA installation flow, select the extracted `NetDisplay-unsigned.ipa`, and use your free Apple Account to sign it locally. Names of buttons can change between iloader releases.
4. Follow its provisioning/trust prompts. On iOS 16 or later, enable **Settings -> Privacy & Security -> Developer Mode** when requested, restart, and confirm. Trust the developer profile under **Settings -> General -> VPN & Device Management** if iOS asks.
5. Launch NetDisplay. Allow Local Network access and motion access when requested.

Apple's free provisioning expires after **7 days**, with up to **3 apps per device** and other Personal Team limits. Re-sign/refresh before expiration. Reusing the same account and app identifier is preferable to deleting the app, which can discard settings or change pairing state. This is personal-device development, not TestFlight or App Store distribution.

iloader is third-party software that handles your Apple credentials. Review its official instructions and do not place Apple passwords, tokens, certificates, or pairing files in GitHub. The build workflow does not need them.

**Optional:** [SideStore](https://docs.sidestore.io/docs/installation/prerequisites) can manage on-device re-signing, but adds its own installation requirements, occupies an app slot, and currently requires Wi-Fi plus its local VPN for refresh. Do not rely on a cellular-only USB-hotspot session to refresh apps. Disable the refresh VPN for clean streaming measurements. Check the current SideStore/iloader instructions for your exact iOS version.

Official free-account limits: [Apple developer account overview](https://developer.apple.com/help/account/basics/about-your-developer-account).

## 4. Test the phone and viewer before networking

Open **Viewer -> Calibration grid**, then **Motion room**. Neither needs Linux. Set **Phone camera on the left** to match the actual physical orientation in the viewer. The room should stay in place as you rotate the phone, rather than following the screen. Test yaw, pitch, and roll separately; use recenter after mounting it.

Single tap shows/hides controls. Double tap recenters. Long press exits. **Recenter in 3s** lets you put the viewer on before setting the forward direction.

Lens correction starts **off**. The included coefficients are editable examples, **not measured Cobra parameters**. Use the grid and adjust gently. Optical-center shift is not interpupillary distance, and this app does not claim Cardboard QR-profile calibration. Test seated for short periods and stop if uncomfortable. Foveation, eye-camera capture, and positional tracking are not enabled.

## 5. Connect over the Lightning USB network

Enable Personal Hotspot and connect Linux by cable. Tethering availability depends on your iOS/carrier configuration. On Linux inspect:

```bash
ip -br addr
ip route
```

Enter the **Linux computer's IPv4 address on the iPhone USB interface** in the app. Do **not** enter the iPhone's gateway address. Do not hard-code an assumed `172.20.10.x` or `192.168.x.x` subnet. Wi-Fi uses the same protocol, but the app follows the route to the IP you enter; it does not force an arbitrary connection to become USB.

Start your existing v6 NetDisplay server using your normal launcher/service. In the app select matching authentication and video-encryption settings. Initial defaults are **H.264, 1280x720, 60 fps**, password authentication, and video encryption off. Enter the original password, not the server's derived `password_key` value, when using Password mode. The raw 32-byte-key mode accepts 64 hex characters.

The server controls encryption policy: `frame_encryption=off` rejects encrypted clients; `allowed` accepts either; `required` rejects plaintext. Both ends must agree. Use encryption on shared/untrusted networks. Prefer keeping development traffic on a trusted local USB network.

Press **Connect**. The app displays the iPhone-side IP, negotiated codec, UDP port, and the Hyprland headless output created by the server. Move the desired Linux content onto that output. A blank headless workspace is not a decoder failure.

Select **Flat screen** for an ordinary desktop, **Mono in both eyes** to duplicate it in a viewer, or **Side-by-side stereo** for an actual left-eye/right-eye source. Splitting an ordinary desktop into two halves does not create VR stereo.

The default control port is TCP 5001. Video arrives on the server-negotiated UDP port, normally 5000. Motion goes back to Linux on UDP 5010. Keep firewall rules restricted to the tether/local interface and the relevant peer. See [troubleshooting](ios/docs/TROUBLESHOOTING.md).

## 6. Exercise the complete head-motion return path

In **Viewer -> Motion to Linux**, copy the pairing token. Transfer it privately to Linux; do not commit it. Store the 64 hex characters in a private file, for example:

```bash
mkdir -p ~/.config/netdisplay
chmod 700 ~/.config/netdisplay
umask 077
read -r -s -p 'Paste the motion token: ' ND_MOTION_TOKEN
printf '\n'
printf '%s\n' "$ND_MOTION_TOKEN" > ~/.config/netdisplay/ios-motion.key
unset ND_MOTION_TOKEN
chmod 600 ~/.config/netdisplay/ios-motion.key
```

Run either the numeric receiver or the stereo demo, not both on the same port:

```bash
python3 tools/ios/pose_receiver.py \
  --token-file ~/.config/netdisplay/ios-motion.key

# Alternative: requires Python's tkinter package (often python3-tk).
python3 tools/ios/pose_demo.py \
  --token-file ~/.config/netdisplay/ios-motion.key \
  --width 1280 --height 720
```

Connect the app to the video server. Motion UDP starts only after that control connection succeeds. Move the **NetDisplay - stereo pose demo** window to the output reported by the app and make it fullscreen with **F11**. Match the app's video resolution and select **Side-by-side stereo**. Rotate your head and recenter as needed. Escape closes the demo.

The demo is deliberately a small test renderer, not a high-performance game engine or a SteamVR/OpenXR driver. The stock NetDisplay server does not consume the motion packets itself; integrate the [documented packet format](ios/docs/MOTION-PROTOCOL.md) into the renderer you actually use.

## What is and is not measured

Telemetry reports TCP round-trip, video assembly, decoding, local receive-to-GPU-submit, frame drops, measured motion sample rate, and thermal state. Those numbers **do not measure end-to-end motion-to-photon latency** and do not include screen scanout. The 120 Hz motion update request is a preference; the actual delivered rate is shown. No latency, optical accuracy, or USB throughput figure is guaranteed by this package.

Read [ios/README.md](ios/README.md) for architecture, development, and limitations.
