import DJCore
import SwiftUI


/// Overall waveform with playhead, cue, hot cues and loop. Tap to seek.
struct Overview: View {
    let deck: DeckView
    let info: TrackInfo?
    var waveKind: WaveKind = .rgb
    var onSeek: (Double) -> Void

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            let dur = max(deck.duration, 0.001)
            ZStack(alignment: .topLeading) {
                Canvas { ctx, size in
                    guard let kw = info?.wave(waveKind, overview: true) else { return }
                    let (kind, ov) = kw
                    let n = ov.count
                    let colW = size.width / CGFloat(n)
                    WaveRenderer.draw(ctx, kind: kind, data: ov, k0: 0, k1: n - 1, binsPerCol: 1, colWidth: colW,
                                      mid: size.height / 2, scale: 1) { k in CGFloat(k) * colW }
                    let played = CGFloat(deck.position / dur) * size.width
                    ctx.fill(Path(CGRect(x: 0, y: 0, width: played, height: size.height)), with: .color(.black.opacity(0.45)))
                    if deck.loopIn >= 0, deck.loopOut > deck.loopIn {
                        let x0 = CGFloat(deck.loopIn / dur) * size.width, x1 = CGFloat(deck.loopOut / dur) * size.width
                        ctx.fill(Path(CGRect(x: x0, y: 0, width: max(2, x1 - x0), height: size.height)),
                                 with: .color((deck.loopActive ? Color.yellow : Color.gray).opacity(0.3)))
                    }
                }
                ForEach(0..<8, id: \.self) { i in
                    if deck.hotcues[i] >= 0 {
                        Rectangle().fill(info?.hotColor(i) ?? Color.hotcueColors[i]).frame(width: 3, height: 10)
                            .offset(x: CGFloat(deck.hotcues[i] / dur) * w - 1)
                    }
                }
                Triangle().fill(Color.cueOrange).frame(width: 8, height: 6)
                    .offset(x: CGFloat(deck.cue / dur) * w - 4, y: h - 6)
                Rectangle().fill(.red).frame(width: 2, height: h)
                    .offset(x: CGFloat(deck.position / dur) * w - 1)
            }
            .background(Color.black, in: RoundedRectangle(cornerRadius: 4))
            .contentShape(Rectangle())
            .onTapGesture { loc in if deck.loaded { onSeek(Double(loc.x / w) * dur) } }
        }
    }
}

struct Triangle: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.midX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}

/// Correct the estimated grid by hand until Phase 3 reads rekordbox grids.
struct GridSheet: View {
    @EnvironmentObject var engine: Engine
    @Environment(\.dismiss) var dismiss
    let index: Int
    @State private var bpm = 120.0
    @State private var first = 0.0

    var body: some View {
        NavigationStack {
            Form {
                Section("Beat grid, deck \(index + 1)") {
                    Stepper(String(format: "BPM %.2f", bpm), value: $bpm, in: 60...200, step: 0.05)
                    HStack {
                        Button("×2") { bpm *= 2 }
                        Button("÷2") { bpm /= 2 }
                        Button("Round") { bpm = bpm.rounded() }
                    }
                    Stepper(String(format: "First beat %.3f s", first), value: $first, in: 0...30, step: 0.005)
                    Button("Set first beat to playhead") { first = engine.decks[index].position }
                }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply") { engine.setGrid(index, bpm: bpm, firstBeat: first); dismiss() }
                }
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            }
        }
        .onAppear {
            bpm = engine.deckTracks[index]?.bpm ?? 120
            first = engine.deckTracks[index]?.firstBeat ?? 0
        }
    }
}
