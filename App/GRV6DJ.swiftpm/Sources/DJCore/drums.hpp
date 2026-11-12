// RMX-1000-style drum roller on the GRV6 GROOVE CIRCUIT section (owner's design, 2026-10-26).
// Left side (decks 1/3) = 909 kit, right side (decks 2/4) = 707 kit, from the owner's own sample packs
// (loaded by the app with djc_drum_set_sample; synthesised stand-ins until then).
// Pads: select a drum (lit) and play it. DRUM ROLL 1-4 = roll the selected drums at 1, 1/2, 1/4, 1/8 beat,
// locked to the grid of the deck you hear. Lever (has a lock) = HOLD the roll; lever back = stop drums + Beat FX.
// CAPTURE = headphone CUE for that side's drums.
#pragma once
#include "dsp.hpp"
#include <cstdint>
#include <vector>

namespace djc {

enum { DR_KICK = 0, DR_SNARE, DR_CLAP, DR_HAT, DR_COUNT };

struct DrumKit {
    std::vector<float> s[DR_COUNT];   // left (or mono)
    std::vector<float> r[DR_COUNT];   // right; empty = use left

    // tiny deterministic noise
    struct Rng { uint32_t x = 0x1234567u; float next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return float(int32_t(x)) / 2147483648.0f; } };

    void build(double sr, bool k707) {
        Rng rng;
        auto len = [&](double sec) { return size_t(sec * sr); };
        // KICK: sine with pitch drop + click
        {
            double f0 = k707 ? 130 : 165, f1 = k707 ? 52 : 46, pd = k707 ? 0.025 : 0.045, dec = k707 ? 0.22 : 0.42;
            auto &b = s[DR_KICK]; b.assign(len(dec * 4), 0);
            double ph = 0;
            for (size_t i = 0; i < b.size(); ++i) {
                double t = double(i) / sr;
                double f = f1 + (f0 - f1) * std::exp(-t / pd);
                ph += 2 * kPi * f / sr;
                double a = std::exp(-t / dec);
                double click = std::exp(-t / 0.0015) * (k707 ? 0.5 : 0.3) * rng.next();
                double v = std::sin(ph) * a + click;
                b[i] = float(std::tanh(v * (k707 ? 1.6 : 1.3)) * 0.95);
            }
        }
        // SNARE: two body tones + filtered noise
        {
            double dec = k707 ? 0.13 : 0.19, nd = k707 ? 0.16 : 0.22;
            double t1 = k707 ? 240 : 185, t2 = k707 ? 390 : 330;
            auto &b = s[DR_SNARE]; b.assign(len(nd * 4), 0);
            Biquad hp; hp.highpass(k707 ? 1800 : 1200, sr, 0.7);
            for (size_t i = 0; i < b.size(); ++i) {
                double t = double(i) / sr;
                double tone = (std::sin(2 * kPi * t1 * t) * 0.6 + std::sin(2 * kPi * t2 * t) * 0.4) * std::exp(-t / (dec * 0.5));
                double noise = hp.tick(rng.next()) * std::exp(-t / nd) * (k707 ? 0.9 : 0.75);
                b[i] = float(std::tanh((tone * 0.7 + noise) * 1.4) * 0.8);
            }
        }
        // CLAP: band-passed noise, a few quick bursts then a short tail
        {
            int bursts = k707 ? 2 : 3;
            double spacing = k707 ? 0.008 : 0.011, tail = k707 ? 0.09 : 0.16, fc = k707 ? 1600 : 1150;
            auto &b = s[DR_CLAP]; b.assign(len(tail * 5 + 0.05), 0);
            Biquad bp1, bp2; bp1.highpass(fc * 0.7, sr, 0.8); bp2.lowpass(fc * 1.8, sr, 0.8);
            for (size_t i = 0; i < b.size(); ++i) {
                double t = double(i) / sr, env = 0;
                for (int k = 0; k < bursts; ++k) {
                    double tk = t - k * spacing;
                    if (tk >= 0 && tk < spacing) env = std::max(env, std::exp(-tk / 0.003));
                }
                double tt = t - (bursts - 1) * spacing;
                if (tt >= 0) env = std::max(env, 0.8 * std::exp(-tt / tail));
                double n = bp2.tick(bp1.tick(rng.next()));
                b[i] = float(std::tanh(n * env * 3.0) * 0.8);
            }
        }
        // HAT (closed): metallic square partials + high-passed noise
        {
            double dec = k707 ? 0.035 : 0.05;
            static const double fr[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
            auto &b = s[DR_HAT]; b.assign(len(dec * 6), 0);
            Biquad hp; hp.highpass(k707 ? 9000 : 7000, sr, 0.7);
            double ph[6] = {0, 0, 0, 0, 0, 0};
            for (size_t i = 0; i < b.size(); ++i) {
                double t = double(i) / sr, m = 0;
                for (int k = 0; k < 6; ++k) {
                    ph[k] += fr[k] * 2.3 / sr; ph[k] -= std::floor(ph[k]);
                    m += ph[k] < 0.5 ? 1 : -1;
                }
                double v = hp.tick(float(m / 6.0 * 0.6 + rng.next() * 0.6));
                b[i] = float(v * std::exp(-t / dec) * 1.6);
            }
        }
    }
};

struct DrumRack {
    static constexpr int kVoices = 16;
    static constexpr double kRollBeats[4] = {1.0, 0.5, 0.25, 0.125};
    DrumKit kit;
    struct Voice { int inst = -1; double pos = 0; float gain = 1; int fade = 0; } voice[kVoices];
    static constexpr int kChoke = 96;  // 2 ms fade when the same drum retriggers (tight rolls, like the RMX)
    double sr = 48000;
    bool selected[DR_COUNT] = {};
    int rollDown = -1;          // DRUM ROLL button held (0..3) or -1
    int rate = 2;               // last roll rate (used by HOLD)
    bool hold = false;          // lever locked / pushed
    bool rollEnabled = true;    // DRUM ROLL buttons work
    bool cue = false;           // CAPTURE/LENGTH lit = this side's drums also go to the headphone cue
    int lastInst = DR_KICK;
    float gain = 1.0f;
    float pitch = 0;            // semitones, -12..+12
    double lastKnob = -1;
    double lastSlot = -1;
    std::vector<float> dl;
    int dlLen = 0, wpos = 0;
    float echoFeed = 0, echoEnergy = 0;

    void prepare(double rate_, bool k707) {
        sr = rate_;
        kit.build(sr, k707);
        dlLen = int(sr * 2.0) + 2;
        dl.assign(size_t(dlLen), 0.0f);
    }

    void trigger(int inst, float g = 1.0f) {
        int slot = 0; double best = -1;
        for (int k = 0; k < kVoices; ++k) {
            if (voice[k].inst < 0) { slot = k; break; }
            if (voice[k].pos > best) { best = voice[k].pos; slot = k; }
        }
        for (auto &v : voice) if (v.inst == inst && v.fade == 0) v.fade = kChoke; // choke the previous hit
        voice[slot] = {inst, 0.0, g, 0};
        lastInst = inst;
    }

    // pad press: select this drum only (radio button) and play it; while rolling it just switches the drum
    void pad(int inst, bool down) {
        if (inst < 0 || inst >= DR_COUNT || !down) return;
        for (bool &b : selected) b = false;
        selected[inst] = true;
        lastInst = inst;
        if (rollDown < 0 && !hold) trigger(inst);
    }

    bool rolling() const { return rollDown >= 0 || hold; }

    void roll(int idx, bool down) {
        if (idx < 0 || idx > 3) return;
        if (down) {
            if (!rollEnabled) return;
            if (rollDown < 0) lastSlot = -1;
            rollDown = idx; rate = idx;           // one roll button at a time: the newest wins
        } else if (rollDown == idx) rollDown = -1;
    }

    void setRollEnabled(bool on) {
        rollEnabled = on;
        if (!on) { rollDown = -1; if (hold) { hold = false; echoFeed = 1.0f; } }
    }

    void setHold(bool on) {
        if (on == hold) return;
        if (on && !rollEnabled) return;
        hold = on;
        if (on) lastSlot = -1;
        else if (rollDown < 0) echoFeed = 1.0f; // let go: echo tail
    }

    // lever back (RMX-1000 RELEASE FX style): everything on this side goes silent, echo included.
    // 5 ms fade so the cut doesn't click.
    int killN = 0;
    static constexpr int kKill = 240;
    void stopAll() {
        rollDown = -1; hold = false; echoFeed = 0;
        if (active()) killN = kKill;
    }
    // the grid source changed (another deck became the one you hear): re-lock on its next grid line
    void resync() { lastSlot = -1; }

    // GAIN knob: unity (same level as a full-scale track) at ~63 %, +6 dB at the end. Was linear x0.7, which left
    // the drums ~9 dB under the music at half-way (owner: "super low").
    void setGain(double v) { v = std::max(0.0, std::min(1.0, v)); gain = float(2.0 * std::pow(v, 1.5)); }
    // knob 0..1 -> -12..+12 semitones, centre zone = 0
    void setPitchKnob(double v) {
        double x = (v - 0.5) * 2.0;
        if (std::fabs(x) < 0.04) x = 0;
        pitch = float(std::max(-12.0, std::min(12.0, x * 12.0)));
    }

    // trim leading silence so every hit lands exactly on the grid
    static void trimFront(std::vector<float> &l, std::vector<float> &r) {
        size_t i = 0;
        while (i + 1 < l.size() && std::fabs(l[i]) < 0.003f && (r.empty() || std::fabs(r[i]) < 0.003f)) ++i;
        if (i > 0 && i + 64 < l.size()) {
            l.erase(l.begin(), l.begin() + long(i));
            if (!r.empty()) r.erase(r.begin(), r.begin() + long(i));
        }
    }

    bool active() const {
        if (hold || rollDown >= 0 || echoEnergy > 1e-5f || echoFeed > 0) return true;
        for (auto &v : voice) if (v.inst >= 0) return true;
        return false;
    }

    void process(float *L, float *R, int n, double beats0, double beatsPerFrame) {
        if (!active()) return;
        int r = rollDown >= 0 ? rollDown : (hold ? rate : -1);
        bool rolling = r >= 0;
        bool want[DR_COUNT];
        bool any = false;
        for (int k = 0; k < DR_COUNT; ++k) { want[k] = selected[k]; any |= want[k]; }
        if (rolling && !any) want[lastInst] = true;
        double step = rolling ? kRollBeats[r] : 1.0;
        double speed = std::pow(2.0, pitch / 12.0);
        for (int k = 0; k < n; ++k) {
            if (rolling) {
                double b = beats0 + beatsPerFrame * k;
                double slot = std::floor(b / step + 1e-9);
                if (slot != lastSlot) {
                    if (lastSlot >= 0 || std::fabs(b / step - slot) < 0.02) {
                        for (int i = 0; i < DR_COUNT; ++i) if (want[i]) trigger(i); // straight, even hits (no swing)
                    }
                    lastSlot = slot;
                }
            }
            float sl = 0, sr_ = 0;
            for (auto &v : voice) {
                if (v.inst < 0) continue;
                auto &bl = kit.s[v.inst];
                auto &br = kit.r[v.inst].empty() ? bl : kit.r[v.inst];
                size_t i = size_t(v.pos);
                if (i + 1 >= bl.size()) { v.inst = -1; continue; }
                float t = float(v.pos - double(i));
                float g = v.gain;
                if (v.fade > 0) { g *= float(v.fade) / kChoke; if (--v.fade == 0) { v.inst = -1; continue; } }
                sl += (bl[i] + (bl[i + 1] - bl[i]) * t) * g;
                sr_ += (br[i] + (br[i + 1] - br[i]) * t) * g;
                v.pos += speed;
            }
            float g = gain;
            sl *= g; sr_ *= g;
            float e = 0;
            if (dlLen > 0) {
                int d = std::min(dlLen - 1, std::max(1, int(0.75 / std::max(beatsPerFrame, 1e-9))));
                int rp = wpos - d; if (rp < 0) rp += dlLen;
                e = dl[size_t(rp)];
                dl[size_t(wpos)] = 0.5f * (sl + sr_) * echoFeed + e * 0.55f;
                if (++wpos >= dlLen) wpos = 0;
                echoEnergy = std::max(echoEnergy * 0.99995f, std::fabs(e));
            }
            if (echoFeed > 0) echoFeed = std::max(0.0f, echoFeed - float(1.0 / (0.25 * sr)));
            float kg = 1.0f;
            if (killN > 0) {
                kg = float(killN) / kKill;
                if (--killN == 0) { // silent now: clear the voices and the echo line
                    for (auto &v : voice) v.inst = -1;
                    std::fill(dl.begin(), dl.end(), 0.0f);
                    echoEnergy = 0;
                    L[k] += (sl + e * 0.8f) * kg; R[k] += (sr_ + e * 0.8f) * kg;
                    break;
                }
            }
            L[k] += (sl + e * 0.8f) * kg;
            R[k] += (sr_ + e * 0.8f) * kg;
        }
        if (!rolling) lastSlot = -1;
    }
};

} // namespace djc
