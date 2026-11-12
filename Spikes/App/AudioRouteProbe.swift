// Phase 0b: does the GRV6 expose master and headphone-cue as separate USB outputs on iPadOS?
// Reports the route and channel counts, then plays a tone on ONE chosen channel at a time
// so you can hear which physical jack (MASTER / PHONES) each channel index reaches.

import AVFoundation
import Combine

final class AudioRouteProbe: ObservableObject {
    @Published var report = ""
    @Published var toneChannel = 0          // 0-based output channel index
    @Published var playing = false

    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    // Shared with the render thread. Plain scalars, no locks, no allocation.
    private let chanBox = UnsafeMutablePointer<Int32>.allocate(capacity: 1)
    private let phaseBox = UnsafeMutablePointer<Double>.allocate(capacity: 1)

    init() { chanBox.pointee = 0; phaseBox.pointee = 0
        NotificationCenter.default.addObserver(forName: AVAudioSession.routeChangeNotification,
                                               object: nil, queue: .main) { [weak self] n in
            let r = (n.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt) ?? 0
            self?.refresh(note: "route change reason \(r)")
        }
    }

    func configureSession() {
        let s = AVAudioSession.sharedInstance()
        do {
            try s.setCategory(.playback, mode: .default, options: [])
            try s.setPreferredIOBufferDuration(0.005)
            try s.setActive(true)
            // Ask for every channel the device offers.
            try s.setPreferredOutputNumberOfChannels(s.maximumOutputNumberOfChannels)
        } catch { report = "session error: \(error)"; return }
        refresh(note: "configured")
    }

    func refresh(note: String) {
        let s = AVAudioSession.sharedInstance()
        var out = "[\(note)]\n"
        out += "sampleRate \(s.sampleRate)  ioBuffer \(String(format: "%.2f", s.ioBufferDuration * 1000)) ms\n"
        out += "outputChannels \(s.outputNumberOfChannels) / max \(s.maximumOutputNumberOfChannels)\n"
        out += "outputLatency \(String(format: "%.2f", s.outputLatency * 1000)) ms\n"
        for p in s.currentRoute.outputs {
            out += "OUT \(p.portName)  type=\(p.portType.rawValue)  uid=\(p.uid)\n"
            for c in p.channels ?? [] {
                out += "   ch#\(c.channelNumber) '\(c.channelName)' label=\(c.channelLabel)\n"
            }
        }
        for p in s.availableInputs ?? [] { out += "IN  \(p.portName) type=\(p.portType.rawValue)\n" }
        report = out
    }

    func startTone() {
        stopTone()
        chanBox.pointee = Int32(toneChannel)
        let hwFormat = engine.outputNode.outputFormat(forBus: 0)
        let channels = Int(hwFormat.channelCount)
        let sr = hwFormat.sampleRate
        let chan = chanBox, phase = phaseBox
        let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sr,
                                channels: AVAudioChannelCount(channels), interleaved: false)!
        let node = AVAudioSourceNode(format: fmt) { _, _, frames, abl in
            let bufs = UnsafeMutableAudioBufferListPointer(abl)
            let target = Int(chan.pointee)
            let inc = 2.0 * Double.pi * 440.0 / sr
            var ph = phase.pointee
            for (i, b) in bufs.enumerated() {
                guard let p = b.mData?.assumingMemoryBound(to: Float.self) else { continue }
                if i == target {
                    var local = ph
                    for f in 0..<Int(frames) { p[f] = Float(sin(local)) * 0.2; local += inc }
                } else {
                    for f in 0..<Int(frames) { p[f] = 0 }
                }
            }
            ph += inc * Double(frames)
            phase.pointee = ph.truncatingRemainder(dividingBy: 2.0 * Double.pi)
            return noErr
        }
        engine.attach(node)
        engine.connect(node, to: engine.outputNode, format: fmt)
        do { try engine.start(); playing = true; source = node
             refresh(note: "tone on channel index \(toneChannel) of \(channels)") }
        catch { report += "\nengine error \(error)" }
    }

    func stopTone() {
        engine.stop()
        if let n = source { engine.detach(n); source = nil }
        playing = false
    }
}
