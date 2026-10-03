// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import CoreVideo

struct DiagnosticSnapshot {
    var packets: UInt64 = 0, received: UInt64 = 0, decoded: UInt64 = 0, drawn: UInt64 = 0
    var bytes: UInt64 = 0, partialDrops: UInt64 = 0, malformed: UInt64 = 0
    var duplicates: UInt64 = 0, decoderReplaced: UInt64 = 0, decodeErrors: UInt64 = 0
    var gpuSkipped: UInt64 = 0, motionSent: UInt64 = 0, motionDropped: UInt64 = 0
    var assemblyMS = 0.0, decodeMS = 0.0, rxToSubmitMS = 0.0, tcpRTTMS = 0.0
    var lastVideoNS: UInt64 = 0, motionHz = 0.0
    var hardwareDecoder = false
    var head = Quaternion()
    var lastError = ""
}
final class Diagnostics {
    private let lock = NSLock()
    private var value = DiagnosticSnapshot()
    func update(_ f: (inout DiagnosticSnapshot) -> Void) { lock.lock(); defer { lock.unlock() }; f(&value) }
    func snapshot() -> DiagnosticSnapshot { lock.lock(); defer { lock.unlock() }; return value }
    func reset() { lock.lock(); value = DiagnosticSnapshot(); lock.unlock() }
}
struct DecodedFrame {
    let pixels: CVPixelBuffer
    let sequence: UInt32
    let firstPacketNS: UInt64
    let decodedNS: UInt64
}
final class FrameStore {
    private let lock = NSLock()
    private var frame: DecodedFrame?
    private var generation: UInt64 = 0
    @discardableResult func reset() -> UInt64 {
        lock.lock(); defer { lock.unlock() }; generation &+= 1; frame = nil; return generation
    }
    func publish(_ f: DecodedFrame, generation token: UInt64) {
        lock.lock(); defer { lock.unlock() }; if generation == token { frame = f }
    }
    func latest() -> DecodedFrame? { lock.lock(); defer { lock.unlock() }; return frame }
}
