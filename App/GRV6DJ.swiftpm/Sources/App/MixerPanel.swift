import DJCore
import SwiftUI

struct MixerPanel: View {
    @EnvironmentObject var engine: Engine

    var body: some View {
        VStack(spacing: 8) {
            HStack(alignment: .bottom, spacing: 6) {
                ForEach([2, 0, 1, 3], id: \.self) { ch in // mixer order 3 1 2 4
                    ChannelStrip(ch: ch)
                }
            }
            HStack(spacing: 10) {
                Knob(label: "MASTER", value: $engine.masterLevel, reset: 0.8409) { v in
                    engine.post(DJC_MASTER_LEVEL, v: v)
                }
                Meter(level: engine.masterPeak).frame(height: 34)
                Knob(label: "CUE/MST", value: $engine.phonesMix, reset: 0) { v in engine.post(DJC_PHONES_MIX, v: v) }
                Knob(label: "PHONES", value: $engine.phonesLevel, reset: 1.0) { v in engine.post(DJC_PHONES_LEVEL, v: v) }
            }
            HStack(spacing: 6) {
                Button { engine.post(DJC_MASTER_CUE, i: 2) } label: {
                    Pill(text: "MST CUE", on: engine.masterCue, color: .yellow)
                }
                Button { engine.post(DJC_COLOR_FX_ON, i: 2) } label: {
                    Pill(text: "COLOR FX", on: engine.colorFxOn, color: .cueOrange)
                }
            }
            .buttonStyle(.plain)
            HFader(value: $engine.crossfader) { v in engine.post(DJC_CROSSFADER, v: v) }
                .padding(.horizontal, 30)
        }
        .padding(8)
        .background(Color.panel, in: RoundedRectangle(cornerRadius: 10))
    }
}

struct ChannelStrip: View {
    @EnvironmentObject var engine: Engine
    let ch: Int

    var body: some View {
        let m = $engine.mixer[ch]
        VStack(spacing: 4) {
            Text("\(ch + 1)").font(.system(size: 12, weight: .heavy))
            Knob(label: "TRIM", value: m.trim, size: 28) { v in engine.setMixer(ch, DJC_TRIM, v) }
            Knob(label: "HI", value: m.hi, size: 28) { v in engine.setMixer(ch, DJC_EQ_HI, v) }
            Knob(label: "MID", value: m.mid, size: 28) { v in engine.setMixer(ch, DJC_EQ_MID, v) }
            Knob(label: "LOW", value: m.low, size: 28) { v in engine.setMixer(ch, DJC_EQ_LOW, v) }
            Knob(label: "COLOR", value: m.color, size: 28) { v in engine.setMixer(ch, DJC_COLOR, v) }
            Button {
                engine.mixer[ch].cue.toggle()
                engine.post(DJC_CH_CUE, deck: ch, i: engine.mixer[ch].cue ? 1 : 0)
            } label: { Pill(text: "CUE", on: engine.mixer[ch].cue, color: .yellow, width: 36) }
            .buttonStyle(.plain)
            HStack(spacing: 2) {
                VFader(value: m.fader) { v in engine.setMixer(ch, DJC_FADER, v) }
                Meter(level: engine.decks[ch].peak).frame(height: 110)
            }
            Picker("", selection: Binding(get: { engine.mixer[ch].xf }, set: { v in
                engine.mixer[ch].xf = v
                engine.post(DJC_XF_ASSIGN, deck: ch, i: Int32(v))
            })) {
                Text("A").tag(0); Text("T").tag(1); Text("B").tag(2)
            }
            .pickerStyle(.segmented)
            .frame(width: 64)
        }
        .frame(width: 66)
    }
}
