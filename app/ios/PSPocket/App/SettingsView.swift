import SwiftUI

/// Settings, opened from the library's gear (or the controller's Menu button): where the games go, and the test
/// host's Self-test and Metal pages, which were tabs before the library became the root screen (D-142).
struct SettingsView: View {
    enum Page: Hashable { case selfTest, metal }

    @Binding var path: [Page]
    @Environment(\.dismiss) private var dismiss
    @Environment(SelfTestModel.self) private var selfTest
    @State private var firmware = Firmware()
    @State private var pickingFirmware = false

    var body: some View {
        NavigationStack(path: $path) {
            List {
                Section {
                    LabeledContent("Games folder", value: "On My iPhone › PSPocket › Games")
                } header: {
                    Text("Library")
                } footer: {
                    Text("Disc images (.iso) copied into that folder with Files appear in the library. Add Game (+) "
                         + "keeps one from anywhere else where it is. Covers come from GameTDB and are cached on "
                         + "this iPhone.")
                }
                Section {
                    LabeledContent("Installed", value: firmware.installed ?? "None")
                    Button {
                        pickingFirmware = true
                    } label: {
                        if firmware.installing {
                            HStack(spacing: 8) { ProgressView(); Text("Installing…") }
                        } else {
                            Label("Install from PS3UPDAT.PUP…", systemImage: "square.and.arrow.down")
                        }
                    }
                    .disabled(firmware.installing)
                    if let message = firmware.message {
                        Text(message).font(.footnote).foregroundStyle(.secondary)
                    }
                } header: {
                    Text("System software")
                } footer: {
                    Text("Games read their system files from /dev_flash. Pick the PS3 update you downloaded "
                         + "(PS3UPDAT.PUP, from playstation.com) and pspocket installs it on this iPhone, once.")
                }
                Section("Diagnostics") {
                    NavigationLink(value: Page.selfTest) { Label("Self-test", systemImage: "checklist") }
                    NavigationLink(value: Page.metal) { Label("Metal", systemImage: "cube.transparent") }
                }
                Section("About") {
                    Text(selfTest.buildInfo)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                }
            }
            .navigationTitle("Settings")
            .fileImporter(isPresented: $pickingFirmware, allowedContentTypes: [.data]) { result in
                if case .success(let url) = result { firmware.install(from: url) }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
            .navigationDestination(for: Page.self) { page in
                switch page {
                case .selfTest: SelfTestView()
                case .metal: MetalTabView().navigationTitle("Metal").toolbarBackground(.visible, for: .navigationBar)
                }
            }
        }
    }
}

/// The PS3 system software on this iPhone (D-156): /dev_flash installed from the user's PS3UPDAT.PUP, in
/// Application Support/pspocket-firmware, with its version beside it.
@Observable
final class Firmware {
    private(set) var installed: String?
    private(set) var installing = false
    private(set) var message: String?

    static var folder: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("pspocket-firmware")
    }
    static var devFlash: URL { folder.appendingPathComponent("dev_flash") }
    private static var versionFile: URL { folder.appendingPathComponent("version.txt") }

    init() {
        installed = try? String(contentsOf: Self.versionFile, encoding: .utf8)
    }

    func install(from url: URL) {
        installing = true
        message = nil
        DispatchQueue.global(qos: .userInitiated).async {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
            var version = [CChar](repeating: 0, count: 64)
            var error = [CChar](repeating: 0, count: 512)
            let status = ps_firmware_install(url.path, Self.devFlash.path, &version, version.count, &error, error.count)
            let v = String(cString: version), e = String(cString: error)
            if status == 0 { try? v.write(to: Self.versionFile, atomically: true, encoding: .utf8) }
            DispatchQueue.main.async {
                self.installing = false
                if status == 0 {
                    self.installed = v
                    self.message = "System software \(v) installed."
                } else {
                    self.message = "Not installed: \(e)"
                }
            }
        }
    }
}
