// SPDX-License-Identifier: GPL-2.0-only
import Foundation

/// Convert the sender's Annex-B access unit into a length-prefixed access unit.
/// Parameter sets are also returned for VideoToolbox's format description.
struct AnnexBFrame {
    let avcc: Data
    let parameterSets: [Int: Data]
    let hasPicture: Bool
    let randomAccess: Bool

    init(_ bytes: Data, hevc: Bool) throws {
        guard !bytes.isEmpty, bytes.count <= 4 * 1024 * 1024 else {
            throw NDError.message("Invalid compressed-frame size")
        }
        var result = Data(); result.reserveCapacity(bytes.count)
        var sets: [Int: Data] = [:]
        var picture = false, random = false
        try bytes.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let b = raw.bindMemory(to: UInt8.self)
            var starts: [(Int, Int)] = []
            var i = 0
            while i + 2 < b.count {
                if b[i] == 0 && b[i + 1] == 0 {
                    if b[i + 2] == 1 { starts.append((i, i + 3)); i += 3; continue }
                    if i + 3 < b.count && b[i + 2] == 0 && b[i + 3] == 1 {
                        starts.append((i, i + 4)); i += 4; continue
                    }
                }
                i += 1
            }
            guard !starts.isEmpty, starts[0].0 <= 3 else {
                throw NDError.message("Expected Annex-B H.264/HEVC bitstream")
            }
            for j in starts.indices {
                let begin = starts[j].1
                var end = j + 1 < starts.count ? starts[j + 1].0 : b.count
                // Annex-B trailing_zero_8bits are not part of the NAL unit.
                while end > begin && b[end - 1] == 0 { end -= 1 }
                guard end > begin else { continue }
                let type = hevc ? Int((b[begin] >> 1) & 63) : Int(b[begin] & 31)
                if hevc && end - begin < 2 { throw NDError.message("Truncated HEVC NAL") }
                if (hevc && [32, 33, 34].contains(type)) || (!hevc && [7, 8].contains(type)) {
                    sets[type] = Data(b[begin..<end])
                }
                if hevc ? type <= 31 : (1...5).contains(type) { picture = true }
                if hevc ? (16...23).contains(type) : type == 5 { random = true }
                var w = ByteWriter(); w.u32(UInt32(end - begin)); result.append(w.data)
                result.append(contentsOf: b[begin..<end])
            }
        }
        avcc = result; parameterSets = sets; hasPicture = picture; randomAccess = random
    }
}
