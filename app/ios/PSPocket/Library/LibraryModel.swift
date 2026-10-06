import CoreImage
import CoreImage.CIFilterBuiltins
import Foundation
import Observation
import os
import UIKit

/// One disc image in the library: an .iso in Documents/Games (Files > On My iPhone > PSPocket > Games), or one the
/// user added from anywhere else with Add Game (kept as a security-scoped bookmark).
struct LibraryEntry: Identifiable, Equatable {
    let id: String // "games:<path inside Games>" or "added:<uuid>"
    let url: URL
    var titleID: String?
    var title: String?
    /// Why the image can't be played (not a PS3 disc, still being copied, its executable failed to decrypt).
    var problem: String?

    var displayTitle: String { title ?? url.deletingPathExtension().lastPathComponent }
}

/// The library screen's state (D-142): the disc images, their covers, play time per title ID, the focused game, which
/// input was used last, and a game being booted from it.
///
/// Files it keeps:
///   Documents/Games/                                 the user's disc images (made at launch so Files shows it)
///   Library/Application Support/pspocket-library/library.json   added images' bookmarks, play time per title ID
///   Library/Caches/pspocket-covers/<TITLE_ID>.jpg     covers from GameTDB; <TITLE_ID>-icon0.png the disc's ICON0.PNG
///   Library/Application Support/pspocket-games/<TITLE_ID>/EBOOT.elf   the disc's executable decrypted here (D-121)
@Observable
final class LibraryModel {
    enum InputMode { case pad, touch }

    /// A game being booted from the library: armed (Play pressed; cancellable until the boot starts) or started
    /// (the game's threads run; the overlay stays until its first frame).
    struct Boot: Equatable {
        /// armed: a moment to call it off; preparing: the game's code is being translated and compiled on this phone,
        /// the first time it is played (D-154); started: the game's threads run.
        enum Phase { case armed, preparing, started }
        let entryID: String
        var phase: Phase
    }

    private(set) var entries: [LibraryEntry] = []
    var focusID: String?
    var inputMode: InputMode = .touch
    private(set) var covers: [String: UIImage] = [:] // by title ID
    private(set) var backdrops: [String: Backdrop] = [:] // the covers prepared for the animated background
    private(set) var decrypting: Set<String> = [] // title IDs
    private(set) var boot: Boot?
    /// Preparing a game on this iPhone the first time it is played (D-154): the overlay shows its progress.
    let preparer = Preparer()
    /// The games for which the system software's translation was checked (and made if need be) this session (D-178,
    /// D-191: each game may link system modules whose SPU programs it does not hold yet), by title ID.
    @ObservationIgnored private var systemChecked = Set<String>()
    /// Why a game runs without its library this session (it could not be prepared), by title ID.
    private(set) var preparationNotes: [String: String] = [:]
    @ObservationIgnored static let log = Logger(subsystem: "com.artzymeri.pspocket", category: "library")
    /// A boot that failed, for the library to show.
    private(set) var bootError: String?

    @ObservationIgnored private var store = Store()
    @ObservationIgnored private var added: [String: URL] = [:] // "added:<uuid>" -> URL (access started)
    @ObservationIgnored private var probed: [String: (size: Int64, id: String?, title: String?, problem: String?)] = [:]
    @ObservationIgnored private var watcher: DispatchSourceFileSystemObject?
    @ObservationIgnored private var rescanPending = false
    @ObservationIgnored private let probeQueue = DispatchQueue(label: "pspocket.library.probe", qos: .userInitiated)
    @ObservationIgnored private let decryptQueue = DispatchQueue(label: "pspocket.library.decrypt", qos: .utility)
    @ObservationIgnored private var coverRequested: Set<String> = []
    @ObservationIgnored private var pendingSave: DispatchWorkItem?
    @ObservationIgnored private var armTimer: Timer?
    @ObservationIgnored private weak var game: GameModel?

    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    static var gamesFolder: URL { documents.appendingPathComponent("Games", isDirectory: true) }
    private static var support: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    }
    private static var storeURL: URL { support.appendingPathComponent("pspocket-library/library.json") }
    private static var coversFolder: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("pspocket-covers")
    }
    /// Where executables decrypted on the phone go: a folder the app makes itself (Documents/game/ is the Mac's,
    /// `scripts/ios.sh push-game`, and the app may not write in it).
    static var decryptedGames: URL { support.appendingPathComponent("pspocket-games") }

    init() {
        try? FileManager.default.createDirectory(at: Self.gamesFolder, withIntermediateDirectories: true)
        try? FileManager.default.createDirectory(at: Self.coversFolder, withIntermediateDirectories: true)
        loadStore()
        migrateOldBookmark()
        restoreAdded()
        rescan()
        watchGamesFolder()
    }

    func attach(_ game: GameModel) {
        self.game = game
        game.onPlayed = { [weak self] seconds in self?.played(seconds) }
    }

    // MARK: the list

    var focusIndex: Int {
        guard let focusID, let i = entries.firstIndex(where: { $0.id == focusID }) else { return 0 }
        return i
    }

    var focused: LibraryEntry? { entries.isEmpty ? nil : entries[focusIndex] }

    func focus(at index: Int) {
        guard !entries.isEmpty else { return }
        let i = max(0, min(entries.count - 1, index))
        if entries[i].id != focusID { focusID = entries[i].id }
    }

    func move(_ delta: Int) { focus(at: focusIndex + delta) }

    /// "Last played today · 14 h 32 min", or "Not played yet"; a problem replaces it.
    func meta(for entry: LibraryEntry) -> String {
        if let problem = entry.problem { return problem }
        if let id = entry.titleID, decrypting.contains(id) { return "Preparing the game's executable…" }
        if let id = entry.titleID, let note = preparationNotes[id] { return note }
        guard let id = entry.titleID, let stats = store.stats[id], let last = stats.lastPlayed else {
            return "Not played yet"
        }
        let calendar = Calendar.current
        let day: String
        if calendar.isDateInToday(last) {
            day = "today"
        } else if calendar.isDateInYesterday(last) {
            day = "yesterday"
        } else {
            let f = DateFormatter()
            f.locale = Locale(identifier: "en_GB")
            f.setLocalizedDateFormatFromTemplate("d MMM")
            day = f.string(from: last)
        }
        let minutes = Int(stats.seconds / 60)
        return "Last played \(day) · \(minutes / 60) h \(String(format: "%02d", minutes % 60)) min"
    }

    /// Images in Documents/Games and the ones added elsewhere, probed, newest played first.
    func rescan() {
        var found: [(id: String, url: URL)] = []
        let games = Self.gamesFolder.standardizedFileURL
        if let walker = FileManager.default.enumerator(at: games, includingPropertiesForKeys: [.isRegularFileKey],
                                                       options: [.skipsHiddenFiles, .skipsPackageDescendants]) {
            for case let url as URL in walker where url.pathExtension.lowercased() == "iso" {
                let path = url.standardizedFileURL.path
                let relative = path.hasPrefix(games.path) ? String(path.dropFirst(games.path.count + 1)) : path
                found.append(("games:" + relative, url))
            }
        }
        let inGames = Set(found.map { $0.url.standardizedFileURL.path })
        for (id, url) in added where !inGames.contains(url.standardizedFileURL.path) { found.append((id, url)) }

        var next: [LibraryEntry] = []
        for (id, url) in found {
            let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize).map { Int64($0) } ?? -1
            var entry = LibraryEntry(id: id, url: url)
            if let known = probed[id], known.size == size {
                entry.titleID = known.id
                entry.title = known.title
                entry.problem = known.problem
            } else {
                entry.problem = "Reading the disc…"
                probe(id: id, url: url, size: size)
            }
            next.append(entry)
        }
        apply(next)
    }

    private func apply(_ next: [LibraryEntry]) {
        let stats = store.stats
        let sorted = next.sorted { a, b in
            let la = a.titleID.flatMap { stats[$0]?.lastPlayed }, lb = b.titleID.flatMap { stats[$0]?.lastPlayed }
            switch (la, lb) {
            case let (x?, y?): return x > y
            case (_?, nil): return true
            case (nil, _?): return false
            default: return a.displayTitle.localizedCaseInsensitiveCompare(b.displayTitle) == .orderedAscending
            }
        }
        if sorted != entries { entries = sorted }
        if focusID == nil || !entries.contains(where: { $0.id == focusID }) { focusID = entries.first?.id }
        for entry in entries { if let id = entry.titleID { requestCover(titleID: id, iso: entry.url) } }
    }

    private func probe(id: String, url: URL, size: Int64) {
        probeQueue.async {
            var tid = [CChar](repeating: 0, count: 32)
            var name = [CChar](repeating: 0, count: 256)
            var error = [CChar](repeating: 0, count: 512)
            let status = ps_game_probe_disc(url.path, &tid, tid.count, &name, name.count, &error, error.count)
            let titleID = status == 0 ? String(cString: tid) : nil
            let title = status == 0 ? String(cString: name) : nil
            // A file still being copied in reads as no disc; it is probed again when its size changes.
            let problem = status == 0 ? nil : "Not a PS3 game disc (\(String(cString: error)))"
            DispatchQueue.main.async {
                self.probed[id] = (size, titleID, title, problem)
                self.update(id) { e in
                    e.titleID = titleID
                    e.title = title
                    e.problem = problem
                }
                if let titleID { self.ensureExecutable(titleID: titleID, iso: url) }
            }
        }
    }

    private func update(_ id: String, _ change: (inout LibraryEntry) -> Void) {
        var next = entries
        guard let i = next.firstIndex(where: { $0.id == id }) else { return }
        change(&next[i])
        apply(next)
    }

    // MARK: Documents/Games

    private func watchGamesFolder() {
        let fd = open(Self.gamesFolder.path, O_EVTONLY)
        guard fd >= 0 else { return }
        let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete],
                                                               queue: .main)
        source.setEventHandler { [weak self] in self?.scheduleRescan() }
        source.setCancelHandler { close(fd) }
        source.resume()
        watcher = source
    }

    /// Files writes come in bursts (a copy): one rescan half a second after the last.
    func scheduleRescan() {
        guard !rescanPending else { return }
        rescanPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            self.rescanPending = false
            self.rescan()
        }
    }

    // MARK: Add Game

    /// An image the user picked in Files: kept where it is (a disc image is 20 GB) through a security-scoped bookmark.
    func add(_ url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        guard accessing || FileManager.default.isReadableFile(atPath: url.path) else {
            bootError = "Can't open \(url.lastPathComponent)"
            return
        }
        if url.standardizedFileURL.path.hasPrefix(Self.gamesFolder.standardizedFileURL.path) {
            rescan() // already in Games
            return
        }
        if let known = added.first(where: { $0.value.standardizedFileURL.path == url.standardizedFileURL.path }) {
            focusID = known.key
            return
        }
        guard let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            bootError = "Can't keep access to \(url.lastPathComponent)"
            return
        }
        let uuid = UUID()
        store.added.append(.init(id: uuid, bookmark: data))
        saveSoon()
        added["added:\(uuid.uuidString)"] = url
        rescan()
        focusID = "added:\(uuid.uuidString)"
    }

    private func restoreAdded() {
        var changed = false
        for (i, item) in store.added.enumerated() {
            var stale = false
            guard let url = try? URL(resolvingBookmarkData: item.bookmark, options: [], relativeTo: nil,
                                     bookmarkDataIsStale: &stale) else { continue }
            _ = url.startAccessingSecurityScopedResource()
            if stale, let fresh = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                store.added[i].bookmark = fresh
                changed = true
            }
            added["added:\(item.id.uuidString)"] = url
        }
        if changed { saveSoon() }
    }

    /// The Game tab of before kept one picked image in UserDefaults: it joins the library once.
    private func migrateOldBookmark() {
        let key = "gameISOBookmark"
        guard !store.migratedOldBookmark else { return }
        store.migratedOldBookmark = true
        if let data = UserDefaults.standard.data(forKey: key) { store.added.append(.init(id: UUID(), bookmark: data)) }
        saveSoon()
    }

    // MARK: the executable

    /// The decrypted executable for a title: Documents/game/<TITLE_ID>/EBOOT.elf (pushed from the Mac), else the one
    /// decrypted here, else Documents/game/EBOOT.elf.
    static func executable(titleID: String) -> URL? {
        let game = documents.appendingPathComponent("game")
        let candidates = [game.appendingPathComponent(titleID).appendingPathComponent("EBOOT.elf"),
                          decryptedGames.appendingPathComponent(titleID).appendingPathComponent("EBOOT.elf"),
                          game.appendingPathComponent("EBOOT.elf")]
        return candidates.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    /// The disc's executable decrypted on the phone with the keys the app carries (D-121), one disc at a time.
    private func ensureExecutable(titleID: String, iso: URL) {
        guard Self.executable(titleID: titleID) == nil, !decrypting.contains(titleID) else { return }
        decrypting.insert(titleID)
        let folder = Self.decryptedGames.appendingPathComponent(titleID)
        let elf = folder.appendingPathComponent("EBOOT.elf")
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        decryptQueue.async {
            var error = [CChar](repeating: 0, count: 512)
            let status = ps_disc_decrypt_executable(iso.path, elf.path, &error, error.count)
            let message = String(cString: error)
            DispatchQueue.main.async {
                self.decrypting.remove(titleID)
                if status != 0 {
                    for e in self.entries where e.titleID == titleID {
                        self.update(e.id) { $0.problem = "Could not decrypt the game's EBOOT.BIN: \(message)" }
                    }
                }
                if let boot = self.boot, boot.phase == .armed { self.armBoot() }
            }
        }
    }

    // MARK: covers

    /// GameTDB's cover (350x408), cached; else the disc's ICON0.PNG; else none (the title shows as text).
    private func requestCover(titleID: String, iso: URL) {
        guard !coverRequested.contains(titleID) else { return }
        coverRequested.insert(titleID)
        let cover = Self.coversFolder.appendingPathComponent("\(titleID).jpg")
        let icon0 = Self.coversFolder.appendingPathComponent("\(titleID)-icon0.png")
        DispatchQueue.global(qos: .utility).async {
            if let image = UIImage(contentsOfFile: cover.path) {
                self.setCover(image, for: titleID)
                return
            }
            // ICON0.PNG first, while the download runs (and for good when there is no network).
            if let image = UIImage(contentsOfFile: icon0.path) ?? Self.readIcon0(iso: iso, to: icon0) {
                self.setCover(image, for: titleID)
            }
            guard let url = URL(string: "https://art.gametdb.com/ps3/coverM/EN/\(titleID).jpg") else { return }
            var request = URLRequest(url: url)
            request.timeoutInterval = 20
            URLSession.shared.dataTask(with: request) { data, response, _ in
                guard let data, (response as? HTTPURLResponse)?.statusCode == 200, let image = UIImage(data: data)
                else { return }
                try? data.write(to: cover, options: .atomic)
                self.setCover(image, for: titleID)
            }.resume()
        }
    }

    private static func readIcon0(iso: URL, to file: URL) -> UIImage? {
        var size = 0
        var error = [CChar](repeating: 0, count: 256)
        guard ps_game_read_disc_file(iso.path, "PS3_GAME/ICON0.PNG", nil, 0, &size, &error, error.count) == 0,
              size > 0, size < 8 << 20 else { return nil }
        var bytes = [UInt8](repeating: 0, count: size)
        guard ps_game_read_disc_file(iso.path, "PS3_GAME/ICON0.PNG", &bytes, size, &size, &error, error.count) == 0
        else { return nil }
        let data = Data(bytes)
        guard let image = UIImage(data: data) else { return nil }
        try? data.write(to: file, options: .atomic)
        return image
    }

    private func setCover(_ image: UIImage, for titleID: String) {
        let backdrop = Backdrop(image: image)
        DispatchQueue.main.async {
            self.covers[titleID] = image
            self.backdrops[titleID] = backdrop
        }
    }

    func cover(for entry: LibraryEntry?) -> UIImage? { entry?.titleID.flatMap { covers[$0] } }
    func backdrop(for entry: LibraryEntry?) -> Backdrop? { entry?.titleID.flatMap { backdrops[$0] } }

    // MARK: booting

    /// Play: the overlay shows at once; the boot starts a moment later (Cancel works until then) or once the
    /// executable is ready.
    func play() {
        guard boot == nil, let entry = focused else { return }
        bootError = nil
        guard entry.titleID != nil, entry.problem == nil else {
            bootError = entry.problem ?? "This disc can't be played"
            return
        }
        boot = Boot(entryID: entry.id, phase: .armed)
        armTimer?.invalidate()
        // A one-shot timer is still valid while its block runs: the block clears it first.
        armTimer = Timer.scheduledTimer(withTimeInterval: 1.2, repeats: false) { [weak self] _ in
            self?.armTimer = nil
            self?.armBoot()
        }
    }

    /// While armed or preparing: Cancel (or ○) returns to the library (a preparation keeps what it compiled, and
    /// goes on from there next time). Once the game's threads run, nothing tears them down yet.
    func cancel() {
        guard let boot, boot.phase != .started else { return }
        armTimer?.invalidate()
        armTimer = nil
        if boot.phase == .preparing {
            preparer.cancel()
            UIApplication.shared.isIdleTimerDisabled = false
        }
        self.boot = nil
    }

    private func armBoot() {
        guard let boot, boot.phase == .armed, armTimer == nil,
              let entry = entries.first(where: { $0.id == boot.entryID }), let titleID = entry.titleID else { return }
        if decrypting.contains(titleID) { return } // ensureExecutable comes back here when done
        guard let elf = Self.executable(titleID: titleID), game != nil else {
            self.boot = nil
            bootError = entry.problem ?? "The game's executable is missing"
            return
        }
        // The PS3 system software's own modules, translated once (D-178), before the first game after it is
        // installed (or after an update of pspocket that translates differently), and the SPU programs of the system
        // modules a game links, the first time a game links them (D-191). A game goes on without them if that fails:
        // its firmware modules then run under the interpreter.
        if !systemChecked.contains(titleID),
           FileManager.default.fileExists(atPath: Firmware.devFlash.appendingPathComponent("sys").path) {
            systemChecked.insert(titleID)
            let system = Preparer.checkSystem(executable: elf)
            if !system.ready {
                Self.log.notice("pspocket: preparing the system software: \(system.why, privacy: .public)")
                let entryID = entry.id
                let message = preparer.startSystem(executable: elf, finish: { [weak self] result in
                    guard let self else { return }
                    UIApplication.shared.isIdleTimerDisabled = false
                    if case .failure(let error) = result {
                        Self.log.error("pspocket: preparing the system software failed: \(error.message, privacy: .public)")
                    }
                    guard self.boot?.entryID == entryID, self.boot?.phase == .preparing else { return }
                    self.boot?.phase = .armed
                    self.armBoot()
                })
                if message == nil {
                    UIApplication.shared.isIdleTimerDisabled = true
                    self.boot?.phase = .preparing
                    return
                }
                Self.log.error("pspocket: cannot prepare the system software: \(message ?? "", privacy: .public)")
            }
        }
        // Route B (D-154): the game runs from the library this iPhone made of it; the first time, it is made now.
        let gameDir = Self.decryptedGames.appendingPathComponent(titleID)
        let check = Preparer.check(gameDir: gameDir, executable: elf)
        if check.ready {
            startGame(entry: entry, titleID: titleID, elf: elf, library: gameDir.appendingPathComponent("game.dylib"))
            return
        }
        Self.log.notice("pspocket: preparing \(titleID, privacy: .public): \(check.why, privacy: .public)")
        let entryID = entry.id
        if let message = preparer.start(executable: elf, titleID: titleID, gameDir: gameDir, finish: { [weak self] result in
            guard let self else { return }
            UIApplication.shared.isIdleTimerDisabled = false
            guard self.boot?.entryID == entryID, self.boot?.phase == .preparing,
                  let entry = self.entries.first(where: { $0.id == entryID }) else { return }
            switch result {
            case .success(let library):
                self.startGame(entry: entry, titleID: titleID, elf: elf, library: library)
            case .failure(let error):
                // The game still starts, the way it did before route B: the translation built into the app when
                // it is this game's, else the interpreters (slow).
                Self.log.error("pspocket: preparing \(titleID, privacy: .public) failed: \(error.message, privacy: .public)")
                self.preparationNotes[titleID] = "Preparing failed: \(error.message)"
                self.startGame(entry: entry, titleID: titleID, elf: elf, library: nil)
            }
        }) {
            Self.log.error("pspocket: cannot prepare \(titleID, privacy: .public): \(message, privacy: .public)")
            preparationNotes[titleID] = "Not prepared: \(message)"
            startGame(entry: entry, titleID: titleID, elf: elf, library: nil)
            return
        }
        UIApplication.shared.isIdleTimerDisabled = true // the screen stays on while the phone prepares the game
        self.boot?.phase = .preparing
    }

    private func startGame(entry: LibraryEntry, titleID: String, elf: URL, library: URL?) {
        guard let game else { return }
        if let message = game.boot(iso: entry.url, elf: elf, library: library) {
            self.boot = nil
            bootError = message
            return
        }
        var stats = store.stats[titleID] ?? Stats()
        stats.lastPlayed = Date()
        store.stats[titleID] = stats
        saveSoon()
        self.boot?.phase = .started
        playingTitleID = titleID
    }

    /// The game showed its first frame: the overlay goes.
    func gameShowedFrames() {
        if boot?.phase == .started { boot = nil }
    }

    // MARK: play time

    @ObservationIgnored private var playingTitleID: String?

    private func played(_ seconds: TimeInterval) {
        guard let id = playingTitleID, seconds > 0 else { return }
        var stats = store.stats[id] ?? Stats()
        stats.seconds += seconds
        stats.lastPlayed = Date()
        store.stats[id] = stats
        saveSoon(after: 15)
    }

    // MARK: persistence

    struct Stats: Codable {
        var lastPlayed: Date?
        var seconds: Double = 0
    }

    private struct Store: Codable {
        struct Added: Codable {
            var id: UUID
            var bookmark: Data
        }
        var added: [Added] = []
        var stats: [String: Stats] = [:]
        var migratedOldBookmark = false
    }

    private func loadStore() {
        guard let data = try? Data(contentsOf: Self.storeURL),
              let decoded = try? JSONDecoder().decode(Store.self, from: data) else { return }
        store = decoded
    }

    private func saveSoon(after delay: TimeInterval = 0.5) {
        guard pendingSave == nil else { return }
        let work = DispatchWorkItem { [weak self] in
            self?.pendingSave = nil
            self?.save()
        }
        pendingSave = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    func save() {
        pendingSave?.cancel()
        pendingSave = nil
        try? FileManager.default.createDirectory(at: Self.storeURL.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        if let data = try? JSONEncoder().encode(store) { try? data.write(to: Self.storeURL, options: .atomic) }
    }
}

/// A cover prepared for the animated background: the cover itself, and its blurred, saturated copies made once with
/// Core Image (the background moves them every frame; blurring them every frame would cost the GPU for nothing).
struct Backdrop {
    let image: UIImage
    let soft: UIImage    // blur 2 pt as shown, saturation 1.5, brightness 0.85
    let glowA: UIImage   // blur 14 pt, saturation 2
    let glowB: UIImage   // blur 17 pt, saturation 2.2
    let glowC: UIImage   // blur 21 pt, saturation 2.2

    init(image: UIImage) {
        self.image = image
        // The copies are drawn about 2.3x the cover's 350 pt width; the blur radii are in the drawn size.
        soft = Self.filtered(image, blur: 2 / 1.3, saturation: 1.5, brightness: -0.08)
        glowA = Self.filtered(image, blur: 14 / 2.3, saturation: 2, brightness: 0)
        glowB = Self.filtered(image, blur: 17 / 2.2, saturation: 2.2, brightness: 0)
        glowC = Self.filtered(image, blur: 21 / 2.05, saturation: 2.2, brightness: 0)
    }

    private static let context = CIContext(options: [.useSoftwareRenderer: false])

    private static func filtered(_ image: UIImage, blur: Double, saturation: Double, brightness: Double) -> UIImage {
        guard let input = CIImage(image: image) else { return image }
        let width = input.extent.width
        let scale = width > 0 ? 350 / width : 1 // radii are for a 350 pt wide cover
        let color = CIFilter.colorControls()
        color.inputImage = input.clampedToExtent()
        color.saturation = Float(saturation)
        color.brightness = Float(brightness)
        let blurred = CIFilter.gaussianBlur()
        blurred.inputImage = color.outputImage
        blurred.radius = Float(blur / scale)
        guard let output = blurred.outputImage?.cropped(to: input.extent),
              let cg = context.createCGImage(output, from: input.extent) else { return image }
        return UIImage(cgImage: cg)
    }
}
