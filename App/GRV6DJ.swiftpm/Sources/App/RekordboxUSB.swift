import DJCore
import Foundation

struct RBTrack: Identifiable, Hashable {
    let id: UInt32
    let title, artist, album, genre, key, label, filePath, analyzePath, artworkPath: String
    let bpm: Double
    let duration: Int
    let rating: Int
}

struct RBPlaylist: Identifiable, Hashable {
    let id: UInt32
    let parentId: UInt32
    let name: String
    let isFolder: Bool
    let sortOrder: UInt32
}

/// A rekordbox USB stick picked in the Files app. iPadOS only lets apps reach external drives through a
/// user-picked folder, so the owner picks the stick root once; we keep a security-scoped bookmark.
/// Read-only: we never write to the stick.
@MainActor
final class RekordboxUSB: ObservableObject, Identifiable {
    enum Format: String { case deviceLibrary = "Device Library", oneLibrary = "OneLibrary" }

    let id = UUID()
    let root: URL
    let label: String // USB1, USB2
    @Published private(set) var name: String
    @Published private(set) var formats: [Format] = []
    @Published private(set) var tracks: [RBTrack] = []
    @Published private(set) var playlists: [RBPlaylist] = []
    @Published private(set) var status = "Reading…"
    @Published private(set) var ready = false
    private var lib: OpaquePointer?
    private var scoped = false

    init(root: URL, label: String) {
        self.root = root
        self.label = label
        self.name = root.lastPathComponent
        scoped = root.startAccessingSecurityScopedResource() // held while the stick is in use
    }

    deinit {
        if let lib { djc_lib_close(lib) }
        if scoped { root.stopAccessingSecurityScopedResource() }
    }

    /// The stick's own volume name (as in the Files app), else USB1 / USB2.
    var displayName: String {
        let n = (try? root.resourceValues(forKeys: [.volumeLocalizedNameKey]))?.volumeLocalizedName ?? ""
        return n.isEmpty || n.count > 30 ? label : n
    }

    var pdbPath: String { root.appendingPathComponent("PIONEER/rekordbox/export.pdb").path }

    func path(_ stickPath: String) -> String { root.path + (stickPath.hasPrefix("/") ? "" : "/") + stickPath }
    func url(_ stickPath: String) -> URL { URL(fileURLWithPath: path(stickPath)) }

    func open() {
        let fm = FileManager.default
        var f: [Format] = []
        if fm.fileExists(atPath: pdbPath) { f.append(.deviceLibrary) }
        // OneLibrary (exportLibrary.db): location per rekordbox 7 exports; we only detect it for now
        let oneCandidates = ["PIONEER/rekordbox/exportLibrary.db", "PIONEER/exportLibrary.db", "exportLibrary.db"]
        if oneCandidates.contains(where: { fm.fileExists(atPath: root.appendingPathComponent($0).path) }) { f.append(.oneLibrary) }
        formats = f
        guard f.contains(.deviceLibrary) else {
            status = f.contains(.oneLibrary)
                ? "OneLibrary only. Export the Device Library too (rekordbox › USB export), OneLibrary support comes later."
                : "No rekordbox export found (PIONEER/rekordbox/export.pdb). Pick the stick's top folder."
            ready = true
            return
        }
        let path = pdbPath
        Task.detached(priority: .userInitiated) {
            let lib = djc_lib_open_pdb(path)
            var tracks: [RBTrack] = []
            var pls: [RBPlaylist] = []
            var err = ""
            if let lib {
                err = String(cString: djc_lib_error(lib))
                let s = { (p: UnsafePointer<CChar>?) in p.map { String(cString: $0) } ?? "" }
                for i in 0..<djc_lib_track_count(lib) {
                    guard let t = djc_lib_track_at(lib, i)?.pointee else { continue }
                    tracks.append(RBTrack(id: t.id, title: s(t.title), artist: s(t.artist), album: s(t.album),
                                          genre: s(t.genre), key: s(t.key), label: s(t.label), filePath: s(t.filePath),
                                          analyzePath: s(t.analyzePath), artworkPath: s(t.artworkPath), bpm: t.bpm, duration: Int(t.duration),
                                          rating: Int(t.rating)))
                }
                for i in 0..<djc_lib_playlist_count(lib) {
                    guard let p = djc_lib_playlist_at(lib, i)?.pointee else { continue }
                    pls.append(RBPlaylist(id: p.id, parentId: p.parentId, name: s(p.name), isFolder: p.isFolder != 0,
                                          sortOrder: p.sortOrder))
                }
            }
            let sorted = tracks.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
            await MainActor.run {
                self.lib = lib
                self.tracks = sorted
                self.playlists = pls
                self.status = err.isEmpty ? "\(sorted.count) tracks, \(pls.filter { !$0.isFolder }.count) playlists" : err
                self.ready = true
            }
        }
    }

    func children(of folder: UInt32) -> [RBPlaylist] {
        playlists.filter { $0.parentId == folder }.sorted { $0.sortOrder < $1.sortOrder }
    }

    func tracks(inPlaylist id: UInt32) -> [RBTrack] {
        guard let lib else { return [] }
        let n = Int(djc_lib_playlist_tracks(lib, id, nil, 0))
        guard n > 0 else { return [] }
        var ids = [UInt32](repeating: 0, count: n)
        _ = djc_lib_playlist_tracks(lib, id, &ids, Int32(n))
        let byId = Dictionary(tracks.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        return ids.compactMap { byId[$0] }
    }

    // MARK: bookmark persistence

    static let bookmarkKey = "usbBookmarks"

    static func saveBookmark(_ url: URL) {
        guard let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else { return }
        var list = (UserDefaults.standard.array(forKey: bookmarkKey) as? [Data]) ?? []
        list.removeAll { (try? resolve($0))?.path == url.path }
        list.insert(data, at: 0)
        UserDefaults.standard.set(Array(list.prefix(8)), forKey: bookmarkKey)  // up to 8 sticks remembered
    }

    static func savedRoots() -> [URL] {
        let list = (UserDefaults.standard.array(forKey: bookmarkKey) as? [Data]) ?? []
        return list.compactMap { try? resolve($0) }
    }

    private static func resolve(_ data: Data) throws -> URL {
        var stale = false
        return try URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
    }
}

// MARK: - Musical keys (for display and KEY SYNC)

enum MusicalKey {
    /// Pitch class (0 = C) of the relative major, or nil. Accepts "Am", "F#m", "Abm", "Emin", "Cmaj", "8A".
    static func relativeMajorPitch(_ raw: String) -> Int? {
        let k = raw.trimmingCharacters(in: .whitespaces)
        guard !k.isEmpty else { return nil }
        // Camelot: 1A..12A (minor), 1B..12B (major). 8B = C major, 8A = A minor (relative major C).
        if let last = k.last, last == "A" || last == "B", let n = Int(k.dropLast()), (1...12).contains(n) {
            return ((n - 8) * 7 % 12 + 12) % 12 // each Camelot step is a fifth
        }
        let names: [Character: Int] = ["C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11]
        guard let first = k.first, var pc = names[Character(first.uppercased())] else { return nil }
        var rest = k.dropFirst()
        if let a = rest.first, a == "#" || a == "♯" { pc += 1; rest = rest.dropFirst() }
        else if let a = rest.first, a == "b" || a == "♭" { pc -= 1; rest = rest.dropFirst() }
        let r = rest.lowercased()
        let minor = r.hasPrefix("m") && !r.hasPrefix("maj")
        return ((pc + (minor ? 3 : 0)) % 12 + 12) % 12
    }

    /// Camelot position: number 1...12 and minor flag. 8B = C major, 8A = A minor.
    static func camelot(_ raw: String) -> (Int, Bool)? {
        let k = raw.trimmingCharacters(in: .whitespaces)
        if let last = k.last, last == "A" || last == "B", let n = Int(k.dropLast()), (1...12).contains(n) {
            return (n, last == "A")
        }
        guard let pc = relativeMajorPitch(k) else { return nil }
        let rest = k.dropFirst().drop(while: { "#♯b♭".contains($0) }).lowercased()
        let minor = rest.hasPrefix("m") && !rest.hasPrefix("maj")
        return ((pc * 7 % 12 + 7) % 12 + 1, minor)
    }

    /// Harmonic mix rule used for the green key highlight: same key, +-1 on the wheel, or relative major/minor.
    static func compatible(_ a: String, _ b: String, shiftB: Int = 0) -> Bool {
        guard let x = camelot(a), var y = camelot(b) else { return false }
        if shiftB != 0 { y.0 = ((y.0 - 1 + shiftB * 7) % 12 + 12) % 12 + 1 }
        if x.1 == y.1 {
            let d = abs(x.0 - y.0)
            return d == 0 || d == 1 || d == 11
        }
        return x.0 == y.0
    }

    /// Semitones to shift `deckKey` (currently playing at `rate` without keylock if `keylock` is false)
    /// so it matches `masterKey`, in -6...+5.
    static func syncShift(deckKey: String, masterKey: String, deckRate: Double, keylock: Bool,
                          masterRate: Double, masterKeylock: Bool) -> Int? {
        guard let a = relativeMajorPitch(deckKey), let b = relativeMajorPitch(masterKey) else { return nil }
        let own = Double(a) + (keylock ? 0 : 12 * log2(deckRate))
        let target = Double(b) + (masterKeylock ? 0 : 12 * log2(masterRate))
        var d = Int((target - own).rounded()) % 12
        if d > 5 { d -= 12 }
        if d < -6 { d += 12 }
        return d
    }
}
