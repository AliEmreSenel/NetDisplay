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
print("PASS: \(checks) Swift protocol / Annex-B / motion checks")
