// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI

@MainActor
struct MainView: View {
    @ObservedObject var model: AppModel
    @State private var copied = false
    var body: some View {
        TabView {
            connectionTab.tabItem { Label("Connect", systemImage: "cable.connector") }
            viewerTab.tabItem { Label("Viewer", systemImage: "viewfinder") }
            telemetryTab.tabItem { Label("Telemetry", systemImage: "waveform.path.ecg") }
            helpTab.tabItem { Label("Setup", systemImage: "questionmark.circle") }
        }
        .tint(.cyan)
        .fullScreenCover(isPresented: $model.showingViewer, onDismiss: { model.closeViewer() }) { ViewerScreen(model: model) }
        .alert("NetDisplay", isPresented: Binding(get: { model.errorText != nil }, set: { if !$0 { model.errorText = nil } })) {
            Button("OK", role: .cancel) { model.errorText = nil }
        } message: { Text(model.errorText ?? "") }
    }
    private var connectionTab: some View {
        NavigationStack {
            Form {
                Section {
                    HStack {
                        VStack(alignment: .leading, spacing: 3) {
                            Text("NETDISPLAY").font(.title2.weight(.bold)).tracking(2)
                            Text("Linux video + iPhone motion / protocol v6").font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Text(model.connected ? "LIVE" : "LOCAL").font(.caption.monospaced().bold())
                            .padding(8).background(model.connected ? Color.green.opacity(0.2) : Color.cyan.opacity(0.15), in: Capsule())
                    }
                    Text(model.status).font(.callout).textSelection(.enabled)
                }
                Section("Linux connection") {
                    TextField("Linux IPv4 address", text: $model.settings.host).keyboardType(.numbersAndPunctuation)
                        .textInputAutocapitalization(.never).autocorrectionDisabled().disabled(model.busy)
                    HStack {
                        Text("Control port")
                        Spacer()
                        TextField("5001", value: $model.settings.controlPort, format: .number.grouping(.never))
                            .keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(width: 110).disabled(model.busy)
                    }
                    Picker("Authentication", selection: $model.settings.authMode) {
                        ForEach(AuthMode.allCases) { Text($0.rawValue).tag($0) }
                    }.disabled(model.busy)
                    if model.settings.authMode != .none {
                        SecureField(model.settings.authMode == .password ? "Server password (not saved)" : "64 hex characters (not saved)", text: $model.secret)
                            .textInputAutocapitalization(.never).autocorrectionDisabled().disabled(model.busy)
                    }
                    Toggle("Encrypt video", isOn: $model.settings.encryptVideo).disabled(model.busy)
                    Text("Authentication and video encryption are separate. Match the server's frame_encryption policy. Manual IPv4 avoids multicast entitlements.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Stream") {
                    Picker("Resolution", selection: $model.settings.resolution) {
                        ForEach(AppSettings.resolutions, id: \.self) { Text($0).tag($0) }
                    }.disabled(model.busy)
                    Picker("Frame rate", selection: $model.settings.fps) { Text("30 fps").tag(30); Text("60 fps").tag(60) }.disabled(model.busy)
                    Toggle("Allow HEVC negotiation", isOn: $model.settings.allowHEVC).disabled(model.busy)
                    Text("H.264 is the default. One stream contains both eyes; the Linux application must render the stereo pair.").font(.caption).foregroundStyle(.secondary)
                    HStack {
                        Button(model.busy ? "Disconnect" : "Connect") { model.busy ? model.disconnect() : model.connect() }
                            .buttonStyle(.borderedProminent)
                        Button("Open viewer") { model.openViewer(test: 0) }.buttonStyle(.bordered).disabled(!model.connected)
                    }
                }
                if let info = model.info {
                    Section("Route and output") {
                        LabeledContent("iPhone receives", value: "\(info.localIP):\(info.stream.port) UDP")
                        LabeledContent("Hyprland output", value: info.stream.outputName)
                        LabeledContent("Codec", value: info.codec)
                        Text("Move your source window to this Hyprland output. A blank new output is not a decoder failure.").font(.caption).foregroundStyle(.secondary)
                    }.textSelection(.enabled)
                }
            }.navigationTitle("NetDisplay").navigationBarTitleDisplayMode(.inline)
        }
    }
    private var viewerTab: some View {
        NavigationStack {
            Form {
                Section("Cobra / manual optics") {
                    Picker("Presentation", selection: $model.settings.viewerMode) { ForEach(ViewerMode.allCases) { Text($0.rawValue).tag($0) } }
                    Toggle("Phone camera on the left", isOn: $model.settings.cameraOnLeft).disabled(model.busy)
                    Toggle("Swap stereo eyes", isOn: $model.settings.swapEyes)
                    Toggle("Lens correction", isOn: $model.settings.lensCorrection)
                    knob("Radial k1", value: $model.settings.k1, range: -0.5...0.8)
                    knob("Radial k2", value: $model.settings.k2, range: -0.3...0.5)
                    knob("Image scale", value: $model.settings.imageScale, range: 0.65...1.6)
                    knob("Optical center shift", value: $model.settings.lensCenterShift, range: -0.15...0.15)
                    knob("Vertical center shift", value: $model.settings.verticalShift, range: -0.15...0.15)
                    Toggle("Show statistics in viewer", isOn: $model.settings.showHUD)
                    Text("These are manual controls, not a measured Cobra profile. Optical center shift is not renderer IPD. Start with correction off and use the grid. Test seated; stop if uncomfortable.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Local tests - no Linux connection required") {
                    HStack {
                        Button("Calibration grid") { model.openViewer(test: 1) }.buttonStyle(.bordered)
                        Button("Motion room") { model.openViewer(test: 2) }.buttonStyle(.borderedProminent)
                    }
                    knob("Test-room vertical FOV", value: $model.settings.demoFOV, range: 50...110)
                    Text("The room uses the phone's orientation locally. It does not measure streaming latency. Single tap shows controls; double tap recenters; hold to exit.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Motion to Linux") {
                    Toggle("Send authenticated motion UDP", isOn: $model.settings.sendMotion).disabled(model.busy)
                    HStack { Text("Motion port"); Spacer()
                        TextField("5010", value: $model.settings.motionPort, format: .number.grouping(.never))
                            .keyboardType(.numberPad).multilineTextAlignment(.trailing).frame(width: 110).disabled(model.busy)
                    }
                    Button(copied ? "Token copied" : "Copy motion pairing token") {
                        model.copyMotionToken(); copied = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { copied = false }
                    }.disabled(!model.motionTokenReady)
                    Text("Paste the token into a private file on Linux, then start tools/ios/pose_receiver.py or pose_demo.py with --token-file. Do not commit the token. Motion starts transmitting after the video control connection succeeds.")
                        .font(.caption).foregroundStyle(.secondary)
                    Text("3DoF rotation only. No positional tracking, gaze tracking, foveation, audio, SteamVR driver or OpenXR runtime is included.").font(.caption).foregroundStyle(.secondary)
                }
            }.navigationTitle("Viewer & motion").navigationBarTitleDisplayMode(.inline)
        }
    }
    private var telemetryTab: some View {
        NavigationStack {
            Form {
                Section("Pipeline") {
                    Text(model.rateText).font(.system(.callout, design: .monospaced))
                    metric("TCP round-trip", model.metrics.tcpRTTMS)
                    metric("Frame assembly (last)", model.metrics.assemblyMS)
                    metric("Parse + hardware decode (last)", model.metrics.decodeMS)
                    metric("First RX packet -> GPU submit (last)", model.metrics.rxToSubmitMS)
                    LabeledContent("Hardware decoder", value: model.metrics.hardwareDecoder ? "Yes" : "Not active")
                    Text("These timings exclude Linux rendering/capture and physical screen scanout. They are NOT motion-to-photon latency. TCP RTT is not a video one-way measurement.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Drops and motion") {
                    LabeledContent("Incomplete frames discarded", value: "\(model.metrics.partialDrops)")
                    LabeledContent("Malformed / unauthenticated packets", value: "\(model.metrics.malformed)")
                    LabeledContent("Pending decoder frames replaced", value: "\(model.metrics.decoderReplaced)")
                    LabeledContent("Decoder errors", value: "\(model.metrics.decodeErrors)")
                    LabeledContent("Measured motion rate", value: String(format: "%.1f Hz", model.metrics.motionHz))
                    LabeledContent("Motion packets sent / dropped", value: "\(model.metrics.motionSent) / \(model.metrics.motionDropped)")
                    LabeledContent("Thermal state", value: model.thermalText)
                    if !model.metrics.lastError.isEmpty { Text(model.metrics.lastError).font(.caption).foregroundStyle(.orange) }
                    ShareLink(item: model.report) { Label("Share diagnostic report", systemImage: "square.and.arrow.up") }
                }
            }.navigationTitle("Telemetry").navigationBarTitleDisplayMode(.inline)
        }
    }
    private var helpTab: some View {
        NavigationStack {
            Form {
                Section("USB hotspot") {
                    Text("Connect a data-capable Lightning cable. Enable Personal Hotspot and trust Linux. On Linux, inspect ip -br addr and find the IPv4 address assigned to the iPhone USB interface. Enter that LINUX address in Connect. Grant this app Local Network permission.")
                    Text("The USB connection carries IP packets, not display input. Carrier/iOS policy can affect whether tethering is available. Do not assume a particular subnet or Wi-Fi network name.")
                }
                Section("First run") {
                    Text("1. Use the calibration grid and motion room.\n2. Start your existing NetDisplay server.\n3. Connect with matching authentication/encryption settings.\n4. For a normal desktop select Mono in both eyes or Flat screen.\n5. For real stereo run the included Linux pose demo, move it to the named Hyprland output, and select Side-by-side stereo.")
                }
                Section("Free signing") {
                    Text("Build the unsigned IPA with the repository's iOS workflow. Sign/install it locally with iloader or SideStore and your free Apple Account. Free provisioning expires after seven days; SideStore itself uses one of the three app slots. The GitHub workflow never needs Apple credentials.")
                    Link("Official SideStore setup", destination: URL(string: "https://docs.sidestore.io/docs/installation/prerequisites")!)
                    Link("Official iloader website", destination: URL(string: "https://iloader.app/")!)
                    Text("SideStore refresh currently needs Wi-Fi and its local VPN; mobile data alone is not sufficient. Disable that VPN while benchmarking streaming. Installation requirements can change with iOS versions.").font(.caption).foregroundStyle(.secondary)
                }
                Section("Scope") {
                    Text("The app streams video and sends head orientation. Your stock NetDisplay server does not consume head pose: use the included Linux tools or integrate their documented packet format into your renderer. Duplicating a desktop in both eyes does not create stereoscopic geometry.")
                    Text("No cloud account, analytics, microphone or camera access is used. Passwords are not saved. The motion token is kept in the local Keychain. Video encryption is optional; motion is authenticated but not encrypted.")
                }
            }.navigationTitle("Setup notes").navigationBarTitleDisplayMode(.inline)
        }
    }
    private func knob(_ title: String, value: Binding<Float>, range: ClosedRange<Float>) -> some View {
        VStack(alignment: .leading) {
            HStack { Text(title); Spacer(); Text(String(format: "%.2f", value.wrappedValue)).monospacedDigit().foregroundStyle(.secondary) }
            Slider(value: value, in: range)
        }
    }
    private func metric(_ title: String, _ value: Double) -> some View {
        LabeledContent(title, value: String(format: "%.2f ms", value))
    }
}

@MainActor
struct ViewerScreen: View {
    @ObservedObject var model: AppModel
    @State private var controls = true
    @State private var countdown = 0
    @State private var centerTask: Task<Void, Never>?
    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            MetalSurface(frames: model.frames, diagnostics: model.diagnostics, settings: model.settings,
                testMode: model.testMode, onError: { model.errorText = $0 })
                .ignoresSafeArea()
                .onTapGesture(count: 2) { model.recenter() }
                .onTapGesture { controls.toggle() }
                .onLongPressGesture(minimumDuration: 1) { model.closeViewer() }
            if controls {
                VStack {
                    HStack {
                        Button { model.closeViewer() } label: { Label("Exit", systemImage: "xmark") }
                        Spacer()
                        Text(model.testMode == 0 ? "STREAM" : (model.testMode == 1 ? "CALIBRATION GRID" : "LOCAL MOTION ROOM"))
                            .font(.caption.monospaced().bold())
                        Spacer()
                        Button("Recenter in 3s") { delayedRecenter() }.disabled(countdown > 0)
                    }.padding(12).background(.black.opacity(0.7))
                    Spacer()
                    if model.settings.showHUD {
                        VStack(spacing: 3) {
                            Text(model.testMode == 0 ? model.status : "Local test - no streaming latency measurement")
                            Text(model.rateText)
                            Text(String(format: "Motion %.0f Hz | RX->submit %.1f ms | %@", model.metrics.motionHz, model.metrics.rxToSubmitMS, model.thermalText))
                            Text("Tap: hide controls | Double tap: recenter | Hold: exit")
                        }.font(.system(size: 11, design: .monospaced)).padding(9).background(.black.opacity(0.75))
                    }
                }.tint(.white)
            }
            if countdown > 0 {
                HStack { Spacer(); Text("\(countdown)"); Spacer(); Text("\(countdown)"); Spacer() }
                    .font(.system(size: 52, weight: .bold)).foregroundStyle(.white).allowsHitTesting(false)
            }
        }
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .onAppear { DispatchQueue.main.asyncAfter(deadline: .now() + 8) { if countdown == 0 { controls = false } } }
        .onDisappear { centerTask?.cancel() }
    }
    private func delayedRecenter() {
        centerTask?.cancel()
        centerTask = Task { @MainActor in
            for n in stride(from: 3, through: 1, by: -1) {
                countdown = n
                do { try await Task.sleep(nanoseconds: 1_000_000_000) } catch { countdown = 0; return }
            }
            model.recenter(); countdown = 0; controls = false
        }
    }
}
