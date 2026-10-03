import Observation
import SwiftUI
import UIKit

/// Owns the renderer and runs it only while the Metal tab is on screen and the app is active, so the GPU load does
/// not run behind the Self-test tab.
@Observable
final class MetalLoadModel {
    var passes = Double(min(max(LaunchOptions.initialPasses ?? 8, 0), LoadRenderer.maxPasses)) {
        didSet { renderer?.passes = Int(passes) }
    }
    private(set) var snapshot = FrameStats.Snapshot()
    private(set) var pathDescription = ""
    private(set) var unavailable: String?

    @ObservationIgnored private var renderer: LoadRenderer?
    @ObservationIgnored private var layer: CAMetalLayer?
    @ObservationIgnored private var visible = false
    @ObservationIgnored private var active = true
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private var lastLogLine = 0.0

    /// Seconds between stats lines on stdout, so a thermal run can be followed with `scripts/ios.sh run`.
    static let logInterval = 5.0

    init() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil,
                                             queue: .main) { [weak self] _ in
            self?.active = true
            self?.update()
        })
        observers.append(center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil,
                                             queue: .main) { [weak self] _ in
            self?.active = false
            self?.update()
        })
    }

    func attach(_ layer: CAMetalLayer?) {
        self.layer = layer
        update()
    }

    func setVisible(_ visible: Bool) {
        self.visible = visible
        update()
    }

    func resetStats() {
        renderer?.stats.reset()
    }

    private func update() {
        let run = visible && active && layer != nil
        if run, renderer == nil, unavailable == nil {
            guard let renderer = LoadRenderer() else {
                unavailable = "Metal is not available"
                return
            }
            renderer.passes = Int(passes)
            renderer.onPublish = { [weak self, weak renderer] snapshot in
                self?.publish(snapshot, path: renderer?.pathDescription ?? "")
            }
            self.renderer = renderer
            print("metal: rendering, \(Int(passes)) load passes, a stats line every \(Int(Self.logInterval)) s")
        }
        renderer?.attach(layer)
        renderer?.setRunning(run)
    }

    private func publish(_ snapshot: FrameStats.Snapshot, path: String) {
        self.snapshot = snapshot
        pathDescription = path
        guard snapshot.elapsed - lastLogLine >= Self.logInterval || snapshot.elapsed < lastLogLine else { return }
        lastLogLine = snapshot.elapsed
        print("metal: t \(StatsOverlay.clock(snapshot.elapsed)) | \(path) | passes 1+\(Int(passes))"
              + " | cpu \(Format.ms(snapshot.cpuAvg)) max \(Format.ms(snapshot.cpuMax))"
              + " | gpu \(Format.ms(snapshot.gpuAvg)) max \(Format.ms(snapshot.gpuMax))"
              + " | frame \(Format.ms(snapshot.intervalAvg)) worst 5 s \(Format.ms(snapshot.intervalWorst))"
              + " (\(snapshot.intervalSource)) | dropped \(snapshot.dropped)/\(snapshot.frames)"
              + " | thermal \(ProcessInfo.processInfo.thermalState.label) | footprint \(Format.bytes(snapshot.footprint))")
    }
}

struct MetalTabView: View {
    @Environment(MetalLoadModel.self) private var model
    @Environment(DeviceStatus.self) private var device

    var body: some View {
        @Bindable var model = model
        ZStack {
            Color.black.ignoresSafeArea()
            MetalLayerView(model: model).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 12) {
                if let unavailable = model.unavailable {
                    Text(unavailable).foregroundStyle(.red)
                } else {
                    StatsOverlay(snapshot: model.snapshot, path: model.pathDescription, passes: Int(model.passes),
                                 thermal: device.thermalState)
                }
                Spacer()
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("Load passes").font(.caption)
                        Slider(value: $model.passes, in: 0...Double(LoadRenderer.maxPasses), step: 1)
                        Text("\(Int(model.passes))").font(.caption.monospacedDigit()).frame(width: 24, alignment: .trailing)
                    }
                    Button("Reset stats", systemImage: "arrow.counterclockwise") { model.resetStats() }
                        .font(.caption)
                }
                .padding(10)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
            }
            .padding()
        }
        .onAppear { model.setVisible(true) }
        .onDisappear { model.setVisible(false) }
    }
}

private struct StatsOverlay: View {
    let snapshot: FrameStats.Snapshot
    let path: String
    let passes: Int
    let thermal: ProcessInfo.ThermalState

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(path.isEmpty ? "starting…" : path)
            Text("target \(Int(LoadRenderer.targetHz)) Hz · 1 + \(passes) passes")
            Text("CPU    \(Format.ms(snapshot.cpuAvg))  max \(Format.ms(snapshot.cpuMax))")
            Text("GPU    \(Format.ms(snapshot.gpuAvg))  max \(Format.ms(snapshot.gpuMax))")
            Text("frame  \(Format.ms(snapshot.intervalAvg))  worst 5 s \(Format.ms(snapshot.intervalWorst))")
            Text("       (\(snapshot.intervalSource))").foregroundStyle(.secondary)
            Text("dropped \(snapshot.dropped) of \(snapshot.frames) frames"
                 + (snapshot.gpuBusySkips > 0 ? " · \(snapshot.gpuBusySkips) GPU-busy skips" : ""))
                .foregroundStyle(snapshot.dropped > 0 ? Color.orange : Color.primary)
            HStack(spacing: 0) {
                Text("thermal ")
                Text(thermal.label).foregroundStyle(thermal.color)
                Text(" · footprint \(Format.bytes(snapshot.footprint))")
            }
            Text("running \(Self.clock(snapshot.elapsed))")
        }
        .font(.caption.monospaced())
        .foregroundStyle(.white)
        .padding(10)
        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 10))
        .environment(\.colorScheme, .dark)
    }

    static func clock(_ seconds: Double) -> String {
        let total = Int(seconds)
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

/// A CAMetalLayer-backed view; tells the model when its layer is in a window and keeps the drawable at native scale.
private struct MetalLayerView: UIViewRepresentable {
    let model: MetalLoadModel

    func makeUIView(context: Context) -> MetalLayerUIView {
        let view = MetalLayerUIView()
        view.backgroundColor = .black
        view.isOpaque = true
        view.onLayerChange = { [weak model] layer in model?.attach(layer) }
        return view
    }

    func updateUIView(_ uiView: MetalLayerUIView, context: Context) {}

    static func dismantleUIView(_ uiView: MetalLayerUIView, coordinator: ()) {
        uiView.onLayerChange?(nil)
    }
}

final class MetalLayerUIView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var onLayerChange: ((CAMetalLayer?) -> Void)?
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        updateDrawableSize()
        onLayerChange?(window == nil ? nil : metalLayer)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateDrawableSize()
    }

    private func updateDrawableSize() {
        guard let screen = window?.screen else { return }
        contentScaleFactor = screen.nativeScale
        metalLayer.drawableSize = CGSize(width: bounds.width * screen.nativeScale,
                                         height: bounds.height * screen.nativeScale)
    }
}
