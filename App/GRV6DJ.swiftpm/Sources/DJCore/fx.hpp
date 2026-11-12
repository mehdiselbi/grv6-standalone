// Beat FX (one unit, like the GRV6/DJM effect section). Own implementations, not AlphaTheta's algorithms.
// All buffers are allocated in prepare(); process() never allocates.
#pragma once
#include "dsp.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace djc {

enum FxType {
    FX_DELAY = 0, FX_ECHO, FX_LOWCUT_ECHO, FX_SPIRAL, FX_HELIX, FX_REVERB, FX_FLANGER, FX_PHASER,
    FX_FILTER, FX_TRANS, FX_PITCH, FX_ROLL, FX_MOBIUS_SAW, FX_MOBIUS_TRI, FX_COUNT
};
static const char *const kFxNames[FX_COUNT] = {"DELAY", "ECHO", "LOW CUT ECHO", "SPIRAL", "HELIX", "REVERB",
                                               "FLANGER", "PHASER", "FILTER", "TRANS", "PITCH", "ROLL",
                                               "MOBIUS SAW", "MOBIUS TRI"};
// beat values in the order of the official beat indicator codes (1/4 .. 32)
static const double kFxBeats[9] = {0.25, 0.5, 0.75, 1, 2, 4, 8, 16, 32};
static const int kFxBeatCodes[9] = {0x03, 0x04, 0x21, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};

struct BeatFx {
    double sr = 48000;
    int type = FX_ECHO;
    bool on = false;
    float level = 0.5f;
    int beatIdx = 3; // 1 beat
    int target = 4;  // 0-3 channel, 4 master, 5 sampler (none)

    // delay line (also used by pitch and helix)
    std::vector<float> dl[2];
    int dlLen = 0, wpos = 0;
    // reverb
    struct Comb { std::vector<float> b; int i = 0; float store = 0; };
    struct AP { std::vector<float> b; int i = 0; };
    Comb comb[2][8];
    AP ap[2][4];
    // roll
    std::vector<float> roll[2];
    int rollCap = 0, rollLen = 0, rollPos = 0, rollRec = 0;
    bool wasOn = false;
    // modulation
    double lfo = 0;
    float apz[2][6] = {{0}}, fbs[2] = {0, 0};
    Svf svf[2];
    float hpz[2] = {0, 0}, hpx[2] = {0, 0};
    float gate = 1.0f;
    double pitchPhase = 0;
    bool tail = false;
    int killN = 0;                       // lever kill: wet fades out over kKill frames, then all lines cleared
    static constexpr int kKill = 480;
    void kill() { on = false; if (tail || wasOn) killN = kKill; }
    void clearLines() {
        for (int c = 0; c < 2; ++c) {
            std::fill(dl[c].begin(), dl[c].end(), 0.0f);
            std::fill(roll[c].begin(), roll[c].end(), 0.0f);
            for (auto &cb : comb[c]) { std::fill(cb.b.begin(), cb.b.end(), 0.0f); cb.store = 0; }
            for (auto &a : ap[c]) std::fill(a.b.begin(), a.b.end(), 0.0f);
            fbs[c] = 0;
        }
        tail = false; tailEnergy = 0;
    }
    // delay time latch: changes crossfade between two taps instead of sweeping (a sweep = pitch shift)
    double curDelay = 0, nextDelay = 0;
    int xfade = 0;
    static constexpr int kXfade = 960;
    float hpA = 0.92f;
    float tailEnergy = 0;
    // loudness guard: the FX output may not get louder than the recent input (+1 dB)
    double msIn = 0, msOut = 0; float wetGain = 1.0f;
    // frequency select (A9-style LOW / MID / HI): the FX only processes the selected bands
    int bandMask = 7; // bit0 LOW, bit1 MID, bit2 HI
    struct Split3 {
        LR4 lpLo, hpLo, lpHi, hpHi, apLp, apHp;
        void setup(double sr) {
            lpLo.lowpass(250, sr); hpLo.highpass(250, sr); lpHi.lowpass(3000, sr); hpHi.highpass(3000, sr);
            apLp.lowpass(3000, sr); apHp.highpass(3000, sr);
        }
        inline void tick(float x, float &lo, float &mid, float &hi) {
            lo = lpLo.tick(x); lo = apLp.tick(lo) + apHp.tick(lo);
            float rest = hpLo.tick(x);
            mid = lpHi.tick(rest); hi = hpHi.tick(rest);
        }
    } split[2];

    void prepare(double rate) {
        split[0].setup(rate); split[1].setup(rate);
        sr = rate;
        dlLen = int(sr * 8.0);
        for (int c = 0; c < 2; ++c) {
            dl[c].assign(size_t(dlLen), 0.0f);
            roll[c].assign(size_t(sr * 8.0), 0.0f);
            static const int combT[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
            static const int apT[4] = {556, 441, 341, 225};
            for (int k = 0; k < 8; ++k) comb[c][k].b.assign(size_t((combT[k] + c * 23) * sr / 44100.0) + 1, 0.0f);
            for (int k = 0; k < 4; ++k) ap[c][k].b.assign(size_t((apT[k] + c * 23) * sr / 44100.0) + 1, 0.0f);
        }
        rollCap = int(roll[0].size());
        hpA = float(std::exp(-2 * kPi * 600.0 / sr));
    }

    double beats() const { return kFxBeats[beatIdx]; }
    bool echoFamily() const {
        return type == FX_DELAY || type == FX_ECHO || type == FX_LOWCUT_ECHO || type == FX_SPIRAL ||
               type == FX_HELIX || type == FX_REVERB;
    }

    float reverbTick(int c, float x, float fb) {
        float out = 0;
        for (auto &cb : comb[c]) {
            float y = cb.b[size_t(cb.i)];
            cb.store = y * 0.7f + cb.store * 0.3f; // damping
            cb.b[size_t(cb.i)] = x + cb.store * fb;
            if (++cb.i >= int(cb.b.size())) cb.i = 0;
            out += y;
        }
        for (auto &a : ap[c]) {
            float bo = a.b[size_t(a.i)];
            float y = -0.5f * out + bo;          // Schroeder allpass: y = -g x + d, d_in = x + g y
            a.b[size_t(a.i)] = out + 0.5f * y;
            if (++a.i >= int(a.b.size())) a.i = 0;
            out = y;
        }
        return out * 0.06f;
    }

    inline float dlRead(int c, double delay) const {
        double rp = double(wpos) - delay;
        while (rp < 0) rp += dlLen;
        int i0 = int(rp);
        float t = float(rp - i0);
        if (i0 >= dlLen) { i0 -= dlLen; if (i0 >= dlLen) i0 = 0; } // rp = -1e-12 + dlLen rounds to dlLen
        int i1 = i0 + 1 >= dlLen ? 0 : i0 + 1;
        return dl[c][size_t(i0)] * (1 - t) + dl[c][size_t(i1)] * t;
    }

    // L/R processed in place. bpm drives all beat-synced times.
    void process(float *L, float *R, int n, double bpm) {
        if (bpm < 40 || bpm > 300) bpm = 120;
        double beatSec = 60.0 / bpm;
        double period = beatSec * beats();
        double target = std::min(period * sr, double(dlLen - 2));
        if (curDelay <= 0 || (on && !wasOn)) { curDelay = target; xfade = 0; }
        else if (xfade == 0 && std::fabs(target - curDelay) > curDelay * 0.01) { nextDelay = target; xfade = kXfade; }
        bool active = on || (tail && echoFamily()) || killN > 0;
        if (on && !wasOn) { // fresh start: roll capture, lfo reset
            rollLen = std::min(int(period * sr), rollCap);
            rollRec = 0; rollPos = 0; lfo = 0; tail = false; wetGain = 1.0f; msOut = 0;
        }
        if (!on && wasOn && killN == 0) killN = kKill; // ON/OFF off = everything cut (10 ms fade), no tail
        wasOn = on;
        if (!active) {
            // keep the delay line moving silently so restarts are clean
            return;
        }
        const bool killing = killN > 0;
        float lv = level;
        double lfoInc = 1.0 / std::max(0.05, period * sr);
        float energy = 0;
        float *io[2] = {L, R};
        double sumIn = 0, sumOut = 0;
        for (int k = 0; k < n; ++k) {
            float xf = xfade > 0 ? 1.0f - float(xfade) / kXfade : 0.0f;
            auto tap = [&](int c) {
                float a = dlRead(c, curDelay);
                return xfade > 0 ? a * (1 - xf) + dlRead(c, nextDelay) * xf : a;
            };
            float in[2] = {L[k], R[k]};
            sumIn += double(in[0]) * in[0] + double(in[1]) * in[1];
            float feedIn = on ? 1.0f : 0.0f; // after OFF, echoes ring out without new input
            for (int c = 0; c < 2; ++c) {
                float x = in[c], rest = 0;
                if (bandMask != 7) { // FX only on the selected bands, the others pass straight through
                    float lo, mi, hi;
                    split[c].tick(x, lo, mi, hi);
                    x = ((bandMask & 1) ? lo : 0.0f) + ((bandMask & 2) ? mi : 0.0f) + ((bandMask & 4) ? hi : 0.0f);
                    rest = lo + mi + hi - x;
                }
                float y = x;
                switch (type) {
                case FX_DELAY: {
                    float d = tap(c);
                    dl[c][size_t(wpos)] = x * feedIn;
                    y = x + d * lv;
                } break;
                case FX_ECHO: case FX_SPIRAL: case FX_HELIX: {
                    float fb = type == FX_HELIX ? 0.85f : (type == FX_SPIRAL ? 0.72f : 0.55f);
                    float d = tap(c);
                    float back = d;
                    if (type == FX_SPIRAL) { // diffuse the repeats so they smear into a swirl (no delay-time sweep)
                        for (int st = 0; st < 2; ++st) {
                            auto &a = ap[c][st];
                            float bo = a.b[size_t(a.i)];
                            float o = -back * 0.6f + bo;           // true allpass (unity gain), so the
                            a.b[size_t(a.i)] = back + o * 0.6f;    // feedback loop can't run away
                            if (++a.i >= int(a.b.size())) a.i = 0;
                            back = o;
                        }
                    }
                    dl[c][size_t(wpos)] = x * feedIn + back * fb;
                    y = x + d * lv;
                } break;
                case FX_LOWCUT_ECHO: {
                    float d = tap(c);
                    // one-pole high-pass ~600 Hz in the feedback path
                    float a = hpA;
                    float hp = a * (hpz[c] + d - hpx[c]);
                    hpx[c] = d; hpz[c] = hp;
                    dl[c][size_t(wpos)] = x * feedIn + hp * 0.6f;
                    y = x + hp * lv;
                } break;
                case FX_REVERB: {
                    float size = float(beatIdx) / 8.0f;
                    float r = reverbTick(c, x * feedIn, 0.70f + 0.27f * size);
                    y = x + r * lv * 2.0f;
                } break;
                case FX_FLANGER: {
                    double m = 0.5 + 0.5 * std::sin(2 * kPi * lfo);
                    double dd = sr * (0.0008 + 0.004 * m);
                    float d = dlRead(c, dd);
                    dl[c][size_t(wpos)] = x + d * 0.6f;
                    y = x * (1 - 0.5f * lv) + d * 0.5f * lv * 1.4f;
                } break;
                case FX_PHASER: case FX_MOBIUS_SAW: case FX_MOBIUS_TRI: {
                    double l = type == FX_PHASER ? 0.5 + 0.5 * std::sin(2 * kPi * lfo)
                             : type == FX_MOBIUS_SAW ? lfo
                             : (lfo < 0.5 ? lfo * 2 : 2 - lfo * 2);
                    double f = 150.0 * std::pow(2500.0 / 150.0, l);
                    double t = std::tan(kPi * f / sr);
                    float a = float((t - 1) / (t + 1));
                    float s = x + fbs[c] * 0.5f;
                    for (int st = 0; st < 6; ++st) {
                        float o = a * s + apz[c][st];
                        apz[c][st] = s - a * o;
                        s = o;
                    }
                    fbs[c] = s;
                    y = x * (1 - 0.5f * lv) + s * 0.5f * lv * 1.6f;
                } break;
                case FX_FILTER: {
                    double l = 0.5 + 0.5 * std::sin(2 * kPi * lfo);
                    SvfCoeffs cf;
                    cf.set(250.0 * std::pow(48.0, l), sr, 2.5);
                    float lo, bp;
                    svf[c].tick(x, cf.a1, cf.a2, cf.a3, lo, bp);
                    y = x * (1 - lv) + lo * lv;
                } break;
                case FX_TRANS:
                    y = x * (1.0f - lv * (1.0f - gate));
                    break;
                case FX_PITCH: {
                    // two-tap delay pitch shifter, LEVEL/DEPTH = -12..+12 semitones
                    double ratio = std::pow(2.0, (lv - 0.5) * 24.0 / 12.0);
                    double win = sr * 0.06;
                    dl[c][size_t(wpos)] = x;
                    double ph = pitchPhase;
                    double d1 = (1 - ph) * win + 8, d2 = (1 - std::fmod(ph + 0.5, 1.0)) * win + 8;
                    float g1 = float(std::sin(kPi * ph)), g2 = float(std::sin(kPi * std::fmod(ph + 0.5, 1.0)));
                    y = dlRead(c, d1) * g1 + dlRead(c, d2) * g2;
                    if (c == 1) { pitchPhase += (ratio - 1.0) / win; pitchPhase -= std::floor(pitchPhase); }
                    (void)ratio;
                } break;
                case FX_ROLL: {
                    if (rollRec < rollLen) {
                        roll[c][size_t(rollRec)] = x;
                        y = x;
                    } else if (rollLen > 0) {
                        float r = roll[c][size_t(rollPos)];
                        y = x * (1 - lv) + r * lv;
                    }
                } break;
                }
                if (echoFamily()) { // only the echoes are level-limited, the dry music is never touched
                    float w = (y - x);
                    sumOut += double(w) * w;
                    y = x + w * wetGain;
                }
                if (killing) y = x + (y - x) * (float(killN) / kKill); // fades to the dry sound, stays dry
                io[c][k] = y + rest;
                energy = std::max(energy, std::fabs(y - x));
            }
            // advance shared state once per frame
            if (killN > 0 && --killN == 0) clearLines();
            if (++wpos >= dlLen) wpos = 0;
            if (xfade > 0 && --xfade == 0) curDelay = nextDelay;
            lfo += lfoInc;
            if (lfo >= 1.0) lfo -= 1.0;
            if (type == FX_TRANS) {
                float target = lfo < 0.5 ? 1.0f : 0.0f;
                gate += (target - gate) * 0.02f;
            }
            if (type == FX_ROLL) {
                if (rollRec < rollLen) ++rollRec;
                else if (rollLen > 0 && ++rollPos >= rollLen) rollPos = 0;
            }
        }
        // echo level guard: the echoes' average level (1 s) may not exceed 0.55 x the music's average level,
        // so repeats can't pile up into something louder than the track. Dry signal untouched.
        if (echoFamily()) {
            double a1 = std::exp(-double(n) / (1.0 * sr));
            msIn = msIn * a1 + sumIn / (2.0 * n) * (1 - a1);
            msOut = msOut * a1 + sumOut / (2.0 * n) * (1 - a1);
            float limit = float(std::sqrt(msIn)) * 0.55f + 1e-6f, wet = float(std::sqrt(msOut));
            float want = wet > limit ? limit / wet : 1.0f;
            wetGain = want < wetGain ? want : wetGain + (want - wetGain) * 0.05f;
        }
        if (tail && killN == 0) {
            tailEnergy = energy;
            if (energy < 1e-4f) tail = false;
        }
    }
};

} // namespace djc
