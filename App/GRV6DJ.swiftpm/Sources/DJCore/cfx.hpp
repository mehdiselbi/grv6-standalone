// Sound Colour FX per mixer channel: DUB ECHO, SWEEP, SPACE (FILTER lives in the engine's channel strip).
// The channel's COLOR knob is bipolar: centre = no effect, left/right = amount with a different flavour.
// Our own DSP, allocation-free after prepare().
#pragma once
#include "dsp.hpp"
#include <vector>

namespace djc {

enum { CFX_FILTER = 0, CFX_DUB_ECHO, CFX_SWEEP, CFX_SPACE, CFX_COUNT };

struct OnePole {
    float a = 0, z = 0;
    void setLow(double fc, double sr) { a = float(std::exp(-2 * kPi * fc / sr)); }
    inline float lp(float x) { z = x + a * (z - x); return z; }
    inline float hp(float x) { return x - lp(x); }
};

struct ColorFx {
    double sr = 48000;
    // dub echo
    std::vector<float> dl[2];
    int dlLen = 0, wpos = 0;
    OnePole fbFilt[2];
    Svf dubBand[2];          // band-limits what goes into the echo AND every repeat (repeats keep thinning out)
    float dubBeats = 0.75f;  // unused since 2026-11-06 (COLOR knob sets the time)
    float dubTime = 0;       // smoothed delay (frames), so tempo changes glide like tape instead of clicking
    // space (small Schroeder reverb per side)
    struct Comb { std::vector<float> b; int i = 0; float store = 0; };
    struct AP { std::vector<float> b; int i = 0; };
    Comb comb[2][4];
    AP ap[2][2];
    OnePole revFilt[2];
    // sweep
    Svf bp[2];
    float envFast = 0, envSlow = 0, gate = 1;
    float sendPrev = 0;
    int lastDubSide = 1;
    int dubHold = 0;         // frames the line must keep running after the last non-silent write
    OnePole loopHp[2], loopLp[2];
    double wow = 0, flut = 0;
    float dubFb = 0.5f, dubA = 0.5f;
    int killN = 0;           // lever kill: wet fades out, then the lines are cleared
    static constexpr int kKill = 480;
    void kill() { if (tailEnergy > 1e-5f || sendPrev > 1e-4f) killN = kKill; }
    float tailEnergy = 0;
    double msIn = 0, msOut = 0; float makeup = 1; // SWEEP loudness match

    void prepare(double rate) {
        sr = rate;
        dlLen = int(sr * 2.2) + 4;
        for (int c = 0; c < 2; ++c) {
            dl[c].assign(size_t(dlLen), 0.0f);
            static const int ct[4] = {1309, 1451, 1601, 1709};
            static const int at[2] = {431, 263};
            for (int k = 0; k < 4; ++k) comb[c][k].b.assign(size_t((ct[k] + c * 37) * sr / 44100.0) + 1, 0.0f);
            for (int k = 0; k < 2; ++k) ap[c][k].b.assign(size_t((at[k] + c * 19) * sr / 44100.0) + 1, 0.0f);
        }
    }

    void clear() {
        for (int c = 0; c < 2; ++c) {
            std::fill(dl[c].begin(), dl[c].end(), 0.0f);
            for (auto &cb : comb[c]) { std::fill(cb.b.begin(), cb.b.end(), 0.0f); cb.store = 0; }
            for (auto &a : ap[c]) std::fill(a.b.begin(), a.b.end(), 0.0f);
            bp[c] = Svf{}; dubBand[c] = Svf{}; loopHp[c].z = 0; loopLp[c].z = 0;
        }
        tailEnergy = 0; sendPrev = 0; gate = 1;
    }

    // amt: -1 (full left) .. 0 (centre) .. +1 (full right)
    void process(float *L, float *R, int n, int type, float amt, double bpm, float param = 0.5f) {
        if (type == CFX_FILTER || dlLen == 0) return;
        if (type != CFX_DUB_ECHO && type != CFX_SPACE) killN = 0;
        float a = std::fabs(amt);
        float send = a;
        if (send < 1e-4f && sendPrev < 1e-4f && tailEnergy < 1e-5f && type != CFX_SWEEP) return; // idle
        if (type == CFX_SWEEP && a < 1e-4f) { sendPrev = 0; makeup = 1; msIn = msOut = 0; return; }
        float *io[2] = {L, R};
        float energy = 0;
        if (type == CFX_DUB_ECHO) {
            // DUB ECHO as the XDJ-AZ / DJM-A9 do it (owner's two walkthrough videos, 2026-11-06):
            //  - COLOR knob = three things at once: the side picks the band (left = lows, right = highs, stronger
            //    the further you turn), the amount sets how much goes into the echo, AND the echo time: a smooth
            //    sweep from 1/16 beat (just opened) to 3/4 beat (fully open), BPM-synced, no steps. Turning it
            //    while the echo rings glides the time, so the tail bends in pitch like tape.
            //  - PARAMETER = feedback only: MIN = one single repeat, MAX = long tail
            //  - every repeat a little darker + light tape saturation; tail rings on with the fader down or the
            //    knob back at centre (time and filter frozen where they were); ON/OFF cuts it (kill)
            if (bpm < 40 || bpm > 300) bpm = 120;
            int side = amt < 0 ? -1 : (amt > 0 ? 1 : lastDubSide);
            float fa = a > 1e-4f ? a : dubA;                          // frozen while the tail rings
            dubA = fa;
            double beats = 0.0625 * std::pow(12.0, double(fa));       // 1/16 -> 3/4 beat, even feel across the turn
            double want = std::min(double(dlLen - 8), 60.0 / bpm * beats * sr);
            if (dubTime <= 0) dubTime = float(want);
            float fbT = 0.95f * param;                                // PARAMETER: 0 = single repeat
            SvfCoeffs cf;
            if (side < 0) cf.set(12000.0 * std::pow(2.0, -4.5 * fa), sr, 0.9);   // lows: 12 kHz -> ~530 Hz
            else cf.set(40.0 * std::pow(2.0, 6.0 * fa), sr, 0.9);              // highs: 40 Hz -> ~2.5 kHz
            for (int c2 = 0; c2 < 2; ++c2) { loopLp[c2].setLow(5000.0, sr); loopHp[c2].setLow(30.0, sr); }
            const double glide = 1.0 - std::exp(-1.0 / (0.08 * sr)); // 80 ms: smooth, audible tape bend
            for (int k = 0; k < n; ++k) {
                float sg = sendPrev + (a - sendPrev) * float(k + 1) / float(n);
                dubFb += (fbT - dubFb) * 0.0005f;
                dubTime += float((want - dubTime) * glide);
                wow += 0.35 / sr; if (wow >= 1.0) wow -= 1.0;
                double t = double(dubTime) * (1.0 + 0.001 * std::sin(2 * kPi * wow));
                if (t < 2.0) t = 2.0;
                double rp = double(wpos) - t;
                while (rp < 0) rp += double(dlLen);
                int i0 = int(rp); float fr = float(rp - double(i0));
                if (i0 >= dlLen) { i0 -= dlLen; if (i0 >= dlLen) i0 = 0; }
                int i1 = i0 + 1 >= dlLen ? 0 : i0 + 1;
                float d[2];
                for (int c2 = 0; c2 < 2; ++c2) d[c2] = dl[c2][size_t(i0)] + (dl[c2][size_t(i1)] - dl[c2][size_t(i0)]) * fr;
                float kg = killN > 0 ? float(killN) / kKill : 1.0f;
                for (int c2 = 0; c2 < 2; ++c2) {
                    float x = io[c2][k];
                    float rep = 0.88f * d[c2] + 0.12f * d[1 - c2];              // light ping-pong
                    float in = x * sg + dubFb * rep;                            // send + repeat, filtered together
                    float lo, band;
                    dubBand[c2].tick(in, cf.a1, cf.a2, cf.a3, lo, band);
                    float y = side < 0 ? lo : in - cf.k * band - lo;            // lows (left) / highs (right)
                    y = loopLp[c2].lp(loopHp[c2].hp(y));                        // a little darker every repeat
                    y = y > 1.5f ? 1.0f : (y < -1.5f ? -1.0f : y - (4.0f / 27.0f) * y * y * y); // tape saturation
                    dl[c2][size_t(wpos)] = y * kg;
                    io[c2][k] = x + d[c2] * kg;
                    energy = std::max(energy, std::max(std::fabs(d[c2]), std::fabs(y)));
                }
                if (killN > 0 && --killN == 0) { clear(); energy = 0; dubHold = 0; }
                if (++wpos >= dlLen) wpos = 0;
            }
            sendPrev = a; send = a;
            if (energy > 1e-5f) dubHold = int(dubTime) + 2 * n; else dubHold = std::max(0, dubHold - n);
            if (dubHold > 0) energy = std::max(energy, 2e-5f);
            lastDubSide = side;
        } else if (type == CFX_SPACE) {
            for (int c = 0; c < 2; ++c) {
                if (amt < 0) revFilt[c].setLow(6000.0 * std::pow(0.15, a), sr);
                else revFilt[c].setLow(150.0 * std::pow(6.0, a), sr);
            }
            float fb = std::min(0.95f, 0.70f + 0.25f * param + 0.04f * a); // PARAMETER = room size
            for (int k = 0; k < n; ++k) {
                float s = sendPrev + (send - sendPrev) * float(k + 1) / float(n);
                for (int c = 0; c < 2; ++c) {
                    float x = io[c][k];
                    float in = x * s * 0.25f, out = 0;
                    for (auto &cb : comb[c]) {
                        float y = cb.b[size_t(cb.i)];
                        cb.store = y * 0.75f + cb.store * 0.25f;
                        cb.b[size_t(cb.i)] = in + cb.store * fb;
                        if (++cb.i >= int(cb.b.size())) cb.i = 0;
                        out += y;
                    }
                    for (auto &ap1 : ap[c]) { // Schroeder allpass: y = -g x + d, d_in = x + g y
                        float d = ap1.b[size_t(ap1.i)];
                        float y = -0.5f * out + d;
                        ap1.b[size_t(ap1.i)] = out + 0.5f * y;
                        if (++ap1.i >= int(ap1.b.size())) ap1.i = 0;
                        out = y;
                    }
                    float w = amt < 0 ? revFilt[c].lp(out) : revFilt[c].hp(out);
                    if (killN > 0) w *= float(killN) / kKill;
                    io[c][k] = x + w * (0.6f + 0.6f * param);
                    energy = std::max(energy, std::fabs(w));
                }
                if (killN > 0 && --killN == 0) { clear(); energy = 0; break; }
            }
        } else if (type == CFX_SWEEP) {
            SvfCoeffs cf;
            if (amt > 0) cf.set(250.0 * std::pow(12.0, a), sr, 1.0 + 4.0 * param); // right: band-pass sweep up, PARAMETER = resonance
            double sIn = 0, sOut = 0;
            float att = float(std::exp(-1.0 / (0.002 * sr))), relF = float(std::exp(-1.0 / (0.030 * sr)));
            float slow = float(std::exp(-1.0 / (0.400 * sr)));
            for (int k = 0; k < n; ++k) {
                float m = 0.5f * (std::fabs(L[k]) + std::fabs(R[k]));
                envFast = m > envFast ? m + att * (envFast - m) : m + relF * (envFast - m);
                envSlow = m + slow * (envSlow - m);
                // left: gate that tightens the sound (only transients above the average pass)
                float open = envFast > envSlow * (1.0f + (0.5f + 2.0f * param) * a) ? 1.0f : 1.0f - a * (0.5f + 0.5f * param);
                gate += (open - gate) * 0.01f;
                float g = makeup;
                for (int c = 0; c < 2; ++c) {
                    float x = io[c][k], y;
                    if (amt > 0) {
                        float lo, band;
                        bp[c].tick(x, cf.a1, cf.a2, cf.a3, lo, band);
                        y = x * (1 - 0.75f * a) + band * cf.k * a * 1.2f; // a little dry stays, like the hardware
                    } else {
                        y = x * gate;
                    }
                    sIn += double(x) * x; sOut += double(y) * y;
                    io[c][k] = y * g;
                }
            }
            // keep SWEEP at the track's loudness (it thins the sound, it shouldn't make it quieter)
            double a1 = std::exp(-double(n) / (0.3 * sr));
            msIn = msIn * a1 + sIn / (2.0 * n) * (1 - a1);
            msOut = msOut * a1 + sOut / (2.0 * n) * (1 - a1);
            float want = msOut > 1e-12 ? float(std::sqrt(msIn / msOut)) : 1.0f;
            want = std::max(1.0f, std::min(4.0f, want));
            makeup += (want - makeup) * 0.1f;
        }
        sendPrev = send;
        tailEnergy = energy;
    }
};

} // namespace djc
