// Phase 0a: GRV6 MIDI logger.
// Records every incoming message, groups them per named control while "armed",
// and exports a mapping table (CSV) to compare against AlphaTheta's
// DDJ-GRV6_MIDI_Message_List_E1.pdf. No MIDI bytes are hard-coded here.

import Foundation
import CoreMIDI
import Combine

struct MIDIEventRecord: Identifiable {
    let id = UUID()
    let hostTime: UInt64
    let source: String
    let words: [UInt32]
    var bytes: [UInt8] {               // MIDI 1.0 bytes for UMP type 0x2 only
        guard let w = words.first, (w >> 28) == 0x2 else { return [] }
        return [UInt8((w >> 16) & 0xFF), UInt8((w >> 8) & 0xFF), UInt8(w & 0xFF)]
    }
    var hex: String {
        let b = bytes
        return b.isEmpty ? words.map { String(format: "%08X", $0) }.joined(separator: " ")
                         : b.map { String(format: "%02X", $0) }.joined(separator: " ")
    }
}

struct MappingRow: Identifiable {
    let id = UUID()
    var control: String       // name typed by you, e.g. "D1 PLAY deck1"
    var status: UInt8
    var data1: UInt8
    var minData2: UInt8
    var maxData2: UInt8
    var count: Int
    var pairedLSB: Bool       // CC d1 and d1+0x20 both seen in the same capture
}

final class MIDILogger: ObservableObject {
    @Published var log: [MIDIEventRecord] = []
    @Published var rows: [MappingRow] = []
    @Published var armedControl: String? = nil
    @Published var endpoints: [String] = []
    @Published var status = "idle"

    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private var capture: [UInt16: (min: UInt8, max: UInt8, n: Int)] = [:]
    private let q = DispatchQueue(label: "midi.logger")

    func start() {
        var err = MIDIClientCreateWithBlock("GRV6Spike" as CFString, &client) { [weak self] n in
            let id = n.pointee.messageID
            if id == .msgObjectAdded || id == .msgObjectRemoved || id == .msgSetupChanged {
                DispatchQueue.main.async { self?.status = "setup change: \(id.rawValue)"; self?.connectAll() }
            }
        }
        guard err == noErr else { status = "client err \(err)"; return }
        err = MIDIInputPortCreateWithProtocol(client, "in" as CFString, ._1_0, &inPort) { [weak self] list, src in
            self?.handle(list, src)
        }
        guard err == noErr else { status = "in port err \(err)"; return }
        MIDIOutputPortCreate(client, "out" as CFString, &outPort)
        connectAll()
    }

    func connectAll() {
        var names: [String] = []
        for i in 0..<MIDIGetNumberOfSources() {
            let src = MIDIGetSource(i)
            let name = Self.name(src)
            names.append("IN  \(name)")
            // srcConnRefCon carries the endpoint so we can label messages.
            MIDIPortConnectSource(inPort, src, UnsafeMutableRawPointer(bitPattern: UInt(src)))
        }
        for i in 0..<MIDIGetNumberOfDestinations() { names.append("OUT \(Self.name(MIDIGetDestination(i)))") }
        endpoints = names
        status = "listening on \(MIDIGetNumberOfSources()) source(s)"
    }

    private func handle(_ list: UnsafePointer<MIDIEventList>, _ refCon: UnsafeMutableRawPointer?) {
        let src = MIDIEndpointRef(UInt32(UInt(bitPattern: refCon)))
        let srcName = Self.name(src)
        var recs: [MIDIEventRecord] = []
        for pkt in list.unsafeSequence() {
            let n = Int(pkt.pointee.wordCount)
            let words = withUnsafeBytes(of: pkt.pointee.words) { Array($0.bindMemory(to: UInt32.self).prefix(n)) }
            recs.append(MIDIEventRecord(hostTime: pkt.pointee.timeStamp, source: srcName, words: words))
        }
        q.async { [weak self] in
            guard let self else { return }
            if self.armedOnQ {
                for r in recs where r.bytes.count == 3 {
                    let key = UInt16(r.bytes[0]) << 8 | UInt16(r.bytes[1])
                    let v = r.bytes[2]
                    let e = self.capture[key] ?? (v, v, 0)
                    self.capture[key] = (min(e.min, v), max(e.max, v), e.n + 1)
                }
            }
            DispatchQueue.main.async {
                self.log.append(contentsOf: recs)
                if self.log.count > 2000 { self.log.removeFirst(self.log.count - 2000) }
            }
        }
    }

    private var armedOnQ = false   // only touched on `q`, avoids main/q deadlock

    func arm(_ control: String) { q.sync { capture.removeAll(); armedOnQ = true }; armedControl = control }

    /// Commits everything seen while armed as rows for that control.
    func commit() {
        guard let name = armedControl else { return }
        let snap = q.sync { () -> [UInt16: (min: UInt8, max: UInt8, n: Int)] in armedOnQ = false; return capture }
        let keys = Set(snap.keys)
        for (k, v) in snap.sorted(by: { $0.key < $1.key }) {
            let st = UInt8(k >> 8), d1 = UInt8(k & 0xFF)
            let isCC = (st & 0xF0) == 0xB0
            let paired = isCC && d1 < 0x20 && keys.contains(UInt16(st) << 8 | UInt16(d1 + 0x20))
            rows.append(MappingRow(control: name, status: st, data1: d1, minData2: v.min,
                                   maxData2: v.max, count: v.n, pairedLSB: paired))
        }
        armedControl = nil
    }

    func discard() { q.sync { armedOnQ = false }; armedControl = nil }

    /// Sends raw MIDI 1.0 bytes (3 bytes) to every destination whose name contains `match`.
    /// Use only values taken from the official list's MIDI-OUT column.
    func send(hex: String, to match: String = "GRV6") -> String {
        let b = hex.split(separator: " ").compactMap { UInt8($0, radix: 16) }
        guard b.count == 3 else { return "need exactly 3 hex bytes" }
        let word: UInt32 = 0x2000_0000 | UInt32(b[0]) << 16 | UInt32(b[1]) << 8 | UInt32(b[2])
        var sent = 0
        for i in 0..<MIDIGetNumberOfDestinations() {
            let d = MIDIGetDestination(i)
            guard Self.name(d).localizedCaseInsensitiveContains(match) else { continue }
            var list = MIDIEventList()
            var w = word
            withUnsafeMutablePointer(to: &list) { lp in
                let pkt = MIDIEventListInit(lp, ._1_0)
                _ = MIDIEventListAdd(lp, MemoryLayout<MIDIEventList>.size, pkt, 0, 1, &w)
                MIDISendEventList(outPort, d, lp)
            }
            sent += 1
        }
        return "sent to \(sent) destination(s)"
    }

    func csv() -> String {
        var s = "control,status_hex,data1_hex,channel,type,data2_min,data2_max,count,cc_lsb_pair\n"
        for r in rows {
            let ch = Int(r.status & 0x0F) + 1
            let type: String
            switch r.status & 0xF0 {
            case 0x90: type = "NOTE"; case 0x80: type = "NOTE_OFF"; case 0xB0: type = "CC"
            default: type = String(format: "0x%02X", r.status & 0xF0)
            }
            s += "\"\(r.control)\",\(String(format: "%02X", r.status)),\(String(format: "%02X", r.data1)),\(ch),\(type),\(r.minData2),\(r.maxData2),\(r.count),\(r.pairedLSB)\n"
        }
        return s
    }

    static func name(_ obj: MIDIObjectRef) -> String {
        var cf: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(obj, kMIDIPropertyDisplayName, &cf)
        return (cf?.takeRetainedValue() as String?) ?? "?"
    }
}
