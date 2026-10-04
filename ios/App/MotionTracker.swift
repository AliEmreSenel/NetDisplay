// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import CoreMotion
import Darwin

final class MotionTracker {
    private let manager = CMMotionManager()
    private let queue = DispatchQueue(label: "netdisplay.motion", qos: .userInteractive)
    private let operationQueue: OperationQueue
    private let diagnostics: Diagnostics
    private var anchor: Quaternion?
    private var generation: UInt32 = 0, sequence: UInt32 = 0
    private var session: UInt64 = 1
    private var cameraOnLeft = true
    private var enabled = false
    private var arMode = false
    private var rearCamera: RearCameraTracker!
    private var fd: Int32 = -1
    private var token = [UInt8]()
    private var rateStart = 0.0, rateCount = 0
    var onError: (@MainActor (String) -> Void)?

    init(diagnostics: Diagnostics) {
        self.diagnostics = diagnostics
        operationQueue = OperationQueue()
        operationQueue.name = "NetDisplay motion callbacks"
        operationQueue.maxConcurrentOperationCount = 1
        operationQueue.qualityOfService = .userInteractive
        operationQueue.underlyingQueue = queue
        rearCamera = RearCameraTracker(queue: queue, diagnostics: diagnostics)
        rearCamera.onSample = { [weak self] in self?.consumeAR($0) }
        rearCamera.onError = { [weak self] in self?.report($0) }
    }
    func start(cameraOnLeft: Bool, positional: Bool = false, headOffset: Vec3 = Vec3()) {
        queue.async {
            self.manager.stopDeviceMotionUpdates()
            self.rearCamera.stop()
            self.arMode = positional
            guard positional || self.manager.isDeviceMotionAvailable else { self.report("Core Motion is unavailable"); return }
            self.cameraOnLeft = cameraOnLeft
            self.anchor = nil; self.sequence = 0; self.generation = 0
            self.rateStart = 0; self.rateCount = 0
            self.session = Crypto.random(8).reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
            if self.session == 0 { self.session = 1 }
            self.enabled = true
            if positional {
                self.rearCamera.start(cameraOnLeft: cameraOnLeft, headOffset: headOffset)
                return
            }
            self.diagnostics.update { $0.trackingStatus = "Core Motion - 3DoF (no measured position)"; $0.position = Vec3(); $0.positionValid = false }
            self.manager.deviceMotionUpdateInterval = 1.0 / 120.0
            self.manager.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: self.operationQueue) {
                [weak self] motion, error in
                guard let self = self, self.enabled else { return }
                if let error = error { self.report(error.localizedDescription); return }
                guard let motion = motion else { return }
                self.consume(motion)
            }
        }
    }
    func recenter() { queue.async { self.anchor = nil; self.rearCamera.recenter() } }
    func setDestination(host: String, localIP: String, port: UInt16, token: [UInt8]) {
        queue.async {
            self.closeSocket()
            guard token.count == 32 else { self.report("A 32-byte motion token is required"); return }
            do {
                let fd = try SocketIO.bindUDP(localIP: localIP, port: 0)
                var destination = try SocketIO.address(host, port: port)
                let r = withUnsafePointer(to: &destination) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
                guard r == 0 else { close(fd); throw SocketIO.error("Connect motion UDP") }
                self.fd = fd; self.token = token
            } catch { self.report(error.localizedDescription) }
        }
    }
    func stop() {
        queue.async {
            self.enabled = false; self.manager.stopDeviceMotionUpdates(); self.rearCamera.stop()
            self.closeSocket(); self.anchor = nil
        }
    }
    private func closeSocket() {
        if fd >= 0 { close(fd); fd = -1 }; Crypto.wipe(&token); token.removeAll()
    }
    private func report(_ s: String) { DispatchQueue.main.async { [weak self] in self?.onError?(s) } }
    private func consume(_ m: CMDeviceMotion) {
        let cq = m.attitude.quaternion
        let raw = Quaternion(x: cq.x, y: cq.y, z: cq.z, w: cq.w).normalized
        if anchor == nil { anchor = raw; generation &+= 1 }
        let basis = Quaternion.headsetBasis(cameraOnLeft: cameraOnLeft)
        let relative = anchor!.inverse * raw
        let head = (basis.inverse * relative * basis).normalized
        let rate = basis.inverse.rotate(Vec3(x: m.rotationRate.x, y: m.rotationRate.y, z: m.rotationRate.z))
        let accel = basis.inverse.rotate(Vec3(x: m.userAcceleration.x * 9.80665,
            y: m.userAcceleration.y * 9.80665, z: m.userAcceleration.z * 9.80665))
        sequence &+= 1
        let sample = MotionSample(sequence: sequence, session: session,
            sampleNS: UInt64(max(0, m.timestamp) * 1e9), sendNS: UInt64(ProcessInfo.processInfo.systemUptime * 1e9),
            head: head, raw: raw, rate: rate, acceleration: accel,
            recenterGeneration: generation, cameraOnLeft: cameraOnLeft)
        if fd >= 0 {
            let packet = Crypto.authenticatedMotion(sample, key: token)
            let sent = packet.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            diagnostics.update { if sent == packet.count { $0.motionSent += 1 } else { $0.motionDropped += 1 } }
        }
        rateCount += 1
        if rateStart == 0 { rateStart = m.timestamp }
        let dt = m.timestamp - rateStart
        diagnostics.update { $0.head = head }
        if dt >= 1 {
            let hz = Double(max(0, rateCount - 1)) / dt
            diagnostics.update { $0.motionHz = hz }
            rateStart = m.timestamp; rateCount = 1
        }
    }
    private func consumeAR(_ observation: SpatialObservation) {
        guard enabled, arMode else { return }
        sequence &+= 1
        let sample = MotionSample(sequence: sequence, session: session,
            sampleNS: UInt64(max(0, observation.timestamp) * 1e9),
            sendNS: UInt64(ProcessInfo.processInfo.systemUptime * 1e9), head: observation.head,
            raw: observation.raw, rate: observation.rate, acceleration: Vec3(),
            recenterGeneration: observation.generation, cameraOnLeft: cameraOnLeft,
            spatial: observation.spatial)
        if fd >= 0 {
            let packet = Crypto.authenticatedMotion(sample, key: token)
            let sent = packet.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            diagnostics.update { if sent == packet.count { $0.motionSent += 1 } else { $0.motionDropped += 1 } }
        }
        rateCount += 1
        if rateStart == 0 { rateStart = observation.timestamp }
        let dt = observation.timestamp - rateStart
        diagnostics.update {
            $0.head = observation.head; $0.position = observation.spatial.position
            $0.positionValid = observation.spatial.positionValid
            $0.trackingStatus = observation.status
        }
        if dt >= 1 {
            let hz = Double(max(0, rateCount - 1)) / dt
            diagnostics.update { $0.motionHz = hz }
            rateStart = observation.timestamp; rateCount = 1
        }
    }
    deinit { manager.stopDeviceMotionUpdates(); if fd >= 0 { close(fd) } }
}
