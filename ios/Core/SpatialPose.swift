// SPDX-License-Identifier: GPL-2.0-only
import Foundation

struct SpatialSample {
    var position = Vec3()
    var velocity = Vec3()
    var orientationValid = false
    var positionValid = false
    /// 0 unavailable, 1 limited, 2 ARKit normal. Only 2 is a measured 6DoF pose.
    var quality: UInt32 = 0
}
struct SpatialObservation {
    var head = Quaternion(), raw = Quaternion(), rate = Vec3()
    var spatial = SpatialSample()
    var timestamp = 0.0
    var generation: UInt32 = 0
    var status = "AR initializing"
}
/// Pure coordinate math, shared with portable tests. AR input is a camera pose
/// already oriented for the landscape interface, +Y up and -Z forward.
struct SpatialOrigin {
    private var yaw: Quaternion?
    private var origin = Vec3()
    private(set) var generation: UInt32 = 0
    mutating func recenter() { yaw = nil }
    mutating func apply(camera: Quaternion, position: Vec3, headOffset: Vec3) -> (Quaternion, Vec3) {
        let offset = camera.rotate(headOffset)
        let head = Vec3(x: position.x + offset.x, y: position.y + offset.y, z: position.z + offset.z)
        if yaw == nil {
            let forward = camera.rotate(Vec3(z: -1))
            // Recenter yaw only. Pitch/roll must not tilt gravity or the floor.
            let angle = atan2(-forward.x, -forward.z)
            yaw = Quaternion(y: sin(angle / 2), w: cos(angle / 2))
            origin = head; generation &+= 1
        }
        let inverse = yaw!.inverse
        return ((inverse * camera).normalized,
                inverse.rotate(Vec3(x: head.x - origin.x, y: head.y - origin.y, z: head.z - origin.z)))
    }
}
