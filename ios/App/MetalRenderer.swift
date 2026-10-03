// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import MetalKit
import SwiftUI
import CoreVideo
import QuartzCore

struct RenderParameters {
    var dimensions = SIMD4<Float>(1,1,1,1)
    var lens = SIMD4<Float>(0,0,1,0)
    var controls = SIMD4<Float>(0,0,0,0)
    var orientation = SIMD4<Float>(0,0,0,1)
    var color = SIMD4<Float>(0,0,Float.pi/2,0)
}
@MainActor
final class MetalRenderer: NSObject, @preconcurrency MTKViewDelegate {
    private let commandQueue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private var cache: CVMetalTextureCache?
    private let inFlight = DispatchSemaphore(value: 1)
    private let frames: FrameStore
    private let diagnostics: Diagnostics
    private let dummyY: MTLTexture, dummyUV: MTLTexture
    var settings = AppSettings()
    var testMode = 0
    private var lastSequence: UInt32?

    init(view: MTKView, frames: FrameStore, diagnostics: Diagnostics) throws {
        guard let device = view.device, let queue = device.makeCommandQueue(),
              let library = device.makeDefaultLibrary(), let vertex = library.makeFunction(name: "nd_vertex"),
              let fragment = library.makeFunction(name: "nd_fragment") else { throw NDError.message("Metal is unavailable or shader library is missing") }
        commandQueue = queue; self.frames = frames; self.diagnostics = diagnostics
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex; descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        let y = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: 1, height: 1, mipmapped: false)
        let uv = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rg8Unorm, width: 1, height: 1, mipmapped: false)
        y.storageMode = .shared; uv.storageMode = .shared
        guard let yt = device.makeTexture(descriptor: y), let uvt = device.makeTexture(descriptor: uv) else { throw NDError.message("Cannot allocate Metal textures") }
        dummyY = yt; dummyUV = uvt
        super.init()
        var black: UInt8 = 16; var neutral: [UInt8] = [128,128]
        dummyY.replace(region: MTLRegionMake2D(0,0,1,1), mipmapLevel: 0, withBytes: &black, bytesPerRow: 1)
        neutral.withUnsafeBytes { dummyUV.replace(region: MTLRegionMake2D(0,0,1,1), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 2) }
        guard CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache) == kCVReturnSuccess else { throw NDError.message("Cannot create video texture cache") }
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.preferredFramesPerSecond = 60
        view.clearColor = MTLClearColorMake(0,0,0,1)
        view.isPaused = false; view.enableSetNeedsDisplay = false
        if let layer = view.layer as? CAMetalLayer { layer.maximumDrawableCount = 2; layer.allowsNextDrawableTimeout = true }
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
    func draw(in view: MTKView) {
        guard inFlight.wait(timeout: .now()) == .success else { diagnostics.update { $0.gpuSkipped += 1 }; return }
        guard let drawable = view.currentDrawable, let pass = view.currentRenderPassDescriptor,
              let command = commandQueue.makeCommandBuffer(), let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { inFlight.signal(); return }
        let frame = frames.latest()
        var yRef: CVMetalTexture?, uvRef: CVMetalTexture?
        var yTex = dummyY, uvTex = dummyUV
        var params = RenderParameters()
        params.dimensions = SIMD4(Float(view.drawableSize.width), Float(view.drawableSize.height), 1920,1080)
        params.controls = SIMD4(settings.viewerMode.shaderValue, settings.swapEyes ? 1 : 0, settings.verticalShift, Float(testMode))
        params.lens = SIMD4(settings.lensCorrection ? settings.k1 : 0, settings.lensCorrection ? settings.k2 : 0,
                            settings.imageScale, settings.lensCenterShift)
        params.color.z = settings.demoFOV * .pi / 180
        let q = diagnostics.snapshot().head
        params.orientation = SIMD4(Float(q.x), Float(q.y), Float(q.z), Float(q.w))
        if testMode == 0, let frame = frame, let cache = cache {
            let pixel = frame.pixels
            let format = CVPixelBufferGetPixelFormatType(pixel)
            if CVPixelBufferGetPlaneCount(pixel) == 2 &&
                (format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
                let w = CVPixelBufferGetWidthOfPlane(pixel, 0), h = CVPixelBufferGetHeightOfPlane(pixel, 0)
                let cw = CVPixelBufferGetWidthOfPlane(pixel, 1), ch = CVPixelBufferGetHeightOfPlane(pixel, 1)
                let a = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pixel, nil, .r8Unorm, w,h,0,&yRef)
                let b = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pixel, nil, .rg8Unorm, cw,ch,1,&uvRef)
                if a == kCVReturnSuccess, b == kCVReturnSuccess, let yr = yRef, let ur = uvRef,
                   let yt = CVMetalTextureGetTexture(yr), let ut = CVMetalTextureGetTexture(ur) {
                    yTex = yt; uvTex = ut; params.dimensions.z = Float(w); params.dimensions.w = Float(h)
                    params.color.x = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ? 1 : 0
                    if let matrix = CVBufferCopyAttachment(pixel, kCVImageBufferYCbCrMatrixKey, nil) as? String {
                        if matrix == kCVImageBufferYCbCrMatrix_ITU_R_601_4 as String { params.color.y = 1 }
                        if matrix == kCVImageBufferYCbCrMatrix_ITU_R_2020 as String { params.color.y = 2 }
                    }
                } else { diagnostics.update { $0.lastError = "Cannot map decoded video to Metal" } }
            } else { diagnostics.update { $0.lastError = "Decoder did not produce 8-bit NV12" } }
        }
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentTexture(yTex, index: 0); encoder.setFragmentTexture(uvTex, index: 1)
        encoder.setFragmentBytes(&params, length: MemoryLayout<RenderParameters>.stride, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        if testMode == 0, let frame = frame, frame.sequence != lastSequence {
            let now = nd_ios_now_ns()
            diagnostics.update { $0.drawn += 1; $0.rxToSubmitMS = Double(now - frame.firstPacketNS) / 1e6 }
            lastSequence = frame.sequence
        }
        let hold = (frame, yRef, uvRef)
        command.addCompletedHandler { [inFlight] _ in withExtendedLifetime(hold) {}; inFlight.signal() }
        command.present(drawable)
        command.commit()
    }
}
@MainActor
struct MetalSurface: UIViewRepresentable {
    let frames: FrameStore
    let diagnostics: Diagnostics
    var settings: AppSettings
    var testMode: Int
    var onError: @MainActor (String) -> Void
    @MainActor final class Coordinator { var renderer: MetalRenderer? }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        do {
            let r = try MetalRenderer(view: v, frames: frames, diagnostics: diagnostics)
            r.settings = settings; r.testMode = testMode
            context.coordinator.renderer = r; v.delegate = r
        } catch { DispatchQueue.main.async { onError(error.localizedDescription) } }
        return v
    }
    func updateUIView(_ view: MTKView, context: Context) {
        context.coordinator.renderer?.settings = settings
        context.coordinator.renderer?.testMode = testMode
    }
    static func dismantleUIView(_ view: MTKView, coordinator: Coordinator) {
        view.isPaused = true; view.delegate = nil; coordinator.renderer = nil
    }
}
