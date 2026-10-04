// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import ARKit
import AVFoundation
import UIKit
import simd

/// Experimental rear-camera VIO. The camera feed stays local; only authenticated
/// pose packets leave the phone. No ARView, scene reconstruction or depth stream.
final class RearCameraTracker: NSObject, ARSessionDelegate {
    private let session = ARSession()
    private let queue: DispatchQueue
    private let diagnostics: Diagnostics
    private var enabled = false, runID: UInt64 = 0
    private var orientation: UIInterfaceOrientation = .landscapeRight
    private var headOffset = Vec3()
    private var origin = SpatialOrigin()
    private var previous: SpatialObservation?
    private var last = SpatialObservation()
    private var needsRecenter = true, requiresUserRecenter = false
    var onSample: ((SpatialObservation) -> Void)?
    var onError: ((String) -> Void)?

    init(queue: DispatchQueue, diagnostics: Diagnostics) {
        self.queue = queue; self.diagnostics = diagnostics
        super.init()
        session.delegateQueue = queue
    }
    // All lifecycle methods and delegate callbacks use MotionTracker's queue.
    func start(cameraOnLeft: Bool, headOffset: Vec3) {
        stop(); enabled = true; runID &+= 1
        let id = runID
        orientation = cameraOnLeft ? .landscapeRight : .landscapeLeft
        self.headOffset = headOffset
        origin = SpatialOrigin(); previous = nil; last = SpatialObservation(); needsRecenter = true; requiresUserRecenter = false
        guard ARWorldTrackingConfiguration.isSupported else { fail("AR world tracking is not supported"); return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: begin(id)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] allowed in
                guard let self = self else { return }
                self.queue.async {
                    guard self.enabled, self.runID == id else { return }
                    if allowed { self.begin(id) }
                    else { self.fail("Camera permission denied. Enable it in Settings, or disable positional tracking.") }
                }
            }
        default: fail("Camera permission denied. Enable it in Settings, or disable positional tracking.")
        }
    }
    private func begin(_ id: UInt64) {
        guard enabled, runID == id else { return }
        session.delegate = self
        let configuration = ARWorldTrackingConfiguration()
        configuration.worldAlignment = .gravity
        configuration.planeDetection = []
        configuration.environmentTexturing = .none
        configuration.isLightEstimationEnabled = false
        configuration.isAutoFocusEnabled = true
        // Prefer the smallest camera format delivering at least 60 updates/s.
        let formats = ARWorldTrackingConfiguration.supportedVideoFormats.filter { $0.framesPerSecond >= 60 }
        if let format = formats.min(by: {
            $0.imageResolution.width * $0.imageResolution.height < $1.imageResolution.width * $1.imageResolution.height
        }) { configuration.videoFormat = format }
        session.run(configuration, options: [.resetTracking, .removeExistingAnchors])
        diagnostics.update { $0.trackingStatus = "AR initializing - uncover rear cameras and view a lit, textured room" }
    }
    func stop() {
        enabled = false; runID &+= 1
        session.pause(); session.delegate = nil
        previous = nil
    }
    func recenter() { origin.recenter(); previous = nil; needsRecenter = true; requiresUserRecenter = false }
    private func fail(_ message: String) {
        diagnostics.update { $0.trackingStatus = message }
        publishInvalid(message, quality: 0, timestamp: ProcessInfo.processInfo.systemUptime)
        onError?(message)
    }
    private func publishInvalid(_ text: String, quality: UInt32, timestamp: Double) {
        guard enabled else { return }
        previous = nil
        last.timestamp = timestamp; last.status = text; last.rate = Vec3()
        last.spatial.velocity = Vec3(); last.spatial.orientationValid = false
        last.spatial.positionValid = false; last.spatial.quality = quality
        onSample?(last)
    }
    func session(_ session: ARSession, didUpdate frame: ARFrame) {
        guard enabled else { return }
        switch frame.camera.trackingState {
        case .normal: break
        case .notAvailable:
            publishInvalid("AR unavailable", quality: 0, timestamp: frame.timestamp); return
        case .limited(let reason):
            // Losing an established map may change its origin. Do not resume
            // room motion silently: the user must explicitly re-anchor.
            if origin.generation > 0 { requiresUserRecenter = true }
            let message: String
            switch reason {
            case .initializing: message = "AR initializing - slowly scan the room"
            case .excessiveMotion: message = "AR limited - move more slowly"
            case .insufficientFeatures: message = "AR limited - uncover rear cameras; add light/visual texture"
            case .relocalizing: message = "AR relocalizing - remain still and look at familiar features"
            @unknown default: message = "AR tracking limited"
            }
            publishInvalid(message, quality: 1, timestamp: frame.timestamp); return
        }
        if requiresUserRecenter {
            publishInvalid("AR tracking recovered - recenter required", quality: 1, timestamp: frame.timestamp)
            return
        }
        // Using viewMatrix(for:) handles the camera's native sensor orientation.
        // Do NOT reuse Core Motion's portrait-to-landscape basis on this matrix.
        let transform = simd_inverse(frame.camera.viewMatrix(for: orientation))
        let rotation = simd_quatf(simd_float3x3(
            SIMD3(transform.columns.0.x, transform.columns.0.y, transform.columns.0.z),
            SIMD3(transform.columns.1.x, transform.columns.1.y, transform.columns.1.z),
            SIMD3(transform.columns.2.x, transform.columns.2.y, transform.columns.2.z)))
        let raw = Quaternion(x: Double(rotation.imag.x), y: Double(rotation.imag.y),
            z: Double(rotation.imag.z), w: Double(rotation.real)).normalized
        let position = Vec3(x: Double(transform.columns.3.x), y: Double(transform.columns.3.y), z: Double(transform.columns.3.z))
        let (head, relative) = origin.apply(camera: raw, position: position, headOffset: headOffset)
        var value = SpatialObservation(head: head, raw: raw, rate: Vec3(),
            spatial: SpatialSample(position: relative, velocity: Vec3(), orientationValid: true, positionValid: true, quality: 2),
            timestamp: frame.timestamp, generation: origin.generation, status: "AR normal - 6DoF")
        if let prior = previous, !needsRecenter {
            let dt = frame.timestamp - prior.timestamp
            if dt >= 1.0 / 240.0 && dt <= 0.1 {
                let velocity = Vec3(x: (relative.x-prior.spatial.position.x)/dt,
                    y: (relative.y-prior.spatial.position.y)/dt, z: (relative.z-prior.spatial.position.z)/dt)
                // Reject tracking jumps; do not predict a relocalization discontinuity.
                if max(abs(velocity.x), abs(velocity.y), abs(velocity.z)) <= 10 {
                    value.spatial.velocity = velocity
                    var delta = (head * prior.head.inverse).normalized
                    if delta.w < 0 { delta = Quaternion(x: -delta.x, y: -delta.y, z: -delta.z, w: -delta.w) }
                    let length = sqrt(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z)
                    if length > 1e-9 {
                        let factor = 2 * atan2(length, delta.w) / (length * dt)
                        value.rate = head.inverse.rotate(Vec3(x: delta.x*factor, y: delta.y*factor, z: delta.z*factor))
                    }
                } else {
                    requiresUserRecenter = true
                    publishInvalid("AR pose jumped - stop and recenter", quality: 1, timestamp: frame.timestamp)
                    return
                }
            }
        }
        needsRecenter = false; previous = value; last = value
        onSample?(value)
    }
    func sessionWasInterrupted(_ session: ARSession) {
        requiresUserRecenter = true
        publishInvalid("AR interrupted - position unavailable", quality: 0, timestamp: ProcessInfo.processInfo.systemUptime)
    }
    func sessionInterruptionEnded(_ session: ARSession) {
        diagnostics.update { $0.trackingStatus = "AR resuming / relocalizing" }
    }
    func sessionShouldAttemptRelocalization(_ session: ARSession) -> Bool { true }
    func session(_ session: ARSession, didFailWithError error: Error) {
        fail("AR session failed: \(error.localizedDescription). Reconnect to retry.")
    }
}
