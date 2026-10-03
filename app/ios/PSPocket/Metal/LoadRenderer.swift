import Metal
import QuartzCore
#if canImport(MetalFX)
import MetalFX // not in the simulator SDK
#endif

/// Previews the plan's output path (section 7, "Output and pacing"): render at the native 1152×640, upscale to the
/// drawable with MetalFX spatial, present at 30 Hz. The scene is a synthetic load: one base pass plus `passes`
/// fullscreen fragment passes, each its own render pass (load + store), like a game's render-target traffic.
final class LoadRenderer {
    static let internalWidth = 1152
    static let internalHeight = 640
    static let targetHz = 30.0
    static let maxPasses = 64

    let device: MTLDevice
    let stats = FrameStats(targetHz: LoadRenderer.targetHz)
    /// Extra load passes per frame (0...maxPasses).
    var passes = 8
    /// Called on the main thread about four times a second.
    var onPublish: ((FrameStats.Snapshot) -> Void)?
    /// e.g. "1152×640 → 2376×1320 MetalFX spatial".
    private(set) var pathDescription = "1152×640"

    private let queue: MTLCommandQueue
    private let basePipeline: MTLRenderPipelineState
    private let loadPipeline: MTLRenderPipelineState
    private let compositePipeline: MTLRenderPipelineState
    private let nearestSampler: MTLSamplerState
    private let linearSampler: MTLSamplerState
    private var sceneTexture: MTLTexture
    private var upscaledTexture: MTLTexture?
    #if canImport(MetalFX)
    private var scaler: MTLFXSpatialScaler?
    #endif
    private var configuredSize = CGSize.zero
    private var outputViewport = MTLViewport()
    private let inFlight = DispatchSemaphore(value: 2)
    private let startTime = CACurrentMediaTime()
    private var lastPublish = 0.0
    private var link: CADisplayLink?
    private weak var layer: CAMetalLayer?

    private static let pixelFormat = MTLPixelFormat.bgra8Unorm

    init?() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
              let library = device.makeDefaultLibrary(),
              let vertex = library.makeFunction(name: "fullscreen_vs"),
              let pattern = library.makeFunction(name: "pattern_fs"),
              let composite = library.makeFunction(name: "composite_fs")
        else { return nil }

        func pipeline(_ fragment: MTLFunction, blend: Bool) -> MTLRenderPipelineState? {
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = vertex
            desc.fragmentFunction = fragment
            let color = desc.colorAttachments[0]!
            color.pixelFormat = Self.pixelFormat
            if blend {
                color.isBlendingEnabled = true
                color.sourceRGBBlendFactor = .sourceAlpha
                color.destinationRGBBlendFactor = .oneMinusSourceAlpha
                color.sourceAlphaBlendFactor = .one
                color.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }
            return try? device.makeRenderPipelineState(descriptor: desc)
        }

        func sampler(_ filter: MTLSamplerMinMagFilter) -> MTLSamplerState? {
            let desc = MTLSamplerDescriptor()
            desc.minFilter = filter
            desc.magFilter = filter
            desc.sAddressMode = .clampToEdge
            desc.tAddressMode = .clampToEdge
            return device.makeSamplerState(descriptor: desc)
        }

        guard let base = pipeline(pattern, blend: false), let load = pipeline(pattern, blend: true),
              let comp = pipeline(composite, blend: false), let nearest = sampler(.nearest),
              let linear = sampler(.linear),
              let scene = Self.makeSceneTexture(device: device, extraUsage: [])
        else { return nil }

        self.device = device
        self.queue = queue
        basePipeline = base
        loadPipeline = load
        compositePipeline = comp
        nearestSampler = nearest
        linearSampler = linear
        sceneTexture = scene
    }

    /// The layer to draw into, or nil to detach.
    func attach(_ layer: CAMetalLayer?) {
        self.layer = layer
        guard let layer else { return }
        layer.device = device
        layer.pixelFormat = Self.pixelFormat
        layer.framebufferOnly = true
        layer.maximumDrawableCount = 3
    }

    /// Starts or stops the 30 Hz display link.
    func setRunning(_ running: Bool) {
        if running, link == nil {
            let link = CADisplayLink(target: DisplayLinkTarget(self), selector: #selector(DisplayLinkTarget.tick(_:)))
            link.preferredFrameRateRange = CAFrameRateRange(minimum: Float(Self.targetHz), maximum: Float(Self.targetHz),
                                                            preferred: Float(Self.targetHz))
            link.add(to: .main, forMode: .common)
            self.link = link
            stats.resumeTiming()
        } else if !running, let link {
            link.invalidate()
            self.link = nil
        }
    }

    fileprivate func render(_ link: CADisplayLink) {
        let cpuStart = CACurrentMediaTime()
        stats.linkTick(at: link.timestamp)
        guard let layer, layer.drawableSize.width >= 1, layer.drawableSize.height >= 1 else { return }
        // Never block the main thread on the GPU: with two frames queued, skip this tick (it shows up as a drop).
        guard inFlight.wait(timeout: .now()) == .success else {
            stats.gpuBusySkip()
            return
        }
        configure(for: layer.drawableSize)
        guard let drawable = layer.nextDrawable(), let commandBuffer = queue.makeCommandBuffer() else {
            inFlight.signal()
            return
        }
        commandBuffer.label = "pspocket.load"

        encodeScene(commandBuffer, time: Float((cpuStart - startTime).truncatingRemainder(dividingBy: 3600)))
        let (source, sampler) = encodeUpscale(commandBuffer)
        encodeComposite(commandBuffer, source: source, sampler: sampler, into: drawable.texture)

        let stats = stats
        let inFlight = inFlight
        commandBuffer.addCompletedHandler { buffer in
            stats.gpuFrame(start: buffer.gpuStartTime, end: buffer.gpuEndTime)
            inFlight.signal()
        }
        #if !targetEnvironment(simulator) // not in the simulator SDK; FrameStats then uses display link timestamps
        drawable.addPresentedHandler { drawable in
            stats.presented(at: drawable.presentedTime)
        }
        #endif
        // On the 30 Hz grid the display link targets, so a frame that finishes early is not shown early.
        commandBuffer.present(drawable, atTime: link.targetTimestamp)
        commandBuffer.commit()
        stats.cpuFrame(CACurrentMediaTime() - cpuStart)

        if cpuStart - lastPublish >= 0.25 {
            lastPublish = cpuStart
            onPublish?(stats.snapshot())
        }
    }

    // MARK: Encoding

    private struct PatternParams {
        var time: Float
        var pass: UInt32
        var passes: UInt32
        var aspect: Float
    }

    private func encodeScene(_ commandBuffer: MTLCommandBuffer, time: Float) {
        let total = 1 + min(max(passes, 0), Self.maxPasses)
        for pass in 0..<total {
            let desc = MTLRenderPassDescriptor()
            desc.colorAttachments[0].texture = sceneTexture
            desc.colorAttachments[0].loadAction = pass == 0 ? .dontCare : .load
            desc.colorAttachments[0].storeAction = .store
            guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: desc) else { return }
            encoder.label = pass == 0 ? "scene base" : "scene load \(pass)"
            encoder.setRenderPipelineState(pass == 0 ? basePipeline : loadPipeline)
            var params = PatternParams(time: time, pass: UInt32(pass), passes: UInt32(total),
                                       aspect: Float(Self.internalWidth) / Float(Self.internalHeight))
            encoder.setFragmentBytes(&params, length: MemoryLayout<PatternParams>.stride, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
    }

    /// Returns the texture the composite pass samples, and how.
    private func encodeUpscale(_ commandBuffer: MTLCommandBuffer) -> (MTLTexture, MTLSamplerState) {
        #if canImport(MetalFX)
        if let scaler, let upscaledTexture {
            scaler.colorTexture = sceneTexture
            scaler.outputTexture = upscaledTexture
            scaler.inputContentWidth = Self.internalWidth
            scaler.inputContentHeight = Self.internalHeight
            scaler.encode(commandBuffer: commandBuffer)
            return (upscaledTexture, nearestSampler) // already at output size: copy 1:1
        }
        #endif
        return (sceneTexture, linearSampler) // fallback: plain bilinear scale in the composite pass
    }

    /// Letterboxes the source into the drawable at the 1152×640 aspect ratio.
    private func encodeComposite(_ commandBuffer: MTLCommandBuffer, source: MTLTexture, sampler: MTLSamplerState,
                                 into target: MTLTexture) {
        let desc = MTLRenderPassDescriptor()
        desc.colorAttachments[0].texture = target
        desc.colorAttachments[0].loadAction = .clear
        desc.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        desc.colorAttachments[0].storeAction = .store
        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: desc) else { return }
        encoder.label = "composite"
        encoder.setRenderPipelineState(compositePipeline)
        encoder.setViewport(outputViewport)
        encoder.setFragmentTexture(source, index: 0)
        encoder.setFragmentSamplerState(sampler, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
    }

    // MARK: Output configuration

    private func configure(for size: CGSize) {
        guard size != configuredSize else { return }
        configuredSize = size
        let drawableWidth = Int(size.width), drawableHeight = Int(size.height)
        let aspect = Double(Self.internalWidth) / Double(Self.internalHeight)
        var width = drawableWidth
        var height = Int((Double(drawableWidth) / aspect).rounded())
        if height > drawableHeight {
            height = drawableHeight
            width = Int((Double(drawableHeight) * aspect).rounded())
        }
        outputViewport = MTLViewport(originX: Double((drawableWidth - width) / 2),
                                     originY: Double((drawableHeight - height) / 2),
                                     width: Double(width), height: Double(height), znear: 0, zfar: 1)
        let scaling = makeScaler(width: width, height: height)
        pathDescription = "\(Self.internalWidth)×\(Self.internalHeight) → \(width)×\(height) \(scaling)"
    }

    /// Sets up MetalFX for the output size when it can. Returns a description of the scaling path.
    private func makeScaler(width: Int, height: Int) -> String {
        upscaledTexture = nil
        #if canImport(MetalFX)
        scaler = nil
        guard MTLFXSpatialScalerDescriptor.supportsDevice(device) else { return "bilinear (MetalFX unsupported)" }
        guard width >= Self.internalWidth, height >= Self.internalHeight else {
            return "bilinear (output smaller than the scene)"
        }
        let desc = MTLFXSpatialScalerDescriptor()
        desc.inputWidth = Self.internalWidth
        desc.inputHeight = Self.internalHeight
        desc.outputWidth = width
        desc.outputHeight = height
        desc.colorTextureFormat = Self.pixelFormat
        desc.outputTextureFormat = Self.pixelFormat
        desc.colorProcessingMode = .perceptual
        guard let scaler = desc.makeSpatialScaler(device: device) else {
            return "bilinear (MetalFX scaler creation failed)"
        }
        if !sceneTexture.usage.contains(scaler.colorTextureUsage),
           let scene = Self.makeSceneTexture(device: device, extraUsage: scaler.colorTextureUsage) {
            sceneTexture = scene
        }
        let outDesc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: Self.pixelFormat, width: width,
                                                               height: height, mipmapped: false)
        outDesc.usage = scaler.outputTextureUsage.union(.shaderRead)
        outDesc.storageMode = .private
        guard let output = device.makeTexture(descriptor: outDesc) else { return "bilinear (no output texture)" }
        output.label = "upscaled"
        upscaledTexture = output
        self.scaler = scaler
        return "MetalFX spatial"
        #else
        return "bilinear (no MetalFX in this SDK)"
        #endif
    }

    private static func makeSceneTexture(device: MTLDevice, extraUsage: MTLTextureUsage) -> MTLTexture? {
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: pixelFormat, width: internalWidth,
                                                            height: internalHeight, mipmapped: false)
        desc.usage = MTLTextureUsage([.renderTarget, .shaderRead]).union(extraUsage)
        desc.storageMode = .private
        let texture = device.makeTexture(descriptor: desc)
        texture?.label = "scene 1152x640"
        return texture
    }
}

/// CADisplayLink retains its target; this keeps the renderer out of that cycle.
private final class DisplayLinkTarget: NSObject {
    weak var renderer: LoadRenderer?

    init(_ renderer: LoadRenderer) {
        self.renderer = renderer
    }

    @objc func tick(_ link: CADisplayLink) {
        renderer?.render(link)
    }
}
