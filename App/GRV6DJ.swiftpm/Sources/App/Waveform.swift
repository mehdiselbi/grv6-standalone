import SwiftUI

/// rekordbox waveform colour modes. The stick's own preference byte is not publicly documented,
/// so the owner picks the mode in Settings (default RGB).
enum WaveKind: String, CaseIterable, Identifiable {
    case rgb = "RGB", threeBand = "3Band", blue = "Blue"
    var id: String { rawValue }
    var anlzCode: Int32 { switch self { case .threeBand: return 7; case .rgb: return 5; case .blue: return 3 } }
}

/// One waveform at 150 entries per second (or downsampled for the overview).
/// h = height 0...1. c = three colour channels: 3Band low/mid/high amplitudes, RGB red/green/blue (0...1),
/// Blue whiteness in c[0].
struct WaveData: Equatable {
    var h: [Float]
    var c: [[Float]]
    var count: Int { h.count }

    /// Moves the picture by `seconds` (positive = later) at `rate` points per second, same length.
    func shifted(seconds: Double, rate: Double) -> WaveData {
        let k = Int((seconds * rate).rounded())
        guard k != 0, count > 0 else { return self }
        func mv(_ a: [Float]) -> [Float] {
            guard !a.isEmpty else { return a }
            if k > 0 { return [Float](repeating: 0, count: min(k, a.count)) + a.dropLast(min(k, a.count)) }
            return Array(a.dropFirst(min(-k, a.count))) + [Float](repeating: 0, count: min(-k, a.count))
        }
        return WaveData(h: mv(h), c: c.map(mv))
    }

    /// Max-height downsample; colour comes from the tallest entry of each bin.
    func downsampled(to bins: Int) -> WaveData {
        guard count > 0, bins > 0 else { return WaveData(h: [], c: [[], [], []]) }
        var nh = [Float](repeating: 0, count: bins)
        var nc = [[Float]](repeating: [Float](repeating: 0, count: bins), count: 3)
        for b in 0..<bins {
            let s = count * b / bins, e = min(count, max(s + 1, count * (b + 1) / bins))
            var best = s
            for i in s..<e where h[i] > h[best] { best = i }
            nh[b] = h[best]
            for k in 0..<3 where c[k].count == count { nc[k][b] = c[k][best] }
        }
        return WaveData(h: nh, c: nc)
    }
}

enum WaveRenderer {
    static let low = Color(red: 0.13, green: 0.40, blue: 1.0)
    static let mid = Color(red: 1.0, green: 0.60, blue: 0.12)
    static let high = Color(white: 0.96)
    private static let blueBase = (0.10, 0.36, 1.0)

    /// Draw columns [k0, k1]; column k covers entries [k*binsPerCol, (k+1)*binsPerCol) and sits at x(k).
    static func draw(_ ctx: GraphicsContext, kind: WaveKind, data: WaveData, k0: Int, k1: Int,
                     binsPerCol: Double, colWidth: CGFloat, mid midY: CGFloat, scale: CGFloat,
                     x: (Int) -> CGFloat) {
        let n = data.count
        guard n > 0, k1 >= k0 else { return }
        func range(_ k: Int) -> (Int, Int)? {
            let a = Double(k) * binsPerCol
            let i0 = max(0, Int(a.rounded(.down)))
            let i1 = min(n, max(i0 + 1, Int((a + binsPerCol).rounded(.down))))
            return i0 < i1 && i0 < n ? (i0, i1) : nil
        }
        switch kind {
        case .threeBand:
            let colors = [low, mid, high]
            let scales: [CGFloat] = [1.0, 0.78, 0.5]
            for band in 0..<3 {
                let d = data.c[band]
                guard d.count == n else { continue }
                var top: [CGPoint] = []
                top.reserveCapacity(k1 - k0 + 1)
                for k in k0...k1 {
                    var v: Float = 0
                    if let r = range(k) { for i in r.0..<r.1 where d[i] > v { v = d[i] } }
                    top.append(CGPoint(x: x(k), y: midY - CGFloat(v) * midY * scale * scales[band]))
                }
                var p = Path()
                p.addLines(top)
                for pt in top.reversed() { p.addLine(to: CGPoint(x: pt.x, y: 2 * midY - pt.y)) }
                p.closeSubpath()
                ctx.fill(p, with: .color(colors[band].opacity(band == 2 ? 0.85 : 1)))
            }
        case .rgb, .blue:
            // colours are 3-bit in the file, so group columns by colour and fill one path per colour
            var paths: [Int: Path] = [:]
            for k in k0...k1 {
                guard let r = range(k) else { continue }
                let (i0, i1) = r
                var best = i0
                for i in i0..<i1 where data.h[i] > data.h[best] { best = i }
                let hgt = CGFloat(data.h[best]) * midY * scale
                guard hgt > 0.2 else { continue }
                let key: Int
                if kind == .rgb {
                    key = Int((data.c[0][best] * 7).rounded()) << 6 | Int((data.c[1][best] * 7).rounded()) << 3
                        | Int((data.c[2][best] * 7).rounded())
                } else {
                    key = Int((data.c[0][best] * 7).rounded())
                }
                paths[key, default: Path()].addRect(CGRect(x: x(k), y: midY - hgt, width: colWidth + 0.35, height: hgt * 2))
            }
            for (key, p) in paths { ctx.fill(p, with: .color(color(kind: kind, key: key))) }
        }
    }

    static func color(kind: WaveKind, key: Int) -> Color {
        if kind == .rgb {
            var r = Double((key >> 6) & 7), g = Double((key >> 3) & 7), b = Double(key & 7)
            let m = max(r, g, b, 1)
            r /= m; g /= m; b /= m // full brightness, keep the hue
            return Color(red: r, green: g, blue: b)
        }
        let w = Double(key) / 7
        return Color(red: blueBase.0 + (1 - blueBase.0) * w, green: blueBase.1 + (1 - blueBase.1) * w,
                     blue: blueBase.2 + (1 - blueBase.2) * w)
    }
}
