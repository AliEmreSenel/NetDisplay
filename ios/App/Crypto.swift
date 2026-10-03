// SPDX-License-Identifier: GPL-2.0-only
import Foundation

enum Crypto {
    static func random(_ count: Int) -> [UInt8] {
        var b = [UInt8](repeating: 0, count: count)
        b.withUnsafeMutableBytes { nd_crypto_random($0.baseAddress, count) }
        return b
    }
    static func parseKey(_ text: String) throws -> [UInt8] {
        var key = [UInt8](repeating: 0, count: 32)
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard value.withCString({ nd_crypto_key_from_hex(&key, $0) }) == 0 else {
            throw NDError.message("Key must contain exactly 64 hexadecimal characters")
        }
        return key
    }
    static func passwordKey(_ text: String) throws -> [UInt8] {
        guard !text.isEmpty, !text.utf8.contains(0) else { throw NDError.message("Enter the server password") }
        var key = [UInt8](repeating: 0, count: 32)
        guard text.withCString({ nd_crypto_password_key(&key, $0) }) == 0 else {
            throw NDError.message("Password derivation failed (memory pressure or invalid password)")
        }
        return key
    }
    static func proof(key: [UInt8], role: String, client: [UInt8], server: [UInt8]) -> [UInt8] {
        var result = [UInt8](repeating: 0, count: 32)
        role.withCString { nd_crypto_proof(&result, key, $0, client, server) }
        return result
    }
    static func streamKey(key: [UInt8], session: UInt64, client: [UInt8], server: [UInt8]) -> [UInt8] {
        var result = [UInt8](repeating: 0, count: 32)
        nd_crypto_stream_key(&result, key, session, client, server)
        return result
    }
    static func authenticatedMotion(_ sample: MotionSample, key: [UInt8]) -> Data {
        precondition(key.count == 32)
        var data = sample.body
        var tag = [UInt8](repeating: 0, count: 32)
        data.withUnsafeBytes { p in
            nd_ios_hmac(&tag, p.bindMemory(to: UInt8.self).baseAddress, p.count, key)
        }
        data.append(contentsOf: tag)
        return data
    }
    static func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined() }
    static func wipe(_ b: inout [UInt8]) { b.withUnsafeMutableBytes { nd_ios_wipe($0.baseAddress, $0.count) } }
}
