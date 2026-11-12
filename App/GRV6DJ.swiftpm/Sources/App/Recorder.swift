import AVFoundation
import DJCore
import Foundation

/// Records the master output (what the audience hears) to Documents/Recordings.
/// The audio thread only copies samples into the engine's lock-free ring; this class drains it on its own
/// queue every 100 ms and writes the file, so disk writes never touch the audio thread.
final class SetRecorder: @unchecked Sendable {
    enum Format: String, CaseIterable, Identifiable {
        case wav = "WAV 24-bit", aac = "AAC 320 kbps"
        var id: String { rawValue }
        var ext: String { self == .wav ? "wav" : "m4a" }
    }

    private let core: OpaquePointer
    private let sampleRate: Double
    private let queue = DispatchQueue(label: "grv6.recorder", qos: .userInitiated)
    private var file: AVAudioFile?
    private var timer: DispatchSourceTimer?
    private var scratch: [Float]
    private let block: AVAudioFrameCount = 8192
    private var pcm: AVAudioPCMBuffer?
    private var written: Int64 = 0      // queue only
    private var lastError: Error?       // queue only

    init(core: OpaquePointer, sampleRate: Double) {
        self.core = core
        self.sampleRate = sampleRate
        scratch = [Float](repeating: 0, count: Int(block) * 2)
    }

    static var folder: URL {
        let d = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Recordings")
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    static func freeBytes() -> Int64 {
        let v = try? folder.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return v?.volumeAvailableCapacityForImportantUsage ?? -1
    }

    /// Opens the file and starts capturing. Returns the file URL.
    func start(format: Format) throws -> URL {
        let df = DateFormatter()
        df.dateFormat = "yyyy-MM-dd HHmm"
        var url = Self.folder.appendingPathComponent("GRV6 Set \(df.string(from: Date())).\(format.ext)")
        var k = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = Self.folder.appendingPathComponent("GRV6 Set \(df.string(from: Date())) (\(k)).\(format.ext)"); k += 1
        }
        let settings: [String: Any]
        switch format {
        case .wav:
            settings = [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: sampleRate, AVNumberOfChannelsKey: 2,
                        AVLinearPCMBitDepthKey: 24, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
                        AVLinearPCMIsNonInterleaved: false]
        case .aac:
            settings = [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: sampleRate, AVNumberOfChannelsKey: 2,
                        AVEncoderBitRateKey: 320_000]
        }
        let f = try AVAudioFile(forWriting: url, settings: settings, commonFormat: .pcmFormatFloat32, interleaved: false)
        let buf = AVAudioPCMBuffer(pcmFormat: f.processingFormat, frameCapacity: block)
        queue.sync {
            file = f; pcm = buf; written = 0; lastError = nil
        }
        djc_rec_start(core)
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.1, repeating: 0.1)
        t.setEventHandler { [weak self] in self?.drain() }
        t.resume()
        timer = t
        return url
    }

    /// Stops capturing, writes what is left and closes the file. Calls back on the main queue.
    func stop(done: @escaping @Sendable (_ seconds: Double, _ dropped: Int64, _ error: Error?) -> Void) {
        djc_rec_stop(core)
        timer?.cancel(); timer = nil
        queue.async { [self] in
            drain()
            let secs = Double(written) / sampleRate, err = lastError
            file = nil // closing the AVAudioFile finalises the header
            let dropped = djc_rec_dropped(core)
            DispatchQueue.main.async { done(secs, dropped, err) }
        }
    }

    private func drain() {
        guard let file, let pcm else { return }
        while true {
            let n = scratch.withUnsafeMutableBufferPointer { djc_rec_read(core, $0.baseAddress, Int32(block)) }
            if n <= 0 { break }
            let l = pcm.floatChannelData![0], r = pcm.floatChannelData![1]
            for i in 0..<Int(n) { l[i] = scratch[2 * i]; r[i] = scratch[2 * i + 1] }
            pcm.frameLength = AVAudioFrameCount(n)
            do { try file.write(from: pcm); written += Int64(n) } catch { lastError = error; break }
            if n < Int32(block) { break }
        }
    }
}

struct RecordingFile: Identifiable, Equatable {
    var id: URL { url }
    let url: URL
    let date: Date
    let bytes: Int64
}
