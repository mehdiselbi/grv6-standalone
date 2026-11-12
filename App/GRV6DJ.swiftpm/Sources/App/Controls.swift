import SwiftUI

// Small, self-drawn controls. No third-party or AlphaTheta assets.

extension Color {
    static let panel = Color(white: 0.11)
    static let panel2 = Color(white: 0.16)
    static let deckBlue = Color(red: 0.20, green: 0.48, blue: 0.95)
    static let cueOrange = Color(red: 1.0, green: 0.55, blue: 0.10)
    static let playGreen = Color(red: 0.25, green: 0.85, blue: 0.35)
    /// default hot cue colours A-H (own palette, close to the hardware defaults in the owner's reference)
    static let hotcueRGB: [SIMD3<Float>] = [
        SIMD3(0.88, 0.08, 0.12), SIMD3(0.12, 0.70, 0.92), SIMD3(0.18, 0.80, 0.18), SIMD3(0.32, 0.22, 0.95),
        SIMD3(0.18, 0.86, 0.32), SIMD3(1.0, 0.45, 0.05), SIMD3(0.10, 0.28, 0.95), SIMD3(1.0, 0.86, 0.05),
    ]
    static let hotcueColors: [Color] = hotcueRGB.map { Color(red: Double($0.x), green: Double($0.y), blue: Double($0.z)) }
}

/// Rotary knob: drag up/down. Double-tap resets to `reset`.
struct Knob: View {
    let label: String
    @Binding var value: Double
    var reset: Double = 0.5
    var size: CGFloat = 34
    var onChange: (Double) -> Void
    @State private var start: Double?

    var body: some View {
        VStack(spacing: 2) {
            ZStack {
                Circle().fill(Color(white: 0.22))
                Circle().trim(from: 0.125, to: 0.125 + 0.75 * value)
                    .stroke(Color.cueOrange, lineWidth: 3)
                    .rotationEffect(.degrees(90))
                    .padding(2)
                Rectangle().fill(.white).frame(width: 2, height: size * 0.32)
                    .offset(y: -size * 0.18)
                    .rotationEffect(.degrees(-135 + 270 * value))
            }
            .frame(width: size, height: size)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { g in
                        if start == nil { start = value }
                        let v = min(1, max(0, (start ?? value) - Double(g.translation.height) / 150))
                        value = v
                        onChange(v)
                    }
                    .onEnded { _ in start = nil }
            )
            .onTapGesture(count: 2) { value = reset; onChange(reset) }
            Text(label).font(.system(size: 9, weight: .semibold)).foregroundStyle(.secondary)
        }
    }
}

/// Vertical fader.
struct VFader: View {
    @Binding var value: Double
    var height: CGFloat = 110
    var onChange: (Double) -> Void

    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            ZStack(alignment: .bottom) {
                Capsule().fill(Color(white: 0.06)).frame(width: 6)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                RoundedRectangle(cornerRadius: 3)
                    .fill(Color(white: 0.75))
                    .frame(width: 28, height: 12)
                    .offset(y: -CGFloat(value) * (h - 12))
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                let v = min(1, max(0, 1 - Double((g.location.y - 6) / (h - 12))))
                value = v
                onChange(v)
            })
        }
        .frame(width: 34, height: height)
    }
}

/// Horizontal slider with a centre detent and double-tap reset.
struct HFader: View {
    @Binding var value: Double // 0..1
    var reset: Double = 0.5
    var onChange: (Double) -> Void

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            ZStack(alignment: .leading) {
                Capsule().fill(Color(white: 0.06)).frame(height: 6)
                Rectangle().fill(Color(white: 0.35)).frame(width: 1, height: 14).offset(x: w / 2)
                RoundedRectangle(cornerRadius: 3).fill(Color(white: 0.75))
                    .frame(width: 14, height: 26)
                    .offset(x: CGFloat(value) * (w - 14))
            }
            .frame(maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                var v = min(1, max(0, Double((g.location.x - 7) / (w - 14))))
                if abs(v - reset) < 0.015 { v = reset }
                value = v
                onChange(v)
            })
            .onTapGesture(count: 2) { value = reset; onChange(reset) }
        }
        .frame(height: 30)
    }
}

/// Button that reports press and release separately (CUE, hot cues).
struct PressButton<Label: View>: View {
    var onDown: () -> Void
    var onUp: () -> Void = {}
    @ViewBuilder var label: () -> Label
    @State private var down = false

    var body: some View {
        label()
            .opacity(down ? 0.6 : 1)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in if !down { down = true; onDown() } }
                .onEnded { _ in down = false; onUp() })
    }
}

struct Pill: View {
    let text: String
    var on = false
    var color: Color = .deckBlue
    var width: CGFloat? = nil

    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .bold))
            .frame(width: width, height: 26)
            .frame(minWidth: 34)
            .padding(.horizontal, 4)
            .background(on ? color : Color.panel2, in: RoundedRectangle(cornerRadius: 5))
            .foregroundStyle(on ? Color.black : Color.white)
    }
}

struct Meter: View {
    var level: Float
    var body: some View {
        GeometryReader { geo in
            let v = CGFloat(min(1, max(0, level)))
            ZStack(alignment: .bottom) {
                Rectangle().fill(Color(white: 0.08))
                Rectangle()
                    .fill(LinearGradient(colors: [.green, .yellow, .orange, .red], startPoint: .bottom, endPoint: .top))
                    .frame(height: geo.size.height * v)
            }
        }
        .frame(width: 5)
    }
}

func timeString(_ s: Double) -> String {
    guard s.isFinite, s >= 0 else { return "--:--" }
    let m = Int(s) / 60, sec = Int(s) % 60, t = Int((s - floor(s)) * 10)
    return String(format: "%02d:%02d.%d", m, sec, t)
}
