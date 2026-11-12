import CoreMIDI
import DJCore
import Foundation

/// CoreMIDI glue for the DDJ-GRV6. Decoding and LED logic live in DJCore/midi.cpp.
/// The input block runs on CoreMIDI's thread and calls straight into C (queue 1 of the engine).
final class Controller {
    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private(set) var destinations: [MIDIEndpointRef] = []
    private(set) var sourceNames: [String] = []
    let midi: OpaquePointer
    var onChange: (() -> Void)?

    init(engine: OpaquePointer) {
        midi = djc_midi_create(engine)
        MIDIClientCreateWithBlock("GRV6DJ" as CFString, &client) { [weak self] n in
            let id = n.pointee.messageID
            if id == .msgSetupChanged || id == .msgObjectAdded || id == .msgObjectRemoved {
                DispatchQueue.main.async { self?.connect() }
            }
        }
        let m = midi
        MIDIInputPortCreateWithProtocol(client, "in" as CFString, ._1_0, &inPort) { list, _ in
            for packet in list.unsafeSequence() {
                let count = Int(packet.pointee.wordCount)
                withUnsafeBytes(of: packet.pointee.words) { raw in
                    let words = raw.bindMemory(to: UInt32.self)
                    for k in 0..<min(count, 64) {
                        let w = words[k]
                        guard (w >> 28) == 0x2 else { continue } // MIDI 1.0 channel voice
                        var bytes: (UInt8, UInt8, UInt8) = (UInt8((w >> 16) & 0xFF), UInt8((w >> 8) & 0xFF), UInt8(w & 0xFF))
                        withUnsafeBytes(of: &bytes) { b in
                            djc_midi_in(m, b.bindMemory(to: UInt8.self).baseAddress, 3)
                        }
                    }
                }
            }
        }
        MIDIOutputPortCreate(client, "out" as CFString, &outPort)
        connect()
    }

    deinit { djc_midi_destroy(midi) }

    var connected: Bool { !sourceNames.isEmpty }

    /// Connect every source whose name mentions the GRV6, and remember matching destinations.
    func connect() {
        var names: [String] = []
        for i in 0..<MIDIGetNumberOfSources() {
            let src = MIDIGetSource(i)
            let name = Self.name(src)
            guard name.localizedCaseInsensitiveContains("GRV6") else { continue }
            MIDIPortConnectSource(inPort, src, nil)
            names.append(name)
        }
        var dests: [MIDIEndpointRef] = []
        for i in 0..<MIDIGetNumberOfDestinations() {
            let d = MIDIGetDestination(i)
            if Self.name(d).localizedCaseInsensitiveContains("GRV6") { dests.append(d) }
        }
        sourceNames = names
        destinations = dests
        djc_midi_reset_leds(midi) // resend every LED to a freshly connected unit
        onChange?()
    }

    /// Called ~30x per second from the UI thread: sends LED/meter changes.
    func flushLEDs() {
        guard !destinations.isEmpty else { return }
        var buf = [UInt8](repeating: 0, count: 3 * 512)
        let n = Int(djc_midi_out(midi, &buf, Int32(buf.count)))
        guard n > 0 else { return }
        var list = MIDIEventList()
        withUnsafeMutablePointer(to: &list) { lp in
            var packet: UnsafeMutablePointer<MIDIEventPacket> = MIDIEventListInit(lp, ._1_0)
            let listSize = MemoryLayout<MIDIEventList>.size
            var k = 0
            while k + 2 < n {
                var word: UInt32 = 0x2000_0000 | UInt32(buf[k]) << 16 | UInt32(buf[k + 1]) << 8 | UInt32(buf[k + 2])
                let next: UnsafeMutablePointer<MIDIEventPacket>? = MIDIEventListAdd(lp, listSize, packet, 0, 1, &word)
                if let next {
                    packet = next
                } else {
                    // list full: send what we have and start a new one
                    for d in destinations { MIDISendEventList(outPort, d, lp) }
                    packet = MIDIEventListInit(lp, ._1_0)
                    let retry: UnsafeMutablePointer<MIDIEventPacket>? = MIDIEventListAdd(lp, listSize, packet, 0, 1, &word)
                    if let retry { packet = retry }
                }
                k += 3
            }
            for d in destinations { MIDISendEventList(outPort, d, lp) }
        }
    }

    /// Ask the unit to send the current value of every fader and knob. SysEx taken from the public Mixxx
    /// DDJ-GRV6 mapping (joergbeyer/dj-controller, init()). Harmless if the unit ignores it.
    func requestControlDump() {
        let msg: [UInt8] = [0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x02, 0x06, 0x00, 0x03, 0x01, 0xF7]
        var list = MIDIPacketList()
        withUnsafeMutablePointer(to: &list) { lp in
            let pkt = MIDIPacketListInit(lp)
            _ = MIDIPacketListAdd(lp, MemoryLayout<MIDIPacketList>.size, pkt, 0, msg.count, msg)
            for d in destinations { MIDISend(outPort, d, lp) }
        }
    }

    static func name(_ obj: MIDIObjectRef) -> String {
        var cf: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(obj, kMIDIPropertyDisplayName, &cf)
        return (cf?.takeRetainedValue() as String?) ?? ""
    }
}
