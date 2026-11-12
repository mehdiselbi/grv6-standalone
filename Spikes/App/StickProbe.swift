// Phase 0c + 0d (on-device half):
// 0c: with GRV6 and a USB stick on the same hub, stream-read a big file from the stick
//     while MIDI and the tone run. Logs throughput and any device add/remove events.
// 0d: walk the stick and list files under PIONEER/ with size and first 16 bytes (hex).
//     Classification by path only; byte meaning is checked later against Deep Symmetry docs.

import Foundation
import Combine
import UniformTypeIdentifiers

final class StickProbe: ObservableObject {
    @Published var report = ""
    private var root: URL?

    func setRoot(_ url: URL) {
        root = url
        report = "picked: \(url.path)\n"
    }

    func inventory() {
        guard let root, root.startAccessingSecurityScopedResource() else { report += "no access\n"; return }
        defer { root.stopAccessingSecurityScopedResource() }
        var out = "== inventory ==\n"
        let fm = FileManager.default
        guard let en = fm.enumerator(at: root, includingPropertiesForKeys: [.fileSizeKey, .isDirectoryKey]) else { return }
        var anlzCount = 0
        for case let u as URL in en {
            let rel = u.path.replacingOccurrences(of: root.path, with: "")
            guard rel.uppercased().contains("PIONEER") else { continue }
            let isDir = (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            if isDir { continue }
            let name = u.lastPathComponent
            if name.uppercased().hasPrefix("ANLZ") { anlzCount += 1; if anlzCount > 6 { continue } }
            let size = (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            let head = (try? FileHandle(forReadingFrom: u).read(upToCount: 16)) ?? Data()
            out += "\(rel)  \(size) B  \(head.map { String(format: "%02X", $0) }.joined(separator: " "))\n"
        }
        out += "ANLZ files total: \(anlzCount)\n"
        report += out
    }

    /// Reads the file in 1 MiB chunks and reports MB/s. Pick a large audio file on the stick.
    func throughput(file: URL) {
        DispatchQueue.global(qos: .userInitiated).async {
            let scoped = file.startAccessingSecurityScopedResource()
            defer { if scoped { file.stopAccessingSecurityScopedResource() } }
            guard let h = try? FileHandle(forReadingFrom: file) else {
                DispatchQueue.main.async { self.report += "open failed\n" }; return
            }
            let t0 = Date(); var total = 0
            while let d = try? h.read(upToCount: 1 << 20), !d.isEmpty { total += d.count }
            let dt = Date().timeIntervalSince(t0)
            DispatchQueue.main.async {
                self.report += String(format: "read %.1f MB in %.2f s = %.1f MB/s\n",
                                      Double(total) / 1e6, dt, Double(total) / 1e6 / dt)
            }
        }
    }
}
