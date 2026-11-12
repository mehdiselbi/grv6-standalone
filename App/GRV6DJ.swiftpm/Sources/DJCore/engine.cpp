// DJCore engine. See include/djcore.h for the threading contract.
// Real-time rule: nothing reachable from djc_render allocates, locks, logs or blocks.
#include "djcore.h"
#include "dsp.hpp"
#include "fx.hpp"
#include "cfx.hpp"
#include "drums.hpp"

#if defined(__APPLE__)
#define SIGNALSMITH_USE_ACCELERATE 1 // vDSP FFT on iOS
#endif
#include "vendor/signalsmith-stretch.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

using namespace djc;

struct djc_track {
    std::vector<float> L, R;
    int64_t frames = 0;
    double sr = 48000;
    double bpm = 0;
    double firstBeat = 0; // frames
    std::vector<double> beatFrames; // rekordbox beat list (frames); empty = constant grid
    bool useBeats = false;
};

namespace {

template <typename T, size_t N> struct Spsc {
    T buf[N];
    std::atomic<size_t> head{0}, tail{0};
    bool push(const T &v) {
        size_t h = head.load(std::memory_order_relaxed), n = (h + 1) % N;
        if (n == tail.load(std::memory_order_acquire)) return false;
        buf[h] = v;
        head.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T &v) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        v = buf[t];
        tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }
};

constexpr int kMaxBlock = 512;
constexpr double kRpm = 100.0 / 3.0;

struct PublishedDeck {
    std::atomic<int32_t> loaded{0}, playing{0}, keylock{0}, sync{0}, master{0}, quantize{1}, loopActive{0}, vinyl{1};
    std::atomic<double> position{0}, duration{0}, trackBpm{0}, bpm{0}, rate{1}, tempoRange{0.1}, cuePoint{0},
        loopIn{-1}, loopOut{-1};
    std::atomic<double> hotcues[DJC_HOTCUES];
    std::atomic<double> tempo{0}, stamp{0};
    std::atomic<double> hotLoopEnd[DJC_HOTCUES];
    std::atomic<int32_t> keyShift{0}, keySync{0};
    std::atomic<float> onAir{0};
    std::atomic<float> peakL{0}, peakR{0};
};

struct PublishedMixer {
    std::atomic<float> trim[DJC_DECKS], hi[DJC_DECKS], mid[DJC_DECKS], low[DJC_DECKS], color[DJC_DECKS], fader[DJC_DECKS];
    std::atomic<int32_t> xf[DJC_DECKS], cue[DJC_DECKS];
    std::atomic<float> crossfader{0.5f}, masterLevel{0.84f}, phonesMix{0}, phonesLevel{1};
    std::atomic<int32_t> masterCue{0}, colorFxOn{1}, colorFxType{0};
    std::atomic<float> colorFxParam{0.5f};
};

struct Deck {
    djc_track *track = nullptr;
    double sr = 48000;
    // transport
    bool playing = false, cuePreview = false, keylock = false, sync = false, quantize = true, vinyl = true;
    double pos = 0;          // frames, what is heard now
    double tempo = 0;        // -1..1
    double range = 0.10;
    double rate = 1.0;       // effective, published
    double baseRate = 1.0;   // fader/sync rate without the jog nudge (shown as BPM, like a CDJ)
    double cue = 0;
    double hot[DJC_HOTCUES];
    double hotLoopEnd[DJC_HOTCUES] = {0, 0, 0, 0, 0, 0, 0, 0}; // frames, 0 = plain cue
    double mem[32];
    double memEnd[32] = {};  // loop end (frames), 0 = plain memory cue
    bool memActive[32] = {}; // rekordbox active loop: engages when playback reaches it
    int memCount = 0;
    bool loopActive = false;
    double loopIn = -1, loopOut = -1;
    // jog
    bool touched = false;
    double ticksPerRev = 720.0 / 0.85; // GRV6 sends 720 per turn (Mixxx); 0.85 = owner asked for a slower platter
    double nudgeGain = 0.03;  // rate offset per revolution/second of the jog (owner: lowest by default)
    bool nudgeSnap = false;   // removed at the owner's request 2026-11-06 (off; DJC_NUDGE_SNAP can re-enable it)
    // quantized hot cue (CDJ style): the jump waits for the next beat, then the cue plays exactly on it
    double pendJump = -1, pendBeat = 0, pendLoopEnd = 0;    // after a jog nudge, glide the beat onto the other deck's grid
    bool snapPending = false;
    double nudgeIdle = 0;
    int keyShift = 0;        // semitones
    bool keySync = false;
    int pendingTurn = 0, pendingSearch = 0;
    double jogVel = 0;       // ticks per second, smoothed (nudge)
    double scratchTarget = 0, scratchVel = 0;
    // keylock
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    bool stretchActive = false, needPrime = true;
    double readPos = 0;      // next source frame fed to the stretcher
    double feedFrac = 0;
    std::vector<float> inBuf[2];
    int inCap = 0;
    float out[2][kMaxBlock];

    Deck() { for (auto &h : hot) h = -1; }

    double fpb() const { return (track && track->bpm > 0) ? sr * 60.0 / track->bpm : 0; }
    double gridOrigin() const { return track ? track->firstBeat : 0; }
    // grid on the rekordbox beat list (the grid drawn on the waveform), constant grid when there is none
    double snap(double p) const {
        if (!quantize || fpb() <= 0) return p;
        return frameAt(std::round(beats(p)));
    }
    double floorBeat(double p) const {
        if (fpb() <= 0) return p;
        return frameAt(std::floor(beats(p) + 1e-6));
    }
    double frameAt(double x) const { // inverse of beats(): beat number (fractional) -> frame
        double b = fpb();
        if (b <= 0) return 0;
        if (track->useBeats) {
            const auto &bf = track->beatFrames;
            double n = double(bf.size());
            double base = std::round((bf[0] - gridOrigin()) / b);
            double i = x - base;
            if (i <= 0) return bf[0] + i * b;
            if (i >= n - 1) return bf[size_t(n - 1)] + (i - (n - 1)) * b;
            size_t k = size_t(std::floor(i));
            return bf[k] + (i - double(k)) * (bf[k + 1] - bf[k]);
        }
        return gridOrigin() + x * b;
    }
    double beats(double p) const { // absolute beat position on the grid (rekordbox beat list when we have it)
        double b = fpb();
        if (b <= 0) return 0;
        if (track->useBeats) {
            const auto &bf = track->beatFrames;
            size_t n = bf.size();
            // beat index 0 of the list sits at (bf[0] - origin) / fpb on the constant grid, keep that numbering
            double base = std::round((bf[0] - gridOrigin()) / b);
            if (p <= bf[0]) return base + (p - bf[0]) / b;
            if (p >= bf[n - 1]) return base + double(n - 1) + (p - bf[n - 1]) / b;
            size_t lo = 0, hi = n - 1;
            while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (bf[mid] <= p) lo = mid; else hi = mid; }
            return base + double(lo) + (p - bf[lo]) / (bf[hi] - bf[lo]);
        }
        return (p - gridOrigin()) / b;
    }
    double phase(double p) const { // 0..1 position inside the beat (beat list grid)
        if (fpb() <= 0) return 0;
        double x = beats(p);
        return x - std::floor(x);
    }
    // fader tempo, rounded to 0.1 BPM (121.05 -> 121.1, 121.04 -> 121.0): what you see is exactly what plays
    double faderBase() const {
        double base = 1.0 + range * tempo;
        if (track && track->bpm > 0) base = std::round(track->bpm * base * 10.0 + 1e-9) / 10.0 / track->bpm;
        return base;
    }
    void jump(double p) {
        if (!track) return;
        pos = std::max(0.0, std::min(p, double(track->frames)));
        needPrime = true;
        scratchTarget = pos;
    }
    inline float sample(int c, int64_t i) const {
        if (i < 0 || i >= track->frames) return 0.0f;
        return c == 0 ? track->L[size_t(i)] : track->R[size_t(i)];
    }
    // advance a source position forward with loop wrap
    inline double wrapFwd(double p) const {
        if (loopActive && loopOut > loopIn) {
            double len = loopOut - loopIn;
            while (p >= loopOut) p -= len;
        }
        return p;
    }
};

} // namespace

struct djc_engine {
    // set recorder ring (interleaved stereo), sized in djc_create, single producer (audio) / single consumer
    std::vector<float> rec;
    size_t recCap = 0;                       // frames
    std::atomic<size_t> recW{0}, recR{0};
    std::atomic<int> recOn{0};
    std::atomic<int64_t> recDropped{0};
    double sr;
    int maxFrames;
    Spsc<djc_cmd, 2048> queues[2];
    Spsc<djc_track *, 64> garbage;
    Deck decks[DJC_DECKS];
    PublishedDeck pub[DJC_DECKS];
    PublishedMixer pubMix;
    int masterDeck = -1;
    double blockStart[DJC_DECKS] = {0, 0, 0, 0}; // positions at block start, for phase compare

    // mixer
    struct Channel {
        float trim = 0.5f, hi = 0.5f, mid = 0.5f, low = 0.5f, color = 0.5f, fader = 1.0f; // UI sets 0 when a controller is found
        int xf = 1;
        bool cue = false;
        float gPrev = 1.0f, cPrev = 0.0f;
        float eqPrev[3] = {1, 1, 1};
        Isolator iso[2];
        Svf svf[2];
        float colorPrev = 0.5f;
        SvfCoeffs coeffs;
        ColorFx cfx;
    } ch[DJC_DECKS];
    int cfxType = CFX_FILTER;
    DrumRack drums[2];          // GROOVE CIRCUIT: [0] left (decks 1/3, 909-style), [1] right (decks 2/4, 707-style)
    double freeBeats = 0;       // drum clock when no deck is playing
    int drumSrc = -1;           // deck whose grid the drums follow (the one you hear)
    std::atomic<int32_t> drumState[2];  // published: bit0 hold, bits1-4 pads sounding/held, bits 8-11 roll held
    float cfxParam = 0.5f; // COLOR FX PARAMETER (A9-style intensity)
    float crossfader = 0.5f, masterLevel = 0.8409f, phonesMix = 0.0f, phonesLevel = 1.0f;
    bool masterCue = false, splitCue = false, colorOn = true;
    int routeML = 0, routeMR = 1, routePL = -1, routePR = -1;
    float mPrev = 1.0f;
    // master EQ (tap the level meter): low shelf 100 Hz, mid peak 1 kHz, high shelf 10 kHz, before the limiter
    float masterEqDb[3] = {0, 0, 0};
    Biquad meq[2][3];
    bool meqOn = false;
    // master limiter: 1.5 ms look-ahead, brick-wall at -0.3 dBFS, 120 ms release (no clipping when kicks stack)
    static constexpr int kLook = 72;
    float limDl[2][kLook] = {};
    float limReq[kLook] = {};
    int limPos = 0;
    float limGain = 1.0f;
    float limGR = 1.0f;            // smallest gain this block (for the CLIP light)

    float bus[2][kMaxBlock], cueBus[2][kMaxBlock], chBuf[2][kMaxBlock], drumBus[2][kMaxBlock], sideBuf[2][kMaxBlock];
    float audible[DJC_DECKS] = {};  // fader x crossfader gain per channel this block
    BeatFx fx;
    std::atomic<int32_t> fxType{FX_ECHO}, fxTarget{4}, fxOn{0}, fxBeat{3};
    std::atomic<float> fxLevel{0.5f};
    std::atomic<int32_t> fxBands{7};
    std::atomic<double> fxBpmPub{120};
    std::atomic<float> mPeakL{0}, mPeakR{0};
    std::atomic<int32_t> xruns{0};
    std::atomic<double> cpu{0};
};

// ---------- helpers ----------

static float trimGain(float x) { // -24 dB .. 0 dB (center) .. +9 dB
    float db = x < 0.5f ? (x - 0.5f) * 48.0f : (x - 0.5f) * 18.0f;
    return x <= 0.001f ? 0.0f : dbToGain(db);
}
static float eqGain(float x) { // kill .. 0 dB (center) .. +6 dB
    if (x <= 0.02f) return 0.0f;
    float db = x < 0.5f ? -26.0f * (0.5f - x) / 0.48f : (x - 0.5f) * 12.0f;
    return dbToGain(db);
}
static float faderGain(float x) { return x * x; }
static void xfGains(float x, float &a, float &b) {
    a = x <= 0.5f ? 1.0f : float(std::cos((x - 0.5f) * kPi));
    b = x >= 0.5f ? 1.0f : float(std::cos((0.5f - x) * kPi));
}
static inline float softLimit(float s) {
    float a = std::fabs(s);
    if (a <= 0.98f) return s;
    float y = 0.98f + 0.02f * std::tanh((a - 0.98f) / 0.02f);
    return s < 0 ? -y : y;
}

// ---------- commands (audio thread) ----------

static void releaseTrack(djc_engine *e, djc_track *t) {
    if (t && !e->garbage.push(t)) {
        // garbage queue full: leak rather than free on the audio thread
    }
}

static void handle(djc_engine *e, const djc_cmd &c) {
    if (c.type >= 200) {
        switch (c.type) {
        case DJC_CROSSFADER: e->crossfader = float(c.v); break;
        case DJC_MASTER_LEVEL: e->masterLevel = float(c.v); break;
        case DJC_MASTER_EQ: {
            int b = c.i;
            if (b < 0 || b > 2) break;
            e->masterEqDb[b] = float(std::max(-12.0, std::min(6.0, c.v)));
            for (int s = 0; s < 2; ++s) {
                auto &q = e->meq[s][b];
                if (b == 0) q.lowShelf(100.0, e->sr, e->masterEqDb[0]);
                else if (b == 1) q.peak(1000.0, e->sr, 0.7, e->masterEqDb[1]);
                else q.highShelf(10000.0, e->sr, e->masterEqDb[2]);
            }
            e->meqOn = e->masterEqDb[0] != 0 || e->masterEqDb[1] != 0 || e->masterEqDb[2] != 0;
        } break;
        case DJC_PHONES_MIX: e->phonesMix = float(c.v); break;
        case DJC_PHONES_LEVEL: e->phonesLevel = float(c.v); break;
        case DJC_MASTER_CUE: e->masterCue = c.i == 2 ? !e->masterCue : c.i != 0; break;
        case DJC_COLOR_FX_ON: {
            bool was = e->colorOn;
            e->colorOn = c.i == 2 ? !e->colorOn : c.i != 0;
            if (was && !e->colorOn) for (auto &m : e->ch) m.cfx.kill(); // ON/OFF off cuts the tails too
        } break;
        case DJC_DUB_BEATS: for (auto &m : e->ch) m.cfx.dubBeats = float(std::max(0.25, std::min(2.0, c.v))); break;
        case DJC_COLOR_FX_PARAM: e->cfxParam = float(std::max(0.0, std::min(1.0, c.v))); break;
        case DJC_FX_BAND: if (c.i >= 0 && c.i < 3) e->fx.bandMask ^= (1 << c.i); break;
        case DJC_DRUM_PAD: e->drums[c.deck & 1].pad(c.i, c.v > 0.5); break;
        case DJC_DRUM_ROLL: { // only one side may roll at a time
            int sd = c.deck & 1;
            if (c.v > 0.5 && e->drums[1 - sd].rolling()) break;
            e->drums[sd].roll(c.i, c.v > 0.5);
        } break;
        case DJC_DRUM_HOLD: {
            int sd = c.deck & 1;
            if (c.i != 0) { if (!e->drums[1 - sd].rolling()) e->drums[sd].setHold(true); break; }
            // lever back (RMX-1000 RELEASE FX): drums, their echo, Beat FX and colour FX tails all cut (5-10 ms fade)
            for (auto &r : e->drums) r.stopAll();
            e->fx.kill();
            for (auto &m : e->ch) m.cfx.kill();
        } break;
        case DJC_DRUM_RELEASE: e->drums[c.deck & 1].setHold(false); break;
        case DJC_DRUM_GAIN: // left knob = GAIN of both kits, right knob = PITCH of both kits
            for (auto &r : e->drums) { if ((c.deck & 1) == 0) r.setGain(c.v); else r.setPitchKnob(c.v); }
            break;
        case DJC_DRUM_CUE: { auto &r = e->drums[c.deck & 1]; r.cue = !r.cue; } break;
        case DJC_COLOR_FX_TYPE: {
            int t = c.i < 0 ? (e->cfxType + 1) % CFX_COUNT : std::min(c.i, CFX_COUNT - 1);
            if (t != e->cfxType) { for (auto &m : e->ch) m.cfx.clear(); e->cfxType = t; }
            e->colorOn = true;
        } break;
        case DJC_FX_TYPE: if (c.i >= 0 && c.i < FX_COUNT) { e->fx.type = c.i; e->fx.tail = false; } break;
        case DJC_FX_TARGET: if (c.i >= 0 && c.i <= 5) { e->fx.target = c.i; e->fx.tail = false; } break;
        case DJC_FX_ON: e->fx.on = c.i == 2 ? !e->fx.on : c.i != 0; break;
        case DJC_FX_LEVEL: e->fx.level = float(std::max(0.0, std::min(1.0, c.v))); break;
        case DJC_FX_BEAT: e->fx.beatIdx = std::max(0, std::min(8, e->fx.beatIdx + c.i)); break;
        case DJC_SPLIT_CUE: e->splitCue = c.i != 0; break;
        case DJC_ROUTING: {
            auto f = [](int v) { return v == 0xFF ? -1 : v; };
            e->routeML = f(c.i & 0xFF); e->routeMR = f((c.i >> 8) & 0xFF);
            e->routePL = f((c.i >> 16) & 0xFF); e->routePR = f((c.i >> 24) & 0xFF);
        } break;
        }
        return;
    }
    if (c.deck < 0 || c.deck >= DJC_DECKS) return;
    if (c.type >= 100) {
        auto &m = e->ch[c.deck];
        switch (c.type) {
        case DJC_TRIM: m.trim = float(c.v); break;
        case DJC_EQ_HI: m.hi = float(c.v); break;
        case DJC_EQ_MID: m.mid = float(c.v); break;
        case DJC_EQ_LOW: m.low = float(c.v); break;
        case DJC_COLOR: m.color = float(c.v); break;
        case DJC_FADER: m.fader = float(c.v); break;
        case DJC_XF_ASSIGN: m.xf = c.i; break;
        case DJC_CH_CUE: m.cue = c.i == 2 ? !m.cue : c.i != 0; break;
        }
        return;
    }
    Deck &d = e->decks[c.deck];
    if (c.type == DJC_LOAD) {
        releaseTrack(e, d.track);
        d.track = static_cast<djc_track *>(c.ptr);
        d.playing = false; d.cuePreview = false; d.pos = 0; d.cue = 0;
        d.loopActive = false; d.loopIn = d.loopOut = -1;
        for (auto &h : d.hot) h = -1;
        for (auto &h : d.hotLoopEnd) h = 0;
        d.memCount = 0;
        d.needPrime = true; d.scratchTarget = 0; d.scratchVel = 0;
        if (d.track && d.track->firstBeat > 0) d.cue = d.track->firstBeat;
        d.pos = d.cue;
        if (e->masterDeck == c.deck) e->masterDeck = -1;
        return;
    }
    if (c.type == DJC_TEMPO) { d.tempo = std::max(-1.0, std::min(1.0, c.v)); return; }
    if (c.type == DJC_TEMPO_RANGE) {
        if (c.v < 0) {
            static const double ranges[4] = {0.06, 0.10, 0.16, 1.0};
            int k = 0;
            while (k < 4 && ranges[k] <= d.range + 1e-9) ++k;
            d.range = ranges[k % 4];
        } else {
            d.range = c.v;
        }
        return;
    }
    if (c.type == DJC_KEYLOCK) { d.keylock = c.i == 2 ? !d.keylock : c.i != 0; d.needPrime = true; return; }
    if (c.type == DJC_QUANTIZE) { d.quantize = c.i == 2 ? !d.quantize : c.i != 0; return; }
    if (c.type == DJC_VINYL_MODE) { d.vinyl = c.i == 2 ? !d.vinyl : c.i != 0; return; }
    if (c.type == DJC_JOG_TICKS_PER_REV) { if (c.v > 0) d.ticksPerRev = c.v; return; }
    if (c.type == DJC_JOG_NUDGE_GAIN) { if (c.v > 0) d.nudgeGain = c.v; return; }
    if (c.type == DJC_NUDGE_SNAP) { d.nudgeSnap = c.i != 0; if (!d.nudgeSnap) d.snapPending = false; return; }
    if (c.type == DJC_KEY_SHIFT) {
        int k = c.i == 100 ? 0 : std::max(-12, std::min(12, c.i == 101 ? d.keyShift + int(c.v) : int(c.v)));
        if (k != d.keyShift) { d.keyShift = k; d.needPrime = true; }
        return;
    }
    if (c.type == DJC_KEY_SYNC) { d.keySync = c.i == 2 ? !d.keySync : c.i != 0; return; }
    if (c.type == DJC_MAKE_MASTER) { if (d.track) e->masterDeck = c.deck; return; }
    if (c.type == DJC_SYNC) {
        bool on = c.i == 2 ? !d.sync : c.i != 0;
        d.sync = on;
        if (on) {
            if (e->masterDeck < 0 || e->masterDeck == c.deck) {
                // pick another playing deck as master, else become master
                int m = -1;
                for (int k = 0; k < DJC_DECKS; ++k)
                    if (k != c.deck && e->decks[k].track && e->decks[k].playing) { m = k; break; }
                e->masterDeck = m >= 0 ? m : c.deck;
            }
            // snap phase once
            int m = e->masterDeck;
            if (m != c.deck && d.track && e->decks[m].track && d.fpb() > 0) {
                double err = e->decks[m].phase(e->decks[m].pos) - d.phase(d.pos);
                err -= std::round(err);
                d.jump(d.pos + err * d.fpb());
            }
        } else if (e->masterDeck == c.deck) {
            e->masterDeck = -1;
        }
        return;
    }
    if (c.type == DJC_SET_CUE) { d.cue = c.v * d.sr; if (!d.playing) d.jump(d.cue); return; }
    if (c.type == DJC_SET_HOTCUE) {
        if (c.i >= 0 && c.i < DJC_HOTCUES) { d.hot[c.i] = c.v * d.sr; d.hotLoopEnd[c.i] = c.v2 > c.v ? c.v2 * d.sr : 0; }
        return;
    }
    if (c.type == DJC_SET_MEMCUE) {
        if (c.i < 0) d.memCount = 0;
        else {
            int k = c.i & 0xFF; // bit 8 = active loop
            if (k < 32) {
                d.mem[k] = c.v * d.sr;
                d.memEnd[k] = c.v2 > c.v ? c.v2 * d.sr : 0;
                d.memActive[k] = (c.i & 0x100) && d.memEnd[k] > 0;
                d.memCount = std::max(d.memCount, k + 1);
            }
        }
        return;
    }
    if (c.type == DJC_SET_GRID) {
        if (d.track) { d.track->bpm = c.v; d.track->firstBeat = c.v2 * d.sr; d.track->useBeats = false; }
        return;
    }
    if (!d.track) return;

    switch (c.type) {
    case DJC_PLAY:
        if (d.cuePreview) { d.cuePreview = false; d.playing = true; }
        else d.playing = !d.playing;
        if (d.playing) d.needPrime = true;
        break;
    case DJC_CUE:
        if (c.i) {
            if (d.playing && !d.cuePreview) { d.playing = false; d.jump(d.cue); }
            else if (!d.playing) {
                if (std::fabs(d.pos - d.cue) > 1.0) d.cue = d.snap(d.pos);
                d.jump(d.cue);
                d.playing = true; d.cuePreview = true;
            }
        } else if (d.cuePreview) {
            d.cuePreview = false; d.playing = false; d.jump(d.cue);
        }
        break;
    case DJC_SEEK: d.jump(c.v * d.sr); break;
    case DJC_JOG_TOUCH:
        d.touched = c.i != 0;
        if (d.touched) { d.scratchTarget = d.pos; d.scratchVel = 0; } // hold the record: stop dead
        else d.needPrime = true;
        break;
    case DJC_JOG_TURN: d.pendingTurn += c.i; break;
    case DJC_JOG_SEARCH: d.pendingSearch += c.i; break;
    case DJC_HOTCUE: {
        int i = c.i;
        if (i < 0 || i >= DJC_HOTCUES || c.v < 0.5) break;
        if (d.hot[i] < 0) { d.hot[i] = d.snap(d.pos); break; }
        double target = d.hot[i];
        d.pendJump = -1;
        if (d.playing && d.quantize && d.fpb() > 0) {
            // quantize: wait for the next beat of this deck's grid, then the cue's beat lands exactly on it.
            // Pressed just after a beat (< 6 %): go now, keeping the phase (the miss is a few ms at most).
            double x = d.beats(d.pos), ph = x - std::floor(x);
            double tb = d.beats(target);
            if (std::fabs(tb - std::round(tb)) < 0.03) target = d.frameAt(std::round(tb)); // cue on the grid: exact
            if (ph < 0.06) {
                if (d.hotLoopEnd[i] > d.hot[i]) { d.loopIn = d.hot[i]; d.loopOut = d.hotLoopEnd[i]; d.loopActive = true; }
                else d.loopActive = false;
                d.jump(d.frameAt(d.beats(target) + ph));
            } else {
                d.pendJump = target; d.pendBeat = std::floor(x) + 1;
                d.pendLoopEnd = d.hotLoopEnd[i] > d.hot[i] ? d.hotLoopEnd[i] : 0;
            }
            d.cuePreview = false;
            break;
        }
        if (d.hotLoopEnd[i] > target) { // hot loop: jump to its start and loop it
            d.loopIn = target; d.loopOut = d.hotLoopEnd[i]; d.loopActive = true;
            d.jump(target);
            if (!d.playing) d.playing = true;
            d.cuePreview = false;
            break;
        }
        d.loopActive = false; // CDJ: a hot cue leaves the loop (RELOOP brings it back)
        d.jump(target);
        if (!d.playing) d.playing = true;
        d.cuePreview = false;
    } break;
    case DJC_HOTCUE_DELETE: if (c.i >= 0 && c.i < DJC_HOTCUES) { d.hot[c.i] = -1; d.hotLoopEnd[c.i] = 0; } break;
    case DJC_LOOP_IN: d.loopIn = d.snap(d.pos); d.loopActive = false; d.loopOut = -1; break;
    case DJC_LOOP_OUT: {
        double o = d.snap(d.pos);
        if (d.loopIn >= 0 && o > d.loopIn + 64) { d.loopOut = o; d.loopActive = true; }
    } break;
    case DJC_LOOP_EXIT:
        if (d.loopActive) d.loopActive = false;
        else if (d.loopIn >= 0 && d.loopOut > d.loopIn) { d.loopActive = true; d.jump(d.loopIn); }
        break;
    case DJC_BEAT_LOOP: {
        double b = d.fpb();
        if (b <= 0 || c.v <= 0) break;
        d.loopIn = d.quantize ? d.floorBeat(d.pos) : d.pos;
        d.loopOut = d.loopIn + c.v * b;
        d.loopActive = true;
        if (d.pos >= d.loopOut) d.jump(d.loopIn);
    } break;
    case DJC_LOOP_HALVE:
        if (d.loopOut > d.loopIn && d.loopIn >= 0) {
            d.loopOut = d.loopIn + (d.loopOut - d.loopIn) * 0.5;
            if (d.loopActive && d.pos >= d.loopOut) d.jump(d.wrapFwd(d.pos));
        }
        break;
    case DJC_LOOP_DOUBLE:
        if (d.loopOut > d.loopIn && d.loopIn >= 0) d.loopOut = d.loopIn + (d.loopOut - d.loopIn) * 2.0;
        break;
    case DJC_CUE_CALL: {
        if (d.loopActive && d.loopIn >= 0 && d.loopOut > d.loopIn) { // CDJ: during a loop, CALL halves / doubles it
            double len = d.loopOut - d.loopIn;
            d.loopOut = d.loopIn + (c.i < 0 ? len * 0.5 : len * 2.0);
            if (d.pos >= d.loopOut) d.jump(d.wrapFwd(d.pos));
            break;
        }
        // nearest cue point (main cue or hot cue) before / after the playhead
        double best = c.i < 0 ? -1.0 : 1e18;
        auto consider = [&](double p) {
            if (p < 0) return;
            if (c.i < 0 && p < d.pos - d.sr * 0.05 && p > best) best = p;
            if (c.i > 0 && p > d.pos + d.sr * 0.05 && p < best) best = p;
        };
        consider(d.cue);
        for (double h : d.hot) consider(h);
        for (int k = 0; k < d.memCount; ++k) consider(d.mem[k]);
        if (best >= 0 && best < 1e17) {
            d.cue = best;
            d.jump(best);
            for (int k = 0; k < d.memCount; ++k) // calling a stored loop sets it and starts looping
                if (d.mem[k] == best && d.memEnd[k] > best) { d.loopIn = best; d.loopOut = d.memEnd[k]; d.loopActive = true; break; }
        }
    } break;
    case DJC_BEAT_JUMP: {
        double b = d.fpb();
        if (b <= 0) break;
        double delta = c.v * b;
        if (d.loopActive) { d.loopIn += delta; d.loopOut += delta; }
        d.jump(d.pos + delta);
    } break;
    }
}

// ---------- deck rendering (audio thread) ----------

static double computeRate(djc_engine *e, int idx, int n) {
    Deck &d = e->decks[idx];
    double base = d.faderBase();
    int m = e->masterDeck;
    if (d.sync && m >= 0 && m != idx && e->decks[m].track && d.track && d.track->bpm > 0) {
        Deck &md = e->decks[m];
        double mBase = md.faderBase();
        base = md.track->bpm * mBase / d.track->bpm;
        if (d.playing && md.playing && d.fpb() > 0) {
            double err = md.phase(e->blockStart[m]) - d.phase(d.pos);
            err -= std::round(err);
            double corr = err * d.fpb() / d.sr * 0.5; // fix half the error per second
            base *= 1.0 + std::max(-0.02, std::min(0.02, corr));
        }
    }
    // nudge: smoothed jog velocity -> temporary rate offset
    double secs = double(n) / d.sr;
    double inst = (d.touched && d.vinyl) ? 0.0 : d.pendingTurn / std::max(secs, 1e-6);
    double a = 1.0 - std::exp(-secs / 0.15); // smooth bursty MIDI ticks
    d.jogVel += (inst - d.jogVel) * a;
    // gentle pitch bend: one full turn of the wheel side moves the track by nudgeGain seconds in total
    double nudge = d.jogVel / d.ticksPerRev * d.nudgeGain;
    d.baseRate = base;
    // nudge snap: once the wheel is let go, glide the beat onto the nearest beat of the deck it is mixed with
    if (d.pendingTurn != 0 && !d.touched) { d.snapPending = d.nudgeSnap; d.nudgeIdle = 0; }
    else d.nudgeIdle += secs;
    if (d.snapPending && d.nudgeIdle > 0.12 && !d.sync && d.playing && d.track && d.fpb() > 0) {
        int r = (m >= 0 && m != idx && e->decks[m].track && e->decks[m].playing) ? m : -1;
        for (int k = 0; r < 0 && k < DJC_DECKS; ++k)
            if (k != idx && e->decks[k].track && e->decks[k].playing && e->decks[k].fpb() > 0) r = k;
        if (r < 0) d.snapPending = false;
        else {
            Deck &rd = e->decks[r];
            double myBpm = d.track->bpm * base, rBpm = rd.track->bpm * rd.baseRate;
            if (std::fabs(myBpm - rBpm) > 0.5) d.snapPending = false; // tempos don't match: nothing to snap to
            else {
                double err = rd.phase(e->blockStart[r]) - d.phase(d.pos);
                err -= std::round(err);
                if (std::fabs(err) < 0.0015) d.snapPending = false; // on the grid (< 1 ms at 128 BPM)
                else base *= 1.0 + std::max(-0.08, std::min(0.08, err * d.fpb() / d.sr * 7.0)); // magnetic pull, max 8 %
            }
        }
    } else if (d.sync) d.snapPending = false;
    return base * (1.0 + std::max(-0.08, std::min(0.08, nudge))); // CDJ-like bend, never more than +-8 %
}

struct InPtrs {
    float *ch[2];
    float *operator[](int c) const { return ch[c]; }
};
struct OutPtrs {
    float *ch[2];
    float *operator[](int c) const { return ch[c]; }
};

// copy `count` source frames starting at d.readPos (loop wrapped) into inBuf at offset
static void feed(Deck &d, int offset, int count) {
    for (int k = 0; k < count; ++k) {
        int64_t i = int64_t(std::floor(d.readPos));
        d.inBuf[0][size_t(offset + k)] = d.sample(0, i);
        d.inBuf[1][size_t(offset + k)] = d.sample(1, i);
        d.readPos = d.wrapFwd(d.readPos + 1.0);
    }
}

static void primeStretch(Deck &d, double rate) {
    int len = d.stretch.outputSeekLength(float(rate));
    len = std::min(len, d.inCap);
    d.readPos = d.pos;
    feed(d, 0, len);
    InPtrs in{{d.inBuf[0].data(), d.inBuf[1].data()}};
    d.stretch.outputSeek(in, len);
    d.feedFrac = 0;
    d.needPrime = false;
}

static void renderDeck(djc_engine *e, int idx, int n) {
    Deck &d = e->decks[idx];
    std::memset(d.out[0], 0, sizeof(float) * size_t(n));
    std::memset(d.out[1], 0, sizeof(float) * size_t(n));
    double rate = computeRate(e, idx, n);
    d.rate = rate;
    int turn = d.pendingTurn, search = d.pendingSearch;
    d.pendingTurn = 0; d.pendingSearch = 0;
    if (!d.track) return;

    double framesPerTick = d.sr * 60.0 / (kRpm * d.ticksPerRev);
    if (search) { d.pendJump = -1; d.jump(d.pos + search * framesPerTick * 40.0); }
    // quantized hot cue waiting for its beat: fire in the block that contains the beat, landing early by the
    // part of the block before the beat, so the cue's first beat sounds exactly on the grid
    if (d.pendJump >= 0) {
        if (!d.playing || d.touched) d.pendJump = -1;
        else {
            double rem = d.frameAt(d.pendBeat) - d.pos;          // source frames until the beat
            if (rem <= rate * double(n)) {
                double tgt = d.pendJump - std::max(0.0, rem);
                if (d.pendLoopEnd > d.pendJump) { d.loopIn = d.pendJump; d.loopOut = d.pendLoopEnd; d.loopActive = true; }
                else d.loopActive = false;
                d.pendJump = -1;
                d.jump(tgt);
            }
        }
    }

    bool scratching = d.touched && d.vinyl;
    if (!d.playing && !scratching) {
        if (turn) d.jump(d.pos + turn * framesPerTick); // paused jog = frame search
        d.stretchActive = false;
        return;
    }

    double v; // source frames per output frame
    if (scratching) {
        d.scratchTarget += turn * framesPerTick;
        double want = (d.scratchTarget - d.pos) / double(n);
        d.scratchVel = 0.6 * d.scratchVel + 0.4 * want;
        v = d.scratchVel;
    } else {
        v = rate;
    }

    // rekordbox active loops engage when the playhead reaches them
    if (!d.loopActive && !scratching && v > 0) {
        double next = d.pos + n * v;
        for (int k = 0; k < d.memCount; ++k)
            if (d.memActive[k] && d.mem[k] >= d.pos && d.mem[k] < next) {
                d.loopIn = d.mem[k]; d.loopOut = d.memEnd[k]; d.loopActive = true;
                break;
            }
    }

    bool useStretch = (d.keylock || d.keyShift != 0) && !scratching && v > 0.05;
    if (useStretch) {
        double semis = std::pow(2.0, d.keyShift / 12.0);
        d.stretch.setTransposeFactor(float(d.keylock ? semis : semis * v)); // no keylock: pitch follows tempo
    }
    if (useStretch) {
        if (!d.stretchActive || d.needPrime) primeStretch(d, v);
        d.stretchActive = true;
        double want = n * v + d.feedFrac;
        int count = int(std::floor(want));
        d.feedFrac = want - count;
        count = std::min(count, d.inCap);
        feed(d, 0, count);
        InPtrs in{{d.inBuf[0].data(), d.inBuf[1].data()}};
        OutPtrs out{{d.out[0], d.out[1]}};
        d.stretch.process(in, count, out, n);
        d.pos = d.wrapFwd(d.pos + n * v);
    } else {
        d.stretchActive = false;
        double p = d.pos;
        for (int k = 0; k < n; ++k) {
            int64_t i = int64_t(std::floor(p));
            float t = float(p - double(i));
            for (int c = 0; c < 2; ++c)
                d.out[c][k] = hermite(d.sample(c, i - 1), d.sample(c, i), d.sample(c, i + 1), d.sample(c, i + 2), t);
            double prev = p;
            p += v;
            if (d.loopActive && d.loopOut > d.loopIn) {
                // loop wraps both ways: playing past OUT jumps to IN, a backspin past IN comes back in at OUT.
                // While scratching, the hand target moves with the wrap so the record doesn't fly off.
                double len = d.loopOut - d.loopIn, q = p;
                if (v > 0) q = d.wrapFwd(p);
                else if (v < 0 && p < d.loopIn && prev >= d.loopIn) q = p + len;
                if (q != p) { if (scratching) d.scratchTarget += q - p; p = q; }
            }
            if (p < 0) p = 0;
        }
        d.pos = p;
    }
    if (d.pos >= double(d.track->frames) && !scratching) { // end of track: stop
        d.pos = double(d.track->frames);
        d.playing = false;
        d.cuePreview = false;
    }
}

// ---------- mixer (audio thread) ----------

// Beat FX tempo: sync master deck, else the FX target deck, else 120
static double fxTempo(djc_engine *e) {
    // fader tempo only: nudges and sync phase corrections must not wobble delay times (pitch artefacts)
    auto deckBpm = [](const Deck &d) { return d.track->bpm * d.faderBase(); };
    int m = e->masterDeck;
    if (m >= 0 && e->decks[m].track && e->decks[m].track->bpm > 0) return deckBpm(e->decks[m]);
    int t = e->fx.target;
    if (t >= 0 && t < DJC_DECKS && e->decks[t].track && e->decks[t].track->bpm > 0) return deckBpm(e->decks[t]);
    return 120.0;
}

static void mixBlock(djc_engine *e, int n) {
    std::memset(e->bus, 0, sizeof(e->bus));
    std::memset(e->cueBus, 0, sizeof(e->cueBus));
    float xa, xb;
    xfGains(e->crossfader, xa, xb);
    double fxBpm = fxTempo(e);
    for (int c = 0; c < DJC_DECKS; ++c) {
        auto &m = e->ch[c];
        Deck &d = e->decks[c];
        float xg = m.xf == 0 ? xa : (m.xf == 2 ? xb : 1.0f);
        float gTarget = trimGain(m.trim);
        float fTarget = faderGain(m.fader) * xg;
        float eqT[3] = {eqGain(m.low), eqGain(m.mid), eqGain(m.hi)};
        // colour filter coefficients once per block
        float col = m.color, dead = 0.03f;
        int mode = (!e->colorOn || e->cfxType != CFX_FILTER) ? 0 : (col < 0.5f - dead ? 1 : (col > 0.5f + dead ? 2 : 0));
        if (mode == 1) {
            double x = (0.5 - dead - col) / (0.5 - dead); // 0..1
            m.coeffs.set(20000.0 * std::pow(200.0 / 20000.0, x), e->sr, 0.6 + 2.0 * e->cfxParam * e->cfxParam);
        } else if (mode == 2) {
            double x = (col - 0.5 - dead) / (0.5 - dead);
            m.coeffs.set(20.0 * std::pow(6000.0 / 20.0, x), e->sr, 0.6 + 2.0 * e->cfxParam * e->cfxParam);
        }
        float pk[2] = {0, 0};
        float inv = 1.0f / float(n);
        // pass 1: trim, EQ, colour -> chBuf
        for (int k = 0; k < n; ++k) {
            float t = float(k) * inv;
            float g = m.gPrev + (gTarget - m.gPrev) * t;
            float el = m.eqPrev[0] + (eqT[0] - m.eqPrev[0]) * t;
            float em = m.eqPrev[1] + (eqT[1] - m.eqPrev[1]) * t;
            float eh = m.eqPrev[2] + (eqT[2] - m.eqPrev[2]) * t;
            for (int s = 0; s < 2; ++s) {
                float x = d.out[s][k] * g;
                x = m.iso[s].tick(x, el, em, eh);
                if (mode) {
                    float lo, bp;
                    m.svf[s].tick(x, m.coeffs.a1, m.coeffs.a2, m.coeffs.a3, lo, bp);
                    x = mode == 1 ? lo : x - m.coeffs.k * bp - lo;
                } else {
                    m.svf[s].ic1 = m.svf[s].ic2 = 0;
                }
                e->chBuf[s][k] = x;
            }
        }
        // DJM-A9 behaviour: the effects sit after the channel fader / crossfader, so pulling the fader down stops
        // feeding them but the echo / reverb tails keep ringing. Keep the pre-fader sound for meters and CH CUE.
        for (int k = 0; k < n; ++k) {
            float fg = m.cPrev + (fTarget - m.cPrev) * (float(k) * inv);
            for (int s = 0; s < 2; ++s) {
                float x = e->chBuf[s][k];
                e->sideBuf[s][k] = x;
                pk[s] = std::max(pk[s], std::fabs(x));
                e->chBuf[s][k] = x * fg;
                e->drumBus[s][k] = x * fg; // scratch: faded dry, to find the FX's wet part for the cue
            }
        }
        // Sound Colour FX (DUB ECHO / SWEEP / SPACE); FILTER is done above
        if (e->cfxType != CFX_FILTER) {
            float amt = 0;
            if (e->colorOn) {
                if (col < 0.5f - dead) amt = -(0.5f - dead - col) / (0.5f - dead);
                else if (col > 0.5f + dead) amt = (col - 0.5f - dead) / (0.5f - dead);
            }
            m.cfx.process(e->chBuf[0], e->chBuf[1], n, e->cfxType, amt, fxBpm, e->cfxParam);
        }
        // Beat FX insert on this channel (post-fader: tails continue after the fader is down)
        if (e->fx.target == c) e->fx.process(e->chBuf[0], e->chBuf[1], n, fxBpm);
        // MASTER CUE on and this channel already heard on the master: its CH CUE would double it in the phones
        // (louder, saturated). Owner's rule: then the phones only take it from the master.
        const bool chCue = m.cue && !(e->masterCue && (d.track && d.playing) && fTarget > 0.05f);
        // pass 2: into the master bus; CH CUE hears the pre-fader sound plus the effects' wet part
        for (int k = 0; k < n; ++k)
            for (int s = 0; s < 2; ++s) {
                float y = e->chBuf[s][k];
                e->bus[s][k] += y;
                if (chCue) e->cueBus[s][k] += e->sideBuf[s][k] + (y - e->drumBus[s][k]);
            }
        e->audible[c] = (d.track && d.playing) ? fTarget : 0.0f;
        m.gPrev = gTarget; m.cPrev = fTarget;
        for (int b = 0; b < 3; ++b) m.eqPrev[b] = eqT[b];
        auto &p = e->pub[c];
        // peak hold with ~300 ms decay, so several readers (UI, controller meters) can sample it
        float decay = float(std::exp(-double(n) / (e->sr * 0.3)));
        p.peakL.store(std::max(pk[0], p.peakL.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
        p.peakR.store(std::max(pk[1], p.peakR.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
    }
    // GROOVE CIRCUIT drums into the master bus, locked to the grid of the deck you hear (loudest playing channel
    // after fader + crossfader, like the RMX-1000 which follows its audio input); free 120 BPM clock if nothing plays
    {
        auto ok = [&](int k) { return k >= 0 && e->decks[k].track && e->decks[k].playing && e->decks[k].fpb() > 0; };
        int best = -1;
        for (int k = 0; k < DJC_DECKS; ++k) if (ok(k) && e->audible[k] > 0.01f && (best < 0 || e->audible[k] > e->audible[best])) best = k;
        int m = e->drumSrc;
        if (!ok(m) || (best >= 0 && best != m && e->audible[best] > 1.5f * e->audible[m] + 0.02f)) m = best; // hysteresis
        if (m < 0) m = ok(e->masterDeck) ? e->masterDeck : -1;
        if (m < 0) for (int k = 0; k < DJC_DECKS; ++k) if (ok(k)) { m = k; break; }
        if (m != e->drumSrc) { for (auto &r : e->drums) r.resync(); e->drumSrc = m; }
        double b0, bpf;
        if (m >= 0) {
            Deck &md = e->decks[m];
            b0 = md.beats(e->blockStart[m]);
            bpf = md.rate / md.fpb();
            e->freeBeats = b0 + bpf * n;
        } else {
            bpf = 2.0 / e->sr;
            b0 = e->freeBeats;
            e->freeBeats += bpf * n;
        }
        // drums go through their own bus so Beat FX on SP can treat them alone (MST treats drums + music)
        std::memset(e->drumBus, 0, sizeof(e->drumBus));
        for (int sd = 0; sd < 2; ++sd) {
            std::memset(e->sideBuf, 0, sizeof(e->sideBuf));
            e->drums[sd].process(e->sideBuf[0], e->sideBuf[1], n, b0, bpf);
            bool cue = e->drums[sd].cue;
            for (int k = 0; k < n; ++k)
                for (int s = 0; s < 2; ++s) {
                    e->drumBus[s][k] += e->sideBuf[s][k];
                    if (cue) e->cueBus[s][k] += e->sideBuf[s][k];   // CAPTURE lit: drums in the headphones too
                }
        }
        if (e->fx.target == 5) e->fx.process(e->drumBus[0], e->drumBus[1], n, fxBpm);
        for (int k = 0; k < n; ++k) { e->bus[0][k] += e->drumBus[0][k]; e->bus[1][k] += e->drumBus[1][k]; }
        for (int sd = 0; sd < 2; ++sd) {
            auto &r = e->drums[sd];
            int st = r.cue ? 1 : 0;
            for (int k = 0; k < DR_COUNT; ++k) if (r.selected[k]) st |= 2 << k;
            int rate = r.rollDown >= 0 ? r.rollDown : (r.hold ? r.rate : -1);
            if (rate >= 0) st |= 256 << rate;
            if (r.hold) st |= 1 << 12;
            st |= (int(std::lround(r.pitch)) + 12) << 16;      // bits 16-20: pitch + 12
            st |= int(std::lround(r.gain * 100)) << 21;         // bits 21-27: gain %
            e->drumState[sd].store(st, std::memory_order_relaxed);
        }
    }
    if (e->fx.target == 4) e->fx.process(e->bus[0], e->bus[1], n, fxBpm);
    // master: -3 dB summing headroom (two full-scale tracks no longer slam the limiter), then the master level
    float mT = e->masterLevel * e->masterLevel * 1.41f * 0.7079f;
    float mp[2] = {0, 0};
    const float ceil = 0.966f;                               // -0.3 dBFS
    const float rel = float(1.0 - std::exp(-1.0 / (0.120 * e->sr)));
    const float att = float(1.0 - std::exp(-1.0 / (djc_engine::kLook * 0.25)));
    float minG = 1.0f;
    for (int k = 0; k < n; ++k) {
        float g = e->mPrev + (mT - e->mPrev) * float(k) / float(n);
        float xl = e->bus[0][k] * g, xr = e->bus[1][k] * g;
        if (e->meqOn)
            for (int b = 0; b < 3; ++b) { xl = e->meq[0][b].tick(xl); xr = e->meq[1][b].tick(xr); }
        float pk = std::max(std::fabs(xl), std::fabs(xr));
        e->limReq[e->limPos] = pk > ceil ? ceil / pk : 1.0f;  // gain this sample will need when it comes out
        float want = 1.0f;
        for (int j = 0; j < djc_engine::kLook; ++j) want = std::min(want, e->limReq[j]);
        e->limGain += (want - e->limGain) * (want < e->limGain ? att : rel);
        float yl = e->limDl[0][e->limPos], yr = e->limDl[1][e->limPos];   // delayed by the look-ahead
        e->limDl[0][e->limPos] = xl; e->limDl[1][e->limPos] = xr;
        if (++e->limPos >= djc_engine::kLook) e->limPos = 0;
        yl = softLimit(yl * e->limGain); yr = softLimit(yr * e->limGain); // safety only, normally untouched
        e->bus[0][k] = yl; e->bus[1][k] = yr;
        minG = std::min(minG, e->limGain);
        mp[0] = std::max(mp[0], std::fabs(yl)); mp[1] = std::max(mp[1], std::fabs(yr));
    }
    e->limGR = minG;
    e->mPrev = mT;
    if (e->recOn.load(std::memory_order_relaxed)) { // set recorder: master output into the ring
        size_t w = e->recW.load(std::memory_order_relaxed), r = e->recR.load(std::memory_order_acquire);
        size_t room = e->recCap - 1 - ((w + e->recCap - r) % e->recCap);
        size_t take = std::min(room, size_t(n));
        for (size_t k = 0; k < take; ++k) {
            e->rec[w * 2] = e->bus[0][k]; e->rec[w * 2 + 1] = e->bus[1][k];
            if (++w == e->recCap) w = 0;
        }
        e->recW.store(w, std::memory_order_release);
        if (take < size_t(n)) e->recDropped.fetch_add(int64_t(size_t(n) - take), std::memory_order_relaxed);
    }
    float mDecay = float(std::exp(-double(n) / (e->sr * 0.3)));
    e->mPeakL.store(std::max(mp[0], e->mPeakL.load(std::memory_order_relaxed) * mDecay), std::memory_order_relaxed);
    e->mPeakR.store(std::max(mp[1], e->mPeakR.load(std::memory_order_relaxed) * mDecay), std::memory_order_relaxed);
    // phones = cue / master blend
    float pl = e->phonesLevel * e->phonesLevel;
    float wc = float(std::cos(e->phonesMix * kPi / 2)), wm = float(std::sin(e->phonesMix * kPi / 2));
    for (int k = 0; k < n; ++k)
        for (int s = 0; s < 2; ++s) {
            // master only reaches the phones when MASTER CUE is on (owner): the MIX knob then blends
            // CH CUE <-> master. Nothing cued = silence, whatever the MIX knob says.
            float m = e->masterCue ? e->bus[s][k] : 0.0f;
            e->cueBus[s][k] = softLimit(((e->cueBus[s][k] + m) * wc + m * wm) * pl);
        }
}

static void publishMixer(djc_engine *e) {
    auto &q = e->pubMix;
    for (int c = 0; c < DJC_DECKS; ++c) {
        auto &m = e->ch[c];
        q.trim[c].store(m.trim, std::memory_order_relaxed); q.hi[c].store(m.hi, std::memory_order_relaxed);
        q.mid[c].store(m.mid, std::memory_order_relaxed); q.low[c].store(m.low, std::memory_order_relaxed);
        q.color[c].store(m.color, std::memory_order_relaxed); q.fader[c].store(m.fader, std::memory_order_relaxed);
        q.xf[c].store(m.xf, std::memory_order_relaxed); q.cue[c].store(m.cue, std::memory_order_relaxed);
    }
    q.crossfader.store(e->crossfader, std::memory_order_relaxed);
    q.masterLevel.store(e->masterLevel, std::memory_order_relaxed);
    q.phonesMix.store(e->phonesMix, std::memory_order_relaxed);
    q.phonesLevel.store(e->phonesLevel, std::memory_order_relaxed);
    q.masterCue.store(e->masterCue, std::memory_order_relaxed);
    q.colorFxOn.store(e->colorOn, std::memory_order_relaxed);
    q.colorFxType.store(e->cfxType, std::memory_order_relaxed);
    q.colorFxParam.store(e->cfxParam, std::memory_order_relaxed);
}

static double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void publish(djc_engine *e) {
    double now = nowSeconds();
    publishMixer(e);
    e->fxType.store(e->fx.type, std::memory_order_relaxed);
    e->fxTarget.store(e->fx.target, std::memory_order_relaxed);
    e->fxOn.store(e->fx.on, std::memory_order_relaxed);
    e->fxBeat.store(e->fx.beatIdx, std::memory_order_relaxed);
    e->fxLevel.store(e->fx.level, std::memory_order_relaxed);
    e->fxBands.store(e->fx.bandMask, std::memory_order_relaxed);
    e->fxBpmPub.store(fxTempo(e), std::memory_order_relaxed);
    for (int i = 0; i < DJC_DECKS; ++i) {
        Deck &d = e->decks[i];
        auto &p = e->pub[i];
        double sr = d.sr;
        p.loaded.store(d.track != nullptr, std::memory_order_relaxed);
        p.playing.store(d.playing, std::memory_order_relaxed);
        p.keylock.store(d.keylock, std::memory_order_relaxed);
        p.sync.store(d.sync, std::memory_order_relaxed);
        p.master.store(e->masterDeck == i, std::memory_order_relaxed);
        p.quantize.store(d.quantize, std::memory_order_relaxed);
        p.loopActive.store(d.loopActive, std::memory_order_relaxed);
        p.vinyl.store(d.vinyl, std::memory_order_relaxed);
        p.tempo.store(d.tempo, std::memory_order_relaxed);
        p.keyShift.store(d.keyShift, std::memory_order_relaxed);
        p.keySync.store(d.keySync, std::memory_order_relaxed);
        p.onAir.store(e->audible[i], std::memory_order_relaxed);
        p.position.store(d.pos / sr, std::memory_order_relaxed);
        p.duration.store(d.track ? double(d.track->frames) / sr : 0, std::memory_order_relaxed);
        double tb = d.track ? d.track->bpm : 0;
        p.trackBpm.store(tb, std::memory_order_relaxed);
        p.bpm.store(tb * d.baseRate, std::memory_order_relaxed);
        p.rate.store(d.rate, std::memory_order_relaxed);
        p.tempoRange.store(d.range, std::memory_order_relaxed);
        p.cuePoint.store(d.cue / sr, std::memory_order_relaxed);
        p.loopIn.store(d.loopIn >= 0 ? d.loopIn / sr : -1, std::memory_order_relaxed);
        p.loopOut.store(d.loopOut >= 0 ? d.loopOut / sr : -1, std::memory_order_relaxed);
        for (int h = 0; h < DJC_HOTCUES; ++h) {
            p.hotcues[h].store(d.hot[h] >= 0 ? d.hot[h] / sr : -1, std::memory_order_relaxed);
            p.hotLoopEnd[h].store(d.hotLoopEnd[h] / sr, std::memory_order_relaxed);
        }
        p.stamp.store(now, std::memory_order_relaxed);
    }
}

// ---------- C API ----------

extern "C" {

djc_engine *djc_create(double sampleRate, int32_t maxFramesPerRender) {
    auto *e = new djc_engine();
    e->sr = sampleRate;
    e->maxFrames = maxFramesPerRender;
    for (int i = 0; i < DJC_DECKS; ++i) {
        Deck &d = e->decks[i];
        d.sr = sampleRate;
        d.stretch.presetDefault(2, float(sampleRate), true); // split FFT work across blocks: flatter CPU
        d.inCap = int(d.stretch.inputLatency() + 2.5 * d.stretch.outputLatency()) + kMaxBlock * 3 + 64;
        d.inBuf[0].assign(size_t(d.inCap), 0.0f);
        d.inBuf[1].assign(size_t(d.inCap), 0.0f);
        // warm up every internal buffer once so render never grows a vector
        d.stretch.outputSeek(InPtrs{{d.inBuf[0].data(), d.inBuf[1].data()}}, d.stretch.outputSeekLength(1.0f));
        float tmp[2][kMaxBlock];
        d.stretch.process(InPtrs{{d.inBuf[0].data(), d.inBuf[1].data()}}, kMaxBlock,
                          OutPtrs{{tmp[0], tmp[1]}}, kMaxBlock);
        d.stretch.reset();
        for (int h = 0; h < DJC_HOTCUES; ++h) { e->pub[i].hotcues[h].store(-1); e->pub[i].hotLoopEnd[h].store(0); }
        e->pubMix.trim[i] = 0.5f; e->pubMix.hi[i] = 0.5f; e->pubMix.mid[i] = 0.5f; e->pubMix.low[i] = 0.5f;
        e->pubMix.color[i] = 0.5f; e->pubMix.fader[i] = 1.0f; e->pubMix.xf[i] = 1; e->pubMix.cue[i] = 0;
        e->ch[i].iso[0].setup(sampleRate);
        e->ch[i].iso[1].setup(sampleRate);
        e->ch[i].cfx.prepare(sampleRate);
    }
    e->drums[0].prepare(sampleRate, false);
    e->drums[1].prepare(sampleRate, true);
    e->drumState[0] = 0; e->drumState[1] = 0;
    e->fx.prepare(sampleRate);
    e->recCap = size_t(sampleRate * 10) + 1;
    e->rec.assign(e->recCap * 2, 0.0f);
    return e;
}

void djc_rec_start(djc_engine *e) {
    e->recOn.store(0);
    e->recR.store(e->recW.load(std::memory_order_acquire), std::memory_order_release); // empty the ring
    e->recDropped.store(0);
    e->recOn.store(1, std::memory_order_release);
}
void djc_rec_stop(djc_engine *e) { e->recOn.store(0, std::memory_order_release); }
int32_t djc_rec_read(djc_engine *e, float *dst, int32_t maxFrames) {
    size_t r = e->recR.load(std::memory_order_relaxed), w = e->recW.load(std::memory_order_acquire);
    int32_t got = 0;
    while (r != w && got < maxFrames) {
        dst[got * 2] = e->rec[r * 2]; dst[got * 2 + 1] = e->rec[r * 2 + 1];
        if (++r == e->recCap) r = 0;
        ++got;
    }
    e->recR.store(r, std::memory_order_release);
    return got;
}
int64_t djc_rec_dropped(const djc_engine *e) { return e->recDropped.load(std::memory_order_relaxed); }

void djc_destroy(djc_engine *e) {
    if (!e) return;
    djc_collect(e);
    djc_cmd c;
    for (auto &q : e->queues)
        while (q.pop(c))
            if (c.type == DJC_LOAD) delete static_cast<djc_track *>(c.ptr);
    for (auto &d : e->decks) delete d.track;
    delete e;
}

double djc_sample_rate(const djc_engine *e) { return e->sr; }
double djc_now(void) { return nowSeconds(); }

void djc_track_set_beats(djc_track *t, const double *seconds, int32_t count) {
    if (!t) return;
    t->beatFrames.clear();
    for (int32_t i = 0; i < count; ++i) {
        double f = seconds[i] * t->sr;
        if (t->beatFrames.empty() || f > t->beatFrames.back() + 1.0) t->beatFrames.push_back(f); // strictly rising
    }
    t->useBeats = t->beatFrames.size() >= 2;
}

djc_track *djc_track_create(const float *left, const float *right, int64_t frames, double sampleRate, double bpm,
                            double firstBeatSeconds) {
    auto *t = new djc_track();
    t->frames = frames;
    t->sr = sampleRate;
    t->L.assign(left, left + frames);
    t->R.assign(right ? right : left, (right ? right : left) + frames);
    t->bpm = bpm;
    t->firstBeat = firstBeatSeconds * sampleRate;
    return t;
}

void djc_track_destroy(djc_track *t) { delete t; }

int32_t djc_send(djc_engine *e, int32_t queue, const djc_cmd *cmd) {
    if (queue < 0 || queue > 1) return 0;
    return e->queues[queue].push(*cmd) ? 1 : 0;
}

int32_t djc_post(djc_engine *e, int32_t queue, int32_t type, int32_t deck, int32_t i, double v) {
    djc_cmd c{type, deck, i, v, 0, nullptr};
    return djc_send(e, queue, &c);
}

void djc_collect(djc_engine *e) {
    djc_track *t;
    while (e->garbage.pop(t)) delete t;
}

void djc_get_deck_state(const djc_engine *e, int32_t deck, djc_deck_state *o) {
    if (deck < 0 || deck >= DJC_DECKS) return;
    auto &p = const_cast<djc_engine *>(e)->pub[deck];
    o->loaded = p.loaded; o->playing = p.playing; o->keylock = p.keylock; o->sync = p.sync;
    o->master = p.master; o->quantize = p.quantize; o->loopActive = p.loopActive; o->vinyl = p.vinyl;
    o->position = p.position; o->duration = p.duration; o->trackBpm = p.trackBpm; o->bpm = p.bpm;
    o->rate = p.rate; o->tempoRange = p.tempoRange; o->cuePoint = p.cuePoint;
    o->loopIn = p.loopIn; o->loopOut = p.loopOut;
    for (int h = 0; h < DJC_HOTCUES; ++h) o->hotcues[h] = p.hotcues[h];
    o->peakL = p.peakL; o->peakR = p.peakR; o->tempo = p.tempo; o->keyShift = p.keyShift; o->keySync = p.keySync; o->onAir = p.onAir.load(std::memory_order_relaxed);
    o->stamp = p.stamp;
    for (int h = 0; h < DJC_HOTCUES; ++h) o->hotLoopEnd[h] = p.hotLoopEnd[h];
}

void djc_get_master_state(const djc_engine *e, djc_master_state *o) {
    auto *m = const_cast<djc_engine *>(e);
    o->masterPeakL = m->mPeakL;
    o->masterPeakR = m->mPeakR;
    o->xruns = m->xruns;
    o->cpuLoad = m->cpu;
}

void djc_get_mixer_state(const djc_engine *e, djc_mixer_state *o) {
    auto &q = const_cast<djc_engine *>(e)->pubMix;
    for (int c = 0; c < DJC_DECKS; ++c) {
        o->trim[c] = q.trim[c]; o->hi[c] = q.hi[c]; o->mid[c] = q.mid[c]; o->low[c] = q.low[c];
        o->color[c] = q.color[c]; o->fader[c] = q.fader[c]; o->xf[c] = q.xf[c]; o->cue[c] = q.cue[c];
    }
    o->crossfader = q.crossfader; o->masterLevel = q.masterLevel; o->phonesMix = q.phonesMix;
    o->phonesLevel = q.phonesLevel; o->masterCue = q.masterCue; o->colorFxOn = q.colorFxOn;
    o->colorFxType = q.colorFxType; o->colorFxParam = q.colorFxParam;
}

void djc_get_fx_state(const djc_engine *e, djc_fx_state *o) {
    auto *m = const_cast<djc_engine *>(e);
    o->type = m->fxType; o->target = m->fxTarget; o->on = m->fxOn; o->beatIndex = m->fxBeat;
    o->level = m->fxLevel; o->bpm = m->fxBpmPub;
    o->beats = kFxBeats[o->beatIndex];
    o->timeMs = 60000.0 / std::max(1.0, o->bpm) * o->beats;
    o->beatCode = kFxBeatCodes[o->beatIndex];
    o->bands = m->fxBands;
}

void djc_drum_set_sample(djc_engine *e, int32_t side, int32_t slot, const float *L, const float *R, int32_t frames) {
    if (side < 0 || side > 1 || slot < 0 || slot >= DR_COUNT || !L || frames < 2) return;
    auto &kit = e->drums[side].kit;
    kit.s[slot].assign(L, L + frames);
    if (R) kit.r[slot].assign(R, R + frames); else kit.r[slot].clear();
    DrumRack::trimFront(kit.s[slot], kit.r[slot]);
}

int32_t djc_drum_state(const djc_engine *e, int32_t side) {
    return (side == 0 || side == 1) ? const_cast<djc_engine *>(e)->drumState[side].load(std::memory_order_relaxed) : 0;
}

const char *djc_fx_name(int32_t type) { return (type >= 0 && type < FX_COUNT) ? kFxNames[type] : ""; }

void djc_render(djc_engine *e, float *const *outputs, int32_t channels, int32_t frames) {
    auto t0 = std::chrono::steady_clock::now();
    djc_cmd c;
    for (auto &q : e->queues)
        while (q.pop(c)) handle(e, c);

    int done = 0;
    while (done < frames) {
        int n = std::min(kMaxBlock, frames - done);
        for (int i = 0; i < DJC_DECKS; ++i) e->blockStart[i] = e->decks[i].pos;
        for (int i = 0; i < DJC_DECKS; ++i) renderDeck(e, i, n);
        mixBlock(e, n);
        for (int ch = 0; ch < channels; ++ch) {
            float *o = outputs[ch] + done;
            const float *src = nullptr;
            if (e->splitCue) {
                // mono master left, mono cue right
                if (ch == e->routeML || ch == e->routeMR) {
                    bool left = ch == e->routeML;
                    for (int k = 0; k < n; ++k)
                        o[k] = left ? 0.5f * (e->bus[0][k] + e->bus[1][k]) : 0.5f * (e->cueBus[0][k] + e->cueBus[1][k]);
                    continue;
                }
            } else {
                if (ch == e->routeML) src = e->bus[0];
                else if (ch == e->routeMR) src = e->bus[1];
                else if (ch == e->routePL) src = e->cueBus[0];
                else if (ch == e->routePR) src = e->cueBus[1];
            }
            if (src) std::memcpy(o, src, sizeof(float) * size_t(n));
            else std::memset(o, 0, sizeof(float) * size_t(n));
        }
        done += n;
    }
    publish(e);
    double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double budget = double(frames) / e->sr;
    double load = used / budget;
    e->cpu.store(load, std::memory_order_relaxed);
    if (load > 0.9) e->xruns.fetch_add(1, std::memory_order_relaxed);
}

} // extern "C"
