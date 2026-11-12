import Accelerate
import AVFoundation
import DJCore
import Foundation

/// Bins per second of the enlarged (scrolling) waveform. Same rate rekordbox uses for PWV5/PWV7.
let waveRate = 150.0

struct CueInfo: Equatable {
    var hot: Int          // 0 = memory cue, 1...8 = hot cue A...H
    var time: Double
    var loopEnd: Double   // 0 = not a loop
    var rgb: (UInt8, UInt8, UInt8)?
    var active = false    // rekordbox active loop
    var comment = ""
    static func == (a: CueInfo, b: CueInfo) -> Bool {
        a.hot == b.hot && a.time == b.time && a.loopEnd == b.loopEnd && a.active == b.active
    }
}

struct PhraseInfo: Equatable {
    var start: Double, end: Double
    var kind: Int
}

/// Decodes a file to 48 kHz stereo float and hands a djc_track to the engine.
/// With a rekordbox analysis path it uses the stick's beat grid, cues, waveform and phrases;
/// without one it falls back to our own estimate.
enum TrackLoader {
    struct Loaded {
        let track: OpaquePointer
        let bpm: Double
        let firstBeat: Double
        let waves: [WaveKind: WaveData]
        let overviews: [WaveKind: WaveData]
        let duration: Double
        let cues: [CueInfo]
        let phrases: [PhraseInfo]
        let mood: Int
        let fromRekordbox: Bool
        let beatTimes: [Double]
        let beatInBar: [UInt8]
        let seconds: Double
        var gridShift: Double = 0   // seconds the rekordbox grid / cues / waveform were moved to match the audio
        var autoCue: Double = 0     // first sound, on the grid beat when one is that close (seconds)
    }

    /// Auto cue: first sample above -36 dBFS; if a grid beat is within 100 ms, cue exactly on that beat.
    static func autoCue(left: [Float], right: [Float], sampleRate: Double, beats: [Double]) -> Double {
        let thr: Float = 0.0158   // -36 dBFS
        var first = -1
        for i in 0..<min(left.count, right.count) where abs(left[i]) > thr || abs(right[i]) > thr { first = i; break }
        guard first >= 0 else { return 0 }
        let t = Double(first) / sampleRate
        if let b = beats.min(by: { abs($0 - t) < abs($1 - t) }), abs(b - t) <= 0.1, b >= 0 { return b }
        return t
    }

    /// Set from Settings: measure each rekordbox grid against the decoded audio and fix clear offsets.
    nonisolated(unsafe) static var alignGrid = true

    enum LoadError: LocalizedError {
        case access, format, missing(String), convert(String)
        var errorDescription: String? {
            switch self {
            case .access: return "No permission to read the file"
            case .format: return "Unsupported audio format"
            case .missing(let p): return "File not found on the stick: \(p)"
            case .convert(let s): return "Decode failed: \(s)"
            }
        }
    }

    static func load(url: URL, sampleRate: Double, anlzPath: String? = nil, pdbBpm: Double = 0,
                     alignGrid: Bool = TrackLoader.alignGrid) -> Result<Loaded, Error> {
        let t0 = Date()
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard FileManager.default.fileExists(atPath: url.path) else { return .failure(LoadError.missing(url.path)) }
        do {
            let (left, right) = try decode(url: url, sampleRate: sampleRate)
            let frames = left.count
            guard frames > 0 else { throw LoadError.format }

            var bpm = 0.0, first = 0.0
            var waves: [WaveKind: WaveData] = [:]
            var gridTimes: [Double] = [], gridInBar: [UInt8] = []
            var cues: [CueInfo] = []
            var phrases: [PhraseInfo] = []
            var mood = 0
            var fromRB = false

            if let anlzPath, let a = djc_anlz_open(anlzPath) {
                defer { djc_anlz_close(a) }
                // beat grid
                let nb = Int(djc_anlz_beat_count(a))
                var beatTimes: [Double] = []
                var inBar: [UInt8] = []
                if nb > 0, let beats = djc_anlz_beats(a) {
                    beatTimes = (0..<nb).map { Double(beats[$0].timeMs) / 1000.0 }
                    inBar = (0..<nb).map { UInt8(clamping: beats[$0].beatNumber) }
                    gridTimes = beatTimes; gridInBar = inBar
                    bpm = Double(beats[0].tempo100) / 100.0
                    // bar origin = first downbeat (beat number 1)
                    if let down = (0..<nb).first(where: { beats[$0].beatNumber == 1 }) {
                        first = beatTimes[down]
                    } else {
                        first = beatTimes[0]
                    }
                    fromRB = true
                }
                if bpm <= 0 { bpm = pdbBpm }
                // cues
                for i in 0..<Int(djc_anlz_cue_count(a)) {
                    guard let c = djc_anlz_cue_at(a, Int32(i))?.pointee else { continue }
                    let t = Double(c.timeMs) / 1000.0
                    let end = c.type == 2 ? Double(c.loopMs) / 1000.0 : 0
                    cues.append(CueInfo(hot: Int(c.hotCue), time: t, loopEnd: end,
                                        rgb: c.hasRgb != 0 ? (c.r, c.g, c.b) : nil, active: c.active != 0 && end > t,
                                        comment: String(cString: djc_anlz_cue_comment(a, Int32(i)))))
                }
                // waveforms (150/s): every kind the stick has, heights normalised so the loudest point is 1
                for kind in WaveKind.allCases {
                    let n = Int(djc_anlz_wave_len(a, kind.anlzCode))
                    guard n > 0, let hp = djc_anlz_wave_data(a, kind.anlzCode, 0) else { continue }
                    var h = Array(UnsafeBufferPointer(start: hp, count: n))
                    var c: [[Float]] = (1...3).map { ch in
                        djc_anlz_wave_data(a, kind.anlzCode, Int32(ch)).map { Array(UnsafeBufferPointer(start: $0, count: n)) } ?? []
                    }
                    let mx = max(vDSP.maximum(h), 1e-6)
                    h = vDSP.multiply(1 / mx, h)
                    if kind == .threeBand { c = c.map { $0.isEmpty ? $0 : vDSP.multiply(1 / mx, $0) } }
                    waves[kind] = WaveData(h: h, c: c)
                }
                // phrases: beat numbers -> seconds through the grid
                mood = Int(djc_anlz_mood(a))
                let np = Int(djc_anlz_phrase_count(a))
                if np > 0, let ph = djc_anlz_phrases(a), !beatTimes.isEmpty {
                    func time(ofBeat b: Int) -> Double {
                        let i = max(0, min(beatTimes.count - 1, b - 1))
                        return beatTimes[i]
                    }
                    let endBeat = Int(djc_anlz_end_beat(a))
                    for i in 0..<np {
                        let s = time(ofBeat: Int(ph[i].beat))
                        let e = i + 1 < np ? time(ofBeat: Int(ph[i + 1].beat)) : time(ofBeat: endBeat)
                        phrases.append(PhraseInfo(start: s, end: max(e, s), kind: Int(ph[i].kind)))
                    }
                }
            }

            if bpm <= 0 || waves.isEmpty {
                let mono = vDSP.multiply(Float(0.5), vDSP.add(left, right))
                if bpm <= 0 { djc_estimate_bpm(mono, Int64(frames), sampleRate, &bpm, &first) }
                if waves.isEmpty {
                    let wbins = max(1, Int(Double(frames) / sampleRate * waveRate))
                    var wlo = [Float](repeating: 0, count: wbins), wmi = wlo, whi = wlo
                    djc_overview(mono, Int64(frames), sampleRate, Int32(wbins), &wlo, &wmi, &whi)
                    let h = vDSP.maximum(vDSP.maximum(wlo, wmi), whi)
                    let mx = max(vDSP.maximum(h), 1e-6)
                    waves[.threeBand] = WaveData(h: vDSP.multiply(1 / mx, h),
                                                 c: [wlo, wmi, whi].map { vDSP.multiply(1 / mx, $0) })
                }
            }
            // rekordbox timeline vs our decode: some MP3/AAC files decode ~13-26 ms apart (encoder/decoder delay).
            // Measured per track from the kick attacks; applied only when clear (>= 5 ms, strong peak). Read-only.
            var shift = 0.0
            if alignGrid, fromRB, gridTimes.count >= 16 {
                let mono = vDSP.multiply(Float(0.5), vDSP.add(left, right))
                var lag = 0.0, conf = 0.0
                djc_grid_offset(mono, Int64(frames), sampleRate, gridTimes, Int32(gridTimes.count), &lag, &conf)
                if conf >= 3, abs(lag) >= 0.005, abs(lag) <= 0.055 {
                    shift = lag
                    gridTimes = gridTimes.map { $0 + lag }
                    first += lag
                    cues = cues.map { var c = $0; c.time += lag; if c.loopEnd > 0 { c.loopEnd += lag }; return c }
                    phrases = phrases.map { PhraseInfo(start: $0.start + lag, end: $0.end + lag, kind: $0.kind) }
                    waves = waves.mapValues { $0.shifted(seconds: lag, rate: waveRate) }
                }
            }
            let overviews = waves.mapValues { $0.downsampled(to: 1200) }
            guard let t = djc_track_create(left, right, Int64(frames), sampleRate, bpm, first) else {
                throw LoadError.format
            }
            if fromRB, gridTimes.count >= 2 { djc_track_set_beats(t, gridTimes, Int32(gridTimes.count)) }
            return .success(Loaded(track: t, bpm: bpm, firstBeat: first, waves: waves, overviews: overviews,
                                   duration: Double(frames) / sampleRate, cues: cues,
                                   phrases: phrases, mood: mood, fromRekordbox: fromRB,
                                   beatTimes: gridTimes, beatInBar: gridInBar,
                                   seconds: Date().timeIntervalSince(t0), gridShift: shift,
                                   autoCue: autoCue(left: left, right: right, sampleRate: sampleRate, beats: gridTimes)))
        } catch {
            return .failure(error)
        }
    }

    static func decode(url: URL, sampleRate: Double) throws -> ([Float], [Float]) {
        let file = try AVAudioFile(forReading: url)
        let inFmt = file.processingFormat
        let estimate = Int(Double(file.length) * sampleRate / inFmt.sampleRate) + 8192
        var left = [Float](); left.reserveCapacity(estimate)
        var right = [Float](); right.reserveCapacity(estimate)
        let chunk: AVAudioFrameCount = 65536
        guard let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: chunk) else { throw LoadError.format }

        func append(_ b: AVAudioPCMBuffer) {
            let n = Int(b.frameLength)
            guard n > 0, let ch = b.floatChannelData else { return }
            let l = UnsafeBufferPointer(start: ch[0], count: n)
            left.append(contentsOf: l)
            if b.format.channelCount > 1 { right.append(contentsOf: UnsafeBufferPointer(start: ch[1], count: n)) }
            else { right.append(contentsOf: l) }
        }

        if inFmt.sampleRate == sampleRate && inFmt.channelCount <= 2 {
            while file.framePosition < file.length {
                try file.read(into: inBuf, frameCount: chunk)
                if inBuf.frameLength == 0 { break }
                append(inBuf)
            }
        } else {
            let midFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate,
                                       channels: min(2, inFmt.channelCount), interleaved: false)!
            guard let conv = AVAudioConverter(from: inFmt, to: midFmt) else { throw LoadError.format }
            conv.sampleRateConverterQuality = AVAudioQuality.max.rawValue
            let outCap = AVAudioFrameCount(Double(chunk) * sampleRate / inFmt.sampleRate) + 1024
            guard let outBuf = AVAudioPCMBuffer(pcmFormat: midFmt, frameCapacity: outCap) else { throw LoadError.format }
            var done = false
            while !done {
                var err: NSError?
                let st = conv.convert(to: outBuf, error: &err) { _, status in
                    do { try file.read(into: inBuf, frameCount: chunk) } catch {
                        status.pointee = .endOfStream
                        return nil
                    }
                    if inBuf.frameLength == 0 { status.pointee = .endOfStream; return nil }
                    status.pointee = .haveData
                    return inBuf
                }
                append(outBuf)
                switch st {
                case .endOfStream: done = true
                case .error: throw LoadError.convert(err?.localizedDescription ?? "unknown")
                default: break
                }
            }
        }
        return (left, right)
    }
}
