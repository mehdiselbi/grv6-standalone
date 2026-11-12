import DJCore
import SwiftUI
import UniformTypeIdentifiers

// XDJ-AZ style screens, laid out on the hardware's 1280 x 800 grid from the owner's reference screenshots
// and scaled to the iPad. Everything is drawn by us with system fonts; neutral wordmark only.

enum XDJ {
    static let bg = Color.black
    static let panel = Color(white: 0.13)
    static let panel2 = Color(white: 0.19)
    static let line = Color(white: 0.30)
    static let blue = Color(red: 0.16, green: 0.50, blue: 0.98)
    static let selBlue = Color(red: 0.22, green: 0.47, blue: 0.86)
    static let orange = Color(red: 1.0, green: 0.52, blue: 0.10)
    static let red = Color(red: 0.90, green: 0.10, blue: 0.12)
    static let green = Color(red: 0.40, green: 0.92, blue: 0.30)
    static let grey = Color(white: 0.55)
    static func font(_ size: CGFloat, _ w: Font.Weight = .regular) -> Font {
        .system(size: size, weight: w, design: .default).monospacedDigit()
    }
}

struct XDJScreen: View {
    @EnvironmentObject var engine: Engine
    @State private var showSettings = false
    @State private var importing = false

    var body: some View {
        GeometryReader { g in
            let s = min(g.size.width / 1280, g.size.height / 800)
            content
                .frame(width: 1280, height: 800)
                .scaleEffect(s, anchor: .center)
                .frame(width: g.size.width, height: g.size.height)
        }
        .background(Color.black.ignoresSafeArea())
        .sheet(isPresented: $showSettings) { SettingsSheet().environmentObject(engine) }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.audio], allowsMultipleSelection: true) { r in
            if case .success(let urls) = r { engine.importFiles(urls); engine.category = .files; engine.showBrowser = true }
        }
    }

    @ViewBuilder private var content: some View {
        VStack(spacing: 0) {
            TopBar(title: engine.showSource ? "SOURCE" : (engine.showBrowser ? "BROWSE" : "WAVEFORM"), showSettings: $showSettings)
                .frame(height: 70)
            if engine.showSource {
                SourceScreen().frame(height: 620)    // full screen like the XDJ-AZ SOURCE screen
                DeckStrip().frame(height: 110)
            } else if engine.showBrowser {
                BrowseScreen(importing: $importing).frame(height: 620)
                DeckStrip().frame(height: 110)
            } else {
                WaveformScreen()
            }
        }
        .foregroundStyle(.white)
        .background(XDJ.bg)
        .overlay {
            if let w = engine.warning {
                WarningPopup(warning: w)
                    .onTapGesture { engine.warning = nil }
                    .transition(.opacity)
            }
        }
        .animation(.easeOut(duration: 0.15), value: engine.warning)
    }
}

/// "125" and ".0" from 124.96, rounded to tenths like the hardware display
func bpmParts(_ v: Double, decimals: Int = 1) -> (String, String) {
    if decimals == 1 {
        let t = Int((v * 10).rounded())
        return (String(t / 10), ".\(t % 10)")
    }
    let t = Int((v * 100).rounded())
    return (String(t / 100), String(format: ".%02d", t % 100))
}

// MARK: - Top bar

private struct TopBar: View {
    @EnvironmentObject var engine: Engine
    let title: String
    @Binding var showSettings: Bool

    var body: some View {
        HStack(spacing: 0) {
            Text(title).font(XDJ.font(25, .medium)).frame(width: 195, alignment: .leading).padding(.leading, 25)
            RecButton()
            if title == "WAVEFORM" {
                Seg(left: "2 DECK", right: "4 DECK", leftOn: !engine.fourDeck).frame(width: 146, height: 34)
                    .contentShape(Rectangle())
                    .onTapGesture { engine.fourDeck.toggle() }
            }
            Spacer()
            UsbSlots()
            Rectangle().fill(XDJ.line).frame(width: 1, height: 22).padding(.horizontal, 14)
            Circle().fill(engine.controllerName.isEmpty ? XDJ.red : XDJ.green).frame(width: 9, height: 9)
            Text(engine.controllerName.isEmpty ? "NO CONTROLLER" : "GRV6")
                .font(XDJ.font(13, .semibold)).foregroundStyle(XDJ.grey).padding(.leading, 6)
            Rectangle().fill(XDJ.line).frame(width: 1, height: 22).padding(.horizontal, 14)
            OutputMeter().padding(.trailing, 14)
            Text(String(format: "DSP %02.0f%%", engine.cpu * 100)).font(XDJ.font(14))
                .foregroundStyle(engine.cpu > 0.7 ? XDJ.red : XDJ.grey)
            Rectangle().fill(XDJ.line).frame(width: 1, height: 22).padding(.horizontal, 14)
            ClockText().font(XDJ.font(17))
            Rectangle().fill(XDJ.line).frame(width: 1, height: 22).padding(.horizontal, 14)
            BatteryView()
            Image(systemName: "info.circle.fill").font(.system(size: 26))
                .padding(.leading, 26).padding(.trailing, 20)
                .onTapGesture { showSettings = true }
        }
    }
}

/// IPAD / USB1 / USB2: lit when present (white), green = the one being browsed. Tap = SOURCE.
private struct UsbSlots: View {
    @EnvironmentObject var engine: Engine
    var body: some View {
        HStack(spacing: 10) {
            slot("IPAD", icon: "ipad.landscape")
            slot("USB1", icon: "externaldrive.fill")
            slot("USB2", icon: "externaldrive.fill")
        }
        .contentShape(Rectangle())
        .onTapGesture { engine.showSource = true }
    }
    private func slot(_ label: String, icon: String) -> some View {
        let present = engine.sources.contains { $0.label == label }
        let browsing = engine.usb?.label == label
        return HStack(spacing: 4) {
            Image(systemName: icon).font(.system(size: 14))
            Text(label).font(XDJ.font(13, .semibold))
        }
        .foregroundStyle(browsing ? XDJ.green : (present ? Color.white : Color(white: 0.35)))
    }
}

/// iPad battery level and charging state.
private struct BatteryView: View {
    @State private var level: Float = -1
    @State private var charging = false
    var body: some View {
        TimelineView(.periodic(from: .now, by: 30)) { tl in
            HStack(spacing: 5) {
                Image(systemName: icon).font(.system(size: 20))
                    .foregroundStyle(level >= 0 && level < 0.2 && !charging ? XDJ.red : .white)
                Text(level >= 0 ? String(format: "%.0f%%", level * 100) : "--%").font(XDJ.font(15))
            }
            .onAppear(perform: read)
            .onChange(of: tl.date) { _, _ in read() }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIDevice.batteryLevelDidChangeNotification)) { _ in read() }
        .onReceive(NotificationCenter.default.publisher(for: UIDevice.batteryStateDidChangeNotification)) { _ in read() }
    }
    private var icon: String {
        if charging { return "battery.100.bolt" }
        switch level {
        case ..<0: return "battery.0"
        case ..<0.15: return "battery.0"
        case ..<0.4: return "battery.25"
        case ..<0.65: return "battery.50"
        case ..<0.9: return "battery.75"
        default: return "battery.100"
        }
    }
    private func read() {
        UIDevice.current.isBatteryMonitoringEnabled = true
        level = UIDevice.current.batteryLevel
        let st = UIDevice.current.batteryState
        charging = st == .charging || st == .full
    }
}

private struct ClockText: View {
    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { tl in
            Text(tl.date, format: .dateTime.hour(.defaultDigits(amPM: .omitted)).minute())
        }
    }
}

private struct Seg: View {
    let left: String, right: String
    let leftOn: Bool
    var body: some View {
        HStack(spacing: 6) {
            Text(left).foregroundStyle(leftOn ? XDJ.blue : XDJ.grey)
            Rectangle().fill(XDJ.grey).frame(width: 1, height: 14)
            Text(right).foregroundStyle(leftOn ? XDJ.grey : XDJ.blue)
        }
        .font(XDJ.font(13, .medium))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(RoundedRectangle(cornerRadius: 2).stroke(Color(white: 0.28), lineWidth: 1.5))
    }
}

private struct BoxLabel: View {
    let text: String
    var on = false
    var width: CGFloat = 93
    var body: some View {
        Text(text).font(XDJ.font(13, .medium)).foregroundStyle(XDJ.blue)
            .frame(width: width, height: 34)
            .background(on ? XDJ.blue.opacity(0.25) : .clear)
            .overlay(RoundedRectangle(cornerRadius: 2).stroke(Color(white: 0.28), lineWidth: 1.5))
    }
}

// MARK: - Waveform screen

private struct WaveformScreen: View {
    @EnvironmentObject var engine: Engine

    var body: some View {
        if engine.fourDeck { fourDeckBody } else { twoDeckBody }
    }

    private var twoDeckBody: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 0) {
                VStack(spacing: 2) {
                    DeckRail(deck: engine.leftDeck)
                    DeckRail(deck: engine.rightDeck)
                }
                .frame(width: 170, height: 516)
                VStack(spacing: 2) {
                    WaveLane(deck: engine.leftDeck)
                    WaveLane(deck: engine.rightDeck)
                }
                .frame(width: 895, height: 500)
                .padding(.leading, 12)
                .padding(.top, 8)
                FxPanel().frame(width: 180, height: 500)
                    .padding(.top, 8)
                    .padding(.leading, 13)
                Spacer(minLength: 0)
            }
            .frame(height: 522)
            HStack(spacing: 20) {
                HotCueList(deck: engine.leftDeck)
                HotCueList(deck: engine.rightDeck)
            }
            .padding(.horizontal, 10)
            .frame(height: 76)
            HStack(spacing: 20) { // only the two decks selected on the GRV6
                DeckCard(deck: engine.leftDeck)
                DeckCard(deck: engine.rightDeck)
            }
            .padding(.horizontal, 10)
            .frame(height: 128)
        }
    }

    /// 4 DECK view: four waveform lanes (decks 1-4) and four deck cards (1 2 / 3 4), like the hardware.
    private var fourDeckBody: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 0) {
                VStack(spacing: 2) {
                    ForEach(0..<4, id: \.self) { DeckRail(deck: $0, compact: true) }
                }
                .frame(width: 170, height: 462)
                VStack(spacing: 2) {
                    ForEach(0..<4, id: \.self) { WaveLane(deck: $0, compact: true) }
                }
                .frame(width: 895, height: 454)
                .padding(.leading, 12)
                .padding(.top, 8)
                FxPanel().frame(width: 180, height: 454)
                    .padding(.top, 8)
                    .padding(.leading, 13)
                Spacer(minLength: 0)
            }
            .frame(height: 466)
            Grid(horizontalSpacing: 20, verticalSpacing: 6) {
                GridRow { DeckCard(deck: 0); DeckCard(deck: 1) }
                GridRow { DeckCard(deck: 2); DeckCard(deck: 3) }
            }
            .padding(.horizontal, 10)
            .frame(height: 262)
        }
    }
}

// MARK: - Left rail

private struct DeckRail: View {
    @EnvironmentObject var engine: Engine
    let deck: Int
    var compact = false
    var body: some View {
        if compact { compactBody } else { fullBody }
    }

    /// 4 DECK rail: deck number, key, Q in three short rows
    private var compactBody: some View {
        let d = engine.decks[deck]
        let info = engine.deckTracks[deck]
        let active = deck == engine.leftDeck || deck == engine.rightDeck
        return VStack(spacing: 1) {
            HStack(spacing: 8) {
                Rectangle().fill(active ? XDJ.blue : XDJ.grey).frame(width: 13, height: 13)
                Text("DECK \(deck + 1)").font(XDJ.font(16, .medium)).foregroundStyle(active ? XDJ.blue : XDJ.grey)
                Spacer()
                Text(info?.source ?? "").font(XDJ.font(12)).foregroundStyle(XDJ.grey)
            }
            .padding(.horizontal, 10).frame(height: 32).background(XDJ.panel)
            HStack(spacing: 6) {
                Text("KEY").font(XDJ.font(10, .medium)).frame(width: 30, alignment: .leading)
                Text(keyText(info, d)).font(XDJ.font(19))
                    .foregroundStyle(d.keySync ? XDJ.green : (d.keyShift != 0 ? XDJ.orange : (info == nil ? XDJ.grey : .white)))
                Spacer()
            }
            .padding(.leading, 10).frame(height: 40).background(XDJ.panel)
            HStack(spacing: 6) {
                Text("Q").font(XDJ.font(15, .medium)).foregroundStyle(d.quantize ? XDJ.red : XDJ.grey).frame(width: 30, alignment: .leading)
                Text(d.quantize ? "1" : "OFF").font(XDJ.font(18)).foregroundStyle(d.quantize ? XDJ.red : XDJ.grey)
                Spacer()
            }
            .padding(.leading, 10).frame(height: 40).background(XDJ.panel)
        }
        .background(Color.black)
    }

    @ViewBuilder private var fullBody: some View {
        let d = engine.decks[deck]
        let info = engine.deckTracks[deck]
        VStack(spacing: 1) {
            HStack(spacing: 8) {
                Rectangle().fill(XDJ.blue).frame(width: 15, height: 15)
                Text("DECK \(deck + 1)").font(XDJ.font(18, .medium)).foregroundStyle(XDJ.blue)
                Spacer()
                Image(systemName: "eject.fill").font(.system(size: 14))
            }
            .padding(.horizontal, 12).frame(height: 34).background(XDJ.panel)
            row {
                Image(systemName: info?.source == "FILES" ? "folder.fill" : "externaldrive.fill")
                    .font(.system(size: 18)).frame(width: 26)
                Text(info?.source ?? "").font(XDJ.font(21))
                Spacer()
            }
            row {
                Text("KEY").font(XDJ.font(10, .medium)).frame(width: 30, alignment: .leading)
                Text(keyText(info, d)).font(XDJ.font(21))
                    .foregroundStyle(d.keySync ? XDJ.green : (d.keyShift != 0 ? XDJ.orange : (info == nil ? XDJ.grey : .white)))
                Spacer()
                Image(systemName: "play.fill").font(.system(size: 13))
                    .frame(width: 34, height: 36).background(XDJ.panel2)
            }
            row {
                Text("BEAT\nJUMP").font(XDJ.font(9, .medium)).lineSpacing(0).frame(width: 30, alignment: .leading)
                Text("4").font(XDJ.font(21))
                Spacer()
            }
            row {
                Text("Q").font(XDJ.font(17, .medium)).foregroundStyle(d.quantize ? XDJ.red : XDJ.grey).frame(width: 30, alignment: .leading)
                Text(d.quantize ? "1" : "OFF").font(XDJ.font(21)).foregroundStyle(d.quantize ? XDJ.red : XDJ.grey)
                Spacer()
            }
        }
        .background(Color.black)
    }

    private func row<C: View>(@ViewBuilder _ c: () -> C) -> some View {
        HStack(spacing: 6) { c() }.padding(.leading, 12).padding(.trailing, 4)
            .frame(height: 55).background(XDJ.panel)
    }

    private func keyText(_ info: TrackInfo?, _ d: DeckView) -> String {
        guard let k = info?.key, !k.isEmpty else { return "--" }
        return d.keyShift == 0 ? k : k + String(format: "%+d", d.keyShift)
    }
}

// MARK: - Enlarged waveform lane (Metal) + bar countdown

private struct WaveLane: View {
    @EnvironmentObject var engine: Engine
    let deck: Int
    var compact = false
    var body: some View {
        MetalWaveformLane(deck: deck)
            .overlay(alignment: .topLeading) {
                GeometryReader { g in
                    countdown.position(x: g.size.width * 0.30 - 44, y: compact ? 10 : 14)
                }
            }
            .overlay {
                if engine.deckTracks[deck] == nil {
                    Text(engine.loading.contains(deck) ? "Loading…" : "")
                        .font(XDJ.font(15)).foregroundStyle(XDJ.grey)
                }
            }
    }

    private var countdown: some View {
        let d = engine.decks[deck]
        var label = "--.-"
        var color = Color.white
        if d.loaded, d.trackBpm > 0 {
            let beatSec = 60 / d.trackBpm
            let mem = engine.deckTracks[deck]?.cues.filter { $0.hot == 0 }.map(\.time) ?? []
            if let next = ([d.cue] + d.hotcues + mem).filter({ $0 > d.position + 0.01 }).min() {
                let beats = Int(((next - d.position) / beatSec).rounded(.down))
                label = "\(beats / 4).\(beats % 4 + 1)"
                color = XDJ.orange
            }
        }
        return HStack(alignment: .lastTextBaseline, spacing: 2) {
            Text(label).font(XDJ.font(17, .semibold))
            Text("Bars").font(XDJ.font(11, .semibold))
        }
        .foregroundStyle(color)
    }
}

// MARK: - Beat FX panel

private struct FxPanel: View {
    @EnvironmentObject var engine: Engine
    var body: some View {
        let f = engine.fx
        VStack(spacing: 0) {
            HStack {
                Text("BEAT FX").font(XDJ.font(14, .medium)).foregroundStyle(Color(white: 0.75))
                Text(["CH1", "CH2", "CH3", "CH4", "MST", "SP"][min(5, max(0, f.target))])
                    .font(XDJ.font(11, .bold)).foregroundStyle(XDJ.orange)
                if f.on {
                    Text("ON").font(XDJ.font(11, .bold)).padding(.horizontal, 4)
                        .background(XDJ.red, in: RoundedRectangle(cornerRadius: 2))
                }
            }
            .frame(maxWidth: .infinity).frame(height: 30).background(XDJ.panel2)
            VStack(spacing: 2) {
                HStack(alignment: .firstTextBaseline, spacing: 3) {
                    Text(bpmParts(f.bpm, decimals: 1).0).font(XDJ.font(30)) + Text(bpmParts(f.bpm, decimals: 1).1).font(XDJ.font(19))
                    VStack(spacing: 0) {
                        Text("AUTO").font(XDJ.font(8, .heavy)).foregroundStyle(.black)
                            .padding(.horizontal, 2).background(Color.white)
                        Text("BPM").font(XDJ.font(9, .medium))
                    }
                }
                Text(f.name).font(XDJ.font(f.name.count > 9 ? 22 : 29, .medium))
                    .foregroundStyle(f.on ? XDJ.orange : .white)
                    .lineLimit(1).minimumScaleFactor(0.6)
            }
            .frame(maxWidth: .infinity).frame(height: 104)
            .overlay(Rectangle().stroke(XDJ.line, lineWidth: 1))
            .contentShape(Rectangle())
            .onTapGesture { engine.post(DJC_FX_ON, i: 2) }
            HStack(spacing: 0) {
                ForEach(0..<9, id: \.self) { i in beatCell(i, current: f.beatIndex) }
            }
            .frame(height: 26)
            // frequency select (LOW / MID / HI): which bands the Beat FX works on
            HStack(spacing: 4) {
                ForEach(Array(["LOW", "MID", "HI"].enumerated()), id: \.offset) { i, name in
                    let on = f.bands & (1 << i) != 0
                    Text(name).font(XDJ.font(20, .semibold))
                        .frame(maxWidth: .infinity).frame(height: 60)
                        .foregroundStyle(on ? Color.white : XDJ.blue)
                        .background(on ? XDJ.blue : Color(white: 0.12))
                        .overlay(Rectangle().stroke(XDJ.blue, lineWidth: 1.5))
                        .contentShape(Rectangle())
                        .onTapGesture { engine.post(DJC_FX_BAND, i: Int32(i)) }
                }
            }
            .padding(5)
            // LEVEL/DEPTH
            GeometryReader { g in
                ZStack(alignment: .leading) {
                    Rectangle().fill(Color(white: 0.2))
                    Rectangle().fill(Color(red: 0.1, green: 0.62, blue: 0.95)).frame(width: g.size.width * CGFloat(f.level))
                }
            }
            .frame(height: 6)
            ColorFxButtons()
            Spacer(minLength: 0)
        }
        .background(Color(white: 0.06))
        .overlay(Rectangle().stroke(XDJ.line, lineWidth: 1))
    }

    private func beatCell(_ idx: Int, current: Int) -> some View {
        let beats: [Double] = [0.25, 0.5, 0.75, 1, 2, 4, 8, 16, 32]
        let sel = idx == current
        return Text(beatLabel(beats[idx]))
            .font(XDJ.font(10, sel ? .bold : .regular))
            .minimumScaleFactor(0.7).lineLimit(1)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .foregroundStyle(sel ? Color.black : Color(white: 0.85))
            .background(sel ? Color(white: 0.85) : .clear)
            .overlay(Rectangle().stroke(XDJ.line, lineWidth: 0.5))
            .contentShape(Rectangle())
            .onTapGesture { engine.post(DJC_FX_BEAT, i: Int32(idx - current)) }
    }
}

/// Sound Colour FX selector: FILTER / DUB ECHO / SWEEP / SPACE. The active one flashes while colour FX is on.
/// GRV6: SOUND COLOR FX ON = next effect, SHIFT + ON = on/off.
private struct ColorFxButtons: View {
    @EnvironmentObject var engine: Engine
    private let names = ["FILTER", "DUB ECHO", "SWEEP", "SPACE"]
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("COLOR FX").font(XDJ.font(14, .medium)).foregroundStyle(Color(white: 0.75))
                if engine.colorFxOn {
                    Text("ON").font(XDJ.font(11, .bold)).padding(.horizontal, 4)
                        .background(XDJ.red, in: RoundedRectangle(cornerRadius: 2))
                }
            }
            .frame(maxWidth: .infinity).frame(height: 30).background(XDJ.panel2)
            .contentShape(Rectangle())
            .onTapGesture { engine.post(DJC_COLOR_FX_ON, i: 2) }
            TimelineView(.periodic(from: .now, by: 0.4)) { tl in
                let blinkOn = Int(tl.date.timeIntervalSinceReferenceDate / 0.4) % 2 == 0
                Grid(horizontalSpacing: 4, verticalSpacing: 4) {
                    GridRow { cell(0, blinkOn); cell(1, blinkOn) }
                    GridRow { cell(2, blinkOn); cell(3, blinkOn) }
                }
                .padding(5)
            }
            VStack(spacing: 0) {
                HStack {
                    Text("PARAMETER").font(XDJ.font(11, .semibold)).foregroundStyle(Color(white: 0.75))
                    Spacer()
                    Text(String(format: "%.0f", engine.colorFxParam * 100)).font(XDJ.font(12))
                }
                Slider(value: $engine.colorFxParam, in: 0...1).tint(.white)
            }
            .padding(.horizontal, 8).padding(.bottom, 4)
        }
    }

    private func cell(_ i: Int, _ blinkOn: Bool) -> some View {
        // black and white like the hardware: the selected effect flashes white while colour FX is on
        let active = engine.colorFxType == i
        let lit = active && (engine.colorFxOn ? blinkOn : true)
        return Text(names[i]).font(XDJ.font(13, .semibold))
            .lineLimit(1).minimumScaleFactor(0.7)
            .frame(maxWidth: .infinity).frame(height: 66)
            .foregroundStyle(lit ? Color.black : .white)
            .background(lit ? Color.white : Color.black)
            .overlay(Rectangle().stroke(Color.white.opacity(active ? 1 : 0.5), lineWidth: active ? 2 : 1))
            .contentShape(Rectangle())
            .onTapGesture { engine.post(DJC_COLOR_FX_TYPE, i: Int32(i)) }
    }
}

// MARK: - HOT CUE list (two rows of four, rekordbox comments)

private struct HotCueList: View {
    @EnvironmentObject var engine: Engine
    let deck: Int
    var body: some View {
        let d = engine.decks[deck]
        let info = engine.deckTracks[deck]
        let mode = engine.padModes[deck]
        let title = [2: "BEAT JUMP", 6: "BEAT LOOP", 7: "KEY SHIFT"][mode] ?? "HOT CUE"
        VStack(spacing: 4) {
            HStack(spacing: 10) {
                Rectangle().fill(XDJ.line).frame(height: 1)
                Text(title).font(XDJ.font(13, .medium)).foregroundStyle(Color(white: 0.7)).fixedSize()
                Rectangle().fill(XDJ.line).frame(height: 1)
            }
            .frame(height: 14)
            Grid(horizontalSpacing: 8, verticalSpacing: 4) {
                ForEach(0..<2, id: \.self) { r in
                    GridRow {
                        ForEach(0..<4, id: \.self) { c in
                            if mode == 2 || mode == 6 || mode == 7 { padCell(r * 4 + c, mode: mode, d: d) }
                            else { cell(r * 4 + c, d: d, info: info) }
                        }
                    }
                }
            }
        }
    }

    /// Pad layout for BEAT JUMP / BEAT LOOP / KEY SHIFT, same order as the pads (top row 1-4, bottom 5-8).
    private func padCell(_ i: Int, mode: Int, d: DeckView) -> some View {
        let jump: [Double] = [-1, 1, -2, 2, -4, 4, -8, 8]
        let loops: [Double] = [0.25, 0.5, 1, 2, 4, 8, 16, 32]
        let label: String
        var lit = false
        switch mode {
        case 2:
            let b = Int(abs(jump[i]))
            label = jump[i] < 0 ? "◀  \(b)" : "\(b)  ▶"
        case 6:
            label = beatLabel(loops[i])
            if d.loopActive, d.trackBpm > 0 { lit = abs((d.loopOut - d.loopIn) * d.trackBpm / 60 - loops[i]) < 0.01 }
        default:
            let k = i < 4 ? i + 1 : -(i - 3)
            label = String(format: "%+d", k)
            lit = d.keyShift == k
        }
        return Text(label).font(XDJ.font(15, .semibold))
            .frame(maxWidth: .infinity).frame(height: 24)
            .foregroundStyle(lit ? Color.black : .white)
            .background(lit ? XDJ.blue : XDJ.panel)
            .contentShape(Rectangle())
            .onTapGesture {
                switch mode {
                case 2: engine.post(DJC_BEAT_JUMP, deck: deck, v: jump[i])
                case 6: engine.post(DJC_BEAT_LOOP, deck: deck, v: loops[i])
                default: engine.post(DJC_KEY_SHIFT, deck: deck, v: Double(i < 4 ? i + 1 : -(i - 3)))
                }
            }
    }

    private func cell(_ i: Int, d: DeckView, info: TrackInfo?) -> some View {
        let set = d.hotcues[i] >= 0
        let col = info?.hotColor(i) ?? Color.hotcueColors[i]
        let name = info?.hotComment(i) ?? ""
        return HStack(spacing: 7) {
            Text(["A", "B", "C", "D", "E", "F", "G", "H"][i]).font(XDJ.font(13, .medium))
                .foregroundStyle(set ? Color.black : XDJ.grey)
                .frame(width: 17, height: 17)
                .background(set ? col : Color(white: 0.25))
            Text(set ? (name.isEmpty ? timeShort(d.hotcues[i]) : name) : "")
                .font(XDJ.font(14)).lineLimit(1)
            Spacer(minLength: 0)
        }
        .padding(.leading, 2)
        .frame(maxWidth: .infinity).frame(height: 24)
        .background(XDJ.panel)
        .contentShape(Rectangle())
        .onTapGesture { engine.post(DJC_HOTCUE, deck: deck, i: Int32(i), v: 1) }
    }

    private func timeShort(_ s: Double) -> String { String(format: "%02d:%02d", Int(s) / 60, Int(s) % 60) }
}

// MARK: - Deck card

private struct DeckCard: View {
    @EnvironmentObject var engine: Engine
    let deck: Int
    @State private var showGrid = false

    var body: some View {
        let d = engine.decks[deck]
        let info = engine.deckTracks[deck]
        let active = deck == engine.leftDeck || deck == engine.rightDeck
        let onAir = d.playing && engine.mixer[deck].fader > 0.01
        HStack(spacing: 2) {
            VStack(spacing: 0) {
                HStack(spacing: 0) {
                    VStack(spacing: -3) {
                        Text("DECK").font(XDJ.font(9, .semibold))
                        Text("\(deck + 1)").font(XDJ.font(22, .medium))
                    }
                    .frame(width: 44, height: 36)
                    .background(active ? XDJ.blue : Color(white: 0.35))
                    .onTapGesture { // touch alternative to the GRV6 DECK buttons: 1 <-> 3, 2 <-> 4
                        if deck % 2 == 0 { engine.leftDeck = deck == 0 ? 2 : 0 } else { engine.rightDeck = deck == 1 ? 3 : 1 }
                    }
                    Image(systemName: "music.note").font(.system(size: 17)).padding(.leading, 12)
                    Text(info?.title ?? (engine.loading.contains(deck) ? "Loading…" : ""))
                        .font(XDJ.font(19)).lineLimit(1).padding(.leading, 6)
                    Spacer(minLength: 4)
                    if onAir {
                        Text("ON AIR").font(XDJ.font(14, .semibold)).padding(.horizontal, 6).frame(height: 22)
                            .background(XDJ.red)
                    }
                }
                .frame(height: 36)
                .background(XDJ.panel2)
                HStack(spacing: 0) {
                    VStack(alignment: .leading, spacing: 0) {
                        (Text("REMAIN").foregroundStyle(.white) + Text(" / TIME").foregroundStyle(XDJ.grey))
                            .font(XDJ.font(12, .medium))
                        Text(remain(d)).font(.system(size: 41, weight: .light).monospacedDigit())
                    }
                    .frame(width: 128, alignment: .leading)
                    .padding(.leading, 8)
                    MiniOverview(deck: deck, flags: true)
                        .padding(.trailing, 6)
                }
                .frame(maxHeight: .infinity)
            }
            .background(Color(white: 0.07))
            BpmBox(d: d, info: info)
                .frame(width: 98)
                .onTapGesture { showGrid = true }
        }
        .frame(height: 124)
        .sheet(isPresented: $showGrid) { GridSheet(index: deck).environmentObject(engine) }
    }

    private func remain(_ d: DeckView) -> String {
        guard d.loaded else { return "" }
        let r = max(0, d.duration - d.position)
        return String(format: "%02d:%02d", Int(r) / 60, Int(r) % 60)
    }
}

private struct BpmBox: View {
    let d: DeckView
    let info: TrackInfo?
    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 4) {
                    Text("BPM").font(XDJ.font(12, .medium))
                    Spacer(minLength: 0)
                    if d.master {
                        Text("MASTER").font(XDJ.font(12, .medium)).foregroundStyle(.black)
                            .padding(.horizontal, 3).background(XDJ.orange)
                    }
                }
                HStack(alignment: .firstTextBaseline, spacing: 0) {
                    Spacer(minLength: 0)
                    Text(d.loaded ? bpmParts(d.bpm).0 : "--")
                        .font(.system(size: 35, weight: .regular).monospacedDigit())
                    Text(d.loaded ? bpmParts(d.bpm).1 : ".-")
                        .font(.system(size: 22).monospacedDigit())
                }
                .foregroundStyle(d.master ? XDJ.orange : .white)
            }
            .padding(.horizontal, 5).padding(.vertical, 3)
            .frame(height: 64)
            .overlay(Rectangle().stroke(d.master ? XDJ.orange : .white, lineWidth: 1.5))
            VStack(spacing: 1) {
                HStack(spacing: 10) {
                    Text("Q").font(XDJ.font(15, .medium)).foregroundStyle(d.quantize ? XDJ.red : XDJ.grey)
                    Text("MT").font(XDJ.font(15, .bold)).foregroundStyle(d.keylock ? XDJ.red : XDJ.grey)
                    Text(d.range >= 1 ? "WIDE" : String(format: "±%.0f", d.range * 100))
                        .font(XDJ.font(16, .medium)).foregroundStyle(d.range >= 1 ? XDJ.red : XDJ.green)
                }
                Text(d.loaded ? String(format: "%+.2f%%", (d.rate - 1) * 100) : "")
                    .font(XDJ.font(14)).foregroundStyle(Color(white: 0.75))
            }
            .frame(maxHeight: .infinity)
        }
    }
}

/// Overall waveform (drawn once per track), hot cue flags, cue markers, playhead and phrase bar.
struct MiniOverview: View {
    @EnvironmentObject var engine: Engine
    let deck: Int
    var flags = true

    var body: some View {
        let d = engine.decks[deck]
        let info = engine.deckTracks[deck]
        let dur = max(d.duration, 0.001)
        GeometryReader { g in
            let w = g.size.width
            let flagH: CGFloat = flags ? 17 : 0
            let phraseH: CGFloat = 12
            let waveH = g.size.height - flagH - phraseH - 3
            ZStack(alignment: .topLeading) {
                if let info {
                    StaticOverview(info: info, kind: engine.waveKind).equatable()
                        .frame(width: w, height: waveH)
                        .offset(y: flagH)
                    // played part dimmed
                    Rectangle().fill(Color.black.opacity(0.5))
                        .frame(width: max(0, CGFloat(d.position / dur) * w), height: waveH)
                        .offset(y: flagH)
                    PhraseBar(info: info, duration: d.duration)
                        .frame(width: w, height: phraseH)
                        .offset(y: flagH + waveH + 3)
                    if flags {
                        ForEach(0..<8, id: \.self) { i in
                            if d.hotcues[i] >= 0 {
                                Text(["A", "B", "C", "D", "E", "F", "G", "H"][i]).font(XDJ.font(12, .medium))
                                    .foregroundStyle(.black)
                                    .frame(width: 15, height: 16)
                                    .background(info.hotColor(i))
                                    .offset(x: CGFloat(d.hotcues[i] / dur) * w - 1)
                            }
                        }
                    }
                    ForEach(Array(info.cues.filter { $0.hot == 0 }.enumerated()), id: \.offset) { _, c in
                        Triangle().fill(XDJ.red).frame(width: 12, height: 9).rotationEffect(.degrees(180))
                            .offset(x: CGFloat(c.time / dur) * w - 6, y: flagH)
                    }
                    Triangle().fill(Color.white).frame(width: 12, height: 9)
                        .offset(x: CGFloat(d.cue / dur) * w - 6, y: flagH + waveH + phraseH - 6)
                    Rectangle().fill(Color.white).frame(width: 2.5, height: waveH + 4)
                        .offset(x: CGFloat(d.position / dur) * w - 1, y: flagH - 2)
                }
            }
            .contentShape(Rectangle())
            .onTapGesture { loc in if d.loaded, !d.playing { engine.post(DJC_SEEK, deck: deck, v: Double(loc.x / w) * dur) } }
        }
    }
}

/// The static part of the overview. Equatable so SwiftUI only redraws it when the track or colour mode changes.
private struct StaticOverview: View, Equatable {
    let info: TrackInfo
    let kind: WaveKind
    static func == (a: StaticOverview, b: StaticOverview) -> Bool { a.info.id == b.info.id && a.kind == b.kind }

    var body: some View {
        Canvas { ctx, size in
            guard let kw = info.wave(kind, overview: true) else { return }
            let (k, ov) = kw
            let n = ov.count
            let colW = size.width / CGFloat(n)
            // overview is drawn bottom-up like the hardware (one-sided)
            WaveRenderer.draw(ctx, kind: k, data: ov, k0: 0, k1: n - 1, binsPerCol: 1, colWidth: colW,
                              mid: size.height, scale: 1) { i in CGFloat(i) * colW }
        }
        .clipped()
        .drawingGroup()
    }
}

/// Phrase bar (rekordbox PSSI), 30-second ticks when the track has no phrases.
private struct PhraseBar: View {
    let info: TrackInfo?
    let duration: Double
    var body: some View {
        Canvas { ctx, size in
            guard duration > 0 else { return }
            func x(_ t: Double) -> CGFloat { CGFloat(t / duration) * size.width }
            if let info, !info.phrases.isEmpty {
                for p in info.phrases {
                    let r = CGRect(x: x(p.start), y: 0, width: max(1, x(p.end) - x(p.start) - 1), height: size.height)
                    ctx.fill(Path(r), with: .color(PhraseStyle.color(mood: info.mood, kind: p.kind)))
                }
            } else {
                ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(white: 0.25)))
                var t = 30.0
                while t < duration {
                    ctx.fill(Path(CGRect(x: x(t), y: 0, width: 1, height: size.height)), with: .color(.gray))
                    t += 30
                }
            }
        }
    }
}

// MARK: - Browse screen

private struct BrowseScreen: View {
    @EnvironmentObject var engine: Engine
    @Binding var importing: Bool
    @StateObject private var previews = PreviewCache()
    @State private var loadTarget: RBTrack?

    var body: some View {
        HStack(spacing: 0) {
            CategoryRail().frame(width: 96)
            Rectangle().fill(Color(white: 0.25)).frame(width: 2)
            VStack(spacing: 0) {
                header.frame(height: 56)
                list
            }
        }
        .confirmationDialog(loadTarget?.title ?? "", isPresented: Binding(get: { loadTarget != nil },
                                                                          set: { if !$0 { loadTarget = nil } })) {
            ForEach(0..<4, id: \.self) { d in
                Button("Load to deck \(d + 1)") { if let t = loadTarget, !engine.loadLocked(d) { engine.loadUSB(t, deck: d); engine.showBrowser = false } }
            }
        }
    }

    private var header: some View {
        HStack(spacing: 0) {
            if engine.stack.count > 1 {
                Image(systemName: "chevron.left").font(.system(size: 20, weight: .semibold))
                    .frame(width: 50, height: 56).onTapGesture { engine.back() }
            }
            Text("Preview").font(XDJ.font(19)).foregroundStyle(Color(white: 0.75))
                .frame(width: engine.stack.count > 1 ? 190 : 240, alignment: .leading).padding(.leading, 12)
            Rectangle().fill(Color(white: 0.25)).frame(width: 1)
            HStack(spacing: 10) {
                HStack(spacing: 10) {
                    Text(engine.levelTitle == engine.category.rawValue ? "Track" : engine.levelTitle).lineLimit(1)
                    sortArrow(.title)
                }
                .foregroundStyle(engine.sortKey == .title || engine.sortKey == .none ? XDJ.blue : Color(white: 0.8))
                .contentShape(Rectangle())
                .onTapGesture { engine.toggleSort(.title) }
                Text(String(format: "[%04d]", engine.items.count)).foregroundStyle(Color(white: 0.85))
                Spacer()
                if engine.category == .files {
                    Text("+ ADD").font(XDJ.font(15, .semibold)).foregroundStyle(XDJ.blue)
                        .padding(.horizontal, 10).frame(height: 34)
                        .overlay(RoundedRectangle(cornerRadius: 2).stroke(XDJ.line))
                        .onTapGesture { importing = true }
                }
            }
            .font(XDJ.font(19)).padding(.horizontal, 12)
            Rectangle().fill(Color(white: 0.25)).frame(width: 1)
            colHead("BPM", .bpm).frame(width: 112)
            Rectangle().fill(Color(white: 0.25)).frame(width: 1)
            colHead("Key", .key).frame(width: 92)
        }
        .background(Color(white: 0.16))
    }

    private func colHead(_ s: String, _ k: SortKey) -> some View {
        HStack(spacing: 8) {
            Text(s).font(XDJ.font(19))
            sortArrow(k)
        }
        .foregroundStyle(engine.sortKey == k ? XDJ.blue : Color(white: 0.8))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .contentShape(Rectangle())
        .onTapGesture { engine.toggleSort(k) }
    }

    private func sortArrow(_ k: SortKey) -> some View {
        let down = engine.sortKey == k && !engine.sortAscending
        return Image(systemName: "triangle.fill").font(.system(size: 12)).rotationEffect(.degrees(down ? 180 : 0))
    }

    @ViewBuilder private var list: some View {
        if engine.items.isEmpty {
            VStack(spacing: 14) {
                Spacer()
                Text(emptyText).font(XDJ.font(18)).foregroundStyle(XDJ.grey).multilineTextAlignment(.center)
                if engine.usb == nil && engine.category != .files {
                    BoxLabel(text: "CONNECT USB").onTapGesture { engine.showSource = true }
                }
                Spacer()
            }
            .frame(maxWidth: .infinity)
        } else {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(Array(engine.items.enumerated()), id: \.element.id) { idx, item in
                            BrowseRow(item: item, index: idx, selected: idx == engine.selection, previews: previews,
                                      onLongPress: { if case .track(let t) = item { loadTarget = t } })
                                .id(item.id)
                                .onTapGesture {
                                    if engine.selection == idx || isContainer(item) { engine.selection = idx; engine.enterSelection() }
                                    else { engine.selection = idx }
                                }
                        }
                    }
                }
                .onChange(of: engine.selection) { _, v in
                    if engine.items.indices.contains(v) { proxy.scrollTo(engine.items[v].id) }
                }
            }
        }
    }

    private var emptyText: String {
        if engine.category == .files { return "No files yet. Tap + ADD to pick audio from the Files app." }
        guard let usb = engine.usb else { return "No USB stick connected." }
        return usb.ready ? usb.status : "Reading \(usb.name)…"
    }

    private func isContainer(_ i: BrowseItem) -> Bool {
        switch i { case .folder, .playlist, .group: return true; default: return false }
    }
}

private struct CategoryRail: View {
    @EnvironmentObject var engine: Engine
    private let cats: [(BrowseCategory, String, String)] = [
        (.artist, "ARTIST", "person"), (.album, "ALBUM", "smallcircle.filled.circle"), (.track, "TRACK", "music.note"),
        (.key, "KEY", "number.square"), (.playlist, "PLAYLIST", "list.bullet.rectangle"), (.files, "FOLDER", "folder"),
    ]
    var body: some View {
        VStack(spacing: 2) {
            ForEach(cats, id: \.1) { c in
                VStack(spacing: 6) {
                    Image(systemName: c.2).font(.system(size: 30, weight: .light))
                    Text(c.1).font(XDJ.font(13, .medium))
                }
                .frame(maxWidth: .infinity).frame(height: 84)
                .background(engine.category == c.0 ? Color(white: 0.30) : Color(white: 0.17))
                .contentShape(Rectangle())
                .onTapGesture { engine.category = c.0 }
            }
            VStack(spacing: 6) {
                Image(systemName: "externaldrive.fill").font(.system(size: 28, weight: .light))
                Text(engine.usb?.label ?? "USB").font(XDJ.font(13, .medium))
            }
            .frame(maxWidth: .infinity).frame(maxHeight: .infinity)
            .foregroundStyle(engine.usb == nil ? XDJ.grey : XDJ.green)
            .background(Color(white: 0.17))
            .onTapGesture { engine.showSource = true }
        }
    }
}

private struct BrowseRow: View {
    @EnvironmentObject var engine: Engine
    let item: BrowseItem
    let index: Int
    let selected: Bool
    @ObservedObject var previews: PreviewCache
    var onLongPress: () -> Void

    var body: some View {
        HStack(spacing: 0) {
            switch item {
            case .track(let t): trackRow(t)
            case .file(let i): fileRow(i)
            case .folder(let p): containerRow("folder.fill", p.name)
            case .playlist(let p): containerRow("music.note.list", p.name)
            case .group(let g): containerRow("chevron.right.circle", g)
            }
        }
        .frame(height: 52)
        .background(selected ? XDJ.selBlue : (index % 2 == 0 ? Color(white: 0.11) : Color.black))
        .contentShape(Rectangle())
        .onLongPressGesture(minimumDuration: 0.5) { onLongPress() }
    }

    private func trackRow(_ t: RBTrack) -> some View {
        let loaded = (0..<4).contains { engine.deckTracks[$0]?.rbId == t.id && engine.deckTracks[$0]?.source == (engine.usb?.label ?? "") }
        let played = loaded || engine.played.contains(Engine.playedKey(source: engine.usb?.label ?? "", id: t.id))
        let compat = engine.keyCompatible(t.key)
        return HStack(spacing: 0) {
            ZStack {
                if let p = previews.preview(for: t, usb: engine.usb) {
                    RowPreview(data: p)
                } else {
                    Color(white: 0.05)
                }
            }
            .frame(width: 190, height: 50)
            ZStack {
                if let img = previews.artwork(for: t, usb: engine.usb) {
                    Image(uiImage: img).resizable().scaledToFill()
                } else {
                    Color(white: 0.2)
                    Image(systemName: "music.note").foregroundStyle(XDJ.grey)
                }
            }
            .frame(width: 50, height: 50).clipped()
            HStack(spacing: 10) {
                Image(systemName: loaded ? "play.fill" : "music.note").font(.system(size: loaded ? 18 : 22))
                    .foregroundStyle(played ? XDJ.green : .white).frame(width: 26)
                Text(t.title).lineLimit(1).foregroundStyle(played ? XDJ.green : .white)
                Spacer(minLength: 0)
            }
            .font(XDJ.font(26)).padding(.leading, 12)
            Text(t.bpm > 0 ? String(format: "%.1f", t.bpm) : "")
                .font(XDJ.font(26)).foregroundStyle(played ? XDJ.green : .white)
                .frame(width: 112, alignment: .trailing).padding(.trailing, 12)
            Text(t.key).font(XDJ.font(26)).foregroundStyle(compat ? Color.black : .white)
                .frame(width: 92, height: 52, alignment: .leading).padding(.leading, 10)
                .background(compat ? XDJ.green : .clear)
        }
    }

    private func fileRow(_ i: Int) -> some View {
        let t = engine.library.indices.contains(i) ? engine.library[i] : nil
        let loaded = (0..<4).contains { engine.deckTracks[$0]?.url == t?.url }
        return HStack(spacing: 10) {
            Image(systemName: loaded ? "play.fill" : "music.note").foregroundStyle(loaded ? XDJ.green : .white).frame(width: 26)
            Text(t?.title ?? "").lineLimit(1).foregroundStyle(loaded ? XDJ.green : .white)
            Spacer()
        }
        .font(XDJ.font(26)).padding(.leading, 252)
    }

    private func containerRow(_ icon: String, _ name: String) -> some View {
        HStack(spacing: 14) {
            Image(systemName: icon).font(.system(size: 22)).foregroundStyle(Color(white: 0.8)).frame(width: 30)
            Text(name).font(XDJ.font(26)).lineLimit(1)
            Spacer()
        }
        .padding(.leading, 20)
    }
}

private struct RowPreview: View, Equatable {
    let data: WaveData
    var body: some View {
        Canvas { ctx, size in
            let n = data.count
            guard n > 0 else { return }
            let colW = size.width / CGFloat(n)
            WaveRenderer.draw(ctx, kind: .threeBand, data: data, k0: 0, k1: n - 1, binsPerCol: 1, colWidth: colW,
                              mid: size.height, scale: 0.95) { i in CGFloat(i) * colW }
        }
        .clipped()
    }
}

/// Small waveform previews and artwork for browse rows, loaded off the main thread and cached.
@MainActor
final class PreviewCache: ObservableObject {
    private var waves: [UInt32: WaveData] = [:]
    private var images: [UInt32: UIImage] = [:]
    private var pending: Set<UInt32> = []
    private let queue = DispatchQueue(label: "previews", qos: .utility)

    func preview(for t: RBTrack, usb: RekordboxUSB?) -> WaveData? {
        if let w = waves[t.id] { return w }
        request(t, usb: usb)
        return nil
    }

    func artwork(for t: RBTrack, usb: RekordboxUSB?) -> UIImage? {
        images[t.id]
    }

    private func request(_ t: RBTrack, usb: RekordboxUSB?) {
        guard let usb, !pending.contains(t.id), !t.analyzePath.isEmpty else { return }
        pending.insert(t.id)
        let anlz = usb.path(t.analyzePath)
        let art = t.artworkPath.isEmpty ? nil : usb.path(t.artworkPath)
        let id = t.id
        queue.async {
            let wave: WaveData? = {
                guard let a = djc_anlz_open(anlz) else { return nil }
                defer { djc_anlz_close(a) }
                let n = Int(djc_anlz_wave_len(a, 7))
                guard n > 0 else { return nil }
                var c: [[Float]] = []
                for ch in 1...3 {
                    if let p = djc_anlz_wave_data(a, 7, Int32(ch)) { c.append(Array(UnsafeBufferPointer(start: p, count: n))) }
                }
                guard c.count == 3 else { return nil }
                let h = (0..<n).map { max(c[0][$0], c[1][$0], c[2][$0]) }
                let mx = max(h.max() ?? 1, 1e-6)
                return WaveData(h: h.map { $0 / mx }, c: c.map { $0.map { $0 / mx } }).downsampled(to: 120)
            }()
            let img = art.flatMap { UIImage(contentsOfFile: $0) }
            Task { @MainActor in
                if let wave { self.waves[id] = wave }
                if let img { self.images[id] = img }
                self.objectWillChange.send()
            }
        }
    }
}

// MARK: - Deck strip under the browser (order 3 1 2 4, like the hardware)

private struct DeckStrip: View {
    @EnvironmentObject var engine: Engine
    var body: some View {
        HStack(spacing: 4) {
            ForEach([2, 0, 1, 3], id: \.self) { MiniDeck(deck: $0) }
        }
        .padding(.top, 4)
    }
}

private struct MiniDeck: View {
    @EnvironmentObject var engine: Engine
    let deck: Int
    var body: some View {
        let d = engine.decks[deck]
        let info = engine.deckTracks[deck]
        let active = deck == engine.leftDeck || deck == engine.rightDeck
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Text("\(deck + 1)").font(XDJ.font(16, .medium)).frame(width: 22, height: 24)
                    .background(active ? XDJ.blue : Color(white: 0.35))
                Image(systemName: "music.note").font(.system(size: 13))
                Text(info?.title ?? "").font(XDJ.font(15)).lineLimit(1)
                Spacer(minLength: 2)
                Text(info?.key ?? "").font(XDJ.font(18)).foregroundStyle(engine.keyCompatible(info?.key ?? "") ? XDJ.green : .white)
                    .padding(.trailing, 6)
            }
            .frame(height: 26).background(XDJ.panel2)
            HStack(spacing: 4) {
                VStack(alignment: .leading, spacing: 0) {
                    Text("REMAIN /\nTIME").font(XDJ.font(11, .medium)).foregroundStyle(Color(white: 0.8))
                    Text(d.loaded ? String(format: "%02d:%02d", Int(max(0, d.duration - d.position)) / 60,
                                           Int(max(0, d.duration - d.position)) % 60) : "")
                        .font(.system(size: 26, weight: .light).monospacedDigit())
                }
                .frame(width: 92, alignment: .leading)
                VStack(alignment: .trailing, spacing: 0) {
                    HStack(alignment: .firstTextBaseline, spacing: 3) {
                        Text("BPM").font(XDJ.font(10, .medium))
                        Text(d.loaded ? String(format: "%.1f", d.bpm) : "").font(XDJ.font(22))
                    }
                    .foregroundStyle(d.master ? XDJ.orange : .white)
                    MiniOverview(deck: deck, flags: true)
                }
            }
            .padding(.horizontal, 6).frame(maxHeight: .infinity)
        }
        .background(Color(white: 0.07))
    }
}

func beatLabel(_ b: Double) -> String {
    let fr: [(Double, String)] = [(0.25, "1/4"), (0.5, "1/2"), (0.75, "3/4")]
    for (v, s) in fr where abs(b - v) < 0.01 { return s }
    return b == b.rounded() ? String(Int(b)) : String(format: "%.2f", b)
}

/// Engine position extrapolated to "now" from the last snapshot, wrapped inside an active loop.
func predictedPosition(_ d: DeckView) -> Double {
    var pos = d.position
    guard d.playing else { return pos }
    pos += d.rate * max(0, min(0.25, djc_now() - d.stamp))
    if d.loopActive, d.loopIn >= 0, d.loopOut > d.loopIn, pos >= d.loopOut {
        pos = d.loopIn + (pos - d.loopIn).truncatingRemainder(dividingBy: d.loopOut - d.loopIn)
    }
    return pos
}

/// Phrase names from the PSSI enums (Deep Symmetry / crate-digger rekordbox_anlz.ksy). Colours are our own palette.
enum PhraseStyle {
    static func label(mood: Int, kind: Int) -> String {
        switch mood {
        case 1: return [1: "Intro", 2: "Up", 3: "Down", 5: "Chorus", 6: "Outro"][kind] ?? "?"
        case 3:
            switch kind {
            case 1: return "Intro"
            case 2...4: return "Verse 1"
            case 5...7: return "Verse 2"
            case 8: return "Bridge"
            case 9: return "Chorus"
            case 10: return "Outro"
            default: return "?"
            }
        default:
            switch kind {
            case 1: return "Intro"
            case 2...7: return "Verse \(kind - 1)"
            case 8: return "Bridge"
            case 9: return "Chorus"
            case 10: return "Outro"
            default: return "?"
            }
        }
    }

    static func color(mood: Int, kind: Int) -> Color {
        let l = label(mood: mood, kind: kind)
        switch l {
        case "Intro": return Color(red: 0.25, green: 0.70, blue: 0.75)
        case "Up": return Color(red: 0.55, green: 0.75, blue: 0.25)
        case "Down", "Bridge": return Color(red: 0.55, green: 0.40, blue: 0.85)
        case "Chorus": return Color(red: 0.90, green: 0.35, blue: 0.35)
        case "Outro": return Color(red: 0.35, green: 0.45, blue: 0.70)
        case "?": return Color.gray
        default: return Color(red: 0.75, green: 0.65, blue: 0.25) // verses
        }
    }
}

// MARK: - USB source sheet

/// SOURCE, full screen like the XDJ-AZ: devices on the left (IPAD = the library copied into the app, then every
/// stick that is plugged in), INFO on the right. Tap a device to highlight it, tap again (or ›) to browse it.
/// Controller: DISCOVER opens / closes it, BROWSE knob moves, press opens, BACK closes.
struct SourceScreen: View {
    @EnvironmentObject var engine: Engine
    @State private var picking = false
    @State private var confirmDelete = false

    private var current: RekordboxUSB? {
        engine.sources.indices.contains(engine.sourceIndex) ? engine.sources[engine.sourceIndex] : engine.sources.first
    }

    var body: some View {
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                columnHeader("DEVICE NAME")
                ScrollView {
                    VStack(spacing: 0) {
                        ForEach(Array(engine.sources.enumerated()), id: \.element.id) { i, lib in
                            SourceRow(lib: lib, highlighted: i == engine.sourceIndex, browsing: engine.usb === lib) {
                                engine.openSource(lib)
                            }
                            .contentShape(Rectangle())
                            .onTapGesture {
                                if i == engine.sourceIndex { engine.openSource(lib) } else { engine.sourceIndex = i }
                            }
                        }
                        if engine.sources.isEmpty {
                            Text("No library yet. Plug in your rekordbox USB stick.")
                                .font(XDJ.font(20)).foregroundStyle(XDJ.grey).padding(30)
                        }
                    }
                }
                Spacer(minLength: 0)
                HStack {
                    Button { picking = true } label: {
                        Label("Add a new USB stick", systemImage: "plus.circle").font(XDJ.font(17, .medium))
                    }
                    .foregroundStyle(XDJ.grey)
                    Spacer()
                }
                .padding(.horizontal, 18).padding(.vertical, 14)
            }
            .frame(width: 780)
            Rectangle().fill(XDJ.line).frame(width: 1)
            VStack(spacing: 0) {
                HStack(spacing: 0) {
                    columnHeader("INFO")
                    Button { engine.showSource = false } label: {
                        Image(systemName: "xmark").font(.system(size: 16, weight: .semibold)).frame(width: 44, height: 34)
                    }
                    .buttonStyle(.plain).background(Color(white: 0.16))
                }
                if let lib = current {
                    SourceInfo(lib: lib, confirmDelete: $confirmDelete)
                } else {
                    Spacer()
                }
            }
        }
        .background(XDJ.bg)
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { r in
            if case .success(let url) = r { engine.attachUSB(url); engine.showSource = false; engine.showBrowser = true }
        }
        .confirmationDialog("Remove the library copied to this iPad?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Remove", role: .destructive) { engine.deleteIPadLibrary() }
        }
    }

    private func columnHeader(_ t: String) -> some View {
        HStack { Text(t).font(XDJ.font(15, .medium)).foregroundStyle(XDJ.grey); Spacer() }
            .padding(.horizontal, 18).frame(height: 34).background(Color(white: 0.16))
    }
}

private struct SourceRow: View {
    @ObservedObject var lib: RekordboxUSB
    let highlighted: Bool
    let browsing: Bool
    let onOpen: () -> Void
    var body: some View {
        HStack(spacing: 14) {
            VStack(spacing: 2) {
                Image(systemName: lib.label == "IPAD" ? "ipad.landscape" : "externaldrive.fill").font(.system(size: 20))
                Text(lib.label).font(XDJ.font(12, .bold))
            }
            .frame(width: 58)
            Text(lib.label == "IPAD" ? "Internal library" : lib.displayName)
                .font(XDJ.font(26)).lineLimit(1).layoutPriority(1)
            if browsing {
                Text("BROWSING").font(XDJ.font(11, .bold)).foregroundStyle(.black)
                    .padding(.horizontal, 5).padding(.vertical, 2).background(XDJ.green, in: RoundedRectangle(cornerRadius: 3))
            }
            Spacer()
            if !lib.ready { ProgressView() }
            Button(action: onOpen) {
                Image(systemName: "chevron.right").font(.system(size: 22, weight: .semibold)).frame(width: 54, height: 60)
            }
            .buttonStyle(.plain)
        }
        .padding(.leading, 12)
        .frame(height: 76)
        .background(highlighted ? XDJ.selBlue : Color.clear)
        .overlay(alignment: .bottom) { Rectangle().fill(XDJ.line).frame(height: 1) }
    }
}

private struct SourceInfo: View {
    @EnvironmentObject var engine: Engine
    @ObservedObject var lib: RekordboxUSB
    @Binding var confirmDelete: Bool

    var body: some View {
        let fm = FileManager.default
        let pdbDate = (try? fm.attributesOfItem(atPath: lib.pdbPath)[.modificationDate]) as? Date
        let vol = try? lib.root.resourceValues(forKeys: [.volumeTotalCapacityKey, .volumeAvailableCapacityForImportantUsageKey])
        let gb = { (b: Int64?) -> String in b.map { String(format: "%.1f GB", Double($0) / 1e9) } ?? "-" }
        VStack(spacing: 0) {
            row("Songs", lib.ready ? "\(lib.tracks.count)" : "…")
            row("Playlists", lib.ready ? "\(lib.playlists.filter { !$0.isFolder }.count)" : "…")
            row("Date", pdbDate.map { $0.formatted(.dateTime.year().month(.twoDigits).day(.twoDigits)) } ?? "-")
            row("Total", gb(vol?.volumeTotalCapacity.map(Int64.init)))
            row("Available", gb(vol?.volumeAvailableCapacityForImportantUsage))
            if lib.ready && !lib.formats.contains(.deviceLibrary) {
                Text(lib.status).font(XDJ.font(14)).foregroundStyle(XDJ.orange).padding(16)
            }
            Spacer()
            if lib.label == "IPAD" {
                bigButton("REMOVE FROM IPAD", color: XDJ.red) { confirmDelete = true }
            } else if lib.formats.contains(.deviceLibrary) {
                bigButton(Engine.hasIPadLibrary ? "UPDATE IPAD COPY" : "COPY TO IPAD", color: XDJ.blue) {
                    engine.copyStickToIPad(lib)
                }
                .disabled(engine.copyProgress != nil || !lib.ready)
            }
            if let p = engine.copyProgress { ProgressView(value: p).padding(.horizontal, 16).padding(.top, 8) }
            if !engine.copyStatus.isEmpty {
                Text(engine.copyStatus).font(XDJ.font(14)).foregroundStyle(XDJ.grey).padding(.horizontal, 16).padding(.top, 6)
            }
            Text(lib.label == "IPAD" ? "Plays without a stick." : "Copies tracks, cues, grids and playlists into the app. Only reads the stick.")
                .font(XDJ.font(13)).foregroundStyle(XDJ.grey).multilineTextAlignment(.center).padding(16)
        }
    }

    private func row(_ k: String, _ v: String) -> some View {
        HStack {
            Text(k).font(XDJ.font(18)).foregroundStyle(XDJ.grey)
            Spacer()
            Text(v).font(XDJ.font(18, .medium)).monospacedDigit()
        }
        .padding(.horizontal, 16).frame(height: 46)
        .overlay(alignment: .bottom) { Rectangle().fill(XDJ.line).frame(height: 1) }
    }

    private func bigButton(_ t: String, color: Color, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(t).font(XDJ.font(17, .bold)).frame(maxWidth: .infinity).frame(height: 50)
                .background(RoundedRectangle(cornerRadius: 5).stroke(color, lineWidth: 1.5))
                .foregroundStyle(color)
        }
        .buttonStyle(.plain)
        .padding(.horizontal, 16)
    }
}

private struct USBStatusRow: View {
    @ObservedObject var usb: RekordboxUSB
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Image(systemName: "externaldrive.fill").foregroundStyle(Color.playGreen)
                Text("\(usb.label)  \(usb.name)").font(.headline)
                Spacer()
                if !usb.ready { ProgressView() }
            }
            Text(usb.formats.isEmpty ? "Format: none found" : "Format: " + usb.formats.map(\.rawValue).joined(separator: " + "))
                .font(.subheadline)
            Text(usb.status).font(.subheadline).foregroundStyle(.secondary)
        }
    }
}

// MARK: - Settings

struct SettingsSheet: View {
    @EnvironmentObject var engine: Engine
    @Environment(\.dismiss) var dismiss
    var body: some View {
        NavigationStack {
            Form {
                Section("Audio output") {
                    LabeledContent("Interface", value: "\(engine.outputChannels) ch: \(engine.routeDescription)")
                    Picker("Routing", selection: $engine.routing) {
                        ForEach(Routing.allCases) { Text($0.rawValue).tag($0) }
                    }
                    LabeledContent("DSP load", value: String(format: "%.0f%%  (xruns %d)", engine.cpu * 100, engine.xruns))
                }
                Section("Deck") {
                    Toggle("Load Lock (can't load to a playing deck)", isOn: $engine.loadLock)
                }
                Section("Waveform") {
                    Picker("Colour", selection: $engine.waveKind) {
                        ForEach(WaveKind.allCases) { Text($0.rawValue).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("Same choice as rekordbox Preferences › View › Waveform colour. Falls back to what the track's analysis has.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Jog") {
                    VStack(alignment: .leading) {
                        Text(String(format: "Nudge: %.2f s per turn of the wheel side (max bend ±8%%)", engine.nudgeGain))
                        Slider(value: $engine.nudgeGain, in: 0.03...0.5)
                    }
                    Toggle("Load tracks at the first memory cue (off = first beat / sound)", isOn: $engine.loadAtMemoryCue)
                    Toggle("Align the rekordbox grid to the audio (fixes MP3/AAC offsets)", isOn: $engine.alignGrid)
                    VStack(alignment: .leading) {
                        Text(String(format: "Platter speed (scratch / search): %.0f%%", engine.scratchSpeed * 100))
                        Slider(value: $engine.scratchSpeed, in: 0.5...1.2)
                    }
                }
                Section("Sound Color FX") {
                    Text("DUB ECHO: COLOR left = lows, right = highs. Opening it also stretches the echo from 1/16 to 3/4 beat. PARAMETER = feedback (MIN one repeat, MAX long tail).")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Recording") {
                    Picker("Format", selection: $engine.recFormat) {
                        ForEach(SetRecorder.Format.allCases) { Text($0.rawValue).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("Records the master output. WAV ≈ 1 GB per hour, AAC ≈ 145 MB per hour.")
                        .font(.footnote).foregroundStyle(.secondary)
                    if engine.recordings.isEmpty {
                        Text("No recordings yet").foregroundStyle(.secondary)
                    }
                    ForEach(engine.recordings) { r in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(r.url.deletingPathExtension().lastPathComponent)
                                Text(ByteCountFormatter.string(fromByteCount: r.bytes, countStyle: .file))
                                    .font(.footnote).foregroundStyle(.secondary)
                            }
                            Spacer()
                            ShareLink(item: r.url) { Image(systemName: "square.and.arrow.up") }
                        }
                        .swipeActions { Button("Delete", role: .destructive) { engine.deleteRecording(r) } }
                    }
                }
                Section("Controller") {
                    LabeledContent("Connected", value: engine.controllerName.isEmpty ? "none" : engine.controllerName)
                    LabeledContent("Last unmapped message", value: engine.lastUnmapped.isEmpty ? "—" : engine.lastUnmapped)
                }
            }
            .navigationTitle("Settings")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}

/// XDJ-AZ style message box: our own drawing, neutral styling.
private struct WarningPopup: View {
    let warning: DJWarning
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 20, weight: .bold))
                Text(warning.code.isEmpty ? "WARNING" : warning.code).font(XDJ.font(18, .bold))
                Spacer()
            }
            .foregroundStyle(.black)
            .padding(.horizontal, 16).frame(height: 40)
            .background(XDJ.orange)
            VStack(spacing: 10) {
                Text(warning.title).font(XDJ.font(28, .bold)).foregroundStyle(.white)
                    .multilineTextAlignment(.center).lineLimit(2).minimumScaleFactor(0.6)
                Text(warning.detail).font(XDJ.font(18)).foregroundStyle(Color(white: 0.8))
                    .multilineTextAlignment(.center).lineLimit(3).minimumScaleFactor(0.7)
            }
            .padding(.horizontal, 24).padding(.vertical, 22)
            .frame(maxWidth: .infinity)
        }
        .frame(width: 560)
        .background(Color(white: 0.09), in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(XDJ.orange, lineWidth: 2))
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .shadow(color: .black.opacity(0.7), radius: 30)
    }
}

/// REC: tap to start recording the master output. Stopping asks first, so a stray tap can't end a set recording.
private struct RecButton: View {
    @EnvironmentObject var engine: Engine
    @State private var confirmStop = false
    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.5)) { t in
            let blink = t.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1.0) < 0.5
            HStack(spacing: 7) {
                Circle().fill(engine.recording ? (blink ? XDJ.red : XDJ.red.opacity(0.3)) : Color(white: 0.45))
                    .frame(width: 14, height: 14)
                Text("REC").font(XDJ.font(15, .bold))
                    .foregroundStyle(engine.recording ? XDJ.red : XDJ.grey)
                if engine.recording {
                    Text(elapsed(t.date)).font(XDJ.font(17, .bold)).monospacedDigit().foregroundStyle(.white)
                        .fixedSize()
                }
            }
            .padding(.horizontal, 12).frame(height: 36)
            .fixedSize()
            .background(RoundedRectangle(cornerRadius: 5).stroke(engine.recording ? XDJ.red : XDJ.line, lineWidth: 1.5))
            .contentShape(Rectangle())
            .onTapGesture { if engine.recording { confirmStop = true } else { engine.startRecording() } }
        }
        .confirmationDialog("Stop recording?", isPresented: $confirmStop, titleVisibility: .visible) {
            Button("Stop and save", role: .destructive) { engine.stopRecording() }
            Button("Keep recording", role: .cancel) {}
        }
    }
    private func elapsed(_ now: Date) -> String {
        let s = Int(now.timeIntervalSince(engine.recStart ?? now))
        return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) : String(format: "%02d:%02d", s / 60, s % 60)
    }
}

/// Output level: two horizontal bars (L / R) in the top bar, green / orange / red like the mixer meters.
/// The limiter ceiling is the right end, so red = the limiter is working. Tap for the master EQ.
private struct OutputMeter: View {
    @EnvironmentObject var engine: Engine
    @State private var showEq = false
    var body: some View {
        VStack(spacing: 3) {
            bar(engine.masterL)
            bar(engine.masterR)
        }
        .frame(width: 150)
        .padding(.vertical, 4).padding(.horizontal, 6)
        .background(RoundedRectangle(cornerRadius: 4).fill(Color(white: 0.08)))
        .overlay(alignment: .topTrailing) {
            if engine.masterEq.contains(where: { $0 != 0 }) {
                Text("EQ").font(XDJ.font(9, .bold)).foregroundStyle(.black)
                    .padding(.horizontal, 3).background(XDJ.orange, in: RoundedRectangle(cornerRadius: 2))
                    .offset(x: 6, y: -6)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { showEq = true }
        .popover(isPresented: $showEq) { MasterEqPanel().environmentObject(engine) }
    }

    private func bar(_ level: Float) -> some View {
        let segs = 20
        let db = 20 * log10(max(Double(level), 1e-5))
        // -40 dB ... 0 dB across the bar
        let lit = Int(((db + 40) / 40 * Double(segs)).rounded(.down))
        return HStack(spacing: 2) {
            ForEach(0..<segs, id: \.self) { i in
                let c: Color = i >= segs - 2 ? XDJ.red : (i >= segs - 5 ? XDJ.orange : XDJ.green)
                RoundedRectangle(cornerRadius: 1).fill(i < lit ? c : c.opacity(0.13))
            }
        }
        .frame(height: 9)
    }
}

private struct MasterEqPanel: View {
    @EnvironmentObject var engine: Engine
    private let names = ["LOW  100 Hz", "MID  1 kHz", "HI  10 kHz"]
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("MASTER EQ").font(XDJ.font(17, .bold))
                Spacer()
                Button("Reset") { engine.masterEq = [0, 0, 0] }
                    .disabled(!engine.masterEq.contains(where: { $0 != 0 }))
            }
            ForEach(0..<3, id: \.self) { b in
                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Text(names[b]).font(XDJ.font(14, .medium))
                        Spacer()
                        Text(String(format: "%+.1f dB", engine.masterEq[b])).font(XDJ.font(14)).monospacedDigit()
                    }
                    Slider(value: Binding(get: { engine.masterEq[b] },
                                          set: { v in var q = engine.masterEq; q[b] = (v * 2).rounded() / 2; engine.masterEq = q }),
                           in: -12...6)
                }
            }
            Text("Applied to the master output and the recording, before the limiter.")
                .font(.footnote).foregroundStyle(.secondary)
        }
        .padding(20)
        .frame(width: 340)
    }
}
