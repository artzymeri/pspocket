import SwiftUI
import UIKit

struct SelfTestView: View {
    @Environment(SelfTestModel.self) private var model
    @Environment(DeviceStatus.self) private var device
    @State private var copied = false

    var body: some View {
        List {
                Section("Build and device") {
                    Text(model.buildInfo)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                    LabeledContent("Device", value: DeviceInfo.model)
                    LabeledContent("System", value: DeviceInfo.system)
                    LabeledContent("Thermal state") {
                        Text(device.thermalState.label).foregroundStyle(device.thermalState.color)
                    }
                    LabeledContent("Low Power Mode") {
                        Text(device.lowPowerMode ? "on (slows the CPU)" : "off")
                            .foregroundStyle(device.lowPowerMode ? Color.orange : Color.secondary)
                    }
                }

                Section {
                    Button {
                        model.runAll()
                    } label: {
                        HStack {
                            Label(model.isRunning ? "Running…" : "Run all", systemImage: "play.fill")
                            Spacer()
                            if model.isRunning { ProgressView() }
                        }
                    }
                    .disabled(model.isRunning)

                    if let run = model.lastRun {
                        Text(run.text)
                            .font(.callout.monospacedDigit().weight(.semibold))
                            .foregroundStyle(run.failed > 0 ? Color.red : Color.green)
                    }
                    if !model.reportStatus.isEmpty {
                        Text(model.reportStatus).font(.caption).foregroundStyle(.secondary)
                    }
                    HStack {
                        Button {
                            UIPasteboard.general.string = model.report
                            copied = true
                            Task {
                                try? await Task.sleep(for: .seconds(1.5))
                                copied = false
                            }
                        } label: {
                            Label(copied ? "Copied" : "Copy report", systemImage: copied ? "checkmark" : "doc.on.doc")
                        }
                        Spacer()
                        ShareLink(item: model.report, subject: Text("PSPocket self-test"),
                                  preview: SharePreview("PSPocket self-test report")) {
                            Label("Share", systemImage: "square.and.arrow.up")
                        }
                    }
                    .buttonStyle(.borderless)
                    .disabled(model.report.isEmpty || model.isRunning)
                } footer: {
                    Text("Long-press or swipe a probe to run it alone.")
                }

                Section("Probes") {
                    ForEach(model.probes) { probe in
                        ProbeRow(probe: probe)
                            .contextMenu {
                                Button("Run \(probe.name)", systemImage: "play") { model.run(probe) }
                                    .disabled(model.isRunning)
                            }
                            .swipeActions {
                                Button("Run") { model.run(probe) }
                                    .tint(.blue)
                                    .disabled(model.isRunning)
                            }
                    }
                }
            }
        .navigationTitle("Self-test")
    }
}

struct ProbeRow: View {
    let probe: ProbeResult
    @State private var showDetails = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(probe.name).font(.body.monospaced().weight(.semibold))
                Spacer(minLength: 4)
                if let seconds = probe.seconds {
                    Text(Format.seconds(seconds)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
                StatusBadge(status: probe.status)
            }
            Text(probe.description).font(.caption).foregroundStyle(.secondary)
            if !probe.summary.isEmpty {
                Text(probe.summary).font(.callout)
            }
            if !probe.metrics.isEmpty {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(probe.metrics.enumerated()), id: \.offset) { _, metric in
                        Text(metric)
                    }
                }
                .font(.caption.monospaced())
                .textSelection(.enabled)
            }
            if !probe.details.isEmpty {
                DisclosureGroup(isExpanded: $showDetails) {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(probe.details.enumerated()), id: \.offset) { _, line in
                            Text(line).foregroundStyle(line.hasPrefix("FAIL") ? Color.red : Color.primary)
                        }
                    }
                    .font(.caption.monospaced())
                    .textSelection(.enabled)
                } label: {
                    Text(probe.details.count == 1 ? "1 info line" : "\(probe.details.count) info lines")
                        .font(.caption)
                }
            }
        }
        .padding(.vertical, 2)
        .onChange(of: probe.status) { _, status in
            if status == .failed { showDetails = true }
        }
    }
}

struct StatusBadge: View {
    let status: ProbeResult.Status

    var body: some View {
        if status == .running {
            ProgressView().controlSize(.small)
        } else {
            Text(label)
                .font(.caption2.monospaced().weight(.bold))
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .foregroundStyle(.white)
                .background(color, in: Capsule())
        }
    }

    private var label: String {
        switch status {
        case .idle: "—"
        case .queued: "WAIT"
        case .running: "RUN"
        case .passed: "PASS"
        case .failed: "FAIL"
        case .skipped: "SKIP"
        }
    }

    private var color: Color {
        switch status {
        case .idle, .queued: .gray
        case .running: .blue
        case .passed: .green
        case .failed: .red
        case .skipped: .orange
        }
    }
}

extension ProcessInfo.ThermalState {
    var color: Color {
        switch self {
        case .nominal: .green
        case .fair: .yellow
        case .serious: .orange
        case .critical: .red
        @unknown default: .secondary
        }
    }
}
