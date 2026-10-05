import SwiftUI

/// Settings, opened from the library's gear (or the controller's Menu button): where the games go, and the test
/// host's Self-test and Metal pages, which were tabs before the library became the root screen (D-142).
struct SettingsView: View {
    enum Page: Hashable { case selfTest, metal }

    @Binding var path: [Page]
    @Environment(\.dismiss) private var dismiss
    @Environment(SelfTestModel.self) private var selfTest

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
