import AVFoundation
import QuartzCore
import Combine
import DJCore
import Foundation
import SwiftUI

/// Snapshot of one deck, copied from the engine ~30 times per second.
struct DeckView: Equatable {
    var loaded = false, playing = false, keylock = false, sync = false, master = false
    var quantize = true, loopActive = false
    var position = 0.0, duration = 0.0, trackBpm = 0.0, bpm = 0.0, rate = 1.0, range = 0.10
    var cue = 0.0, loopIn = -1.0, loopOut = -1.0
    var hotcues = [Double](repeating: -1, count: 8)
    var hotLoopEnd = [Double](repeating: 0, count: 8)
    var peak: Float = 0
    var tempo = 0.0
    var keyShift = 0, keySync = false
    var onAir: Float = 0     // fader x crossfader gain while playing
    var stamp = 0.0 // djc_now() of the engine snapshot, for smooth waveform scrolling
}

/// On-screen warning, modelled on the XDJ-AZ's messages (manual DRI1936C: Load Lock p58/p132,
/// error display E-8302 / E-8304 / E-8305 p154, Beat Sync needs rekordbox analysis p89, CLIP p105).
struct DJWarning: Identifiable, Equatable {
    let id = UUID()
    var code = ""       // e.g. "E-8302"
    var title: String
    var detail: String
}

struct TrackInfo: Identifiable, Equatable {
    let id = UUID()
    var url: URL
    var title: String
    var artist = ""
    var key = ""
    var source = "FILES"
    var rbId: UInt32 = 0
    var bpm: Double = 0
    var firstBeat: Double = 0
    var waves: [WaveKind: WaveData] = [:]
    var overviews: [WaveKind: WaveData] = [:]
    var duration: Double = 0

    /// The waveform to draw for the chosen colour mode, falling back to what the track has.
    func wave(_ want: WaveKind, overview: Bool = false) -> (WaveKind, WaveData)? {
        let src = overview ? overviews : waves
        for k in [want, .rgb, .threeBand, .blue] { if let w = src[k], w.count > 0 { return (k, w) } }
        return nil
    }
    var cues: [CueInfo] = []
    var phrases: [PhraseInfo] = []
    var mood = 0
    var fromRekordbox = false
    /// rekordbox beat grid: time (s) and beat-in-bar (1...4) of every beat
    var beatTimes: [Double] = [] { didSet { barCount = [] } }
    var beatInBar: [UInt8] = [] { didSet { barCount = [] } }
    private var barCount: [Int32] = []

    /// Number of downbeats up to and including beat i, minus one (bar index of beat i).
    mutating func prepareBars() {
        var c: Int32 = -1
        barCount = beatInBar.map { if $0 == 1 { c += 1 }; return c }
    }
    func barIndex(beforeOrAt i: Int) -> Int {
        if barCount.indices.contains(i) { return Int(barCount[i]) }
        var c = -1
        for k in 0...min(i, beatInBar.count - 1) where beatInBar[k] == 1 { c += 1 }
        return c
    }

    func hotColorRGB(_ i: Int) -> SIMD3<Float> {
        if let c = cues.first(where: { $0.hot == i + 1 })?.rgb {
            return SIMD3(Float(c.0) / 255, Float(c.1) / 255, Float(c.2) / 255)
        }
        return Color.hotcueRGB[i]
    }

    /// rekordbox comment of hot cue i, shown in the HOT CUE list
    func hotComment(_ i: Int) -> String { cues.first(where: { $0.hot == i + 1 })?.comment ?? "" }

    /// Colour of hot cue i (0-7) from rekordbox, else our default palette.
    func hotColor(_ i: Int) -> Color {
        if let c = cues.first(where: { $0.hot == i + 1 })?.rgb {
            return Color(red: Double(c.0) / 255, green: Double(c.1) / 255, blue: Double(c.2) / 255)
        }
        return Color.hotcueColors[i]
    }
}

struct FxView: Equatable {
    var name = "ECHO", type = 1, target = 4, on = false, beatIndex = 3
    var level: Float = 0.5
    var bpm = 120.0, beats = 1.0, timeMs = 500.0
    var bands = 7 // bit0 LOW, bit1 MID, bit2 HI
}

enum Routing: String, CaseIterable, Identifiable {
    case masterAndPhones = "Master 1-2, Phones 3-4"
    case masterOnly = "Master 1-2 only"
    case splitCue = "Split: master L, cue R"
    var id: String { rawValue }
}

// MARK: - Browser model

enum BrowseCategory: String, CaseIterable, Identifiable {
    case playlist = "PLAYLIST", track = "TRACK", artist = "ARTIST", album = "ALBUM", key = "KEY", files = "FILES"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .playlist: return "list.bullet"
        case .track: return "music.note"
        case .artist: return "person"
        case .album: return "opticaldisc"
        case .key: return "number"
        case .files: return "folder"
        }
    }
}

enum SortKey: String { case none, title, bpm, key }

enum BrowseItem: Identifiable, Hashable {
    case folder(RBPlaylist), playlist(RBPlaylist), group(String), track(RBTrack), file(Int)
    var id: String {
        switch self {
        case .folder(let p): return "f\(p.id)"
        case .playlist(let p): return "p\(p.id)"
        case .group(let s): return "g" + s
        case .track(let t): return "t\(t.id)"
        case .file(let i): return "x\(i)"
        }
    }
}

enum BrowseLevel: Hashable {
    case root
    case folder(RBPlaylist)
    case playlist(RBPlaylist)
    case group(String)
}

@MainActor
final class Engine: ObservableObject {
    static let sampleRate = 48_000.0

    @Published var decks = [DeckView](repeating: DeckView(), count: 4)
    @Published var deckTracks: [TrackInfo?] = [nil, nil, nil, nil]
    @Published var library: [TrackInfo] = [] // tracks added from the Files app
    @Published var loading: Set<Int> = []
    @Published var masterPeak: Float = 0
    /// Tracks played on the master this session (green in the browser), key = source + rekordbox id.
    @Published var played = Set<String>()
    static func playedKey<T>(source: String, id: T) -> String { "\(source)#\(id)" }
    @Published var masterL: Float = 0     // output level bars (fast attack, slow fall)
    @Published var masterR: Float = 0
    /// Master EQ in dB: low 100 Hz shelf, mid 1 kHz, high 10 kHz shelf (-12 ... +6).
    @Published var masterEq: [Double] = (UserDefaults.standard.array(forKey: "masterEq") as? [Double]) ?? [0, 0, 0] {
        didSet {
            UserDefaults.standard.set(masterEq, forKey: "masterEq")
            for b in 0..<3 where b < masterEq.count { post(DJC_MASTER_EQ, i: Int32(b), v: masterEq[b]) }
        }
    }
    @Published var cpu = 0.0
    @Published var xruns = 0
    @Published var outputChannels = 0
    @Published var routeDescription = ""
    @Published var status = ""
    @Published var warning: DJWarning?
    // set recorder
    @Published var recording = false
    @Published var recStart: Date?
    @Published var recordings: [RecordingFile] = []
    @Published var recFormat = SetRecorder.Format(rawValue: UserDefaults.standard.string(forKey: "recFormat") ?? "") ?? .wav {
        didSet { UserDefaults.standard.set(recFormat.rawValue, forKey: "recFormat") }
    }
    private var recorder: SetRecorder?
    private var lastSpaceCheck = Date.distantPast
    /// DUB ECHO delay in beats. The DJM manuals don't state it, so it is a setting, tuned by ear.
    @Published var dubBeats = UserDefaults.standard.object(forKey: "dubBeats") as? Double ?? 0.75 {
        didSet { UserDefaults.standard.set(dubBeats, forKey: "dubBeats"); post(DJC_DUB_BEATS, v: dubBeats) }
    }

    func toggleRecording() { recording ? stopRecording() : startRecording() }

    func startRecording() {
        guard !recording else { return }
        let free = SetRecorder.freeBytes()
        if free >= 0 && free < 1_000_000_000 {
            warn("STORAGE ALMOST FULL", String(format: "%.1f GB free on the iPad. Free up space before recording.", Double(free) / 1e9), seconds: 5)
            return
        }
        let r = recorder ?? SetRecorder(core: core, sampleRate: Engine.sampleRate)
        recorder = r
        do {
            let url = try r.start(format: recFormat)
            recording = true; recStart = Date()
            status = "Recording to \(url.lastPathComponent)"
        } catch {
            warn("CANNOT RECORD", error.localizedDescription, seconds: 5)
        }
    }

    func stopRecording() {
        guard recording, let r = recorder else { return }
        recording = false; recStart = nil
        r.stop { [weak self] secs, dropped, err in
            MainActor.assumeIsolated {
                guard let self else { return }
                if let err {
                    self.warn("RECORDING ERROR", err.localizedDescription, seconds: 6)
                } else if dropped > 0 {
                    self.warn("RECORDING HAS GAPS", String(format: "%.1f s were lost because the iPad was too busy writing.", Double(dropped) / Engine.sampleRate), seconds: 6)
                } else {
                    let m = Int(secs) / 60, s = Int(secs) % 60
                    self.status = "Recording saved (\(m):" + String(format: "%02d", s) + ")"
                }
                self.refreshRecordings()
            }
        }
    }

    func refreshRecordings() {
        let keys: [URLResourceKey] = [.creationDateKey, .fileSizeKey]
        let urls = (try? FileManager.default.contentsOfDirectory(at: SetRecorder.folder, includingPropertiesForKeys: keys)) ?? []
        recordings = urls.compactMap { u in
            let v = try? u.resourceValues(forKeys: Set(keys))
            return RecordingFile(url: u, date: v?.creationDate ?? .distantPast, bytes: Int64(v?.fileSize ?? 0))
        }
        .sorted { $0.date > $1.date }
    }

    func deleteRecording(_ r: RecordingFile) {
        try? FileManager.default.removeItem(at: r.url)
        refreshRecordings()
    }
    @Published var clipping = false
    private var clipUntil = 0.0
    /// XDJ-AZ Utility › Deck › Load Lock: can't load another track to a deck that is playing.
    @Published var loadLock = UserDefaults.standard.object(forKey: "loadLock") as? Bool ?? true {
        didSet { UserDefaults.standard.set(loadLock, forKey: "loadLock") }
    }

    func warn(_ title: String, _ detail: String, code: String = "", seconds: Double = 3.0) {
        let w = DJWarning(code: code, title: title, detail: detail)
        warning = w
        status = code.isEmpty ? title : "\(code) \(title)"
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
            if self?.warning?.id == w.id { self?.warning = nil }
        }
    }
    @Published var routing: Routing = .masterAndPhones { didSet { applyRouting() } }

    // mixer state (mirrors the GRV6 / engine)
    @Published var mixer = [MixerChannel](repeating: MixerChannel(), count: 4)
    @Published var crossfader = 0.5
    @Published var masterLevel = 0.8409
    @Published var phonesMix = 0.0
    @Published var phonesLevel = 1.0
    @Published var colorFxOn = true
    @Published var masterCue = false

    // controller + screen
    @Published var controllerName = ""
    @Published var lastUnmapped = ""
    @Published var leftDeck = 0
    @Published var rightDeck = 1
    @Published var showBrowser = false
    @Published var showSource = false
    @Published var fx = FxView()
    @Published var zoomSeconds = 8.0
    @Published var scratchSpeed = 0.85 {
        didSet { for d in 0..<4 { post(DJC_JOG_TICKS_PER_REV, deck: d, v: 720.0 / max(0.3, scratchSpeed)) } }
    }
    @Published var colorFxType = 0
    @Published var padModes = [0, 0, 0, 0] // pad mode per deck, from the GRV6
    /// 2 DECK / 4 DECK waveform screen (remembered)
    @Published var fourDeck = UserDefaults.standard.bool(forKey: "fourDeck") {
        didSet { UserDefaults.standard.set(fourDeck, forKey: "fourDeck") }
    }
    /// COLOR FX PARAMETER (intensity), A9-style; saved between sessions
    @Published var colorFxParam: Double = UserDefaults.standard.object(forKey: "cfxParam") as? Double ?? 0.5 {
        didSet { post(DJC_COLOR_FX_PARAM, v: colorFxParam); UserDefaults.standard.set(colorFxParam, forKey: "cfxParam") }
    }
    @Published var sortKey: SortKey = .none { didSet { refreshItems() } }
    @Published var sortAscending = true { didSet { refreshItems() } }
    @Published var nudgeGain = UserDefaults.standard.object(forKey: "nudge003") as? Double ?? 0.03 {
        didSet { UserDefaults.standard.set(nudgeGain, forKey: "nudge003"); for d in 0..<4 { post(DJC_JOG_NUDGE_GAIN, deck: d, v: nudgeGain) } }
    }
    /// Measure each rekordbox grid against the decoded audio at load and fix clear offsets (some MP3/AAC files).
    @Published var alignGrid = UserDefaults.standard.object(forKey: "alignGrid") as? Bool ?? true {
        didSet { UserDefaults.standard.set(alignGrid, forKey: "alignGrid"); TrackLoader.alignGrid = alignGrid }
    }
    /// Load a track parked at its first memory cue (rekordbox) instead of the start of the track.
    @Published var loadAtMemoryCue = UserDefaults.standard.object(forKey: "loadAtMemCue") as? Bool ?? false {
        didSet { UserDefaults.standard.set(loadAtMemoryCue, forKey: "loadAtMemCue") }
    }
    @Published var waveKind: WaveKind = WaveKind(rawValue: UserDefaults.standard.string(forKey: "waveKind") ?? "") ?? .rgb {
        didSet { UserDefaults.standard.set(waveKind.rawValue, forKey: "waveKind") }
    }
    private(set) var controller: Controller?

    // USB / browser
    @Published var usb: RekordboxUSB?
    @Published var category: BrowseCategory = .playlist { didSet { stack = [.root]; selection = 0; refreshItems() } }
    @Published var stack: [BrowseLevel] = [.root]
    @Published var items: [BrowseItem] = []
    @Published var selection = 0
    private var usbObserver: AnyCancellable?

    let core: OpaquePointer
    private var unit: UnsafeMutableRawPointer?
    private var timer: Timer?
    private var routeObserver: NSObjectProtocol?

    init() {
        core = djc_create(Engine.sampleRate, 4096)
        loadDrumSamples() // before audio starts: the engine reads these buffers on the audio thread
        startAudio()
        routeObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.restartAudio() }
        }
        let c = Controller(engine: core)
        c.onChange = { [weak self, weak c] in
            Task { @MainActor in
                guard let self, let c else { return }
                let wasEmpty = self.controllerName.isEmpty
                self.controllerName = c.sourceNames.joined(separator: ", ")
                if wasEmpty && c.connected { self.controllerArrived() }
            }
        }
        controller = c
        controllerName = c.sourceNames.joined(separator: ", ")
        if c.connected { controllerArrived() }
        startSourceScan()   // IPAD + every remembered stick that is plugged in; picks what to browse
        post(DJC_COLOR_FX_PARAM, v: colorFxParam)
        post(DJC_DUB_BEATS, v: dubBeats)
        for b in 0..<3 where b < masterEq.count { post(DJC_MASTER_EQ, i: Int32(b), v: masterEq[b]) }
        TrackLoader.alignGrid = alignGrid
        for d in 0..<4 {
            post(DJC_JOG_NUDGE_GAIN, deck: d, v: nudgeGain)
            post(DJC_NUDGE_SNAP, deck: d, i: 0)   // nudge snap removed (owner, 2026-11-06)
        }
        refreshRecordings()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    /// A GRV6 was just connected: channel faders start closed until the unit reports where they are,
    /// then we ask it for the current position of every fader and knob.
    private func controllerArrived() {
        for ch in 0..<4 { post(DJC_FADER, deck: ch, v: 0) }
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 300_000_000)
            self?.controller?.requestControlDump()
        }
    }

    /// GROOVE CIRCUIT drum sounds from the owner's packs: left side 909, right side 707.
    private func loadDrumSamples() {
        // the Samples folder is copied into the app bundle (directly, or inside a resource bundle)
        let fm = FileManager.default
        var roots: [URL] = (Bundle.allBundles + Bundle.allFrameworks).compactMap { $0.resourceURL }
        if let main = Bundle.main.resourceURL,
           let inner = try? fm.contentsOfDirectory(at: main, includingPropertiesForKeys: nil) {
            roots += inner.filter { $0.pathExtension == "bundle" }
        }
        guard let root = roots.map({ $0.appendingPathComponent("Samples") })
            .first(where: { fm.fileExists(atPath: $0.appendingPathComponent("909/kick.wav").path) }) else {
            warn("DRUM SAMPLES NOT FOUND", "The GROOVE CIRCUIT uses the built-in stand-in sounds.")
            return
        }
        let kits: [(Int32, String, [String])] = [(0, "909", ["kick", "snare", "clap", "hat"]),
                                                 (1, "707", ["kick", "snare", "clap", "crash"])]
        for (side, folder, names) in kits {
            for (slot, name) in names.enumerated() {
                let url = root.appendingPathComponent("\(folder)/\(name).wav")
                guard let lr = try? TrackLoader.decode(url: url, sampleRate: Engine.sampleRate), lr.0.count > 1 else {
                    status = "Missing drum sample \(folder)/\(name).wav"; continue
                }
                djc_drum_set_sample(core, side, Int32(slot), lr.0, lr.1, Int32(lr.0.count))
            }
        }
    }

    // MARK: audio output

    func startAudio() {
        let s = AVAudioSession.sharedInstance()
        do {
            try s.setCategory(.playback, mode: .default, options: [])
            try s.setPreferredSampleRate(Engine.sampleRate)
            try s.setPreferredIOBufferDuration(256.0 / Engine.sampleRate)
            try s.setActive(true)
            try? s.setPreferredOutputNumberOfChannels(s.maximumOutputNumberOfChannels)
        } catch {
            status = "Audio session error: \(error.localizedDescription)"
        }
        let ch = max(2, s.outputNumberOfChannels)
        var err: Int32 = 0
        unit = djc_io_start(core, Int32(ch), &err)
        outputChannels = ch
        routeDescription = s.currentRoute.outputs.map { "\($0.portName) (\($0.channels?.count ?? 0) ch)" }
            .joined(separator: ", ")
        if unit == nil { warn("AUDIO OUTPUT FAILED", "OSStatus \(err). Reconnect the controller.", seconds: 5) }
        applyRouting()
        // the GRV6's 4 channels can show up a moment after the session starts: look again shortly
        if ch < 4 {
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 700_000_000)
                guard let self else { return }
                let s2 = AVAudioSession.sharedInstance()
                try? s2.setPreferredOutputNumberOfChannels(s2.maximumOutputNumberOfChannels)
                if s2.outputNumberOfChannels >= 4, self.outputChannels < 4 { self.restartAudio() }
            }
        }
    }

    func restartAudio() {
        if let u = unit { djc_io_stop(u); unit = nil }
        startAudio()
    }

    /// The routing you picked stays your choice. With fewer than 4 outputs we fall back to master only for now,
    /// and go back to master + phones as soon as the GRV6's 4 channels are there.
    private func applyRouting() {
        let none: Int32 = 0xFF
        let effective: Routing = (outputChannels < 4 && routing == .masterAndPhones) ? .masterOnly : routing
        switch effective {
        case .masterAndPhones:
            post(DJC_ROUTING, i: 0 | (1 << 8) | (2 << 16) | (3 << 24))
            post(DJC_SPLIT_CUE, i: 0)
        case .masterOnly:
            post(DJC_ROUTING, i: 0 | (1 << 8) | (none << 16) | (none << 24))
            post(DJC_SPLIT_CUE, i: 0)
        case .splitCue:
            post(DJC_ROUTING, i: 0 | (1 << 8) | (none << 16) | (none << 24))
            post(DJC_SPLIT_CUE, i: 1)
        }
    }

    // MARK: commands

    func post(_ type: djc_cmd_type, deck: Int = 0, i: Int32 = 0, v: Double = 0, v2: Double = 0) {
        var c = djc_cmd(type: Int32(type.rawValue), deck: Int32(deck), i: i, v: v, v2: v2, ptr: nil)
        if djc_send(core, 0, &c) == 0 { status = "Command queue full" }
    }

    func setMixer(_ ch: Int, _ type: djc_cmd_type, _ value: Double) { post(type, deck: ch, v: value) }

    // MARK: polling

    private func poll() {
        djc_collect(core)
        var next = decks
        for d in 0..<4 {
            var s = djc_deck_state()
            djc_get_deck_state(core, Int32(d), &s)
            var v = next[d]
            let wasSync = v.sync
            v.loaded = s.loaded != 0; v.playing = s.playing != 0; v.keylock = s.keylock != 0
            v.sync = s.sync != 0; v.master = s.master != 0; v.quantize = s.quantize != 0
            v.loopActive = s.loopActive != 0
            v.position = s.position; v.duration = s.duration; v.trackBpm = s.trackBpm; v.bpm = s.bpm
            v.rate = s.rate; v.range = s.tempoRange; v.cue = s.cuePoint; v.loopIn = s.loopIn; v.loopOut = s.loopOut
            v.tempo = s.tempo
            v.keyShift = Int(s.keyShift); v.keySync = s.keySync != 0
            v.stamp = s.stamp
            v.hotcues = withUnsafeBytes(of: s.hotcues) { Array($0.bindMemory(to: Double.self)) }
            v.hotLoopEnd = withUnsafeBytes(of: s.hotLoopEnd) { Array($0.bindMemory(to: Double.self)) }
            v.peak = max(s.peakL, s.peakR, v.peak * 0.85)
            v.onAir = s.onAir
            next[d] = v
            if s.playing != 0, s.onAir > 0.05, let t = deckTracks[d], t.rbId > 0 {
                let k = Engine.playedKey(source: t.source, id: t.rbId)
                if !played.contains(k) { played.insert(k) }
            }
            if v.sync && !wasSync, v.loaded, deckTracks[d]?.fromRekordbox == false {
                warn("NOT ANALYZED BY REKORDBOX", "Deck \(d + 1): Beat Sync follows the app's own estimated beat grid.")
            }
        }
        decks = next
        var fs = djc_fx_state()
        djc_get_fx_state(core, &fs)
        let nf = FxView(name: String(cString: djc_fx_name(fs.type)), type: Int(fs.type), target: Int(fs.target),
                        on: fs.on != 0, beatIndex: Int(fs.beatIndex), level: fs.level, bpm: fs.bpm,
                        beats: fs.beats, timeMs: fs.timeMs, bands: Int(fs.bands))
        if nf != fx { fx = nf }
        var mx = djc_mixer_state()
        djc_get_mixer_state(core, &mx)
        func f<T>(_ t: T) -> [Float] { withUnsafeBytes(of: t) { Array($0.bindMemory(to: Float.self)) } }
        func i32<T>(_ t: T) -> [Int32] { withUnsafeBytes(of: t) { Array($0.bindMemory(to: Int32.self)) } }
        let trims = f(mx.trim), his = f(mx.hi), mids = f(mx.mid), lows = f(mx.low), cols = f(mx.color), fads = f(mx.fader)
        let xfs = i32(mx.xf), cues = i32(mx.cue)
        var newMixer = mixer
        for c in 0..<4 {
            newMixer[c] = MixerChannel(trim: Double(trims[c]), hi: Double(his[c]), mid: Double(mids[c]), low: Double(lows[c]),
                                       color: Double(cols[c]), fader: Double(fads[c]), xf: Int(xfs[c]), cue: cues[c] != 0)
        }
        if newMixer != mixer { mixer = newMixer }
        if abs(Double(mx.crossfader) - crossfader) > 1e-4 { crossfader = Double(mx.crossfader) }
        if abs(Double(mx.phonesMix) - phonesMix) > 1e-4 { phonesMix = Double(mx.phonesMix) }
        if (mx.colorFxOn != 0) != colorFxOn { colorFxOn = mx.colorFxOn != 0 }
        if Int(mx.colorFxType) != colorFxType { colorFxType = Int(mx.colorFxType) }
        if (mx.masterCue != 0) != masterCue { masterCue = mx.masterCue != 0 }

        if let c = controller {
            var ev = djc_ui_event()
            while djc_midi_poll_event(c.midi, &ev) != 0 { handle(ev) }
            c.flushLEDs()
            let pm = (0..<4).map { Int(djc_midi_pad_mode(c.midi, Int32($0))) }
            if pm != padModes { padModes = pm }
        }
        var m = djc_master_state()
        djc_get_master_state(core, &m)
        masterPeak = max(m.masterPeakL, m.masterPeakR, masterPeak * 0.85)
        masterL = max(m.masterPeakL, masterL * 0.82); masterR = max(m.masterPeakR, masterR * 0.82)
        // CLIP indicator (XDJ-AZ blinks it on excessive master output): -0.5 dB, held 1 s
        let now = djc_now()
        if max(m.masterPeakL, m.masterPeakR) >= 0.944 { clipUntil = now + 1.0 }
        if (now < clipUntil) != clipping { clipping = now < clipUntil }
        // long sets: stop cleanly before the iPad runs out of space (checked every 30 s)
        if recording, Date().timeIntervalSince(lastSpaceCheck) > 30 {
            lastSpaceCheck = Date()
            let free = SetRecorder.freeBytes()
            if free >= 0 && free < 500_000_000 {
                stopRecording()
                warn("STORAGE FULL", "Recording stopped and saved. Less than 0.5 GB left on the iPad.", seconds: 8)
            }
        }
        cpu = cpu * 0.9 + m.cpuLoad * 0.1
        xruns = Int(m.xruns)
    }

    /// KEY SYNC on: transpose this deck to the sync master's key (or the other playing deck). Off: back to 0.
    /// KEY SYNC (owner's rules, 2026-11-06):
    ///  - it always retunes the QUEUED deck (loaded, not live) to the live one, never a deck that is live on its own
    ///  - pressed on the live deck: retunes the queued deck; no queued track = nothing happens
    ///  - pressed on the queued deck: retunes that deck to the live one
    ///  - both decks live: retunes the deck you pressed
    ///  - harmonic, like the green keys in the browser: already compatible (same key, +-1 on the wheel,
    ///    relative minor/major) = no change; otherwise the smallest shift that makes it compatible, max +-3 st
    /// "Live" = playing and heard on the master (fader x crossfader up).
    func keySyncPressed(deck: Int) {
        let st = decks
        func live(_ i: Int) -> Bool { st[i].loaded && st[i].playing && st[i].onAir > 0.05 }
        func hasKey(_ i: Int) -> Bool { st[i].loaded && !(deckTracks[i]?.key.isEmpty ?? true) }
        let liveDecks = (0..<4).filter { live($0) && hasKey($0) }
        var target = deck, ref: Int?
        if live(deck) {
            if let other = liveDecks.filter({ $0 != deck }).max(by: { st[$0].onAir < st[$1].onAir }) {
                ref = other                                                 // both live: the pressed deck follows
            } else {
                // the pressed deck is live alone: retune the queued deck instead (prefer the opposite side, 1<->2, 3<->4)
                let queued = (0..<4).filter { $0 != deck && hasKey($0) && !live($0) }
                guard let q = queued.first(where: { $0 == (deck ^ 1) }) ?? queued.first else {
                    status = "KEY SYNC: no queued track"; return
                }
                target = q; ref = deck
            }
        } else {
            ref = liveDecks.max(by: { st[$0].onAir < st[$1].onAir })
                ?? (0..<4).first(where: { $0 != deck && hasKey($0) && st[$0].playing })
        }
        guard let ref, hasKey(target), let tKey = deckTracks[target]?.key, let rKey = deckTracks[ref]?.key else {
            status = "KEY SYNC: nothing to match"; return
        }
        // already on: second press turns it off and puts the queued track back to its own key
        if st[target].keySync {
            post(DJC_KEY_SYNC, deck: target, i: 0)
            post(DJC_KEY_SHIFT, deck: target, i: 100)
            status = "KEY SYNC off, deck \(target + 1)"
            return
        }
        func rateSemis(_ i: Int) -> Int { st[i].keylock ? 0 : Int((12 * log2(max(0.01, st[i].rate))).rounded()) }
        let refOffset = st[ref].keyShift + rateSemis(ref)
        let candidates = [0, 1, -1, 2, -2, 3, -3]   // smallest change first
        guard let s = candidates.first(where: { MusicalKey.compatible(rKey, tKey, shiftB: $0 + rateSemis(target) - refOffset) }) else {
            warn("KEY SYNC", "Deck \(target + 1) (\(tKey)) can't be made compatible with \(rKey) within 3 semitones.")
            return
        }
        post(DJC_KEY_SYNC, deck: target, i: 1)
        post(DJC_KEY_SHIFT, deck: target, v: Double(s))
        status = s == 0 ? "KEY SYNC deck \(target + 1): \(tKey) already fits \(rKey)"
                        : "KEY SYNC deck \(target + 1): \(s > 0 ? "+" : "")\(s) st to fit \(rKey)"
    }

    private func handle(_ ev: djc_ui_event) {
        switch Int(ev.kind) {
        case Int(DJC_EV_BROWSE_TURN) where showSource:      // SOURCE: knob moves the highlight
            if !sources.isEmpty { sourceIndex = max(0, min(sources.count - 1, sourceIndex + (ev.value > 0 ? 1 : -1))) }
        case Int(DJC_EV_BROWSE_PRESS) where showSource:
            if sources.indices.contains(sourceIndex) { openSource(sources[sourceIndex]) }
        case Int(DJC_EV_BROWSE_BACK) where showSource:
            showSource = false
        case Int(DJC_EV_BROWSE_TURN):
            if !showBrowser { // waveform screen: the knob zooms the waveforms
                stepZoom(ev.value > 0 ? 1 : -1)
                return
            }
            guard !items.isEmpty else { return }
            selection = min(max(0, selection + Int(ev.value)), items.count - 1)
        case Int(DJC_EV_BROWSE_PRESS):
            if showBrowser { enterSelection() }
        case Int(DJC_EV_BROWSE_BACK):
            guard showBrowser else { return }
            if ev.value == 2 { stack = [.root]; selection = 0; refreshItems() }   // SHIFT+BACK: top level
            else if stack.count > 1 { back() } else { showBrowser = false }      // BACK at top: waveform screen
        case Int(DJC_EV_VIEW):
            if showSource { showSource = false; showBrowser = true } else { showBrowser.toggle() }
        case Int(DJC_EV_SOURCE):
            showSource.toggle()
            if showSource, let i = sources.firstIndex(where: { $0 === usb }) { sourceIndex = i }
        case Int(DJC_EV_KEY_SYNC):
            keySyncPressed(deck: Int(ev.deck))
        case Int(DJC_EV_TILT):
            switch ev.value {
            case 1, 2: // forward / backward: page up / down
                guard showBrowser, !items.isEmpty else { return }
                selection = min(max(0, selection + (ev.value == 1 ? -10 : 10)), items.count - 1)
            case 3: // left: previous category
                let all = BrowseCategory.allCases, i = all.firstIndex(of: category) ?? 0
                if showBrowser { category = all[(i + all.count - 1) % all.count] }
            case 4: // right: next category
                let all = BrowseCategory.allCases, i = all.firstIndex(of: category) ?? 0
                if showBrowser { category = all[(i + 1) % all.count] }
            default: break
            }
        case Int(DJC_EV_LOAD):
            loadSelection(to: Int(ev.deck))
        case Int(DJC_EV_DECK_LAYER):
            guard ev.value == 1 else { return }
            if ev.deck % 2 == 0 { leftDeck = Int(ev.deck) } else { rightDeck = Int(ev.deck) }
        case Int(DJC_EV_UNMAPPED):
            lastUnmapped = String(format: "%02X %02X %02X", ev.raw.0, ev.raw.1, ev.raw.2)
        default:
            break
        }
    }

    /// Green key highlight in the browser: compatible with the sync master deck (else the left deck).
    func keyCompatible(_ key: String) -> Bool {
        guard !key.isEmpty else { return false }
        let ref = (0..<4).first(where: { decks[$0].master && deckTracks[$0] != nil })
            ?? (deckTracks[leftDeck] != nil ? leftDeck : nil)
        guard let r = ref, let rk = deckTracks[r]?.key, !rk.isEmpty else { return false }
        return MusicalKey.compatible(key, rk, shiftB: decks[r].keyShift)
    }

    // MARK: USB source

    /// A stick picked in the Files picker (needed once per stick: iPadOS only lets an app read a drive the user
    /// picked). It is remembered and from then on shows up in SOURCE by itself whenever it is plugged in.
    func attachUSB(_ url: URL, remember: Bool = true, label: String = "USB1") {
        if remember { RekordboxUSB.saveBookmark(url) }
        scanSources()
        if let lib = sources.first(where: { $0.root.standardizedFileURL.path == url.standardizedFileURL.path }) { select(lib) }
    }

    // MARK: SOURCE (XDJ-AZ style): IPAD internal library + plugged-in sticks

    @Published var sources: [RekordboxUSB] = []
    @Published var sourceIndex = 0          // highlighted row on the SOURCE screen

    /// Browse a source from the SOURCE screen.
    func openSource(_ lib: RekordboxUSB) {
        select(lib)
        showSource = false
        showBrowser = true
    }
    private var sourceTimer: Timer?

    func startSourceScan() {
        scanSources()
        let wantIPad = UserDefaults.standard.bool(forKey: "useIPadLibrary")
        if let pick = sources.first(where: { wantIPad ? $0.label == "IPAD" : $0.label != "IPAD" }) ?? sources.first { select(pick) }
        sourceTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.scanSources() }
        }
    }

    /// Every 2 s: which remembered sticks are plugged in. New ones are opened (USB1, USB2, ...), pulled ones dropped.
    func scanSources() {
        let fm = FileManager.default
        var next: [RekordboxUSB] = []
        if Engine.hasIPadLibrary {
            let r = Engine.iPadLibraryRoot
            if let lib = sources.first(where: { $0.label == "IPAD" }) { next.append(lib) }
            else { let lib = RekordboxUSB(root: r, label: "IPAD"); lib.open(); next.append(lib) }
        }
        var used = Set<String>()
        var fresh: [URL] = []
        for url in RekordboxUSB.savedRoots() {
            if let lib = sources.first(where: { $0.label != "IPAD" && $0.root.path == url.path }) {
                if fm.fileExists(atPath: lib.pdbPath) || fm.fileExists(atPath: lib.root.path) {
                    next.append(lib); used.insert(lib.label)
                }
            } else {
                let ok = url.startAccessingSecurityScopedResource()
                let here = fm.fileExists(atPath: url.path)
                if ok { url.stopAccessingSecurityScopedResource() }
                if here { fresh.append(url) }
            }
        }
        for url in fresh {
            let label = (1...8).map { "USB\($0)" }.first { !used.contains($0) } ?? "USB"
            used.insert(label)
            let lib = RekordboxUSB(root: url, label: label)
            lib.open()
            next.append(lib)
        }
        next.sort { a, b in a.label == "IPAD" || (b.label != "IPAD" && a.label < b.label) }
        let gone = sources.filter { s in !next.contains { $0 === s } }
        if gone.map(\.id) != [] || next.map(\.id) != sources.map(\.id) { sources = next }
        if let u = usb, gone.contains(where: { $0 === u }) {
            usb = nil; usbObserver = nil; refreshItems()
            if u.label != "IPAD" { warn("\(u.label) REMOVED", "Tracks already on the decks keep playing.") }
        }
        if usb == nil, let first = next.first(where: { $0.label != "IPAD" }) ?? next.first, !fresh.isEmpty || gone.isEmpty == false {
            select(first)
        }
    }

    /// Browse this source (tap in SOURCE).
    func select(_ lib: RekordboxUSB) {
        if usb === lib { return }
        usb = lib
        UserDefaults.standard.set(lib.label == "IPAD", forKey: "useIPadLibrary")
        usbObserver = lib.objectWillChange.sink { [weak self] in
            DispatchQueue.main.async { self?.refreshItems() }
        }
        stack = [.root]
        category = .playlist
        refreshItems()
    }

    // MARK: library copied onto the iPad (same layout as the stick: PIONEER/… + Contents/…)

    @Published var copyProgress: Double?
    @Published var copyStatus = ""
    static var iPadLibraryRoot: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("iPad Library")
    }
    static var hasIPadLibrary: Bool {
        FileManager.default.fileExists(atPath: iPadLibraryRoot.appendingPathComponent("PIONEER/rekordbox/export.pdb").path)
    }

    func useIPadLibrary() {
        guard Engine.hasIPadLibrary else { return }
        scanSources()
        if let lib = sources.first(where: { $0.label == "IPAD" }) { select(lib) }
    }

    /// Copies the connected stick's rekordbox export onto the iPad: the audio files the library uses, then the
    /// PIONEER folder (database, analysis, artwork) last, so an interrupted copy keeps the previous library usable.
    /// Files already on the iPad with the same size are skipped, so re-copying after a new export is quick.
    /// Only reads from the stick.
    func copyStickToIPad(_ from: RekordboxUSB? = nil) {
        guard copyProgress == nil, let usb = from ?? usb, usb.label != "IPAD", usb.ready, usb.formats.contains(.deviceLibrary) else { return }
        let src = usb.root, dst = Engine.iPadLibraryRoot
        let audio = Array(Set(usb.tracks.map(\.filePath).filter { !$0.isEmpty }))
        copyProgress = 0; copyStatus = "Listing files…"
        Task.detached(priority: .utility) { [weak self] in
            let fm = FileManager.default
            func rel(_ p: String) -> String { p.hasPrefix("/") ? String(p.dropFirst()) : p }
            var files: [(String, Int64)] = []
            for p in audio {
                let r = rel(p)
                let size = (try? fm.attributesOfItem(atPath: src.appendingPathComponent(r).path)[.size] as? NSNumber)?.int64Value ?? -1
                if size >= 0 { files.append((r, size)) }
            }
            let pioneer = src.appendingPathComponent("PIONEER")
            if let en = fm.enumerator(at: pioneer, includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey]) {
                for case let u as URL in en {
                    let v = try? u.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                    guard v?.isRegularFile == true else { continue }
                    let r = "PIONEER" + u.path.dropFirst(pioneer.path.count)
                    files.append((r, Int64(v?.fileSize ?? 0)))
                }
            }
            var need: Int64 = 0
            for (r, size) in files {
                let have = (try? fm.attributesOfItem(atPath: dst.appendingPathComponent(r).path)[.size] as? NSNumber)?.int64Value ?? -1
                if have != size { need += size }
            }
            let free = SetRecorder.freeBytes()
            if free >= 0 && need + 1_000_000_000 > free {
                await MainActor.run {
                    self?.copyProgress = nil; self?.copyStatus = ""
                    self?.warn("NOT ENOUGH SPACE", String(format: "The library needs %.1f GB, the iPad has %.1f GB free (1 GB kept spare).",
                                                          Double(need) / 1e9, Double(free) / 1e9), seconds: 6)
                }
                return
            }
            let total = Double(max(1, files.reduce(0) { $0 + $1.1 }))
            var done: Int64 = 0, failed = 0, lastShown = -1.0
            for (r, size) in files {
                let s = src.appendingPathComponent(r), d = dst.appendingPathComponent(r)
                let have = (try? fm.attributesOfItem(atPath: d.path)[.size] as? NSNumber)?.int64Value ?? -1
                if have != size {
                    do {
                        try fm.createDirectory(at: d.deletingLastPathComponent(), withIntermediateDirectories: true)
                        if have >= 0 { try fm.removeItem(at: d) }
                        try fm.copyItem(at: s, to: d)
                    } catch { failed += 1 }
                }
                done += size
                let p = Double(done) / total
                if p - lastShown > 0.005 {
                    lastShown = p
                    await MainActor.run { self?.copyProgress = p; self?.copyStatus = String(format: "Copying… %.0f%%", p * 100) }
                }
            }
            await MainActor.run {
                guard let self else { return }
                self.copyProgress = nil
                // re-read the iPad copy so SOURCE shows the new counts
                let wasActive = self.usb?.label == "IPAD"
                self.sources.removeAll { $0.label == "IPAD" }
                if wasActive { self.usb = nil }
                self.scanSources()
                if wasActive, let lib = self.sources.first(where: { $0.label == "IPAD" }) { self.select(lib) }
                self.copyStatus = failed == 0 ? "Copied \(audio.count) tracks to the iPad" : "Copied, \(failed) files failed"
                if failed > 0 { self.warn("COPY INCOMPLETE", "\(failed) files could not be copied. Try again with the stick connected.", seconds: 6) }
            }
        }
    }

    func deleteIPadLibrary() {
        if usb?.label == "IPAD" { usb = nil; usbObserver = nil; refreshItems() }
        UserDefaults.standard.set(false, forKey: "useIPadLibrary")
        try? FileManager.default.removeItem(at: Engine.iPadLibraryRoot)
        sources.removeAll { $0.label == "IPAD" }
        scanSources()
        copyStatus = "Removed from the iPad"
    }

    func ejectUSB() {
        if usb?.label == "IPAD" { UserDefaults.standard.set(false, forKey: "useIPadLibrary") }
        else { UserDefaults.standard.removeObject(forKey: RekordboxUSB.bookmarkKey) }
        usb = nil
        usbObserver = nil
        refreshItems()
    }

    // MARK: browser

    var levelTitle: String {
        switch stack.last ?? .root {
        case .root: return category.rawValue
        case .folder(let p), .playlist(let p): return p.name
        case .group(let s): return s
        }
    }

    func refreshItems() {
        let level = stack.last ?? .root
        var out: [BrowseItem] = []
        if category == .files {
            out = library.indices.map { .file($0) }
        } else if let usb {
            switch (category, level) {
            case (.playlist, .root):
                out = usb.children(of: 0).map { $0.isFolder ? .folder($0) : .playlist($0) }
            case (.playlist, .folder(let p)):
                out = usb.children(of: p.id).map { $0.isFolder ? .folder($0) : .playlist($0) }
            case (_, .playlist(let p)):
                out = usb.tracks(inPlaylist: p.id).map { .track($0) }
            case (.track, _):
                out = usb.tracks.map { .track($0) }
            case (.artist, .root):
                out = Set(usb.tracks.map { $0.artist.isEmpty ? "(no artist)" : $0.artist }).sorted().map { .group($0) }
            case (.album, .root):
                out = Set(usb.tracks.map { $0.album.isEmpty ? "(no album)" : $0.album }).sorted().map { .group($0) }
            case (.key, .root):
                out = Set(usb.tracks.map { $0.key.isEmpty ? "(no key)" : $0.key }).sorted().map { .group($0) }
            case (.artist, .group(let g)):
                out = usb.tracks.filter { ($0.artist.isEmpty ? "(no artist)" : $0.artist) == g }.map { .track($0) }
            case (.album, .group(let g)):
                out = usb.tracks.filter { ($0.album.isEmpty ? "(no album)" : $0.album) == g }.map { .track($0) }
            case (.key, .group(let g)):
                out = usb.tracks.filter { ($0.key.isEmpty ? "(no key)" : $0.key) == g }
                    .sorted { $0.bpm < $1.bpm }.map { .track($0) }
            default:
                out = []
            }
        }
        if sortKey != .none, out.allSatisfy({ if case .track = $0 { return true }; return false }) {
            let tracks = out.compactMap { i -> RBTrack? in if case .track(let t) = i { return t }; return nil }
            let sorted: [RBTrack]
            switch sortKey {
            case .title: sorted = tracks.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
            case .bpm: sorted = tracks.sorted { $0.bpm < $1.bpm }
            case .key: sorted = tracks.sorted {
                let a = MusicalKey.camelot($0.key), b = MusicalKey.camelot($1.key)
                return (a?.0 ?? 99, a?.1 == true ? 0 : 1) < (b?.0 ?? 99, b?.1 == true ? 0 : 1)
            }
            case .none: sorted = tracks
            }
            out = (sortAscending ? sorted : sorted.reversed()).map { .track($0) }
        }
        items = out
        if selection >= items.count { selection = max(0, items.count - 1) }
    }

    /// Header tap: first tap sorts ascending, second tap flips the order.
    func toggleSort(_ k: SortKey) {
        if sortKey == k { sortAscending.toggle() } else { sortKey = k; sortAscending = true }
    }

    /// Waveform zoom steps (seconds shown across a lane), like the hardware's zoom levels.
    static let zoomSteps: [Double] = [2, 4, 6, 8, 12, 16, 24, 32]
    func stepZoom(_ dir: Int) {
        let steps = Engine.zoomSteps
        let i = steps.firstIndex(where: { $0 >= zoomSeconds - 0.01 }) ?? 3
        zoomSeconds = steps[min(max(0, i - dir), steps.count - 1)] // turn right = zoom in
    }

    func enterSelection() {
        guard items.indices.contains(selection) else { return }
        switch items[selection] {
        case .folder(let p): stack.append(.folder(p))
        case .playlist(let p): stack.append(.playlist(p))
        case .group(let g): stack.append(.group(g))
        default: return
        }
        selection = 0
        refreshItems()
    }

    func back() {
        guard stack.count > 1 else { return }
        stack.removeLast()
        selection = 0
        refreshItems()
    }

    /// Load Lock: the deck is playing, so the load is refused (warning shown) and the browser stays open.
    func loadLocked(_ deck: Int) -> Bool {
        guard loadLock, decks.indices.contains(deck), decks[deck].playing, decks[deck].loaded else { return false }
        warn("DECK \(deck + 1) IS PLAYING", "Load Lock is on. Pause deck \(deck + 1) to load another track.")
        return true
    }

    func loadSelection(to deck: Int) {
        guard items.indices.contains(selection) else { return }
        if loadLocked(deck) { return }   // stay on the browser
        switch items[selection] {
        case .track(let t): loadUSB(t, deck: deck); showBrowser = false
        case .file(let i): if library.indices.contains(i) { load(library[i], deck: deck); showBrowser = false }
        default: break
        }
    }

    func importFiles(_ urls: [URL]) {
        for u in urls where !library.contains(where: { $0.url == u }) {
            library.append(TrackInfo(url: u, title: u.deletingPathExtension().lastPathComponent))
        }
        if category == .files { refreshItems() }
    }

    func loadUSB(_ t: RBTrack, deck: Int) {
        guard let usb else { return }
        var info = TrackInfo(url: usb.url(t.filePath), title: t.title)
        info.artist = t.artist
        info.key = t.key
        info.source = usb.label
        info.rbId = t.id
        load(info, deck: deck, anlzPath: t.analyzePath.isEmpty ? nil : usb.path(t.analyzePath), pdbBpm: t.bpm)
    }

    func load(_ info: TrackInfo, deck: Int, anlzPath: String? = nil, pdbBpm: Double = 0) {
        guard !loading.contains(deck) else { return }
        if loadLock && decks[deck].playing && decks[deck].loaded {
            warn("DECK \(deck + 1) IS PLAYING", "Load Lock is on. Pause deck \(deck + 1) to load another track.")
            return
        }
        loading.insert(deck)
        status = "Loading \(info.title) to deck \(deck + 1)…"
        let url = info.url
        Task.detached(priority: .userInitiated) {
            let result = TrackLoader.load(url: url, sampleRate: Engine.sampleRate, anlzPath: anlzPath, pdbBpm: pdbBpm)
            await MainActor.run {
                self.loading.remove(deck)
                switch result {
                case .failure(let e):
                    // XDJ-AZ error display: E-8304/E-8305 unsupported format, E-8302 can't read the file
                    let unsupported: Bool = {
                        if let le = e as? TrackLoader.LoadError, case .format = le { return true }
                        let ns = e as NSError
                        return ns.domain == "com.apple.coreaudio.avfaudio" || ns.code == 1718449215 // 'fmt?'
                    }()
                    if unsupported {
                        self.warn("UNSUPPORTED FILE FORMAT", "\(info.title)\nThis audio format can't be played.", code: "E-8304", seconds: 5)
                    } else {
                        self.warn("CANNOT PLAY TRACK", "\(info.title)\n\(e.localizedDescription)", code: "E-8302", seconds: 5)
                    }
                case .success(let t):
                    var c = djc_cmd(type: Int32(DJC_LOAD.rawValue), deck: Int32(deck), i: 0, v: 0, v2: 0,
                                    ptr: UnsafeMutableRawPointer(t.track))
                    if djc_send(self.core, 0, &c) == 0 { djc_track_destroy(t.track); return }
                    if t.gridShift != 0 {
                        self.status = String(format: "Deck %d: rekordbox grid moved %+.0f ms to match the audio", deck + 1, t.gridShift * 1000)
                    }
                    // rekordbox cues: first memory cue = CUE point, all memory cues for CALL, hot cues A-H
                    let mem = t.cues.filter { $0.hot == 0 }.sorted { $0.time < $1.time }
                    // auto cue (owner): the first beat / first sound, not the silence before it; memory cues stay on CALL
                    if self.loadAtMemoryCue, let first = mem.first { self.post(DJC_SET_CUE, deck: deck, v: first.time) }
                    else { self.post(DJC_SET_CUE, deck: deck, v: t.autoCue) }
                    for (k, m) in mem.prefix(32).enumerated() {
                        self.post(DJC_SET_MEMCUE, deck: deck, i: Int32(k) | (m.active ? 0x100 : 0), v: m.time, v2: m.loopEnd)
                    }
                    for h in t.cues where (1...8).contains(h.hot) {
                        self.post(DJC_SET_HOTCUE, deck: deck, i: Int32(h.hot - 1), v: h.time, v2: h.loopEnd)
                    }
                    var i = info
                    i.bpm = t.bpm; i.firstBeat = t.firstBeat; i.overviews = t.overviews
                    i.waves = t.waves; i.duration = t.duration
                    i.beatTimes = t.beatTimes; i.beatInBar = t.beatInBar; i.prepareBars()
                    i.cues = t.cues; i.phrases = t.phrases; i.mood = t.mood; i.fromRekordbox = t.fromRekordbox
                    self.deckTracks[deck] = i
                    let grid = t.fromRekordbox ? "rekordbox grid" : "estimated grid"
                    self.status = String(format: "Deck %d: %@ (%.2f BPM, %@, %d cues, %.1f s)",
                                         deck + 1, info.title, t.bpm, grid, t.cues.count, t.seconds)
                }
            }
        }
    }

    func setGrid(_ deck: Int, bpm: Double, firstBeat: Double) {
        post(DJC_SET_GRID, deck: deck, v: bpm, v2: firstBeat)
        deckTracks[deck]?.bpm = bpm
        deckTracks[deck]?.firstBeat = firstBeat
    }
}

struct MixerChannel: Equatable {
    var trim = 0.5, hi = 0.5, mid = 0.5, low = 0.5, color = 0.5, fader = 1.0
    var xf = 1, cue = false
}
