// SPDX-License-Identifier: GPL-2.0-only
import Foundation

struct Vec3: Equatable {
    var x: Double = 0, y: Double = 0, z: Double = 0
}
struct Quaternion: Equatable {
    var x: Double = 0, y: Double = 0, z: Double = 0, w: Double = 1
    var normalized: Quaternion {
        let n = sqrt(x*x + y*y + z*z + w*w)
        guard n.isFinite, n > 1e-12 else { return Quaternion() }
        return Quaternion(x: x/n, y: y/n, z: z/n, w: w/n)
    }
    var inverse: Quaternion { Quaternion(x: -x, y: -y, z: -z, w: w).normalized }
    static func * (a: Quaternion, b: Quaternion) -> Quaternion {
        Quaternion(x: a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                   y: a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                   z: a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                   w: a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z)
    }
    func rotate(_ v: Vec3) -> Vec3 {
        let a = self * Quaternion(x: v.x, y: v.y, z: v.z, w: 0) * inverse
        return Vec3(x: a.x, y: a.y, z: a.z)
    }
    /// Headset +X=screen right, +Y=screen up, +Z=towards the wearer.
    /// basis maps landscape headset coordinates to portrait device coordinates.
    static func headsetBasis(cameraOnLeft: Bool) -> Quaternion {
        let angle = cameraOnLeft ? -Double.pi / 2 : Double.pi / 2
        return Quaternion(z: sin(angle / 2), w: cos(angle / 2))
    }
}
struct MotionSample {
    var sequence: UInt32 = 0
    var session: UInt64 = 0
    var sampleNS: UInt64 = 0, sendNS: UInt64 = 0
    var head = Quaternion(), raw = Quaternion()
    var rate = Vec3(), acceleration = Vec3()
    var recenterGeneration: UInt32 = 0
    var cameraOnLeft = true
    var spatial: SpatialSample? = nil
    /// 96-byte NDM1 or 128-byte NDM2 body, followed by HMAC-SHA256.
    var body: Data {
        var w = ByteWriter(); w.u32(spatial == nil ? 0x4e444d31 : 0x4e444d32); w.u16(spatial == nil ? 1 : 2)
        var flags: UInt16 = cameraOnLeft ? 1 : 0
        if let spatial = spatial {
            if spatial.positionValid { flags |= 2 }
            if spatial.orientationValid { flags |= 4 }
        }
        w.u16(flags); w.u32(sequence); w.u64(session)
        w.u64(sampleNS); w.u64(sendNS)
        for v in [head.x, head.y, head.z, head.w, rate.x, rate.y, rate.z,
                  acceleration.x, acceleration.y, acceleration.z,
                  raw.x, raw.y, raw.z, raw.w] { w.f32(Float(v)) }
        w.u32(recenterGeneration)
        if let spatial = spatial {
            for v in [spatial.position.x, spatial.position.y, spatial.position.z,
                      spatial.velocity.x, spatial.velocity.y, spatial.velocity.z] { w.f32(Float(v)) }
            w.u32(spatial.quality); w.u32(0)
        }
        return w.data
    }
}
