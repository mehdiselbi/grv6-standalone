import DJCore
import MetalKit
import SwiftUI
import UIKit

// GPU waveform for the enlarged (scrolling) view.
// The waveform geometry is uploaded once per track; every frame only the scroll position changes, read straight
// from the C++ engine (not through SwiftUI), so scrolling stays smooth whatever the UI is doing.

private let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct WaveVertex { float t; float a; float4 color; };
struct WaveUniforms { float t0; float pxPerSec; float2 size; float midY; float amp; float scale; float pad; };
struct VOut { float4 pos [[position]]; float4 color; float2 uv; };

vertex VOut wave_vertex(const device WaveVertex *v [[buffer(0)]], constant WaveUniforms &u [[buffer(1)]],
                        uint vid [[vertex_id]]) {
    WaveVertex w = v[vid];
    float x = (w.t - u.t0) * u.pxPerSec;
    float y = u.midY - w.a * u.amp * u.scale;
    VOut o;
    o.pos = float4(x / u.size.x * 2.0 - 1.0, 1.0 - y / u.size.y * 2.0, 0, 1);
    o.color = w.color;
    o.uv = float2(-1, -1);
    return o;
}

struct OverlayVertex { float2 p; float2 uv; float4 color; };

vertex VOut overlay_vertex(const device OverlayVertex *v [[buffer(0)]], constant float2 &size [[buffer(1)]],
                           uint vid [[vertex_id]]) {
    OverlayVertex w = v[vid];
    VOut o;
    o.pos = float4(w.p.x / size.x * 2.0 - 1.0, 1.0 - w.p.y / size.y * 2.0, 0, 1);
    o.color = w.color;
    o.uv = w.uv;
    return o;
}

fragment float4 flat_fragment(VOut in [[stage_in]], texture2d<float> atlas [[texture(0)]]) {
    if (in.uv.x < 0) return in.color;
    constexpr sampler s(filter::linear);
    float a = atlas.sample(s, in.uv).a;
    return float4(in.color.rgb, in.color.a * a);
}
"""

private struct WaveVertex { var t: Float; var a: Float; var color: SIMD4<Float> }
private struct WaveUniforms { var t0: Float; var pxPerSec: Float; var size: SIMD2<Float>; var midY: Float; var amp: Float; var scale: Float; var pad: Float = 0 }
private struct OverlayVertex { var p: SIMD2<Float>; var uv: SIMD2<Float>; var color: SIMD4<Float> }

/// Shared Metal objects (device, pipelines, glyph atlas).
final class WaveGPU {
    static let shared: WaveGPU? = WaveGPU()
    let device: MTLDevice
    let queue: MTLCommandQueue
    let wavePipeline: MTLRenderPipelineState
    let overlayPipeline: MTLRenderPipelineState
    let atlas: MTLTexture
    let glyphs: [Character: (uv: CGRect, advance: CGFloat)]
    let glyphHeight: CGFloat = 32

    private init?() {
        guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue(),
              let lib = try? dev.makeLibrary(source: shaderSource, options: nil) else { return nil }
        device = dev
        queue = q
        let pipe = { (vfn: String) -> MTLRenderPipelineState? in
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: vfn)
            d.fragmentFunction = lib.makeFunction(name: "flat_fragment")
            d.rasterSampleCount = 4
            let c = d.colorAttachments[0]!
            c.pixelFormat = .bgra8Unorm
            c.isBlendingEnabled = true
            c.sourceRGBBlendFactor = .sourceAlpha
            c.destinationRGBBlendFactor = .oneMinusSourceAlpha
            c.sourceAlphaBlendFactor = .one
            c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            return try? dev.makeRenderPipelineState(descriptor: d)
        }
        guard let wp = pipe("wave_vertex"), let op = pipe("overlay_vertex") else { return nil }
        wavePipeline = wp
        overlayPipeline = op

        // glyph atlas (system font, drawn by us)
        let chars = Array("0123456789.-/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz ")
        let cellW = 28, cellH = 32, cols = 16
        let rows = (chars.count + cols - 1) / cols
        let W = cellW * cols, H = cellH * rows
        let font = UIFont.systemFont(ofSize: 24, weight: .bold)
        var map: [Character: (CGRect, CGFloat)] = [:]
        // draw the glyphs into our own RGBA buffer and copy it into a texture (no image decoding involved)
        var pixels = [UInt8](repeating: 0, count: W * H * 4)
        let cs = CGColorSpaceCreateDeviceRGB()
        pixels.withUnsafeMutableBytes { buf in
            guard let ctx = CGContext(data: buf.baseAddress, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W * 4,
                                      space: cs, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return }
            ctx.translateBy(x: 0, y: CGFloat(H)) // UIKit drawing is top-down
            ctx.scaleBy(x: 1, y: -1)
            UIGraphicsPushContext(ctx)
            for (i, ch) in chars.enumerated() {
                let x = (i % cols) * cellW, y = (i / cols) * cellH
                let s = NSAttributedString(string: String(ch), attributes: [.font: font, .foregroundColor: UIColor.white])
                let sz = s.size()
                s.draw(at: CGPoint(x: CGFloat(x) + 2, y: CGFloat(y) + (CGFloat(cellH) - sz.height) / 2))
                map[ch] = (CGRect(x: CGFloat(x) / CGFloat(W), y: CGFloat(y) / CGFloat(H),
                                  width: CGFloat(cellW) / CGFloat(W), height: CGFloat(cellH) / CGFloat(H)), sz.width + 1)
            }
            UIGraphicsPopContext()
        }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: false)
        td.usage = .shaderRead
        guard let tex = dev.makeTexture(descriptor: td) else { return nil }
        pixels.withUnsafeBytes { buf in
            tex.replace(region: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0, withBytes: buf.baseAddress!, bytesPerRow: W * 4)
        }
        atlas = tex
        glyphs = map.mapValues { (uv: $0.0, advance: $0.1) }
    }
}

/// GPU copy of one track's waveform in one colour mode.
private final class WaveMesh {
    let key: String
    let buffer: MTLBuffer
    let layers: [Int] // vertex offset of each layer; each layer has count*2 vertices
    let count: Int

    init?(key: String, kind: WaveKind, data: WaveData, device: MTLDevice) {
        let n = data.count
        guard n > 1 else { return nil }
        var v: [WaveVertex] = []
        var layers: [Int] = []
        func strip(height: (Int) -> Float, color: (Int) -> SIMD4<Float>) {
            layers.append(v.count)
            for i in 0..<n {
                let t = Float(Double(i) / waveRate), h = height(i), c = color(i)
                v.append(WaveVertex(t: t, a: h, color: c))
                v.append(WaveVertex(t: t, a: -h, color: c))
            }
        }
        switch kind {
        case .threeBand:
            let cols: [SIMD4<Float>] = [SIMD4(0.10, 0.38, 1.0, 1), SIMD4(0.90, 0.55, 0.16, 1), SIMD4(0.97, 0.91, 0.80, 1)]
            let scale: [Float] = [1.0, 0.8, 0.55]
            for b in 0..<3 where data.c[b].count == n {
                strip(height: { data.c[b][$0] * scale[b] }, color: { _ in cols[b] })
            }
        case .rgb:
            strip(height: { data.h[$0] }, color: { i in
                let r = data.c[0][i], g = data.c[1][i], b = data.c[2][i]
                let m = max(r, g, b, 0.01)
                return SIMD4(r / m, g / m, b / m, 1)
            })
        case .blue:
            strip(height: { data.h[$0] }, color: { i in
                let w = data.c[0].count == n ? data.c[0][i] : 0
                return SIMD4(0.10 + 0.9 * w, 0.36 + 0.64 * w, 1.0, 1)
            })
        }
        guard !layers.isEmpty,
              let buf = device.makeBuffer(bytes: v, length: v.count * MemoryLayout<WaveVertex>.stride, options: .storageModeShared)
        else { return nil }
        self.key = key
        self.buffer = buf
        self.layers = layers
        self.count = n
    }
}

/// One enlarged waveform lane for one deck.
struct MetalWaveformLane: UIViewRepresentable {
    @EnvironmentObject var engine: Engine
    let deck: Int

    func makeCoordinator() -> LaneRenderer { LaneRenderer() }

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: WaveGPU.shared?.device)
        v.colorPixelFormat = .bgra8Unorm
        v.sampleCount = 4
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        v.preferredFramesPerSecond = 60
        v.isPaused = false
        v.enableSetNeedsDisplay = false
        v.isOpaque = true
        v.delegate = context.coordinator
        context.coordinator.engine = engine
        context.coordinator.deck = deck
        return v
    }

    func updateUIView(_ v: MTKView, context: Context) {
        context.coordinator.engine = engine
        context.coordinator.deck = deck
    }
}

@MainActor
final class LaneRenderer: NSObject, MTKViewDelegate {
    weak var engine: Engine?
    var deck = 0
    private var mesh: WaveMesh?
    private var overlay: [OverlayVertex] = []

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let gpu = WaveGPU.shared, let engine, let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cmd = gpu.queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return }
        defer { enc.endEncoding(); cmd.present(drawable); cmd.commit() }

        // work in points; the drawable is points * contentScaleFactor
        let size = view.bounds.size
        guard size.width > 1, size.height > 1 else { return }
        var s = djc_deck_state()
        djc_get_deck_state(engine.core, Int32(deck), &s)
        overlay.removeAll(keepingCapacity: true)
        let w = Float(size.width), h = Float(size.height), mid = h / 2
        let playX = w * 0.30

        guard s.loaded != 0, let info = engine.deckTracks[deck], let kw = info.wave(engine.waveKind) else {
            mesh = nil
            flushOverlay(enc, size: size)
            return
        }
        let (kind, data) = kw
        let key = "\(info.id)-\(kind.rawValue)"
        if mesh?.key != key { mesh = WaveMesh(key: key, kind: kind, data: data, device: gpu.device) }

        // position extrapolated from the engine's last block, wrapped inside an active loop
        var pos = s.position
        if s.playing != 0 {
            pos += s.rate * max(0, min(0.25, djc_now() - s.stamp))
            if s.loopActive != 0, s.loopIn >= 0, s.loopOut > s.loopIn, pos >= s.loopOut {
                pos = s.loopIn + (pos - s.loopIn).truncatingRemainder(dividingBy: s.loopOut - s.loopIn)
            }
        }
        // the lane shows zoomSeconds of real time: scale track time by the playback tempo so beats of two
        // decks playing at the same BPM line up on screen even when their original BPMs differ
        let tempoRatio = (s.trackBpm > 0 && s.bpm > 0) ? s.bpm / s.trackBpm : 1
        let secs = engine.zoomSeconds * tempoRatio
        let pxPerSec = Double(w) / secs
        let t0 = pos - Double(playX) / pxPerSec, t1 = t0 + secs
        func x(_ t: Double) -> Float { Float((t - t0) * pxPerSec) }

        // loop area behind the waveform only: the grid tick rows (top / bottom 16 px) stay clear
        if s.loopIn >= 0, s.loopOut > s.loopIn {
            rect(x(s.loopIn), 16, x(s.loopOut) - x(s.loopIn), h - 32,
                 s.loopActive != 0 ? SIMD4(1.0, 0.62, 0.1, 0.28) : SIMD4(0.6, 0.6, 0.6, 0.14))
        }
        flushOverlay(enc, size: size)

        // waveform
        if let mesh {
            var u = WaveUniforms(t0: Float(t0), pxPerSec: Float(pxPerSec), size: SIMD2(w, h), midY: mid,
                                 amp: mid * 0.86, scale: 1)
            let i0 = max(0, Int(t0 * waveRate) - 2), i1 = min(mesh.count - 1, Int(t1 * waveRate) + 2)
            if i1 > i0 {
                enc.setRenderPipelineState(gpu.wavePipeline)
                enc.setVertexBuffer(mesh.buffer, offset: 0, index: 0)
                enc.setVertexBytes(&u, length: MemoryLayout<WaveUniforms>.stride, index: 1)
                enc.setFragmentTexture(gpu.atlas, index: 0)
                for base in mesh.layers {
                    enc.drawPrimitives(type: .triangleStrip, vertexStart: base + i0 * 2, vertexCount: (i1 - i0 + 1) * 2)
                }
            }
        }

        // beat grid: short ticks at top and bottom edges, bar numbers every 4 bars (16-bar phrase count).
        // Uses rekordbox's own beat list when the track has one (exact positions), else the constant grid.
        func tick(_ t: Double, isBar: Bool, bar: Int) {
            let bx = x(t)
            // same 2 px x 16 px tick for every beat (precision); downbeats are just red, like the CDJ-3000
            let c: SIMD4<Float> = isBar ? SIMD4(1, 0.15, 0.15, 1) : SIMD4(1, 1, 1, 0.85)
            rect(bx - 1, 0, 2, 16, c)
            rect(bx - 1, h - 16, 2, 16, c)
            let b16 = ((bar % 16) + 16) % 16
            if isBar, b16 % 4 == 3 { text(String(b16 + 1), x: bx + 4, y: h - 22, size: 14, color: SIMD4(1, 1, 1, 1)) }
        }
        if info.beatTimes.count > 1 {
            let bt = info.beatTimes
            var lo = 0, hi = bt.count // first beat >= t0
            while lo < hi { let m = (lo + hi) / 2; if bt[m] < t0 { lo = m + 1 } else { hi = m } }
            // bar index = number of downbeats before this beat
            var bar = -1
            if lo > 0 { bar = info.barIndex(beforeOrAt: lo - 1) }
            var i = lo
            while i < bt.count, bt[i] <= t1 {
                let isBar = info.beatInBar.indices.contains(i) && info.beatInBar[i] == 1
                if isBar { bar += 1 }
                tick(bt[i], isBar: isBar, bar: bar)
                i += 1
            }
        } else if info.bpm > 0 {
            let beat = 60 / info.bpm
            var b = ((t0 - info.firstBeat) / beat).rounded(.down)
            while info.firstBeat + b * beat < t1 {
                let isBar = ((Int(b) % 4) + 4) % 4 == 0
                tick(info.firstBeat + b * beat, isBar: isBar, bar: Int((b / 4).rounded(.down)))
                b += 1
            }
        }
        // memory cues / memory loops (red), active loops (orange)
        for c in info.cues where c.hot == 0 && c.time >= t0 - 30 && c.time <= t1 {
            if c.loopEnd > c.time {
                rect(x(c.time), h - 5, x(c.loopEnd) - x(c.time), 3, c.active ? SIMD4(1, 0.62, 0.1, 0.9) : SIMD4(0.9, 0.1, 0.1, 0.9))
            }
            triangle(x(c.time), h, up: true, size: 8, SIMD4(0.92, 0.08, 0.1, 1))
        }
        // CUE point
        if s.cuePoint >= t0, s.cuePoint <= t1 { triangle(x(s.cuePoint), h, up: true, size: 9, SIMD4(1, 0.55, 0.1, 1)) }
        // LOOP IN set, waiting for OUT: a triangle at the in point. Once the loop exists the highlight is enough.
        if s.loopIn >= 0, !(s.loopOut > s.loopIn), s.loopIn >= t0, s.loopIn <= t1 {
            triangle(x(s.loopIn), 0, up: false, size: 12, SIMD4(1, 0.62, 0.1, 1))
        }
        // hot cues: coloured flag with letter at the top
        let hot = withUnsafeBytes(of: s.hotcues) { Array($0.bindMemory(to: Double.self)) }
        for i in 0..<8 where hot[i] >= 0 && hot[i] >= t0 - 1 && hot[i] <= t1 {
            let hx = x(hot[i])
            let c = info.hotColorRGB(i)
            rect(hx, 0, 1.5, h, SIMD4(c.x, c.y, c.z, 0.7))
            rect(hx, 0, 13, 13, SIMD4(c.x, c.y, c.z, 1))
            text(["A", "B", "C", "D", "E", "F", "G", "H"][i], x: hx + 2.5, y: 0.5, size: 11, color: SIMD4(0, 0, 0, 1))
        }
        // playhead
        rect(playX - 1.5, 0, 3, h, SIMD4(1, 1, 1, 1))
        flushOverlay(enc, size: size)
    }

    // MARK: overlay primitives (points)

    private func rect(_ x: Float, _ y: Float, _ w: Float, _ h: Float, _ c: SIMD4<Float>) {
        let uv = SIMD2<Float>(-1, -1)
        let a = OverlayVertex(p: SIMD2(x, y), uv: uv, color: c), b = OverlayVertex(p: SIMD2(x + w, y), uv: uv, color: c)
        let d = OverlayVertex(p: SIMD2(x, y + h), uv: uv, color: c), e = OverlayVertex(p: SIMD2(x + w, y + h), uv: uv, color: c)
        overlay += [a, b, d, b, e, d]
    }

    private func triangle(_ x: Float, _ y: Float, up: Bool, size s: Float, _ c: SIMD4<Float>) {
        let uv = SIMD2<Float>(-1, -1)
        overlay += [OverlayVertex(p: SIMD2(x - s * 0.7, y), uv: uv, color: c),
                    OverlayVertex(p: SIMD2(x + s * 0.7, y), uv: uv, color: c),
                    OverlayVertex(p: SIMD2(x, up ? y - s : y + s), uv: uv, color: c)]
    }

    private func text(_ str: String, x: Float, y: Float, size: Float, color: SIMD4<Float>) {
        guard let gpu = WaveGPU.shared else { return }
        let k = size / Float(gpu.glyphHeight) * 1.25
        var cx = x
        for ch in str {
            guard let g = gpu.glyphs[ch] else { cx += size * 0.5; continue }
            let gw = Float(28) * k, gh = Float(gpu.glyphHeight) * k
            let u0 = Float(g.uv.minX), v0 = Float(g.uv.minY), u1 = Float(g.uv.maxX), v1 = Float(g.uv.maxY)
            let a = OverlayVertex(p: SIMD2(cx, y), uv: SIMD2(u0, v0), color: color)
            let b = OverlayVertex(p: SIMD2(cx + gw, y), uv: SIMD2(u1, v0), color: color)
            let d = OverlayVertex(p: SIMD2(cx, y + gh), uv: SIMD2(u0, v1), color: color)
            let e = OverlayVertex(p: SIMD2(cx + gw, y + gh), uv: SIMD2(u1, v1), color: color)
            overlay += [a, b, d, b, e, d]
            cx += Float(g.advance) * k
        }
    }

    private func flushOverlay(_ enc: MTLRenderCommandEncoder, size: CGSize) {
        guard !overlay.isEmpty, let gpu = WaveGPU.shared else { overlay.removeAll(keepingCapacity: true); return }
        var sz = SIMD2<Float>(Float(size.width), Float(size.height))
        enc.setRenderPipelineState(gpu.overlayPipeline)
        let bytes = overlay.count * MemoryLayout<OverlayVertex>.stride
        if bytes <= 4096 {
            overlay.withUnsafeBytes { enc.setVertexBytes($0.baseAddress!, length: bytes, index: 0) }
        } else if let buf = gpu.device.makeBuffer(bytes: overlay, length: bytes, options: .storageModeShared) {
            enc.setVertexBuffer(buf, offset: 0, index: 0)
        } else { overlay.removeAll(keepingCapacity: true); return }
        enc.setVertexBytes(&sz, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
        enc.setFragmentTexture(gpu.atlas, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: overlay.count)
        overlay.removeAll(keepingCapacity: true)
    }
}
