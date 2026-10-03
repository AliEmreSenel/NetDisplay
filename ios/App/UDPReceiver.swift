// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import Darwin

struct EncodedFrame {
    let bytes: Data
    let sequence: UInt32
    let firstNS: UInt64
    let completeNS: UInt64
}
final class UDPReceiver {
    private var source: DispatchSourceRead?
    private let queue = DispatchQueue(label: "netdisplay.udp", qos: .userInteractive)
    private let diagnostics: Diagnostics
    private let onFrame: (EncodedFrame) -> Void
    private var lastReport: UInt64 = 0

    init(localIP: String, host: String, port: UInt16, session: UInt64,
         key: [UInt8]?, diagnostics: Diagnostics, onFrame: @escaping (EncodedFrame) -> Void) throws {
        self.diagnostics = diagnostics; self.onFrame = onFrame
        let fd = try SocketIO.bindUDP(localIP: localIP, port: port)
        let assembler: OpaquePointer?
        if let key = key { assembler = key.withUnsafeBufferPointer { nd_ios_assembler_create(session, $0.baseAddress) } }
        else { assembler = nd_ios_assembler_create(session, nil) }
        guard let assembly = assembler else { close(fd); throw NDError.message("Cannot allocate video reassembler") }
        let remote = try SocketIO.address(host, port: 0).sin_addr.s_addr
        let reader = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        reader.setEventHandler { [weak self] in
            guard let self = self else { return }
            var packet = [UInt8](repeating: 0, count: 1473)
            // Bound each callback; another ready event drains any remaining data.
            for _ in 0..<256 {
                var addr = sockaddr_in(), size = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = packet.withUnsafeMutableBytes { bytes in
                    withUnsafeMutablePointer(to: &addr) {
                        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                            Darwin.recvfrom(fd, bytes.baseAddress, bytes.count, 0, $0, &size)
                        }
                    }
                }
                if n < 0 {
                    if errno == EINTR { continue }
                    if errno != EAGAIN && errno != EWOULDBLOCK {
                        self.diagnostics.update { $0.lastError = "UDP receive: \(String(cString: strerror(errno)))" }
                    }
                    break
                }
                // Sender uses an ephemeral source port; filter IP + session, not port.
                guard addr.sin_family == sa_family_t(AF_INET), addr.sin_addr.s_addr == remote else { continue }
                let result = packet.withUnsafeBufferPointer { nd_ios_assembler_feed(assembly, $0.baseAddress, n) }
                if result == 1 {
                    let s = nd_ios_assembler_stats(assembly)
                    guard let bytes = nd_ios_assembler_bytes(assembly) else { continue }
                    self.onFrame(EncodedFrame(bytes: Data(bytes: bytes, count: Int(s.frame_bytes)),
                        sequence: s.sequence, firstNS: s.first_ns, completeNS: s.complete_ns))
                    self.diagnostics.update {
                        $0.assemblyMS = Double(s.complete_ns - s.first_ns) / 1e6
                        $0.lastVideoNS = s.complete_ns
                    }
                }
            }
            let now = nd_ios_now_ns()
            if now - self.lastReport >= 100_000_000 {
                let s = nd_ios_assembler_stats(assembly)
                self.diagnostics.update {
                    $0.packets = s.packets; $0.received = s.complete; $0.bytes = s.bytes
                    $0.partialDrops = s.partial_drop; $0.malformed = s.bad; $0.duplicates = s.duplicate
                }
                self.lastReport = now
            }
        }
        // The source owns both objects until its final callback has completed.
        reader.setCancelHandler { close(fd); nd_ios_assembler_destroy(assembly) }
        source = reader
        reader.resume()
    }
    func stop() { source?.cancel(); source = nil }
    deinit { stop() }
}
