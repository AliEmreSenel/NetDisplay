// SPDX-License-Identifier: GPL-2.0-only
import Foundation

enum NDError: Error, LocalizedError {
    case message(String)
    var errorDescription: String? { if case let .message(s) = self { return s }; return nil }
}

struct ByteWriter {
    var data = Data()
    mutating func u16(_ v: UInt16) { data.append(UInt8(v >> 8)); data.append(UInt8(v & 255)) }
    mutating func u32(_ v: UInt32) { u16(UInt16(v >> 16)); u16(UInt16(v & 65535)) }
    mutating func u64(_ v: UInt64) { u32(UInt32(v >> 32)); u32(UInt32(v & 0xffffffff)) }
    mutating func f32(_ v: Float) { u32(v.bitPattern) }
    mutating func fixedString(_ s: String, count: Int) {
        let bytes = Array(s.utf8.prefix(max(0, count - 1)))
        data.append(contentsOf: bytes)
        data.append(contentsOf: repeatElement(UInt8(0), count: count - bytes.count))
    }
}
struct ByteReader {
    private let bytes: [UInt8]
    private(set) var offset = 0
    init(_ data: Data) { bytes = Array(data) }
    var remaining: Int { bytes.count - offset }
    mutating func take(_ n: Int) throws -> Data {
        guard n >= 0, n <= remaining else { throw NDError.message("Truncated wire message") }
        defer { offset += n }
        return Data(bytes[offset..<(offset + n)])
    }
    mutating func u16() throws -> UInt16 {
        let d = try take(2); return UInt16(d[0]) << 8 | UInt16(d[1])
    }
    mutating func u32() throws -> UInt32 { let a = try u16(); return UInt32(a) << 16 | UInt32(try u16()) }
    mutating func u64() throws -> UInt64 { let a = try u32(); return UInt64(a) << 32 | UInt64(try u32()) }
    mutating func f32() throws -> Float { Float(bitPattern: try u32()) }
    mutating func fixedString(_ n: Int) throws -> String {
        let d = try take(n)
        return String(decoding: d.prefix { $0 != 0 }, as: UTF8.self)
    }
}

enum ControlType: UInt16 {
    case hello = 1, welcome = 2, input = 3, ping = 4, pong = 5, stop = 6
    case ready = 7, displayState = 8, challenge = 9, auth = 10, display = 11
    case stream = 12, streamReady = 13, reject = 14
}
struct ControlMessage {
    static let magic: UInt32 = 0x4e444333
    static let version: UInt16 = 6
    let type: UInt16
    let payload: Data
    init(_ type: ControlType, _ payload: Data = Data()) { self.type = type.rawValue; self.payload = payload }
    init(rawType: UInt16, payload: Data) { self.type = rawType; self.payload = payload }
    var encoded: Data {
        precondition(payload.count <= 512)
        var w = ByteWriter(); w.u32(Self.magic); w.u16(Self.version)
        w.u16(type); w.u32(UInt32(payload.count)); w.data.append(payload)
        return w.data
    }
    static func decodeHeader(_ header: Data) throws -> (type: UInt16, count: Int) {
        guard header.count == 12 else { throw NDError.message("Invalid control header length") }
        var r = ByteReader(header)
        guard try r.u32() == magic else { throw NDError.message("Not a NetDisplay server") }
        let v = try r.u16()
        guard v == version else { throw NDError.message("Server protocol v\(v); this app requires NetDisplay v6") }
        let t = try r.u16(), n = try r.u32()
        guard n <= 512 else { throw NDError.message("Oversized control message") }
        return (t, Int(n))
    }
    func require(_ expected: ControlType, size: Int) throws {
        if type == ControlType.reject.rawValue {
            throw NDError.message("Server rejected connection. Check password/key, video encryption policy, codec and server log.")
        }
        guard type == expected.rawValue, payload.count == size else {
            throw NDError.message("Expected \(expected) (\(size) bytes), received type \(type) (\(payload.count) bytes)")
        }
    }
}
struct StreamDescription {
    let displayID: UInt32, sessionID: UInt64
    let port: UInt16, width: UInt16, height: UInt16, fps: UInt16
    let outputName: String
    init(_ payload: Data) throws {
        guard payload.count == 84 else { throw NDError.message("Invalid STREAM length") }
        var r = ByteReader(payload)
        displayID = try r.u32(); sessionID = try r.u64(); port = try r.u16()
        width = try r.u16(); height = try r.u16(); fps = try r.u16()
        outputName = try r.fixedString(64)
        guard displayID == 1, sessionID != 0, port != 0,
              width > 0, width <= 4096, width % 2 == 0,
              height > 0, height <= 2160, height % 2 == 0, fps > 0, fps <= 60 else {
            throw NDError.message("Unsupported stream dimensions, rate or identifier")
        }
    }
}
enum Wire {
    static func hello(nonce: Data, encrypt: Bool, haveKey: Bool, codecs: UInt32) -> Data {
        precondition(nonce.count == 16)
        var w = ByteWriter()
        // Deliberately omit network-test, input and power flags: all are optional.
        w.u32((encrypt ? 2 : 0) | (haveKey ? 4 : 0)); w.u16(1); w.u16(0)
        w.u32(codecs); w.data.append(nonce); return w.data
    }
    static func display(width: UInt16, height: UInt16, fps: UInt16) -> Data {
        var w = ByteWriter(); w.u32(1); w.u16(width); w.u16(height); w.u16(fps)
        w.u16(0); w.fixedString("iPhone", count: 64); return w.data
    }
    static func streamReady() -> Data { var w = ByteWriter(); w.u32(1); return w.data }
}
