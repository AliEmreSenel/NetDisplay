// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import UIKit
import SwiftUI

@MainActor
final class AppModel: ObservableObject {
    @Published var settings = AppSettings.load()
    @Published var secret = ""
    @Published var status = "Ready"
    @Published var busy = false
    @Published var connected = false
    @Published var info: ConnectedInfo?
    @Published var errorText: String?
    @Published var showingViewer = false
    @Published var testMode = 0
    @Published var metrics = DiagnosticSnapshot()
    @Published var rateText = "No stream"
    @Published var thermalText = "Nominal"
    @Published var motionTokenReady = false
    let frames = FrameStore()
    let diagnostics = Diagnostics()
    private var motion: MotionTracker!
    private var token: [UInt8] = []
    private var connection: StreamConnection?
    private var connectionID = UUID()
    private var timer: Timer?
    private var previous = DiagnosticSnapshot()
    private var previousNS: UInt64 = 0

    init() {
        guard nd_ios_init() >= 0 else { errorText = "Cryptographic library failed to initialize"; return }
        motion = MotionTracker(diagnostics: diagnostics)
        motion.onError = { [weak self] in self?.errorText = $0 }
        do { token = try KeyStore.readOrCreateToken(); motionTokenReady = true }
        catch { errorText = error.localizedDescription }
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            DispatchQueue.main.async { self?.refreshMetrics() }
        }
    }
    func connect() {
        guard !busy, motion != nil else { return }
        do { try settings.validate() } catch { errorText = error.localizedDescription; return }
        settings.save(); diagnostics.reset(); previous = DiagnosticSnapshot(); previousNS = 0
        busy = true; connected = false; status = "Starting"; info = nil; errorText = nil
        let id = UUID(); connectionID = id
        let c = StreamConnection(settings: settings, secret: secret, frames: frames, diagnostics: diagnostics)
        c.onStatus = { [weak self] text in guard self?.connectionID == id else { return }; self?.status = text }
        c.onConnected = { [weak self] info in
            guard let self = self, self.connectionID == id else { return }
            self.info = info; self.connected = true; self.status = "Streaming"
            if self.settings.sendMotion, self.token.count == 32 {
                self.motion.setDestination(host: self.settings.host, localIP: info.localIP,
                    port: UInt16(self.settings.motionPort), token: self.token)
            }
        }
        c.onEnded = { [weak self] error in
            guard let self = self, self.connectionID == id else { return }
            self.connection = nil; self.busy = false; self.connected = false
            self.status = error == nil ? "Disconnected" : "Connection ended"
            self.frames.reset(); self.motion.stop()
            UIApplication.shared.isIdleTimerDisabled = self.showingViewer
            if let error = error { self.errorText = error }
        }
        connection = c
        motion.start(cameraOnLeft: settings.cameraOnLeft)
        UIApplication.shared.isIdleTimerDisabled = true
        c.start()
    }
    func disconnect() {
        connectionID = UUID(); connection?.stop(); connection = nil
        motion?.stop(); frames.reset()
        busy = false; connected = false; info = nil; status = "Disconnected"
        UIApplication.shared.isIdleTimerDisabled = false
    }
    func openViewer(test: Int) {
        testMode = test
        if !busy { motion?.start(cameraOnLeft: settings.cameraOnLeft) }
        showingViewer = true; UIApplication.shared.isIdleTimerDisabled = true
    }
    func closeViewer() {
        showingViewer = false
        if !busy { motion?.stop(); UIApplication.shared.isIdleTimerDisabled = false }
    }
    func recenter() { motion?.recenter() }
    func copyMotionToken() {
        guard token.count == 32 else { errorText = "Motion key is unavailable. Unlock the phone and relaunch."; return }
        UIPasteboard.general.string = Crypto.hex(token)
    }
    func suspend() { closeViewer(); disconnect() }
    private func refreshMetrics() {
        let now = nd_ios_now_ns(), value = diagnostics.snapshot()
        if previousNS != 0 {
            let dt = Double(now - previousNS) / 1e9
            func difference(_ a: UInt64, _ b: UInt64) -> Double { Double(a >= b ? a - b : 0) / dt }
            rateText = String(format: "RX %.0f | decode %.0f | draw %.0f fps | %.1f Mb/s",
                difference(value.received, previous.received), difference(value.decoded, previous.decoded),
                difference(value.drawn, previous.drawn), difference(value.bytes, previous.bytes) * 8 / 1e6)
        }
        metrics = value; previous = value; previousNS = now
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: thermalText = "Nominal"
        case .fair: thermalText = "Warm"
        case .serious: thermalText = "HOT - lower resolution / take a break"
        case .critical: thermalText = "CRITICAL - stop streaming and cool the phone"
        @unknown default: thermalText = "Unknown"
        }
        if connected, let info = info, value.lastVideoNS == 0 {
            status = "Connected; waiting for UDP on \(info.localIP):\(info.stream.port)"
        } else if connected, value.lastVideoNS != 0, now - value.lastVideoNS > 3_000_000_000 {
            status = "Video stalled - check source / UDP / firewall"
        } else if connected { status = "Streaming" }
    }
    var report: String {
        """
        NetDisplay iOS 1.0 / protocol v6
        Status: \(status)
        Linux: \(settings.host):\(settings.controlPort)
        iPhone: \(info?.localIP ?? "not connected")
        Hyprland output: \(info?.stream.outputName ?? "none")
        Codec: \(info?.codec ?? "none"); hardware: \(metrics.hardwareDecoder)
        \(rateText)
        TCP RTT: \(metrics.tcpRTTMS) ms
        Last assembly: \(metrics.assemblyMS) ms
        Last decode including parse/copy: \(metrics.decodeMS) ms
        Last first-RX-packet -> GPU-submit: \(metrics.rxToSubmitMS) ms
        Partial frame drops: \(metrics.partialDrops); malformed/auth failures: \(metrics.malformed)
        Pending decode replacements: \(metrics.decoderReplaced); decode errors: \(metrics.decodeErrors)
        Motion: \(metrics.motionHz) Hz; sent: \(metrics.motionSent); send drops: \(metrics.motionDropped)
        Thermal: \(thermalText)
        Last pipeline error: \(metrics.lastError)
        These are LOCAL stage measurements, not end-to-end motion-to-photon latency.
        No passwords, authentication keys or motion tokens are included.
        """
    }
    deinit { timer?.invalidate() }
}
