// SPDX-License-Identifier: GPL-2.0-only
import Foundation
var checks = 0
func check(_ condition: Bool, _ name: String) {
    checks += 1
    if !condition { fatalError("FAIL: \(name)") }
}
func rejects(_ name: String, _ f: () throws -> Void) {
    do { try f(); fatalError("FAIL: accepted \(name)") } catch { checks += 1 }
}
var w = ByteWriter(); w.u16(0xabcd); w.u32(0x12345678); w.u64(0x0102030405060708); w.f32(1.25)
var r = ByteReader(w.data)
check(try r.u16() == 0xabcd, "u16")
check(try r.u32() == 0x12345678, "u32")
check(try r.u64() == 0x0102030405060708, "u64")
check(try r.f32() == 1.25, "float")
rejects("short read") { _ = try r.u16() }
let hello = Wire.hello(nonce: Data(0..<16), encrypt: true, haveKey: true, codecs: 1)
check(hello.count == 28, "v6 HELLO size")
check(Array(hello.prefix(12)) == [0,0,0,6,0,1,0,0,0,0,0,1], "HELLO capabilities and byte order")
let packet = ControlMessage(.hello, hello).encoded
let header = try ControlMessage.decodeHeader(Data(packet.prefix(12)))
check(header.type == 1 && header.count == 28, "control header")
check(Wire.display(width: 1920, height: 1080, fps: 60).count == 76, "DISPLAY size")
check(Wire.streamReady() == Data([0,0,0,1]), "STREAM_READY")
var oldHeader = Data(packet.prefix(12)); oldHeader[5] = 5
rejects("protocol v5") { _ = try ControlMessage.decodeHeader(oldHeader) }
var oversized = Data(packet.prefix(12)); oversized[10] = 3
rejects("oversized control") { _ = try ControlMessage.decodeHeader(oversized) }
rejects("reject message") { try ControlMessage(.reject).require(.welcome, size: 8) }
var sw = ByteWriter(); sw.u32(1); sw.u64(0x123456789abcdef0); sw.u16(5000)
sw.u16(1920); sw.u16(1080); sw.u16(60); sw.fixedString("netdisplay-2-1", count: 64)
let stream = try StreamDescription(sw.data)
check(stream.sessionID == 0x123456789abcdef0 && stream.outputName == "netdisplay-2-1", "STREAM parse")
var invalidStream = sw.data; invalidStream[17] = 57 // odd height 1081
rejects("odd stream height") { _ = try StreamDescription(invalidStream) }
let avc = Data([0,0,0,1,0x67,0x42,0x80,0,0,1,0x68,0xee,0,0,0,1,0x65,0x88,0x80])
let parsed = try AnnexBFrame(avc, hevc: false)
check(parsed.parameterSets[7] == Data([0x67,0x42,0x80]), "SPS extraction")
check(parsed.parameterSets[8] == Data([0x68,0xee]), "PPS extraction")
check(parsed.hasPicture && parsed.randomAccess, "IDR detection")
check(Array(parsed.avcc.prefix(4)) == [0,0,0,3], "AVCC prefix")
let hevc = try AnnexBFrame(Data([0,0,1,64,1,0x80,0,0,1,66,1,0x80,0,0,1,68,1,0x80,0,0,1,38,1,0x80]), hevc: true)
check(hevc.parameterSets.count == 3 && hevc.hasPicture && hevc.randomAccess, "HEVC parse")
rejects("non Annex-B") { _ = try AnnexBFrame(Data([1,2,3]), hevc: false) }
let q = Quaternion(y: sin(.pi/4), w: cos(.pi/4))
let forward = q.rotate(Vec3(z: -1))
check(abs(forward.x + 1) < 1e-9 && abs(forward.z) < 1e-9, "quaternion rotation")
let identity = q.inverse * q
check(abs(identity.w - 1) < 1e-9 && abs(identity.y) < 1e-9, "recenter identity")
let basis = Quaternion.headsetBasis(cameraOnLeft: true)
let screenRight = basis.rotate(Vec3(x: 1))
check(abs(screenRight.x) < 1e-9 && abs(screenRight.y + 1) < 1e-9, "camera-left landscape basis")
let motion = MotionSample(sequence: 7, session: 0x0102030405060708, sampleNS: 123, sendNS: 456,
                          head: Quaternion(), raw: Quaternion(), rate: Vec3(x: 1,y: 2,z: 3), acceleration: Vec3(),
                          recenterGeneration: 2, cameraOnLeft: true)
check(motion.body.count == 96, "motion body size")
check(Array(motion.body.prefix(12)) == [78,68,77,49,0,1,0,1,0,0,0,7], "motion header bytes")
if CommandLine.arguments.count > 1 { try motion.body.write(to: URL(fileURLWithPath: CommandLine.arguments[1])) }
let touchHello = Wire.hello(nonce: Data(repeating: 0, count: 16), encrypt: false, haveKey: false, codecs: 1, touch: true)
check(Array(touchHello.prefix(4)) == [0, 0, 0, 65], "touch capability request")
check(TouchWire.position(x: 0, y: 0, width: 200, height: 100, sourceWidth: 100, sourceHeight: 100, clamp: false) == nil, "letterbox taps ignored")
let center = TouchWire.position(x: 100, y: 50, width: 200, height: 100, sourceWidth: 100, sourceHeight: 100, clamp: false)!
check(center.0 == 32768 && center.1 == 32768, "touch maps to video center")
let edge = TouchWire.position(x: 300, y: -1, width: 200, height: 100, sourceWidth: 100, sourceHeight: 100, clamp: true)!
check(edge.0 == 65535 && edge.1 == 0, "drag clamps outside video")
let touches = TouchWire.frame(contacts: [TouchContact(slot: 0, x: 10, y: 20, pressure: 255, major: 8),
    TouchContact(slot: 1, x: 30, y: 40, pressure: 100, major: 6)], began: [0, 1], ended: [])
check(touches.count == 16, "two contacts in one SYN frame")
check(touches.last!.payload == Data([3,0,0,0,0,0,0,0,0,0,0,0]), "touch SYN report")
let releases = TouchWire.frame(contacts: [], began: [], ended: [0, 1])
check(releases.count == 6, "all contacts release together")
check(releases[2].payload == Data([3,0,0,3,0,57,0,0,255,255,255,255]), "signed tracking release wire format")
check(releases[0].payload == Data([3,0,0,1,1,74,0,0,0,0,0,0]), "BTN_TOUCH release")

var origin = SpatialOrigin()
let tilted = q * Quaternion(x: sin(0.15), w: cos(0.15))
let zero = origin.apply(camera: tilted, position: Vec3(x: 2, y: 1.6, z: 3), headOffset: Vec3())
check(zero.1 == Vec3() && origin.generation == 1, "AR anchors first normal pose")
check(abs(zero.0.x - sin(0.15)) < 1e-9 && abs(zero.0.y) < 1e-9, "AR yaw-only recenter preserves pitch")
let moved = origin.apply(camera: tilted, position: Vec3(x: 1.8, y: 1.8, z: 3), headOffset: Vec3())
check(abs(moved.1.y - 0.2) < 1e-9 && abs(moved.1.z + 0.2) < 1e-9, "AR metres and rotated axes")
origin.recenter()
let reset = origin.apply(camera: Quaternion(), position: Vec3(x: 3, y: 2, z: 4), headOffset: Vec3())
check(reset.1 == Vec3() && origin.generation == 2, "AR explicit recenter")
var offsetOrigin = SpatialOrigin()
_ = offsetOrigin.apply(camera: Quaternion(), position: Vec3(), headOffset: Vec3(z: 0.1))
let offsetTurn = offsetOrigin.apply(camera: q, position: Vec3(), headOffset: Vec3(z: 0.1))
check(abs(offsetTurn.1.x - 0.1) < 1e-9 && abs(offsetTurn.1.z + 0.1) < 1e-9, "camera-to-head lever arm rotates")
var arMotion = motion
arMotion.spatial = SpatialSample(position: Vec3(x: 0.25, y: -0.125, z: -0.5),
    velocity: Vec3(x: 0.1, y: 0.2, z: 0.3), orientationValid: true, positionValid: true, quality: 2)
check(arMotion.body.count == 128, "NDM2 body size")
check(Array(arMotion.body.prefix(12)) == [78,68,77,50,0,2,0,7,0,0,0,7], "NDM2 header and flags")
var spatialReader = ByteReader(Data(arMotion.body.suffix(32)))
check(try spatialReader.f32() == 0.25 && spatialReader.f32() == -0.125 && spatialReader.f32() == -0.5, "NDM2 position byte order")
_ = try spatialReader.f32(); _ = try spatialReader.f32(); _ = try spatialReader.f32()
check(try spatialReader.u32() == 2 && spatialReader.u32() == 0, "NDM2 quality and reserved")
if CommandLine.arguments.count > 1 {
    let output = URL(fileURLWithPath: CommandLine.arguments[1]).deletingLastPathComponent().appendingPathComponent("motion-v2-body.bin")
    try arMotion.body.write(to: output)
}
arMotion.spatial?.positionValid = false; arMotion.spatial?.orientationValid = false; arMotion.spatial?.quality = 1
check(arMotion.body[7] == 1, "NDM2 limited tracking does not claim valid pose")
print("PASS: \(checks) Swift protocol / Annex-B / motion / spatial checks")
