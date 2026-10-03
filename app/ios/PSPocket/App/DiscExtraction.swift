import Foundation

/// `-extract-iso [-iso NAME]`: copies the executables (PARAM.SFO, EBOOT.BIN, modules; never assets) out of a PS3 disc
/// image in Documents (Files: On My iPhone → PSPocket) into Documents/extracted/<image name>/, prints each report line,
/// writes Documents/extract-report.txt and exits with the extractor's status. Headless, for `scripts/ios.sh run`.
enum DiscExtraction {
    static func runIfRequested() {
        guard LaunchOptions.extractISO else { return }
        let thread = Thread { run() }
        thread.name = "pspocket.extract"
        thread.qualityOfService = .userInitiated
        thread.stackSize = 8 << 20
        thread.start()
    }

    private static func run() {
        let documents = URL.documentsDirectory
        let images = ((try? FileManager.default.contentsOfDirectory(atPath: documents.path)) ?? [])
            .filter { $0.lowercased().hasSuffix(".iso") }
            .sorted()
        guard let name = LaunchOptions.isoName ?? images.first else {
            print("extract: no .iso in Documents (Files → On My iPhone → PSPocket)")
            exit(1)
        }
        let image = documents.appending(path: name)
        let output = documents.appending(path: "extracted/\((name as NSString).deletingPathExtension)")
        print("extract: \(name) → extracted/\(output.lastPathComponent)/")

        let lines = Lines()
        let status = ps_disc_extract_executables(image.path, output.path, { user, line in
            guard let user, let line else { return }
            let text = String(cString: line)
            print("extract: " + text)
            Unmanaged<Lines>.fromOpaque(user).takeUnretainedValue().append(text)
        }, Unmanaged.passUnretained(lines).toOpaque())

        let report = lines.joined() + "status: \(status)\n"
        try? report.write(to: documents.appending(path: "extract-report.txt"), atomically: true, encoding: .utf8)
        print("extract: exit \(status)")
        fflush(stdout)
        exit(status)
    }
}

/// Report lines collected from the C callback (called on the extraction thread only).
private final class Lines {
    private var lines: [String] = []
    func append(_ line: String) { lines.append(line) }
    func joined() -> String { lines.map { $0 + "\n" }.joined() }
}
