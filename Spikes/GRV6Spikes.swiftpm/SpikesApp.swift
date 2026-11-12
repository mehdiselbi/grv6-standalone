import SwiftUI
import UniformTypeIdentifiers

@main
struct GRV6SpikesApp: App {
    var body: some Scene { WindowGroup { ContentView() } }
}

struct ContentView: View {
    @StateObject var midi = MIDILogger()
    @StateObject var audio = AudioRouteProbe()
    @StateObject var stick = StickProbe()
    @State private var controlName = ""
    @State private var rawOut = ""
    @State private var sendResult = ""
    @State private var pickFolder = false
    @State private var pickFile = false
    @State private var shareCSV = false

    var body: some View {
        TabView {
            midiTab.tabItem { Label("MIDI", systemImage: "pianokeys") }
            audioTab.tabItem { Label("Audio", systemImage: "speaker.wave.2") }
            stickTab.tabItem { Label("Stick", systemImage: "externaldrive") }
        }
        .onAppear { midi.start(); audio.configureSession() }
    }

    var midiTab: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(midi.status).font(.caption)
            Text(midi.endpoints.joined(separator: "\n")).font(.caption.monospaced())
            HStack {
                TextField("Control name (use the list's Fig. id, e.g. D1 PLAY L)", text: $controlName)
                    .textFieldStyle(.roundedBorder)
                if midi.armedControl == nil {
                    Button("Arm") { midi.arm(controlName) }.disabled(controlName.isEmpty)
                } else {
                    Button("Commit") { midi.commit(); controlName = "" }
                    Button("Discard", role: .destructive) { midi.discard() }
                }
            }
            HStack {
                TextField("LED test: 3 hex bytes from MIDI-OUT column", text: $rawOut)
                    .textFieldStyle(.roundedBorder).font(.body.monospaced())
                Button("Send") { sendResult = midi.send(hex: rawOut) }
                Text(sendResult).font(.caption)
            }
            HStack {
                Text("\(midi.rows.count) mapping rows")
                Spacer()
                ShareLink(item: midi.csv(), preview: SharePreview("grv6_mapping.csv")) { Text("Export CSV") }
            }
            List(Array(midi.log.suffix(200).reversed())) { r in
                Text("\(r.hex)   \(r.source)").font(.caption.monospaced())
            }
        }.padding()
    }

    var audioTab: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Button("Refresh") { audio.refresh(note: "manual") }
                Stepper("Tone channel index: \(audio.toneChannel)", value: $audio.toneChannel, in: 0...15)
                Button(audio.playing ? "Stop" : "Play 440 Hz") {
                    audio.playing ? audio.stopTone() : audio.startTone()
                }
            }
            ScrollView { Text(audio.report).font(.caption.monospaced()).frame(maxWidth: .infinity, alignment: .leading) }
        }.padding()
    }

    var stickTab: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Button("Pick stick root") { pickFolder = true }
                    .fileImporter(isPresented: $pickFolder, allowedContentTypes: [.folder]) { r in
                        if case .success(let u) = r { stick.setRoot(u) }
                    }
                Button("Inventory PIONEER/") { stick.inventory() }
                Button("Throughput test…") { pickFile = true }
                    .fileImporter(isPresented: $pickFile, allowedContentTypes: [.audio, .data]) { r in
                        if case .success(let u) = r { stick.throughput(file: u) }
                    }
            }
            ScrollView { Text(stick.report).font(.caption.monospaced()).frame(maxWidth: .infinity, alignment: .leading) }
        }
        .padding()
    }
}
