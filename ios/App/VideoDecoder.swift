// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo

/// One pending compressed access unit, one synchronous VT decode in progress.
/// No temporal processing and no FIFO of old compressed frames.
final class VideoDecoder {
    private let queue = DispatchQueue(label: "netdisplay.decode", qos: .userInteractive)
    private let lock = NSLock()
    private var pending: EncodedFrame?
    private var running = false, stopped = false
    private var vt: VTDecompressionSession?
    private var format: CMVideoFormatDescription?
    private var parameterSets: [Int: Data] = [:]
    private var needsRandomAccess = true
    private let hevc: Bool
    private let frames: FrameStore
    private let generation: UInt64
    private let diagnostics: Diagnostics
    private let width: Int32, height: Int32, fps: Int32

    init(hevc: Bool, width: UInt16, height: UInt16, fps: UInt16, frames: FrameStore,
         generation: UInt64, diagnostics: Diagnostics) {
        self.hevc = hevc; self.width = Int32(width); self.height = Int32(height); self.fps = Int32(fps)
        self.frames = frames; self.generation = generation; self.diagnostics = diagnostics
    }
    func submit(_ frame: EncodedFrame) {
        lock.lock()
        guard !stopped else { lock.unlock(); return }
        if pending != nil { diagnostics.update { $0.decoderReplaced += 1 } }
        pending = frame
        let schedule = !running
        running = true
        lock.unlock()
        if schedule { queue.async { [weak self] in self?.drain() } }
    }
    private func drain() {
        while true {
            lock.lock()
            guard !stopped, let frame = pending else {
                running = false; pending = nil; lock.unlock(); return
            }
            pending = nil; lock.unlock()
            autoreleasepool {
                do { try decode(frame) }
                catch {
                    diagnostics.update { $0.decodeErrors += 1; $0.lastError = error.localizedDescription }
                    needsRandomAccess = true
                }
            }
        }
    }
    func stop() {
        lock.lock(); stopped = true; pending = nil; lock.unlock()
        queue.async { [weak self] in self?.invalidate() }
    }
    private func invalidate() {
        if let vt = vt { VTDecompressionSessionInvalidate(vt) }
        vt = nil; format = nil
    }
    deinit { if let vt = vt { VTDecompressionSessionInvalidate(vt) } }

    private func configureIfNeeded(_ sets: [Int: Data]) throws {
        var changed = false
        for (type, data) in sets where parameterSets[type] != data {
            parameterSets[type] = data; changed = true
        }
        if vt != nil && !changed { return }
        let keys = hevc ? [32, 33, 34] : [7, 8]
        let data = keys.compactMap { parameterSets[$0] }
        guard data.count == keys.count else { return }
        let buffers: [UnsafeMutablePointer<UInt8>] = data.map {
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: $0.count)
            $0.copyBytes(to: p, count: $0.count); return p
        }
        defer { buffers.forEach { $0.deallocate() } }
        var pointers = buffers.map { UnsafePointer($0) }
        var sizes = data.map { $0.count }
        var newFormat: CMVideoFormatDescription?
        let status: OSStatus
        if hevc {
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(allocator: kCFAllocatorDefault,
                parameterSetCount: pointers.count, parameterSetPointers: &pointers,
                parameterSetSizes: &sizes, nalUnitHeaderLength: 4, extensions: nil,
                formatDescriptionOut: &newFormat)
        } else {
            status = CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: kCFAllocatorDefault,
                parameterSetCount: pointers.count, parameterSetPointers: &pointers,
                parameterSetSizes: &sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &newFormat)
        }
        guard status == noErr, let f = newFormat else { throw NDError.message("Invalid codec parameter sets (\(status))") }
        let dimensions = CMVideoFormatDescriptionGetDimensions(f)
        guard dimensions.width == width, dimensions.height == height else {
            throw NDError.message("Bitstream dimensions do not match the negotiated stream")
        }
        invalidate()
        let specification: [String: Any] = [kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String: true]
        let attributes: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any]
        ]
        var session: VTDecompressionSession?
        let created = VTDecompressionSessionCreate(allocator: kCFAllocatorDefault,
            formatDescription: f, decoderSpecification: specification as CFDictionary,
            imageBufferAttributes: attributes as CFDictionary, outputCallback: nil,
            decompressionSessionOut: &session)
        guard created == noErr, let session = session else {
            throw NDError.message("Cannot create hardware \(hevc ? "HEVC" : "H.264") decoder (\(created)). Try H.264 / 1280x720.")
        }
        _ = VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        var hardware: CFTypeRef?
        _ = VTSessionCopyProperty(session, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                                  allocator: kCFAllocatorDefault, valueOut: &hardware)
        diagnostics.update { $0.hardwareDecoder = (hardware as? NSNumber)?.boolValue ?? true }
        vt = session; format = f; needsRandomAccess = true
    }
    private func decode(_ input: EncodedFrame) throws {
        let start = nd_ios_now_ns()
        let parsed = try AnnexBFrame(input.bytes, hevc: hevc)
        try configureIfNeeded(parsed.parameterSets)
        guard parsed.hasPicture, let session = vt, let format = format else { return }
        if needsRandomAccess && !parsed.randomAccess { return }
        let size = parsed.avcc.count
        var block: CMBlockBuffer?
        var result = CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault,
            memoryBlock: nil, blockLength: size, blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil, offsetToData: 0, dataLength: size, flags: 0,
            blockBufferOut: &block)
        guard result == noErr, let block = block else { throw NDError.message("Allocate bitstream buffer: \(result)") }
        result = parsed.avcc.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: size)
        }
        guard result == noErr else { throw NDError.message("Copy bitstream: \(result)") }
        var timing = CMSampleTimingInfo(duration: .invalid,
            presentationTimeStamp: CMTime(value: Int64(input.sequence), timescale: fps), decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?, sampleSize = size
        result = CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block,
            formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 1,
            sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &sampleSize,
            sampleBufferOut: &sample)
        guard result == noErr, let sample = sample else { throw NDError.message("Create video sample: \(result)") }
        var delivered = false
        // Both decode flags clear: output is delivered before this call returns.
        // This makes the one-slot pending-frame policy meaningful at the decoder.
        result = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample, flags: [], infoFlagsOut: nil) {
            [weak self] status, _, image, _, _ in
            guard let self = self else { return }
            guard status == noErr, let image = image else {
                self.diagnostics.update { $0.lastError = "Hardware decode callback: \(status)" }; return
            }
            let now = nd_ios_now_ns()
            self.frames.publish(DecodedFrame(pixels: image, sequence: input.sequence,
                firstPacketNS: input.firstNS, decodedNS: now), generation: self.generation)
            self.diagnostics.update { $0.decoded += 1; $0.decodeMS = Double(now - start) / 1e6 }
            delivered = true
        }
        guard result == noErr else { throw NDError.message("Hardware decode: \(result)") }
        needsRandomAccess = !delivered
    }
}
