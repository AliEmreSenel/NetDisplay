// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import Darwin
import VideoToolbox
import CoreMedia

struct ConnectedInfo {
    let localIP: String
    let stream: StreamDescription
    let codec: String
    let encrypted: Bool
}
final class StreamConnection {
    private let settings: AppSettings
    private let secret: String
    private let lifetime = SocketLifetime()
    private let queue = DispatchQueue(label: "netdisplay.control", qos: .userInitiated)
    private let frames: FrameStore
    private let diagnostics: Diagnostics
    private let generation: UInt64
    var onStatus: (@MainActor (String) -> Void)?
    var onConnected: (@MainActor (ConnectedInfo) -> Void)?
    var onEnded: (@MainActor (String?) -> Void)?

    init(settings: AppSettings, secret: String, frames: FrameStore, diagnostics: Diagnostics) {
        self.settings = settings; self.secret = secret; self.frames = frames; self.diagnostics = diagnostics
        generation = frames.reset()
    }
    func start() { queue.async { self.run() } }
    func stop() { lifetime.cancel() }
    private func status(_ text: String) { DispatchQueue.main.async { [weak self] in self?.onStatus?(text) } }
    private func run() {
        var fd: Int32 = -1
        var receiver: UDPReceiver?
        var decoder: VideoDecoder?
        var key = [UInt8]()
        var finalError: String?
        defer {
            receiver?.stop(); decoder?.stop(); Crypto.wipe(&key)
            if fd >= 0 { lifetime.finish(fd) }
            let problem = lifetime.isCancelled ? nil : finalError
            DispatchQueue.main.async { [weak self] in self?.onEnded?(problem) }
        }
        do {
            try settings.validate()
            guard nd_ios_init() >= 0 else { throw NDError.message("Cryptographic initialization failed") }
            if settings.authMode == .password { status("Deriving password key"); key = try Crypto.passwordKey(secret) }
            else if settings.authMode == .key { key = try Crypto.parseKey(secret) }
            status("Connecting")
            fd = try SocketIO.connectTCP(host: settings.host, port: UInt16(settings.controlPort), lifetime: lifetime)
            let localIP = try SocketIO.localIP(fd)
            let cn = Crypto.random(16)
            var codecMask: UInt32 = 1
            if settings.allowHEVC && VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC) { codecMask |= 2 }
            try SocketIO.sendMessage(fd, ControlMessage(.hello, Wire.hello(nonce: Data(cn),
                encrypt: settings.encryptVideo, haveKey: !key.isEmpty, codecs: codecMask)))
            let challenge = try SocketIO.readMessage(fd)
            try challenge.require(.challenge, size: 20)
            var cr = ByteReader(challenge.payload)
            let flags = try cr.u32(), sn = Array(try cr.take(16))
            let requiresAuth = (flags & 4) != 0, usesPassword = (flags & 8) != 0
            guard requiresAuth == !key.isEmpty else {
                throw NDError.message(requiresAuth ? "Server requires authentication; enter its password/key" : "Server is unauthenticated; explicitly select No authentication or secure the server")
            }
            guard ((flags & 2) != 0) == settings.encryptVideo else { throw NDError.message("Server/client video encryption settings do not match") }
            if requiresAuth && settings.authMode == .password && !usesPassword {
                throw NDError.message("Server uses a PSK, not a password. Select 32-byte key (hex).")
            }
            if requiresAuth {
                status("Authenticating server")
                let proof = Crypto.proof(key: key, role: "client", client: cn, server: sn)
                try SocketIO.sendMessage(fd, ControlMessage(.auth, Data(proof)))
                let answer = try SocketIO.readMessage(fd); try answer.require(.auth, size: 32)
                let expected = Crypto.proof(key: key, role: "server", client: cn, server: sn)
                guard nd_crypto_verify(Array(answer.payload), expected) == 1 else { throw NDError.message("Server authentication failed") }
            }
            let (width, height) = settings.dimensions
            try SocketIO.sendMessage(fd, ControlMessage(.display,
                Wire.display(width: width, height: height, fps: UInt16(settings.fps))))
            status("Negotiating video / creating Hyprland output")
            let welcome = try SocketIO.readMessage(fd); try welcome.require(.welcome, size: 8)
            var wr = ByteReader(welcome.payload)
            let selectedFlags = try wr.u32(), count = try wr.u16(), codec = try wr.u16()
            guard count == 1, codec == 1 || (codec == 2 && codecMask & 2 != 0),
                  ((selectedFlags & 2) != 0) == settings.encryptVideo,
                  selectedFlags & 16 == 0 else { throw NDError.message("Unsupported server negotiation") }
            let streamMessage = try SocketIO.readMessage(fd); try streamMessage.require(.stream, size: 84)
            let stream = try StreamDescription(streamMessage.payload)
            guard stream.width == width, stream.height == height, stream.fps == UInt16(settings.fps) else {
                throw NDError.message("Server changed the requested display mode")
            }
            var streamKey: [UInt8]? = settings.encryptVideo
                ? Crypto.streamKey(key: key, session: stream.sessionID, client: cn, server: sn) : nil
            defer { if streamKey != nil { Crypto.wipe(&streamKey!) } }
            let video = VideoDecoder(hevc: codec == 2, width: stream.width, height: stream.height, fps: stream.fps,
                frames: frames, generation: generation, diagnostics: diagnostics)
            decoder = video
            receiver = try UDPReceiver(localIP: localIP, host: settings.host, port: stream.port,
                session: stream.sessionID, key: streamKey, diagnostics: diagnostics) { [weak video] in video?.submit($0) }
            try SocketIO.sendMessage(fd, ControlMessage(.streamReady, Wire.streamReady()))
            SocketIO.timeout(fd, seconds: 5)
            let info = ConnectedInfo(localIP: localIP, stream: stream, codec: codec == 2 ? "HEVC" : "H.264", encrypted: settings.encryptVideo)
            DispatchQueue.main.async { [weak self] in self?.onConnected?(info) }
            var pingTime: UInt64 = 0, lastPing: UInt64 = 0, lastPong = nd_ios_now_ns()
            while !lifetime.isCancelled {
                let now = nd_ios_now_ns()
                if now - lastPing >= 1_000_000_000 && pingTime == 0 {
                    try SocketIO.sendMessage(fd, ControlMessage(.ping)); pingTime = now; lastPing = now
                }
                if now - lastPong > 10_000_000_000 { throw NDError.message("Server heartbeat timed out") }
                var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                let ready = Darwin.poll(&p, 1, 100)
                if ready < 0 { if errno == EINTR { continue }; throw SocketIO.error("Control poll") }
                if ready == 0 { continue }
                let message = try SocketIO.readMessage(fd)
                switch message.type {
                case ControlType.pong.rawValue:
                    guard message.payload.isEmpty else { throw NDError.message("Invalid PONG") }
                    lastPong = nd_ios_now_ns()
                    if pingTime != 0 { diagnostics.update { $0.tcpRTTMS = Double(lastPong - pingTime) / 1e6 }; pingTime = 0 }
                case ControlType.ping.rawValue:
                    guard message.payload.isEmpty else { throw NDError.message("Invalid PING") }
                    try SocketIO.sendMessage(fd, ControlMessage(.pong))
                case ControlType.displayState.rawValue:
                    // Deliberately do not remotely change iPhone brightness or lock state.
                    guard message.payload.count == 8 else { throw NDError.message("Invalid display state") }
                case ControlType.stop.rawValue: throw NDError.message("Server stopped the video stream")
                default: throw NDError.message("Unexpected control message \(message.type)")
                }
            }
        } catch { finalError = error.localizedDescription }
    }
}
