// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import Darwin

/// Owner closes the descriptor; cancel only shuts it down, avoiding FD-reuse races.
final class SocketLifetime {
    private let lock = NSLock()
    private var fd: Int32 = -1
    private var cancelled = false
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    func install(_ value: Int32) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard !cancelled else { return false }; fd = value; return true
    }
    func cancel() {
        lock.lock(); defer { lock.unlock() }; cancelled = true
        if fd >= 0 { _ = Darwin.shutdown(fd, SHUT_RDWR) }
    }
    func finish(_ value: Int32) {
        lock.lock(); defer { lock.unlock() }
        if fd == value { fd = -1 }; _ = Darwin.close(value)
    }
}
enum SocketIO {
    static func isIPv4(_ host: String) -> Bool {
        var a = in_addr(); return host.withCString { inet_pton(AF_INET, $0, &a) } == 1
    }
    static func address(_ ip: String, port: UInt16) throws -> sockaddr_in {
        var a = sockaddr_in(); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        a.sin_family = sa_family_t(AF_INET); a.sin_port = port.bigEndian
        guard ip.withCString({ inet_pton(AF_INET, $0, &a.sin_addr) }) == 1 else { throw NDError.message("Invalid IPv4 address: \(ip)") }
        return a
    }
    static func error(_ operation: String) -> NDError {
        .message("\(operation): \(String(cString: strerror(errno)))")
    }
    static func setNoSignal(_ fd: Int32) {
        var one: Int32 = 1
        _ = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one)))
    }
    static func timeout(_ fd: Int32, seconds: Int) {
        var value = timeval(tv_sec: seconds, tv_usec: 0)
        _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, socklen_t(MemoryLayout.size(ofValue: value)))
        _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, socklen_t(MemoryLayout.size(ofValue: value)))
    }
    static func connectTCP(host: String, port: UInt16, lifetime: SocketLifetime) throws -> Int32 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw error("Create TCP socket") }
        guard lifetime.install(fd) else { close(fd); throw NDError.message("Cancelled") }
        do {
            setNoSignal(fd)
            var one: Int32 = 1
            _ = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4)
            _ = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, 4)
            let old = fcntl(fd, F_GETFL)
            guard old >= 0, fcntl(fd, F_SETFL, old | O_NONBLOCK) == 0 else { throw error("Configure TCP") }
            var a = try address(host, port: port)
            let result = withUnsafePointer(to: &a) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
            if result != 0 {
                guard errno == EINPROGRESS else { throw error("Connect") }
                let deadline = nd_ios_now_ns() + 20_000_000_000
                while true {
                    guard !lifetime.isCancelled else { throw NDError.message("Cancelled") }
                    var p = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
                    let ready = Darwin.poll(&p, 1, 100)
                    if ready > 0 {
                        var code: Int32 = 0, length = socklen_t(MemoryLayout<Int32>.size)
                        guard getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &length) == 0 else { throw error("TCP status") }
                        guard code == 0 else { throw NDError.message("Connect: \(String(cString: strerror(code))). Check Local Network permission, Linux IP and TCP port.") }
                        break
                    }
                    if ready < 0 && errno != EINTR { throw error("Wait for connection") }
                    if nd_ios_now_ns() >= deadline { throw NDError.message("Connection timed out. Allow Local Network access and check the USB interface address/firewall.") }
                }
            }
            guard fcntl(fd, F_SETFL, old) == 0 else { throw error("Restore TCP mode") }
            timeout(fd, seconds: 30)
            return fd
        } catch { lifetime.finish(fd); throw error }
    }
    static func localIP(_ fd: Int32) throws -> String {
        var a = sockaddr_in(), size = socklen_t(MemoryLayout<sockaddr_in>.size)
        let status = withUnsafeMutablePointer(to: &a) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &size) }
        }
        guard status == 0 else { throw error("Read local IP") }
        var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        guard inet_ntop(AF_INET, &a.sin_addr, &text, socklen_t(text.count)) != nil else { throw error("Format local IP") }
        return String(cString: text)
    }
    static func bindUDP(localIP: String, port: UInt16) throws -> Int32 {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { throw error("Create UDP socket") }
        do {
            setNoSignal(fd)
            // No SO_REUSEPORT: a second process must not silently steal video.
            var size: Int32 = 1_048_576
            _ = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, 4)
            var a = try address(localIP, port: port)
            let rc = withUnsafePointer(to: &a) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
            guard rc == 0 else { throw error("Bind UDP \(localIP):\(port)") }
            let flags = fcntl(fd, F_GETFL)
            guard flags >= 0, fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 else { throw error("Set nonblocking UDP") }
            return fd
        } catch { close(fd); throw error }
    }
    static func readExact(_ fd: Int32, count: Int) throws -> Data {
        var result = Data(count: count)
        try result.withUnsafeMutableBytes { (p: UnsafeMutableRawBufferPointer) in
            var offset = 0
            while offset < count {
                let n = Darwin.recv(fd, p.baseAddress!.advanced(by: offset), count - offset, 0)
                if n > 0 { offset += n; continue }
                if n == 0 { throw NDError.message("Server closed the connection") }
                if errno == EINTR { continue }
                throw error("Read TCP")
            }
        }
        return result
    }
    static func readMessage(_ fd: Int32) throws -> ControlMessage {
        let header = try ControlMessage.decodeHeader(readExact(fd, count: 12))
        return ControlMessage(rawType: header.type, payload: try readExact(fd, count: header.count))
    }
    static func sendMessage(_ fd: Int32, _ m: ControlMessage) throws {
        let bytes = m.encoded
        try bytes.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            var offset = 0
            while offset < p.count {
                let n = Darwin.send(fd, p.baseAddress!.advanced(by: offset), p.count - offset, 0)
                if n > 0 { offset += n; continue }
                if n < 0 && errno == EINTR { continue }
                throw error("Write TCP")
            }
        }
    }
}
