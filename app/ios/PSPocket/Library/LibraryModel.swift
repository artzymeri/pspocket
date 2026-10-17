import CoreImage
import CoreImage.CIFilterBuiltins
import Foundation
import Observation
import os
import UIKit

/// One game in the library: a disc image (an .iso in Documents/Games, Files > On My iPhone > PSPocket > Games, or one
/// the user added from anywhere else with Add Game, kept as a security-scoped bookmark), or a game installed from its
/// package into Documents/hdd0/game (D-939: a PSN game).
struct LibraryEntry: Identifiable, Equatable {
    enum Kind { case disc, installed }
    let id: String // "games:<path inside Games>", "added:<uuid>" or "hdd0:<folder>"
    let url: URL   // the disc image, or the installed game's folder
    var kind: Kind = .disc
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
///   Library/Application Support/pspocket-games/<TITLE_ID>/EBOOT.elf   the disc's executable decrypted here (D-121), or
///                                                    an installed game's; update-<version>/EBOOT.elf an update's
///   Documents/hdd0/game/<folder>/                     games and updates installed from packages put in Games (D-939)
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
    /// How installing the packages and licences put in Games goes (D-939), for the library to show.
    private(set) var notice: String?

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
    /// Why an installed game cannot be played (its executable did not decrypt: no licence yet), by entry ID.
    @ObservationIgnored private var installedProblems: [String: String] = [:]
    @ObservationIgnored private var installing: String? // the package being installed (its path)
    @ObservationIgnored private var installQueue: [(url: URL, scoped: Bool)] = []
    @ObservationIgnored private var seenPackages: [String: (signature: String, since: Date)] = [:]
    @ObservationIgnored private var packageCheckPending = false
    @ObservationIgnored private let packageQueue = DispatchQueue(label: "pspocket.library.packages", qos: .utility)

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
    /// The folder that is the games' /dev_hdd0 (GameModel mounts it): installed games and updates in game/<folder>,
    /// the player's licences in home/00000001/exdata.
    static var hdd0: URL { documents.appendingPathComponent("hdd0", isDirectory: true) }

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

    /// Images in Documents/Games and the ones added elsewhere, probed, and the games installed in /dev_hdd0, newest
    /// played first; packages and licences put in Games are installed.
    func rescan() {
        var found: [(id: String, url: URL)] = []
        var dropped: [URL] = []
        let games = Self.gamesFolder.standardizedFileURL
        if let walker = FileManager.default.enumerator(at: games, includingPropertiesForKeys: [.isRegularFileKey],
                                                       options: [.skipsHiddenFiles, .skipsPackageDescendants]) {
            for case let url as URL in walker {
                switch url.pathExtension.lowercased() {
                case "iso":
                    let path = url.standardizedFileURL.path
                    let relative = path.hasPrefix(games.path) ? String(path.dropFirst(games.path.count + 1)) : path
                    found.append(("games:" + relative, url))
                case "pkg", "rap":
                    dropped.append(url)
                default:
                    break
                }
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
        let installed = installedGames()
        next += installed
        apply(next)
        for entry in installed where entry.problem == nil { ensureExecutable(entry) }
        installDropped(dropped)
    }

    /// Games installed from their packages (D-939): folders of /dev_hdd0/game whose PARAM.SFO says HG (a PSN game),
    /// with an executable. Game data and disc games' updates (category GD) are not games of their own.
    private func installedGames() -> [LibraryEntry] {
        let folder = Self.hdd0.appendingPathComponent("game", isDirectory: true)
        guard let folders = try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil,
                                                                          options: [.skipsHiddenFiles]) else { return [] }
        var out: [LibraryEntry] = []
        for game in folders where FileManager.default.fileExists(atPath: game.appendingPathComponent("USRDIR/EBOOT.BIN").path) {
            guard let probe = Self.probeInstalled(game), probe.category == "HG" else { continue }
            let id = "hdd0:" + game.lastPathComponent
            var entry = LibraryEntry(id: id, url: game, kind: .installed)
            entry.titleID = probe.titleID
            entry.title = probe.title.replacingOccurrences(of: "\n", with: " ")
            entry.problem = installedProblems[id]
            out.append(entry)
        }
        return out
    }

    /// An installed game's folder: its PARAM.SFO's title ID, title, category and version.
    static func probeInstalled(_ folder: URL) -> (titleID: String, title: String, category: String, version: String)? {
        var tid = [CChar](repeating: 0, count: 32)
        var name = [CChar](repeating: 0, count: 256)
        var category = [CChar](repeating: 0, count: 8)
        var version = [CChar](repeating: 0, count: 16)
        var error = [CChar](repeating: 0, count: 256)
        guard ps_game_probe_installed(hdd0.path, folder.path, &tid, tid.count, &name, name.count, &category,
                                      category.count, &version, version.count, &error, error.count) == 0 else {
            return nil
        }
        return (String(cString: tid), String(cString: name), String(cString: category), String(cString: version))
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
        for entry in entries where entry.titleID != nil { requestCover(for: entry) }
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
                if titleID != nil, let entry = self.entries.first(where: { $0.id == id }) { self.ensureExecutable(entry) }
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
    /// A package or a licence is installed from where it is (D-939), and not kept.
    func add(_ url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        guard accessing || FileManager.default.isReadableFile(atPath: url.path) else {
            bootError = "Can't open \(url.lastPathComponent)"
            return
        }
        switch url.pathExtension.lowercased() {
        case "rap":
            if let signature = Self.signature(of: url) { installLicence(url, key: url.standardizedFileURL.path, signature: signature) }
            if accessing { url.stopAccessingSecurityScopedResource() }
            return
        case "pkg":
            installQueue.append((url, accessing))
            installNext()
            return
        default:
            break
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

    /// The update installed for a disc game (D-939): /dev_hdd0/game/<TITLE_ID> with category GD and an executable of
    /// its own, which a PS3 starts in the disc's place, with the disc.
    static func update(titleID: String) -> (folder: URL, version: String)? {
        var dir = [CChar](repeating: 0, count: 1024)
        var version = [CChar](repeating: 0, count: 16)
        guard ps_game_find_update(hdd0.path, titleID, &dir, dir.count, &version, version.count) == 1 else { return nil }
        return (URL(fileURLWithPath: String(cString: dir), isDirectory: true), String(cString: version))
    }

    /// What an entry starts (D-939): the disc's own executable; its update's, when one is installed (a PS3 starts that
    /// with the disc); or an installed game's. `elf` is where it is decrypted to (it may not exist yet), `folder` the
    /// game's folder in /dev_hdd0/game for the last two.
    private func target(for entry: LibraryEntry, titleID: String) -> (elf: URL, folder: URL?) {
        let dir = Self.decryptedGames.appendingPathComponent(titleID)
        switch entry.kind {
        case .installed:
            return (dir.appendingPathComponent("EBOOT.elf"), entry.url)
        case .disc:
            if let update = Self.update(titleID: titleID) {
                let name = update.version.isEmpty ? "update" : "update-\(update.version)"
                return (dir.appendingPathComponent(name).appendingPathComponent("EBOOT.elf"), update.folder)
            }
            return (Self.executable(titleID: titleID) ?? dir.appendingPathComponent("EBOOT.elf"), nil)
        }
    }

    /// The entry's executable decrypted on the phone, one at a time: a disc's with the keys the app carries (D-121),
    /// an installed game's or an update's with its licence (D-582: free content, or the player's .rap).
    private func ensureExecutable(_ entry: LibraryEntry) {
        guard let titleID = entry.titleID, !decrypting.contains(titleID) else { return }
        let target = self.target(for: entry, titleID: titleID)
        guard !FileManager.default.fileExists(atPath: target.elf.path) else { return }
        decrypting.insert(titleID)
        try? FileManager.default.createDirectory(at: target.elf.deletingLastPathComponent(), withIntermediateDirectories: true)
        let iso = entry.url, hdd0 = Self.hdd0, kind = entry.kind, entryID = entry.id
        decryptQueue.async {
            var error = [CChar](repeating: 0, count: 512)
            let status: Int32
            if let folder = target.folder {
                status = ps_psn_decrypt_executable(folder.path, hdd0.path, target.elf.path, &error, error.count)
            } else {
                status = ps_disc_decrypt_executable(iso.path, target.elf.path, &error, error.count)
            }
            let message = String(cString: error)
            DispatchQueue.main.async {
                self.decrypting.remove(titleID)
                if status != 0 {
                    let problem = "Could not decrypt the game's EBOOT.BIN: \(message)"
                    if kind == .installed { self.installedProblems[entryID] = problem }
                    for e in self.entries where e.titleID == titleID { self.update(e.id) { $0.problem = problem } }
                }
                if let boot = self.boot, boot.phase == .armed { self.armBoot() }
            }
        }
    }

    /// Where a game's own PRX modules and SPU ELFs come from for its preparation (D-239): its disc, and an installed
    /// game's or an update's folder in /dev_hdd0/game (D-939), each scanned once (per version for a folder).
    private struct ModuleSource {
        enum Kind { case disc(URL), folder(URL) }
        let kind: Kind
        let marker: String // the file that says it was scanned, in the game's folder
    }

    private func moduleSources(entry: LibraryEntry, folder: URL?, gameDir: URL) -> [ModuleSource] {
        var sources: [ModuleSource] = []
        if entry.kind == .disc { sources.append(ModuleSource(kind: .disc(entry.url), marker: "disc-scanned")) }
        if let folder {
            let version = Self.probeInstalled(folder)?.version ?? ""
            sources.append(ModuleSource(kind: .folder(folder), marker: "\(folder.lastPathComponent)-\(version)-scanned"))
        }
        return sources.filter { !FileManager.default.fileExists(atPath: gameDir.appendingPathComponent($0.marker).path) }
    }

    /// The game's own PRX modules and SPU ELFs saved into the game's folder for its preparation (D-239), once.
    private func ensureModules(titleID: String, sources: [ModuleSource], gameDir: URL) {
        guard !decrypting.contains(titleID) else { return }
        decrypting.insert(titleID)
        try? FileManager.default.createDirectory(at: gameDir, withIntermediateDirectories: true)
        let hdd0 = Self.hdd0
        decryptQueue.async {
            let prx = gameDir.appendingPathComponent("disc-prx").path, spu = gameDir.appendingPathComponent("disc-spu").path
            for source in sources {
                var error = [CChar](repeating: 0, count: 512)
                let saved: Int32
                switch source.kind {
                case .disc(let iso):
                    saved = ps_disc_save_modules(iso.path, prx, spu, &error, error.count)
                case .folder(let folder):
                    saved = ps_psn_save_modules(folder.path, hdd0.path, prx, spu, &error, error.count)
                }
                if saved < 0 {
                    Self.log.error("pspocket: \(titleID, privacy: .public): its own modules: \(String(cString: error), privacy: .public)")
                } else {
                    Self.log.notice("pspocket: \(titleID, privacy: .public): \(saved) executable files of its own (\(source.marker, privacy: .public))")
                }
                // Done, whatever it found (a disc that cannot be read now is not read again on every start).
                FileManager.default.createFile(atPath: gameDir.appendingPathComponent(source.marker).path, contents: nil)
            }
            DispatchQueue.main.async {
                self.decrypting.remove(titleID)
                if let boot = self.boot, boot.phase == .armed { self.armBoot() }
            }
        }
    }

    // MARK: packages and licences (D-939)

    /// The size and date of a file put in Games: a package or a licence is installed once per version of the file.
    private static func signature(of url: URL) -> String? {
        guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey]),
              let size = values.fileSize else { return nil }
        return "\(size):\(values.contentModificationDate?.timeIntervalSince1970 ?? 0)"
    }

    /// A package split into parts is installed from its first, NAME_00.pkg (ps_psn_install_pkg).
    private static func isLaterPart(_ url: URL) -> Bool {
        let name = url.deletingPathExtension().lastPathComponent
        guard name.count > 3, let part = Int(name.suffix(2)), name.dropLast(2).hasSuffix("_") else { return false }
        return part != 0
    }

    /// Packages (.pkg: a PSN game, an update, add-ons) and licences (.rap) put in Games are installed into /dev_hdd0
    /// as RPCS3 installs them (D-583), once per file; the files stay (the user deletes them in Files). A file is
    /// taken once it has not changed for 3 s (Files copies a big one in for a while).
    private func installDropped(_ urls: [URL]) {
        var waiting = false
        for url in urls.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let key = url.standardizedFileURL.path
            guard let signature = Self.signature(of: url), store.packages?[key]?.signature != signature,
                  installing != key, !installQueue.contains(where: { $0.url.standardizedFileURL.path == key }),
                  !Self.isLaterPart(url) else { continue }
            guard let seen = seenPackages[key], seen.signature == signature,
                  Date().timeIntervalSince(seen.since) >= 3 else {
                if seenPackages[key]?.signature != signature { seenPackages[key] = (signature, Date()) }
                waiting = true
                continue
            }
            if url.pathExtension.lowercased() == "rap" {
                installLicence(url, key: key, signature: signature)
            } else {
                installQueue.append((url, false))
            }
        }
        if waiting && !packageCheckPending {
            packageCheckPending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.5) {
                self.packageCheckPending = false
                self.rescan()
            }
        }
        installNext()
    }

    private func record(_ key: String, signature: String, ok: Bool) {
        var records = store.packages ?? [:]
        records[key] = PackageRecord(signature: signature, ok: ok)
        store.packages = records
        saveSoon()
    }

    /// A licence (<CONTENT_ID>.rap) put where pspocket looks for it (D-580); the games it opens are tried again.
    private func installLicence(_ url: URL, key: String, signature: String) {
        var error = [CChar](repeating: 0, count: 512)
        let ok = ps_psn_install_rap(Self.hdd0.path, url.path, &error, error.count) == 0
        record(key, signature: signature, ok: ok)
        guard ok else {
            notice = "\(url.lastPathComponent) was not installed: \(String(cString: error))"
            return
        }
        notice = "Licence \(url.deletingPathExtension().lastPathComponent) installed"
        installedProblems.removeAll()
        for entry in entries where entry.problem?.hasPrefix("Could not decrypt") == true {
            update(entry.id) { $0.problem = nil }
            if let fresh = entries.first(where: { $0.id == entry.id }) { ensureExecutable(fresh) }
        }
    }

    /// The next package, in the background; the screen stays on meanwhile (a locked phone pauses the install).
    private func installNext() {
        guard installing == nil, !installQueue.isEmpty else { return }
        let (url, scoped) = installQueue.removeFirst()
        let key = url.standardizedFileURL.path
        guard let signature = Self.signature(of: url) else {
            if scoped { url.stopAccessingSecurityScopedResource() }
            installNext()
            return
        }
        installing = key
        notice = "Installing \(url.lastPathComponent)…"
        UIApplication.shared.isIdleTimerDisabled = true
        let hdd0 = Self.hdd0
        try? FileManager.default.createDirectory(at: hdd0, withIntermediateDirectories: true)
        let progress = InstallProgress()
        let timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            let files = progress.files
            if files > 0 { self?.notice = "Installing \(url.lastPathComponent)… \(files) files" }
        }
        packageQueue.async {
            var error = [CChar](repeating: 0, count: 1024)
            let user = Unmanaged.passRetained(progress).toOpaque()
            let status = ps_psn_install_pkg(url.path, hdd0.path, { user, line in
                guard let user, let line else { return }
                Unmanaged<InstallProgress>.fromOpaque(user).takeUnretainedValue().add(String(cString: line))
            }, user, &error, error.count)
            Unmanaged<InstallProgress>.fromOpaque(user).release()
            let message = String(cString: error)
            DispatchQueue.main.async {
                timer.invalidate()
                if scoped { url.stopAccessingSecurityScopedResource() }
                self.installing = nil
                if self.boot == nil { UIApplication.shared.isIdleTimerDisabled = false }
                self.record(key, signature: signature, ok: status == 0)
                if status == 0 {
                    // A package refused before (an update put in before its game) is tried again.
                    if var records = self.store.packages {
                        records = records.filter { $0.value.ok }
                        self.store.packages = records
                    }
                    var text = "Installed \(url.lastPathComponent); you can delete it from Games now"
                    if let licence = progress.licenceLine { text += ". \(licence)" }
                    self.notice = text
                    self.rescan()
                } else {
                    self.notice = "\(url.lastPathComponent) was not installed: \(message)"
                }
                self.installNext()
            }
        }
    }

    // MARK: covers

    /// GameTDB's cover (350x408), cached; else the disc's (or the installed game's) ICON0.PNG; else none (the title
    /// shows as text).
    private func requestCover(for entry: LibraryEntry) {
        guard let titleID = entry.titleID, !coverRequested.contains(titleID) else { return }
        coverRequested.insert(titleID)
        let cover = Self.coversFolder.appendingPathComponent("\(titleID).jpg")
        let icon0 = Self.coversFolder.appendingPathComponent("\(titleID)-icon0.png")
        DispatchQueue.global(qos: .utility).async {
            if let image = UIImage(contentsOfFile: cover.path) {
                self.setCover(image, for: titleID)
                return
            }
            // ICON0.PNG first, while the download runs (and for good when there is no network).
            let own = entry.kind == .installed ? Self.copyIcon0(folder: entry.url, to: icon0)
                : Self.readIcon0(iso: entry.url, to: icon0)
            if let image = UIImage(contentsOfFile: icon0.path) ?? own {
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

    private static func copyIcon0(folder: URL, to file: URL) -> UIImage? {
        guard let data = try? Data(contentsOf: folder.appendingPathComponent("ICON0.PNG")), let image = UIImage(data: data)
        else { return nil }
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
        let target = self.target(for: entry, titleID: titleID)
        if !FileManager.default.fileExists(atPath: target.elf.path), entry.problem == nil {
            ensureExecutable(entry) // an update installed since the disc was read: decrypted now, then back here
            if decrypting.contains(titleID) { return }
        }
        guard FileManager.default.fileExists(atPath: target.elf.path), game != nil else {
            self.boot = nil
            bootError = entry.problem ?? "The game's executable is missing"
            return
        }
        let elf = target.elf, folder = target.folder
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
        // The game's own PRX modules and SPU programs (D-239): its disc's, its update's or the installed game's, found
        // once, so its first session runs them translated too (scanning reads only files named as executables).
        let gameDir = Self.decryptedGames.appendingPathComponent(titleID)
        let sources = moduleSources(entry: entry, folder: folder, gameDir: gameDir)
        if !sources.isEmpty {
            ensureModules(titleID: titleID, sources: sources, gameDir: gameDir)
            return // comes back here when done
        }
        // Route B (D-154): the game runs from the library this iPhone made of it; the first time, it is made now.
        let check = Preparer.check(gameDir: gameDir, executable: elf)
        if check.ready {
            startGame(entry: entry, titleID: titleID, elf: elf, folder: folder,
                      library: gameDir.appendingPathComponent("game.dylib"))
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
                self.startGame(entry: entry, titleID: titleID, elf: elf, folder: folder, library: library)
            case .failure(let error):
                // The game still starts, the way it did before route B: the translation built into the app when
                // it is this game's, else the interpreters (slow).
                Self.log.error("pspocket: preparing \(titleID, privacy: .public) failed: \(error.message, privacy: .public)")
                self.preparationNotes[titleID] = "Preparing failed: \(error.message)"
                self.startGame(entry: entry, titleID: titleID, elf: elf, folder: folder, library: nil)
            }
        }) {
            Self.log.error("pspocket: cannot prepare \(titleID, privacy: .public): \(message, privacy: .public)")
            preparationNotes[titleID] = "Not prepared: \(message)"
            startGame(entry: entry, titleID: titleID, elf: elf, folder: folder, library: nil)
            return
        }
        UIApplication.shared.isIdleTimerDisabled = true // the screen stays on while the phone prepares the game
        self.boot?.phase = .preparing
    }

    /// `folder`: the game's folder in /dev_hdd0/game when it starts from there (D-939): an installed game's, or the
    /// disc's update's (the disc goes in the drive all the same).
    private func startGame(entry: LibraryEntry, titleID: String, elf: URL, folder: URL?, library: URL?) {
        guard let game else { return }
        if let message = game.boot(iso: entry.kind == .disc ? entry.url : nil, gameFolder: folder, elf: elf,
                                   library: library) {
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

    /// A package or licence installed (or refused) from a file: its size and date then (D-939).
    struct PackageRecord: Codable {
        var signature: String
        var ok: Bool
    }

    private struct Store: Codable {
        struct Added: Codable {
            var id: UUID
            var bookmark: Data
        }
        var added: [Added] = []
        var stats: [String: Stats] = [:]
        var migratedOldBookmark = false
        var packages: [String: PackageRecord]? // by the file's path; optional: stores written before it decode
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

    // MARK: removing a game (D-1092)

    /// What removing a game deletes: its disc image when it is in Games (an image added from elsewhere only leaves the
    /// library: the file is the user's), an installed game's folder, the executable and translation this iPhone made
    /// (pspocket-games/<TITLE_ID>), what its sessions learnt and translated (pspocket-coverage, tmp/pspocket-session),
    /// the data the game installed or cached in hdd0/game/<TITLE_ID>* (GTA IV's map copies, L.A. Noire's install, its
    /// updates) and its cover; with `saves`, its saves too (hdd0/home/00000001/savedata/<TITLE_ID>*) and its play time.
    struct Removal {
        var paths: [URL] = []
        var saves: [URL] = []
        var bytes: Int64 = 0
        var savesBytes: Int64 = 0
        var forgetsAdded = false // an image added from elsewhere: the library forgets it, the file stays
    }

    func removal(for entry: LibraryEntry) -> Removal {
        let fm = FileManager.default
        var r = Removal()
        var paths: [URL] = []
        switch entry.kind {
        case .installed: paths.append(entry.url)
        case .disc:
            if entry.id.hasPrefix("added:") { r.forgetsAdded = true } else { paths.append(entry.url) }
        }
        if let id = entry.titleID, !id.isEmpty {
            paths.append(Self.decryptedGames.appendingPathComponent(id))
            paths.append(Self.support.appendingPathComponent("pspocket-coverage/\(id)"))
            paths.append(fm.temporaryDirectory.appendingPathComponent("pspocket-session/\(id)"))
            let gameDir = Self.hdd0.appendingPathComponent("game")
            for name in (try? fm.contentsOfDirectory(atPath: gameDir.path)) ?? [] where name.hasPrefix(id) {
                paths.append(gameDir.appendingPathComponent(name))
            }
            for name in (try? fm.contentsOfDirectory(atPath: Self.coversFolder.path)) ?? [] where name.hasPrefix(id) {
                paths.append(Self.coversFolder.appendingPathComponent(name))
            }
            let savedata = Self.hdd0.appendingPathComponent("home/00000001/savedata")
            for name in (try? fm.contentsOfDirectory(atPath: savedata.path)) ?? [] where name.hasPrefix(id) {
                r.saves.append(savedata.appendingPathComponent(name))
            }
        }
        var seen = Set<String>()
        r.paths = paths.filter { fm.fileExists(atPath: $0.path) && seen.insert($0.standardizedFileURL.path).inserted }
        r.bytes = r.paths.reduce(0) { $0 + Self.size(of: $1) }
        r.savesBytes = r.saves.reduce(0) { $0 + Self.size(of: $1) }
        return r
    }

    /// Removes the game (see Removal); false while a game runs or is being prepared. The library rescans when done.
    @discardableResult
    func remove(_ entry: LibraryEntry, saves: Bool) -> Bool {
        guard boot == nil, game?.running != true else { return false }
        let plan = removal(for: entry)
        if plan.forgetsAdded, let uuid = UUID(uuidString: String(entry.id.dropFirst("added:".count))) {
            added[entry.id]?.stopAccessingSecurityScopedResource()
            added[entry.id] = nil
            store.added.removeAll { $0.id == uuid }
        }
        if let id = entry.titleID {
            covers[id] = nil
            backdrops[id] = nil
            coverRequested.remove(id)
            preparationNotes[id] = nil
            systemChecked.remove(id)
            if saves { store.stats[id] = nil }
        }
        saveSoon()
        let targets = plan.paths + (saves ? plan.saves : [])
        let title = entry.displayTitle
        Self.log.notice("pspocket: removing \(title, privacy: .public): \(targets.count) item(s)")
        decryptQueue.async { [weak self] in
            for url in targets { try? FileManager.default.removeItem(at: url) }
            DispatchQueue.main.async {
                guard let self else { return }
                self.notice = "Removed \(title) (\(ByteCountFormatter.string(fromByteCount: plan.bytes + (saves ? plan.savesBytes : 0), countStyle: .file)))"
                self.rescan()
            }
        }
        return true
    }

    private static func size(of url: URL) -> Int64 {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir) else { return 0 }
        if !isDir.boolValue { return (try? fm.attributesOfItem(atPath: url.path)[.size] as? Int64) ?? 0 }
        var total: Int64 = 0
        if let e = fm.enumerator(at: url, includingPropertiesForKeys: [.totalFileAllocatedSizeKey, .fileSizeKey]) {
            for case let f as URL in e {
                let v = try? f.resourceValues(forKeys: [.totalFileAllocatedSizeKey, .fileSizeKey])
                total += Int64(v?.totalFileAllocatedSize ?? v?.fileSize ?? 0)
            }
        }
        return total
    }
}

/// What ps_psn_install_pkg reports while it installs (its callback runs on the install's thread): the files written,
/// and the line saying which licence the game needs when it is not in place yet.
final class InstallProgress: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    private var licence: String?

    var files: Int { lock.withLock { count } }
    var licenceLine: String? { lock.withLock { licence } }

    func add(_ line: String) {
        lock.withLock {
            if line.contains("which is not there yet") {
                licence = line.trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("file ") || line.hasPrefix("replaced ") {
                count += 1
            }
        }
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
