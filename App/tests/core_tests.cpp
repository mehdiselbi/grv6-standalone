// Host-side tests for DJCore. Build: see App/tests/run.sh
#include "djcore.h"
#include "cfx.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include <string>
#ifndef FIXTURES
#define FIXTURES "fixtures"
#endif

// ---- allocation tracking: any allocation while gAudio is true is a failure ----
static std::atomic<bool> gAudio{false};
static std::atomic<long> gAudioAllocs{0};
void *operator new(size_t n) {
    if (gAudio.load()) gAudioAllocs++;
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void *operator new[](size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { if (gAudio.load()) gAudioAllocs++; std::free(p); }
void operator delete[](void *p) noexcept { operator delete(p); }
void operator delete(void *p, size_t) noexcept { operator delete(p); }
void operator delete[](void *p, size_t) noexcept { operator delete(p); }

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } else { std::printf("ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const double SR = 48000;
static const int CH = 4, BLOCK = 256;

struct Out {
    std::vector<float> b[CH];
    float *p[CH];
    Out(int n) { for (int c = 0; c < CH; ++c) { b[c].assign(n, 0); p[c] = b[c].data(); } }
};

static void render(djc_engine *e, double seconds, std::vector<float> *capL = nullptr, std::vector<float> *capP = nullptr) {
    Out o(BLOCK);
    int blocks = int(seconds * SR / BLOCK);
    for (int i = 0; i < blocks; ++i) {
        gAudio = true;
        djc_render(e, o.p, CH, BLOCK);
        gAudio = false;
        if (capL) capL->insert(capL->end(), o.b[0].begin(), o.b[0].end());
        if (capP) capP->insert(capP->end(), o.b[2].begin(), o.b[2].end());
    }
}

static djc_track *sineTrack(double hz, double secs, double bpm = 120) {
    int64_t n = int64_t(secs * SR);
    std::vector<float> x(n);
    for (int64_t i = 0; i < n; ++i) x[i] = 0.5f * float(std::sin(2 * M_PI * hz * i / SR));
    return djc_track_create(x.data(), x.data(), n, SR, bpm, 0);
}

static djc_track *clickTrack(double bpm, double first, double secs, std::vector<float> *keep = nullptr) {
    int64_t n = int64_t(secs * SR);
    std::vector<float> x(n, 0.0f);
    double period = 60.0 / bpm * SR;
    for (double t = first * SR; t < n; t += period)
        for (int k = 0; k < 2000 && int64_t(t) + k < n; ++k)
            x[int64_t(t) + k] = 0.8f * float(std::exp(-k / 300.0) * std::sin(2 * M_PI * 60 * k / SR));
    if (keep) *keep = x;
    return djc_track_create(x.data(), x.data(), n, SR, bpm, first);
}

static double freqOf(const std::vector<float> &x, size_t from) {
    int zc = 0;
    size_t first = 0, last = 0;
    for (size_t i = from + 1; i < x.size(); ++i)
        if (x[i - 1] < 0 && x[i] >= 0) { if (!zc) first = i; last = i; zc++; }
    if (zc < 3) return 0;
    return (zc - 1) * SR / double(last - first);
}
static double rms(const std::vector<float> &x, size_t from) {
    double s = 0; size_t n = 0;
    for (size_t i = from; i < x.size(); ++i) { s += double(x[i]) * x[i]; n++; }
    return std::sqrt(s / std::max<size_t>(n, 1));
}

static void post(djc_engine *e, int type, int deck, int i = 0, double v = 0) { djc_post(e, 0, type, deck, i, v); }

static void route4(djc_engine *e) { post(e, DJC_ROUTING, 0, 0 | (1 << 8) | (2 << 16) | (3 << 24)); }

int main() {
    // 1. varispeed changes pitch, keylock keeps it; no allocation on the audio thread
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, sineTrack(440, 30)};
        djc_send(e, 0, &c);
        post(e, DJC_TEMPO_RANGE, 0, 0, 0.10);
        post(e, DJC_TEMPO, 0, 0, 1.0); // +10%
        post(e, DJC_PLAY, 0);
        std::vector<float> L;
        render(e, 2.0, &L);
        double f = freqOf(L, size_t(0.5 * SR));
        CHECK(std::fabs(f - 484.0) < 2.0, "varispeed +10%%: %.1f Hz (want 484)", f);

        post(e, DJC_KEYLOCK, 0, 1);
        L.clear();
        render(e, 2.0, &L);
        f = freqOf(L, size_t(0.5 * SR));
        CHECK(std::fabs(f - 440.0) < 3.0, "keylock +10%%: %.1f Hz (want 440)", f);
        djc_deck_state s;
        djc_get_deck_state(e, 0, &s);
        CHECK(std::fabs(s.position - 4.0 * 1.10) < 0.05, "position advances at rate: %.3f s (want 4.40)", s.position);

        post(e, DJC_TEMPO, 0, 0, -1.0); // -10% with keylock
        L.clear();
        render(e, 2.0, &L);
        f = freqOf(L, size_t(0.5 * SR));
        CHECK(std::fabs(f - 440.0) < 3.0, "keylock -10%%: %.1f Hz (want 440)", f);
        // hot cue jump while keylocked forces a re-prime inside render
        post(e, DJC_QUANTIZE, 0, 0);
        post(e, DJC_HOTCUE, 0, 0, 1); // set
        render(e, 0.5);
        post(e, DJC_HOTCUE, 0, 0, 1); // jump
        post(e, DJC_LOOP_IN, 0); render(e, 0.3); post(e, DJC_LOOP_OUT, 0);
        render(e, 1.0);
        CHECK(gAudioAllocs.load() == 0, "no heap allocation inside djc_render (%ld)", gAudioAllocs.load());
        djc_destroy(e);
    }
    // 2. beat loop holds position, beat jump, CUE behaviour
    {
        djc_engine *e = djc_create(SR, 4096);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(120, 0.0, 60)};
        djc_send(e, 0, &c);
        post(e, DJC_SEEK, 0, 0, 10.2);
        post(e, DJC_PLAY, 0);
        post(e, DJC_BEAT_LOOP, 0, 0, 4); // 2 s at 120 bpm, quantized to 10.0
        render(e, 5.0);
        djc_deck_state s;
        djc_get_deck_state(e, 0, &s);
        CHECK(s.loopActive && std::fabs(s.loopIn - 10.0) < 1e-3 && std::fabs(s.loopOut - 12.0) < 1e-3,
              "4 beat loop quantized 10.0..12.0 (%.3f..%.3f)", s.loopIn, s.loopOut);
        CHECK(s.position >= 10.0 && s.position < 12.0, "position stays in loop: %.3f", s.position);
        post(e, DJC_LOOP_EXIT, 0);
        post(e, DJC_BEAT_JUMP, 0, 0, 8);
        render(e, 0.01);
        djc_get_deck_state(e, 0, &s);
        CHECK(!s.loopActive && s.position > 14.0, "exit + jump 8 beats: %.3f", s.position);
        post(e, DJC_CUE, 0, 1); // playing -> back to cue and pause
        render(e, 0.1);
        djc_get_deck_state(e, 0, &s);
        CHECK(!s.playing && std::fabs(s.position - s.cuePoint) < 1e-3, "CUE while playing returns to cue and pauses");
        post(e, DJC_SEEK, 0, 0, 20.01); post(e, DJC_CUE, 0, 1); render(e, 0.5);
        djc_get_deck_state(e, 0, &s);
        CHECK(s.playing && std::fabs(s.cuePoint - 20.0) < 1e-3, "CUE while paused sets quantized cue %.3f and previews", s.cuePoint);
        post(e, DJC_CUE, 0, 0); render(e, 0.01);
        djc_get_deck_state(e, 0, &s);
        CHECK(!s.playing && std::fabs(s.position - 20.0) < 1e-3, "CUE release returns to cue");
        djc_destroy(e);
    }
    // 3. sync: 126 bpm deck follows 120 bpm master and locks phase
    {
        djc_engine *e = djc_create(SR, 4096);
        djc_cmd a{DJC_LOAD, 0, 0, 0, 0, clickTrack(120, 0.0, 60)};
        djc_cmd b{DJC_LOAD, 1, 0, 0, 0, clickTrack(126, 0.0, 60)};
        djc_send(e, 0, &a); djc_send(e, 0, &b);
        post(e, DJC_PLAY, 0); post(e, DJC_SEEK, 1, 0, 3.1); post(e, DJC_PLAY, 1);
        render(e, 1.0);
        post(e, DJC_SYNC, 1, 1);
        render(e, 8.0);
        djc_deck_state s0, s1;
        djc_get_deck_state(e, 0, &s0); djc_get_deck_state(e, 1, &s1);
        CHECK(s0.master && !s1.master, "deck 1 became master");
        CHECK(std::fabs(s1.bpm - 120.0) < 0.3, "synced bpm %.2f (want 120)", s1.bpm);
        double ph0 = s0.position * 120 / 60, ph1 = s1.position * 126 / 60;
        double err = (ph0 - std::floor(ph0)) - (ph1 - std::floor(ph1));
        err -= std::round(err);
        CHECK(std::fabs(err) < 0.02, "beat phase error %.4f beats", err);
        djc_destroy(e);
    }
    // 4. mixer: EQ kill, unity flatness, fader, crossfader, cue bus
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, sineTrack(60, 30)};
        djc_send(e, 0, &c);
        post(e, DJC_PLAY, 0);
        std::vector<float> L, P;
        render(e, 1.0, &L);
        double ref = rms(L, size_t(0.5 * SR));
        CHECK(std::fabs(20 * std::log10(ref / (0.5 / std::sqrt(2.0)) )) < 3.5,
              "unity path level %.2f dB", 20 * std::log10(ref / (0.5 / std::sqrt(2.0))));
        post(e, DJC_EQ_LOW, 0, 0, 0.0);
        L.clear(); render(e, 1.0, &L);
        double killed = 20 * std::log10(rms(L, size_t(0.5 * SR)) / ref + 1e-12);
        CHECK(killed < -30, "low kill on 60 Hz: %.1f dB", killed);
        post(e, DJC_EQ_LOW, 0, 0, 0.5);
        post(e, DJC_XF_ASSIGN, 0, 2); post(e, DJC_CROSSFADER, 0, 0, 0.0); // ch1 on B, fader hard A
        L.clear(); render(e, 1.0, &L);
        CHECK(rms(L, size_t(0.5 * SR)) < ref * 1e-3, "crossfader cuts channel assigned to B");
        post(e, DJC_XF_ASSIGN, 0, 1);
        post(e, DJC_FADER, 0, 0, 0.0); post(e, DJC_CH_CUE, 0, 1); post(e, DJC_PHONES_MIX, 0, 0, 0.0);
        L.clear(); P.clear(); render(e, 1.0, &L, &P);
        CHECK(rms(L, size_t(0.5 * SR)) < ref * 1e-3 && rms(P, size_t(0.5 * SR)) > ref * 0.1,
              "fader down: master silent, cue still in phones (%.4f)", rms(P, size_t(0.5 * SR)));
        djc_destroy(e);
    }
    // 5. bpm estimate
    {
        std::vector<float> x;
        djc_track *t = clickTrack(124, 0.35, 40, &x);
        double bpm, fb;
        djc_estimate_bpm(x.data(), int64_t(x.size()), SR, &bpm, &fb);
        double beat = 60.0 / 124;
        double off = std::fmod(fb - 0.35 + 10 * beat, beat);
        off = std::min(off, beat - off);
        CHECK(std::fabs(bpm - 124) < 0.05, "bpm estimate %.2f (want 124)", bpm);
        CHECK(off < 0.02, "first beat %.3f s (grid offset error %.3f s)", fb, off);
        djc_track_destroy(t);
    }
    // 6. benchmark: 4 decks, keylock on, varied tempos
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        for (int d = 0; d < 4; ++d) {
            djc_cmd c{DJC_LOAD, d, 0, 0, 0, clickTrack(120 + d, 0.0, 120)};
            djc_send(e, 0, &c);
            post(e, DJC_KEYLOCK, d, 1);
            post(e, DJC_TEMPO, d, 0, 0.3 * (d - 1.5));
            post(e, DJC_PLAY, d);
        }
        render(e, 0.5);
        auto t0 = std::chrono::steady_clock::now();
        render(e, 20.0);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("info 4 decks keylock: %.2f%% of one core (x86 host; iPad A15 will differ)\n", 100 * secs / 20.0);
        djc_master_state m;
        djc_get_master_state(e, &m);
        CHECK(gAudioAllocs.load() == 0, "still no audio-thread allocation after benchmark");
        djc_destroy(e);
    }
    // 7. GRV6 MIDI mapping: official-list bytes in, engine state out
    {
        djc_engine *e = djc_create(SR, 4096);
        djc_midi *m = djc_midi_create(e);
        auto in = [&](uint8_t a, uint8_t b, uint8_t c) { uint8_t msg[3] = {a, b, c}; djc_midi_in(m, msg, 3); };
        int unsourced = 0;
        for (int i = 0; i < djc_midi_table_size(); ++i) if (!djc_midi_table_source(i) || !*djc_midi_table_source(i)) unsourced++;
        CHECK(unsourced == 0, "all %d mapping rows carry a source tag", djc_midi_table_size());

        djc_cmd c{DJC_LOAD, 2, 0, 0, 0, clickTrack(120, 0.0, 60)};
        djc_send(e, 0, &c);
        render(e, 0.01);
        in(0x92, 0x0B, 0x7F); in(0x92, 0x0B, 0x00);          // deck 3 PLAY (ch 3)
        in(0xB2, 0x00, 0x7F); in(0xB2, 0x20, 0x7F);          // deck 3 TEMPO fully '+' (MSB+LSB)
        in(0x92, 0x1A, 0x7F);                                // MASTER TEMPO (official 9n 1A) -> keylock
        in(0x9B, 0x00, 0x7F); in(0x9B, 0x00, 0x00);          // deck 3 pad 1, hot cue mode (ch 12)
        in(0xB2, 0x13, 0x00); in(0xB2, 0x33, 0x00);          // deck 3 channel fader to 0
        in(0xB6, 0x1F, 0x00);                                // crossfader hard left
        in(0xB6, 0x17 + 2, 0x00);                            // colour FX CH3 fully left
        in(0x92, 0x54, 0x7F);                                // CH CUE 3
        render(e, 1.0);
        djc_deck_state s; djc_mixer_state mx;
        djc_get_deck_state(e, 2, &s); djc_get_mixer_state(e, &mx);
        CHECK(s.playing, "0x92 0x0B -> deck 3 plays");
        CHECK(std::fabs(s.rate - 1.10) < 1e-3 && std::fabs(s.tempo - 1.0) < 1e-6, "14-bit tempo '+' end -> +10%% (rate %.4f)", s.rate);
        CHECK(s.keylock, "MASTER TEMPO toggles keylock");
        CHECK(s.hotcues[0] >= 0, "pad 1 sets hot cue A at %.3f s", s.hotcues[0]);
        CHECK(mx.fader[2] == 0.0f && mx.crossfader == 0.0f && mx.color[2] == 0.0f && mx.cue[2] == 1,
              "fader, crossfader, colour, CH CUE land on channel 3");
        in(0x9C, 0x00, 0x7F);                                // SHIFT pad 1 -> delete
        in(0xB2, 0x29, 0x41);                                // SHIFT jog search +1
        in(0x9B, 0x64, 0x7F);                                // beat loop mode pad 5 = 4 beats
        render(e, 0.05);
        djc_get_deck_state(e, 2, &s);
        CHECK(s.hotcues[0] < 0, "SHIFT+pad deletes hot cue");
        CHECK(s.loopActive && std::fabs((s.loopOut - s.loopIn) - 2.0) < 1e-3, "beat loop pad 5 = 4 beats (%.3f s)", s.loopOut - s.loopIn);
        // LED output reflects state
        uint8_t buf[2048];
        int n = djc_midi_out(m, buf, sizeof buf);
        bool playLed = false, padLed = false, loadLed = false, keyLed = false;
        for (int k = 0; k + 2 < n; k += 3) {
            if (buf[k] == 0x92 && buf[k+1] == 0x0B && buf[k+2] == 0x7F) playLed = true;
            if (buf[k] == 0x92 && buf[k+1] == 0x1A && buf[k+2] == 0x7F) keyLed = true;
            if (buf[k] == 0x9B && buf[k+1] == 0x64 && buf[k+2] == 0x7F) padLed = true;
            if (buf[k] == 0x9F && buf[k+1] == 0x02 && buf[k+2] == 0x7F) loadLed = true;
        }
        CHECK(playLed && keyLed && padLed && loadLed, "LEDs: PLAY, MASTER TEMPO, beat-loop pad 5, LOAD 3 (%d bytes)", n);
        CHECK(djc_midi_out(m, buf, sizeof buf) <= 3 * 8, "second LED pass only sends changes");
        // browse / load go to the UI, unknown messages are reported
        in(0xB6, 0x40, 0x7E); in(0x96, 0x47, 0x7F); in(0x96, 0x7B, 0x7F);
        djc_ui_event ev; int turn = 0, load = -1, unmapped = 0;
        while (djc_midi_poll_event(m, &ev)) {
            if (ev.kind == DJC_EV_BROWSE_TURN) turn += ev.value;
            if (ev.kind == DJC_EV_LOAD) load = ev.deck;
            if (ev.kind == DJC_EV_UNMAPPED && ev.raw[1] == 0x7B) unmapped++;
        }
        CHECK(turn == -2 && load == 1 && unmapped == 1, "browse -2, LOAD 2, unknown 0x96 0x7B reported");
        // CALL during a loop halves / doubles it (CDJ behaviour)
        in(0x92, 0x51, 0x7F); render(e, 0.02);
        djc_get_deck_state(e, 2, &s);
        CHECK(std::fabs((s.loopOut - s.loopIn) - 1.0) < 1e-3, "CALL < in loop halves 4 -> 2 beats (%.3f s)", s.loopOut - s.loopIn);
        in(0x92, 0x53, 0x7F); in(0x92, 0x53, 0x7F); render(e, 0.02);
        djc_get_deck_state(e, 2, &s);
        CHECK(std::fabs((s.loopOut - s.loopIn) - 4.0) < 1e-3, "CALL > twice doubles 2 -> 8 beats (%.3f s)", s.loopOut - s.loopIn);
        // KEY SYNC (official 9n 65) goes to the app (it picks the queued deck); key shift pads (mode 7) transpose
        while (djc_midi_poll_event(m, &ev)) {}
        in(0x92, 0x65, 0x7F); in(0x9B, 0x71, 0x7F); render(e, 0.02);
        bool ksEvent = false;
        while (djc_midi_poll_event(m, &ev)) if (ev.kind == DJC_EV_KEY_SYNC && ev.deck == 2) ksEvent = true;
        djc_get_deck_state(e, 2, &s);
        CHECK(ksEvent && !s.keySync && s.keyShift == 2, "KEY SYNC -> app event (no direct retune), key-shift pad 2 = +2 semitones (%d)", s.keyShift);
        // touching the platter in vinyl mode stops the deck dead
        in(0x92, 0x4D, 0x7F); render(e, 0.05);  // exit loop
        djc_get_deck_state(e, 2, &s); double p0 = s.position;
        in(0x92, 0x36, 0x7F); render(e, 0.2);
        djc_get_deck_state(e, 2, &s);
        CHECK(std::fabs(s.position - p0) < 0.03, "jog touch holds the track (moved %.3f s in 0.2 s)", s.position - p0);
        in(0x92, 0x36, 0x00);
        // deck layer event
        in(0x92, 0x3C, 0x7F);
        bool layer = false;
        while (djc_midi_poll_event(m, &ev)) if (ev.kind == DJC_EV_DECK_LAYER && ev.deck == 2 && ev.value == 1) layer = true;
        CHECK(layer, "0x92 0x3C 0x7F reports deck 3 layer on");
        // Beat FX from the GRV6 FX section: ECHO on channel 3, level full
        in(0x94, 0x21, 0x7F); in(0x94, 0x12, 0x7F); in(0xB4, 0x02, 0x7F); in(0xB4, 0x22, 0x7F); in(0x94, 0x47, 0x7F);
        in(0x94, 0x4B, 0x7F); // beat > : 1 -> 2 beats
        render(e, 0.05);
        djc_fx_state fx; djc_get_fx_state(e, &fx);
        CHECK(fx.on && fx.type == 1 && fx.target == 2 && fx.level > 0.99f && fx.beats == 2.0,
              "FX: %s on CH%d, level %.2f, %g beats, %.0f ms", djc_fx_name(fx.type), fx.target + 1, fx.level, fx.beats, fx.timeMs);
        CHECK(gAudioAllocs.load() == 0, "no audio-thread allocation with MIDI traffic");
        djc_midi_destroy(m);
        djc_destroy(e);
    }
    // 8. every Beat FX runs, stays finite, changes the signal, and tails out after OFF
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(124, 0.0, 60)};
        djc_send(e, 0, &c);
        post(e, DJC_PLAY, 0);
        std::vector<float> dry;
        render(e, 2.0, &dry);
        int bad = 0;
        auto rms = [](const std::vector<float> &v, size_t from) {
            double s = 0; for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
            return std::sqrt(s / double(std::max<size_t>(1, v.size() - from)));
        };
        double dryRms = rms(dry, dry.size() / 4), worst = 0; int worstFx = 0;
        for (int t = 0; t < 14; ++t) {
            post(e, DJC_FX_TYPE, 0, t); post(e, DJC_FX_TARGET, 0, 4); post(e, DJC_FX_LEVEL, 0, 0, 1.0); post(e, DJC_FX_ON, 0, 1);
            std::vector<float> L;
            render(e, 1.5, &L);
            double ratio = rms(L, L.size() / 3) / dryRms;
            if (ratio > worst) { worst = ratio; worstFx = t; }
            std::printf("     %-14s %+.1f dB\n", djc_fx_name(t), 20 * std::log10(ratio));
            post(e, DJC_FX_ON, 0, 0);
            render(e, 0.5);
            bool finite = true;
            for (float x : L) if (!std::isfinite(x) || std::fabs(x) > 4) finite = false;
            if (!finite) { bad++; std::printf("     fx %s not finite\n", djc_fx_name(t)); }
        }
        CHECK(bad == 0, "all 14 Beat FX stay finite and bounded on master");
        CHECK(worst < 1.5, "FX at full level stay within +3.5 dB of the dry mix on a click track (loudest %s: %+.1f dB)", djc_fx_name(worstFx), 20 * std::log10(worst));
        CHECK(gAudioAllocs.load() == 0, "no audio-thread allocation across all FX");
        djc_destroy(e);
    }
    // 8b. echo-type FX and colour FX ring out and stop once the music stops (no endless feedback)
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(124, 0.0, 60)};
        djc_send(e, 0, &c);
        auto rmsOf = [](const std::vector<float> &v) { double s = 0; for (float x : v) s += double(x) * x; return std::sqrt(s / std::max<size_t>(1, v.size())); };
        int stuck = 0;
        for (int t = 0; t < 14; ++t) {
            post(e, DJC_PLAY, 0); render(e, 0.05);
            post(e, DJC_FX_TYPE, 0, t); post(e, DJC_FX_TARGET, 0, 4); post(e, DJC_FX_LEVEL, 0, 0, 1.0); post(e, DJC_FX_ON, 0, 1);
            render(e, 2.0);
            post(e, DJC_FX_ON, 0, 0); post(e, DJC_PLAY, 0); // FX off, then pause (PLAY toggles)
            render(e, 12.0);
            std::vector<float> L; render(e, 1.0, &L);
            if (rmsOf(L) > 1e-3) { stuck++; std::printf("     %s still sounding: rms %.4f\n", djc_fx_name(t), rmsOf(L)); }
            djc_deck_state s; djc_get_deck_state(e, 0, &s);
            if (s.playing) post(e, DJC_PLAY, 0);
        }
        CHECK(stuck == 0, "every Beat FX tail dies out after OFF + stop");
        const char *cfxName[4] = {"FILTER", "DUB ECHO", "SWEEP", "SPACE"};
        int bad = 0;
        for (int t = 0; t < 4; ++t) {
            post(e, DJC_COLOR_FX_TYPE, 0, t);
            post(e, DJC_COLOR, 0, 0, 0.0); post(e, DJC_PLAY, 0);
            std::vector<float> L; render(e, 2.0, &L);
            post(e, DJC_COLOR, 0, 0, 1.0); render(e, 2.0, &L);
            bool fin = true; for (float x : L) if (!std::isfinite(x) || std::fabs(x) > 4) fin = false;
            post(e, DJC_COLOR, 0, 0, 0.5); post(e, DJC_PLAY, 0); render(e, 12.0);
            std::vector<float> T; render(e, 1.0, &T);
            if (!fin || rmsOf(T) > 1e-3) { bad++; std::printf("     colour FX %s: finite %d, tail rms %.4f\n", cfxName[t], fin, rmsOf(T)); }
        }
        djc_mixer_state mx; djc_get_mixer_state(e, &mx);
        CHECK(bad == 0 && mx.colorFxType == 3, "colour FX FILTER / DUB ECHO / SWEEP / SPACE: bounded, tails stop");
        CHECK(gAudioAllocs.load() == 0, "no audio-thread allocation in colour FX");
        djc_destroy(e);
    }
    // 8c. after Beat FX OFF the echoes fade while the music keeps playing (compare with a dry twin engine)
    {
        int stuck = 0;
        for (int t = 0; t < 14; ++t) {
            djc_engine *a = djc_create(SR, 4096), *b = djc_create(SR, 4096);
            for (djc_engine *e : {a, b}) {
                route4(e);
                djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(124, 0.0, 60)};
                djc_send(e, 0, &c);
                post(e, DJC_PLAY, 0);
            }
            post(a, DJC_FX_TYPE, 0, t); post(a, DJC_FX_TARGET, 0, 4); post(a, DJC_FX_LEVEL, 0, 0, 1.0); post(a, DJC_FX_ON, 0, 1);
            render(a, 3.0); render(b, 3.0);
            post(a, DJC_FX_ON, 0, 0);
            render(a, 10.0); render(b, 10.0);
            std::vector<float> A, B; render(a, 1.0, &A); render(b, 1.0, &B);
            double d = 0, r = 0;
            for (size_t i = 0; i < std::min(A.size(), B.size()); ++i) { d += double(A[i] - B[i]) * (A[i] - B[i]); r += double(B[i]) * B[i]; }
            double ratio = std::sqrt(d / std::max(r, 1e-12));
            if (ratio > 0.01) { stuck++; std::printf("     %s still audible 10 s after OFF (%.1f dB vs music)\n", djc_fx_name(t), 20 * std::log10(ratio)); }
            djc_destroy(a); djc_destroy(b);
        }
        CHECK(stuck == 0, "every Beat FX is gone (< -40 dB) 10 s after OFF while the track plays");
    }
    // 8d. SWEEP keeps the loudness; Beat FX frequency select leaves unselected bands alone
    {
        auto rmsOf = [](const std::vector<float> &v, size_t from) { double s = 0; for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i]; return std::sqrt(s / std::max<size_t>(1, v.size() - from)); };
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(124, 0.0, 60)};
        djc_send(e, 0, &c);
        post(e, DJC_PLAY, 0);
        std::vector<float> D; render(e, 2.0, &D);
        double dry = rmsOf(D, D.size() / 2), worst = 1e9;
        post(e, DJC_COLOR_FX_TYPE, 0, 2);
        for (double knob : {0.0, 0.15, 0.85, 1.0}) {
            post(e, DJC_COLOR, 0, 0, knob);
            std::vector<float> L; render(e, 3.0, &L);
            worst = std::min(worst, rmsOf(L, L.size() / 2) / dry);
            std::printf("     sweep knob %.2f: %+.1f dB\n", knob, 20 * std::log10(rmsOf(L, L.size() / 2) / dry));
        }
        CHECK(worst > 0.7, "SWEEP keeps the level (quietest setting %.1f dB)", 20 * std::log10(worst));
        post(e, DJC_COLOR, 0, 0, 0.5);
        djc_destroy(e);
        // ECHO on HI only, fed a 100 Hz sine: the output should match the dry signal
        djc_engine *a = djc_create(SR, 4096), *b = djc_create(SR, 4096);
        for (djc_engine *x : {a, b}) {
            route4(x);
            djc_cmd l{DJC_LOAD, 0, 0, 0, 0, sineTrack(100, 30)};
            djc_send(x, 0, &l);
            post(x, DJC_PLAY, 0);
        }
        post(a, DJC_FX_BAND, 0, 0); post(a, DJC_FX_BAND, 0, 1); // LOW and MID off -> HI only
        post(a, DJC_FX_TYPE, 0, 1); post(a, DJC_FX_TARGET, 0, 4); post(a, DJC_FX_LEVEL, 0, 0, 1.0); post(a, DJC_FX_ON, 0, 1);
        std::vector<float> A, B; render(a, 3.0, &A); render(b, 3.0, &B);
        double ra = rmsOf(A, A.size() / 2), rb = rmsOf(B, B.size() / 2);
        djc_fx_state fs; djc_get_fx_state(a, &fs);
        CHECK(fs.bands == 4 && std::fabs(20 * std::log10(ra / rb)) < 0.5, "ECHO on HI only leaves a 100 Hz tone untouched (%+.2f dB)", 20 * std::log10(ra / rb));
        djc_destroy(a); djc_destroy(b);
    }
    // 8e. GROOVE CIRCUIT drum roller: taps, grid-locked rolls, HOLD latch, RELEASE, LEDs
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_midi *m = djc_midi_create(e);
        auto in = [&](uint8_t a, uint8_t b, uint8_t c) { uint8_t msg[3] = {a, b, c}; djc_midi_in(m, msg, 3); };
        auto rmsOf = [](const std::vector<float> &v) { double s = 0; for (float x : v) s += double(x) * x; return std::sqrt(s / std::max<size_t>(1, v.size())); };
        // count hits: rising edges of a fast envelope
        auto hits = [](const std::vector<float> &v) { // onsets: peaks above half the max, at least 40 ms apart
            float mx = 0; for (float x : v) mx = std::max(mx, std::fabs(x));
            int c = 0; long last = -100000;
            for (size_t i = 0; i < v.size(); ++i)
                if (std::fabs(v[i]) > mx * 0.5f && long(i) - last > long(0.04 * SR)) { c++; last = long(i); }
            return c;
        };
        std::vector<float> L;
        in(0x90, 0x00, 0x7F); in(0x90, 0x00, 0x00); render(e, 0.4, &L);  // KICK: select + play
        double tap = rmsOf(L);
        render(e, 2.0);
        std::vector<float> P0, P1;
        in(0x90, 0x03, 0x7F); in(0x90, 0x03, 0x00); render(e, 0.3, nullptr, &P0); // HAT (radio: KICK off), CAPTURE off
        in(0x90, 0x08, 0x7F); in(0x90, 0x08, 0x00);                       // CAPTURE: left drums into the headphones
        in(0x90, 0x03, 0x7F); in(0x90, 0x03, 0x00); render(e, 0.3, nullptr, &P1);
        int disabledHits = 0;
        double phonesOff = rmsOf(P0), phonesOn = rmsOf(P1);
        in(0x90, 0x06, 0x7F);                                               // hold ROLL 3 (1/4 beat)
        render(e, 0.1);
        std::vector<float> R; render(e, 2.0, &R);                          // free clock 120 BPM -> 8 hits/s
        int h = hits(R);
        in(0x91, 0x06, 0x7F); render(e, 0.05);                             // right ROLL while left rolls: blocked
        int rightRoll = (djc_drum_state(e, 1) >> 8) & 15;
        in(0x91, 0x06, 0x00);
        in(0x90, 0x24, 0x7F);                                               // lever down (locked): HOLD
        in(0x90, 0x06, 0x00);                                               // let go of the roll button
        std::vector<float> H; render(e, 1.0, &H);
        int held = hits(H);
        in(0xB1, 0x12, 0x60); in(0xB1, 0x32, 0x00);                       // right knob: pitch up for both kits
        render(e, 0.05);
        int st = djc_drum_state(e, 0);
        int pitch = ((st >> 16) & 31) - 12;
        uint8_t buf[2048]; int n = djc_midi_out(m, buf, sizeof buf);
        bool led = false, hatLed = false, kickOff = true;
        for (int k = 0; k + 2 < n; k += 3) {
            if (buf[k] == 0x90 && buf[k + 1] == 0x08 && buf[k + 2] == 0x7F) led = true;
            if (buf[k] == 0x90 && buf[k + 1] == 0x03 && buf[k + 2] == 0x7F) hatLed = true;
            if (buf[k] == 0x90 && buf[k + 1] == 0x00 && buf[k + 2] == 0x7F) kickOff = false;
        }
        post(e, DJC_FX_ON, 0, 1); render(e, 0.05);
        in(0x90, 0x24, 0x00);                                               // lever up: stop drums + Beat FX, echo tail
        render(e, 0.05);
        djc_fx_state fs; djc_get_fx_state(e, &fs);
        render(e, 6.0);
        std::vector<float> T; render(e, 1.0, &T);
        CHECK(fs.on == 0 && phonesOff < 1e-6 && phonesOn > 0.005, "CAPTURE = drum CUE: phones %.4f -> %.4f, lever up turns Beat FX off (%d)",
              phonesOff, phonesOn, fs.on);
        CHECK(tap > 0.01 && disabledHits == 0 && h >= 14 && h <= 18 && rightRoll == 0 && held >= 7 && led && hatLed && kickOff &&
              pitch >= 5 && (djc_drum_state(e, 1) >> 16 & 31) - 12 == pitch && rmsOf(T) < 1e-4,
              "drums: tap %.3f, (%d), roll %d hits/2 s, right blocked %d, lever HOLD %d hits/s, ROLL LED %d, "
              "radio HAT only %d/%d, shared pitch %+d st, silent after lever up",
              tap, disabledHits, h, rightRoll == 0, held, led, hatLed, kickOff, pitch);
        in(0xB0, 0x12, 0x00); in(0xB0, 0x32, 0x00);                       // left knob = GAIN of both kits -> 0
        std::vector<float> Z; in(0x91, 0x00, 0x7F); render(e, 0.3, &Z); in(0x91, 0x00, 0x00);
        CHECK(rmsOf(Z) < 1e-6 && gAudioAllocs.load() == 0, "left GAIN at 0 also mutes the right (707) kit, no audio-thread allocation");
        djc_midi_destroy(m);
        djc_destroy(e);
    }
    // 8f. drums follow the deck you hear, on its rekordbox beat list
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        // deck 1: 120 BPM grid; deck 2: drifting beats (beat list differs from the constant 126 BPM grid)
        djc_track *a = clickTrack(120, 0.25, 40);
        std::vector<double> bl; double t = 0.30;
        for (int k = 0; k < 80; ++k) { bl.push_back(t); t += 60.0 / 126.0 * (1.0 + 0.01 * std::sin(k * 0.3)); }
        int64_t nf = int64_t(40 * SR); std::vector<float> x(size_t(nf), 0.0f);
        for (double bt : bl) for (int k = 0; k < 400 && int64_t(bt * SR) + k < nf; ++k) x[size_t(int64_t(bt * SR) + k)] = 0.8f * float(std::exp(-k / 60.0));
        djc_track *b = djc_track_create(x.data(), x.data(), nf, SR, 126, 0.30);
        djc_track_set_beats(b, bl.data(), int32_t(bl.size()));
        djc_cmd c1{DJC_LOAD, 0, 0, 0, 0, a}; djc_send(e, 0, &c1);
        djc_cmd c2{DJC_LOAD, 1, 0, 0, 0, b}; djc_send(e, 0, &c2);
        post(e, DJC_FADER, 0, 0, 0.0); post(e, DJC_FADER, 1, 0, 1.0); post(e, DJC_CROSSFADER, 0, 0, 0.5);
        post(e, DJC_MAKE_MASTER, 0);                       // master = deck 1, but you hear deck 2
        post(e, DJC_PLAY, 0); post(e, DJC_PLAY, 1);
        render(e, 1.0);
        // drums only on the master output: deck 2's fader down after we know its beats come from the list
        post(e, DJC_DRUM_PAD, 0, 3, 1); post(e, DJC_DRUM_GAIN, 0, 0, 1.0);
        post(e, DJC_DRUM_ROLL, 0, 0, 1); render(e, 0.6);
        djc_deck_state ds2; djc_get_deck_state(e, 1, &ds2);
        double t0 = ds2.position;                          // deck 2 time at the start of the capture
        post(e, DJC_FADER, 1, 0, 0.0); render(e, 0.05);   // only the drums from here (grid source holds)
        t0 += int(0.05 * SR / BLOCK) * BLOCK / SR;
        std::vector<float> L; render(e, 8.0, &L);
        std::vector<double> on; long last = -100000;
        for (size_t i = 0; i < L.size(); ++i) if (std::fabs(L[i]) > 0.05f && long(i) - last > long(0.2 * SR)) { on.push_back(i / SR + t0); last = long(i); }
        double worst = 0; int n = 0;
        for (double o : on) {
            double best = 1e9; for (double bt : bl) if (std::fabs(o - bt) < std::fabs(best)) best = o - bt;
            if (std::fabs(best) < 0.1) { worst = std::max(worst, std::fabs(best)); ++n; }
        }
        CHECK(n >= 14 && worst < 0.003, "drums follow the audible deck's beat list: %d of %zu onsets on its beats, worst %.2f ms",
              n, on.size(), worst * 1000);
        djc_destroy(e);
    }
    // 8g. DUB ECHO (DJM-A9 / 900NXS2 manuals): left = echo on the mids only, right = highs only,
    //     PARAMETER = feedback, the dry sound is untouched and the tail rings out with the knob back at centre
    {
        auto run = [&](float amt, float param, double &lo, double &mid, double &hi, double &tail, double &late) {
            djc::ColorFx fx; fx.prepare(SR);
            std::vector<float> L(size_t(SR * 12), 0.0f), R;
            uint32_t x = 1;
            for (size_t i = 0; i < size_t(0.05 * SR); ++i) { x = x * 1664525u + 1013904223u; L[i] = 0.5f * (float(int32_t(x)) / 2147483648.0f); }
            R = L;
            for (size_t o = 0; o < L.size(); o += 256) {
                int n = int(std::min<size_t>(256, L.size() - o));
                float a = o < size_t(0.1 * SR) ? amt : 0.0f;           // knob back to centre after 100 ms
                fx.process(&L[o], &R[o], n, djc::CFX_DUB_ECHO, a, 120.0, param);
            }
            // first repeat window (0.375 s at 120 BPM, 3/4 beat): split into bands
            djc::Biquad l1, h1, h2, l2; l1.lowpass(300, SR); h1.highpass(300, SR); l2.lowpass(3000, SR); h2.highpass(3000, SR);
            lo = mid = hi = 0;
            for (size_t i = size_t(0.2 * SR); i < size_t(0.7 * SR); ++i) {
                float v = L[i];
                float a = l1.tick(v), b = l2.tick(h1.tick(v)), c = h2.tick(v);
                lo += a * a; mid += b * b; hi += c * c;
            }
            tail = 0; for (size_t i = size_t(1.4 * SR); i < size_t(2.0 * SR); ++i) tail = std::max(tail, double(std::fabs(L[i])));
            late = 0; for (size_t i = size_t(11 * SR); i < L.size(); ++i) late = std::max(late, double(std::fabs(L[i])));
        };
        double lo, mid, hi, tail, late, lo2, mid2, hi2, tail2, late2, t3, l3, a3, b3, c3;
        run(-1.0f, 0.5f, lo, mid, hi, tail, late);
        run(1.0f, 0.5f, lo2, mid2, hi2, tail2, late2);
        run(-1.0f, 0.0f, a3, b3, c3, t3, l3);
        CHECK(lo + mid > 8 * hi && hi2 + mid2 > 8 * lo2,
              "DUB ECHO: left = dark (low+mid / high %.1f), right = bright (mid+high / low %.1f)",
              (lo + mid) / hi, (hi2 + mid2) / lo2);
        CHECK(tail > 0.002 && tail > 4 * t3 && late < 1e-3,
              "DUB ECHO tail after knob back: %.4f (feedback 0.5) vs %.4f (feedback 0), gone by 11 s (%.6f)", tail, t3, late);
    }
    // 8g2. DUB ECHO (XDJ-AZ walkthrough): COLOR amount sweeps the echo time 1/16 -> 3/4 beat;
    //      PARAMETER MIN = one single repeat, MAX = long tail
    {
        // returns the times (s) of echo peaks after a 3 ms 3 kHz blip at t = 0.5 s, knob held at amt
        auto echoes = [&](float amt, float param, double secs) {
            djc::ColorFx fx; fx.prepare(SR);
            std::vector<float> L(size_t(SR * secs), 0.0f), R;
            size_t b0 = size_t(0.5 * SR);
            for (size_t i = 0; i < size_t(0.003 * SR); ++i) L[b0 + i] = 0.6f * float(std::sin(2 * M_PI * 3000 * i / SR));
            R = L;
            std::vector<float> dry = L;
            for (size_t o = 0; o < L.size(); o += 256) {
                int n = int(std::min<size_t>(256, L.size() - o));
                fx.process(&L[o], &R[o], n, djc::CFX_DUB_ECHO, amt, 120.0, param);
            }
            std::vector<double> t; size_t last = 0;
            for (size_t i = b0 + size_t(0.01 * SR); i < L.size(); ++i) {
                float w = std::fabs(L[i] - dry[i]);
                if (w > 0.01f && (t.empty() || i - last > size_t(0.012 * SR))) { t.push_back((double(i) - double(b0)) / SR); }
                if (w > 0.01f) last = i;
            }
            return t;
        };
        auto small = echoes(0.06f, 0.6f, 2.0), full = echoes(1.0f, 0.6f, 3.0);
        double beat = 0.5, tSmall = small.empty() ? -1 : small[0], tFull = full.empty() ? -1 : full[0];
        double wantSmall = beat * 0.0625 * std::pow(12.0, 0.06), wantFull = beat * 0.75;
        CHECK(std::fabs(tSmall - wantSmall) < 0.006 && std::fabs(tFull - wantFull) < 0.006,
              "DUB ECHO time follows COLOR: just opened %.1f ms (~1/16 beat %.1f), fully open %.1f ms (3/4 beat %.1f)",
              tSmall * 1000, wantSmall * 1000, tFull * 1000, wantFull * 1000);
        auto one = echoes(1.0f, 0.0f, 3.0), many = echoes(1.0f, 1.0f, 6.0);
        CHECK(one.size() == 1 && many.size() >= 5, "PARAMETER MIN = %zu repeat, MAX = %zu repeats", one.size(), many.size());
    }
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(120, 0.1, 30)}; djc_send(e, 0, &c);
        post(e, DJC_FADER, 0, 0, 1.0); post(e, DJC_COLOR_FX_TYPE, 0, 1); post(e, DJC_COLOR, 0, 0, 0.0);
        post(e, DJC_PLAY, 0); render(e, 2.0);
        post(e, DJC_FADER, 0, 0, 0.0); post(e, DJC_COLOR, 0, 0, 0.5); render(e, 0.05);
        std::vector<float> L; render(e, 0.7, &L);
        double tail = 0; for (float v : L) tail = std::max(tail, double(std::fabs(v)));
        post(e, DJC_COLOR_FX_ON, 0, 0); render(e, 0.05);
        std::vector<float> M; render(e, 0.5, &M);
        double after = 0; for (float v : M) after = std::max(after, double(std::fabs(v)));
        CHECK(tail > 0.01 && after < 1e-6, "fader down: DUB ECHO tail still rings (%.3f); COLOR FX OFF cuts it (%.1e)", tail, after);
        djc_destroy(e);
    }
    // 8h. set recorder: exact copy of the master output, lock-free, no audio-thread allocation
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, sineTrack(220, 30)}; djc_send(e, 0, &c);
        post(e, DJC_PLAY, 0); render(e, 0.2);
        djc_rec_start(e);
        std::vector<float> M; render(e, 2.0, &M);
        djc_rec_stop(e);
        std::vector<float> got(size_t(SR * 3) * 2);
        int nr = 0, k;
        while ((k = djc_rec_read(e, got.data() + nr * 2, 4096)) > 0) nr += k;
        double err = 0;
        for (int i = 0; i < nr && i < int(M.size()); ++i) err = std::max(err, double(std::fabs(got[size_t(i) * 2] - M[size_t(i)])));
        CHECK(nr == int(M.size()) && err < 1e-7 && djc_rec_dropped(e) == 0 && gAudioAllocs.load() == 0,
              "recorder: %d frames (master %zu), max diff %.1e, dropped %lld", nr, M.size(), err, (long long)djc_rec_dropped(e));
        // writer stalls for 12 s: ring keeps the newest 10 s worth of room, counts the rest as dropped
        djc_rec_start(e); render(e, 12.0); djc_rec_stop(e);
        CHECK(djc_rec_dropped(e) > int64_t(SR * 1.5), "writer 12 s behind: %lld frames dropped and reported", (long long)djc_rec_dropped(e));
        djc_destroy(e);
    }
    // 8i. two full-scale kicks on top of each other: limiter keeps it clean (no flat-topped clipping)
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        auto kick = [&]() {
            int64_t n = int64_t(10 * SR); std::vector<float> x(size_t(n), 0.0f);
            for (double t = 0; t < n; t += 0.5 * SR)
                for (int k = 0; k < 12000 && int64_t(t) + k < n; ++k) {
                    double ph = 2 * M_PI * (50 * k / SR + 60 * 0.03 * (1 - std::exp(-k / (0.03 * SR)))); // pitch drop
                    x[size_t(int64_t(t) + k)] = 0.99f * float(std::exp(-k / 6000.0) * std::sin(ph));
                }
            return djc_track_create(x.data(), x.data(), n, SR, 120, 0);
        };
        djc_cmd c1{DJC_LOAD, 0, 0, 0, 0, kick()}; djc_send(e, 0, &c1);
        djc_cmd c2{DJC_LOAD, 1, 0, 0, 0, kick()}; djc_send(e, 0, &c2);
        post(e, DJC_FADER, 0, 0, 1.0); post(e, DJC_FADER, 1, 0, 1.0); post(e, DJC_CROSSFADER, 0, 0, 0.5);
        post(e, DJC_MASTER_LEVEL, 0, 0, 1.0);
        post(e, DJC_PLAY, 0); post(e, DJC_PLAY, 1);
        std::vector<float> L; render(e, 4.0, &L);
        double pk = 0; int run = 0, worstRun = 0;
        for (float v : L) { pk = std::max(pk, double(std::fabs(v))); run = std::fabs(v) > 0.96f ? run + 1 : 0; worstRun = std::max(worstRun, run); }
        CHECK(pk <= 0.985 && worstRun < 24 && gAudioAllocs.load() == 0,
              "2 full-scale kicks, master full: peak %.3f, longest run at the ceiling %d samples (clipping would be 100s)", pk, worstRun);
        djc_destroy(e);
    }
    // 8j. hot cue during a loop leaves the loop; quantized hot cue lands on the beat-list grid
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_track *t = clickTrack(120, 0.25, 60);
        std::vector<double> bl; for (int k = 0; k < 110; ++k) bl.push_back(0.25 + k * 0.5 + 0.004 * k); // grid drifts from 120 BPM
        djc_track_set_beats(t, bl.data(), int32_t(bl.size()));
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, t}; djc_send(e, 0, &c);
        post(e, DJC_QUANTIZE, 0, 1);
        post(e, DJC_SEEK, 0, 0, 30.13); post(e, DJC_PLAY, 0); render(e, 0.02);
        post(e, DJC_HOTCUE, 0, 0, 1);                                  // set A near 30.13 s: snaps to the list beat
        render(e, 0.02);
        djc_deck_state s; djc_get_deck_state(e, 0, &s);
        double nearest = 1e9; for (double b : bl) if (std::fabs(b - 30.13) < std::fabs(nearest - 30.13)) nearest = b;
        post(e, DJC_BEAT_LOOP, 0, 0, 4.0); render(e, 1.0);             // 4-beat loop running
        post(e, DJC_SEEK, 0, 0, 40.0); render(e, 0.01);                // (seek does not exit)
        post(e, DJC_BEAT_LOOP, 0, 0, 2.0); render(e, 0.3);
        post(e, DJC_HOTCUE, 0, 0, 1); render(e, 2.5);                  // press A during the loop
        djc_get_deck_state(e, 0, &s);
        CHECK(!s.loopActive && s.position > nearest + 2.0, "hot cue during a loop leaves the loop (loop %d, at %.2f s, cue %.3f)",
              s.loopActive, s.position, nearest);
        // phase kept on the list grid: the playhead's distance to the nearest list beat equals what it was before
        djc_deck_state a; djc_get_deck_state(e, 0, &a);
        auto ph = [&](double p) { size_t k = 0; while (k + 1 < bl.size() && bl[k + 1] <= p) ++k; return (p - bl[k]) / (bl[k + 1] - bl[k]); };
        // press A mid-beat: CDJ quantize waits for the next beat, then the cue's beat sounds exactly on it
        post(e, DJC_SEEK, 0, 0, bl[80] + 0.5 * (bl[81] - bl[80])); render(e, 0.05);
        djc_get_deck_state(e, 0, &a);
        double before = ph(a.position);
        post(e, DJC_HOTCUE, 0, 0, 1); render(e, 0.05);
        djc_deck_state w; djc_get_deck_state(e, 0, &w);
        bool waited = w.position > bl[80] + 0.5 * (bl[81] - bl[80]);   // still playing on, not jumped yet
        render(e, 0.6);
        djc_get_deck_state(e, 0, &a);
        double since = a.position - nearest;                             // how far past the cue we are now
        double after = ph(a.position), dph = std::fabs(after - (before + 0.65 / 0.5 * 0)); (void)dph;
        // the jump fired on the beat: now we are (time since that beat) past the cue, i.e. phase continued
        double expect = std::fmod(before + (0.05 + 0.6) / (bl[81] - bl[80]), 1.0);
        double err = std::fabs(after - expect); err = std::min(err, 1 - err);
        CHECK(waited && since > 0 && since < 0.6 && err < 0.03,
              "quantized hot cue waits for the beat (%d), cue lands on it: %.3f s past the cue, phase error %.3f beat",
              waited, since, err);
        djc_destroy(e);
    }
    // 8k. lever back: drums, their echo and Beat FX tails are silent within 15 ms
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        post(e, DJC_DRUM_GAIN, 0, 0, 1.0);
        post(e, DJC_DRUM_PAD, 0, 1, 1);                                 // snare
        post(e, DJC_FX_TYPE, 0, 1); post(e, DJC_FX_TARGET, 0, 5); post(e, DJC_FX_LEVEL, 0, 0, 1.0); post(e, DJC_FX_ON, 0, 1);
        post(e, DJC_DRUM_HOLD, 0, 1); post(e, DJC_DRUM_ROLL, 0, 2, 1); render(e, 2.0);
        post(e, DJC_DRUM_ROLL, 0, 2, 0);
        post(e, DJC_DRUM_HOLD, 0, 0);                                   // lever back
        std::vector<float> L; render(e, 1.0, &L);
        double after = 0; for (size_t i = size_t(0.015 * SR); i < L.size(); ++i) after = std::max(after, double(std::fabs(L[i])));
        double first = 0; for (size_t i = 0; i < 64; ++i) first = std::max(first, double(std::fabs(L[i])));
        djc_fx_state fs; djc_get_fx_state(e, &fs);
        CHECK(after < 1e-6 && fs.on == 0, "lever back kills the roll, its echo and the Beat FX tail (%.2e after 15 ms)", after);
        djc_destroy(e);
    }
    // 8l. tempo fader lands on 0.01 BPM steps; nudge snaps back onto the other deck's grid
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c1{DJC_LOAD, 0, 0, 0, 0, clickTrack(127, 0.1, 60)}; djc_send(e, 0, &c1);
        djc_cmd c2{DJC_LOAD, 1, 0, 0, 0, clickTrack(125, 0.1, 60)}; djc_send(e, 0, &c2);
        post(e, DJC_TEMPO, 1, 0, 0.0161234);                            // 125 * (1 + 0.1 * 0.0161) = 125.2015..
        post(e, DJC_PLAY, 0); post(e, DJC_PLAY, 1); render(e, 0.2);
        djc_deck_state b; djc_get_deck_state(e, 1, &b);
        double tenths = b.bpm * 10 - std::round(b.bpm * 10);
        CHECK(std::fabs(tenths) < 1e-6 && std::fabs(b.bpm - 125.2) < 1e-6, "tempo fader BPM rounded to 0.1 (125.2015 -> %.4f)", b.bpm);
        // match deck 2 to 127.00, nudge it off by ~0.15 beat, let go: snaps back onto deck 1's beats
        post(e, DJC_TEMPO, 1, 0, (127.0 / 125.0 - 1) / 0.1); render(e, 0.5);
        post(e, DJC_SYNC, 1, 1); render(e, 0.5); post(e, DJC_SYNC, 1, 0); render(e, 0.3); // start in phase
        post(e, DJC_JOG_NUDGE_GAIN, 1, 0, 0.5);
        for (int k = 0; k < 40; ++k) { djc_post(e, 1, DJC_JOG_TURN, 1, 12, 0); render(e, 0.01); }
        djc_deck_state a1, a2; djc_get_deck_state(e, 0, &a1); djc_get_deck_state(e, 1, &a2);
        auto phs = [](double p, double bpm, double first) { double x = (p - first) * bpm / 60.0; return x - std::floor(x); };
        double off = phs(a2.position, 125, 0.1) - phs(a1.position, 127, 0.1); off -= std::round(off);
        render(e, 4.0);
        djc_get_deck_state(e, 0, &a1); djc_get_deck_state(e, 1, &a2);
        double end = phs(a2.position, 125, 0.1) - phs(a1.position, 127, 0.1); end -= std::round(end);
        CHECK(std::fabs(off) > 0.02 && std::fabs(end) >= std::fabs(off), "nudge snap removed: %.3f beat off after the nudge, not pulled back (%.3f beat 4 s later)", off, end);
        djc_destroy(e);
    }
    // 8m. rekordbox grid vs decoded audio: measured offset (encoder / decoder delay ~26 ms on some MP3/AAC)
    {
        auto music = [&](double bpm, double first, double secs) {
            int64_t n = int64_t(secs * SR); std::vector<float> x(size_t(n), 0.0f);
            uint32_t r = 7;
            for (int64_t i = 0; i < n; ++i) { r = r * 1664525u + 1013904223u; x[size_t(i)] = 0.03f * (float(int32_t(r)) / 2147483648.0f); } // hiss
            for (double t = first; t < secs; t += 60.0 / bpm) {
                int64_t i0 = int64_t(t * SR);
                for (int k = 0; k < 9000 && i0 + k < n; ++k) {                                           // kick
                    double ph = 2 * M_PI * (52 * k / SR + 90 * 0.012 * (1 - std::exp(-k / (0.012 * SR))));
                    x[size_t(i0 + k)] += 0.7f * float(std::exp(-k / 5000.0) * std::sin(ph));
                }
                int64_t h0 = int64_t((t + 30.0 / bpm) * SR);                                              // off-beat hat
                for (int k = 0; k < 1500 && h0 + k < n; ++k) { r = r * 1664525u + 1013904223u; x[size_t(h0 + k)] += 0.2f * float(std::exp(-k / 300.0)) * (float(int32_t(r)) / 2147483648.0f); }
            }
            return x;
        };
        auto grid = [&](double bpm, double first, double secs, double shift) {
            std::vector<double> b; for (double t = first; t < secs; t += 60.0 / bpm) b.push_back(t + shift); return b;
        };
        std::vector<float> x = music(124, 0.30, 90);
        double lag, conf;
        auto g0 = grid(124, 0.30, 90, 0.0);  djc_grid_offset(x.data(), int64_t(x.size()), SR, g0.data(), int32_t(g0.size()), &lag, &conf);
        double lag0 = lag, conf0 = conf;
        auto g1 = grid(124, 0.30, 90, -0.026); djc_grid_offset(x.data(), int64_t(x.size()), SR, g1.data(), int32_t(g1.size()), &lag, &conf);
        double lag1 = lag, conf1 = conf;
        auto g2 = grid(124, 0.30, 90, +0.013); djc_grid_offset(x.data(), int64_t(x.size()), SR, g2.data(), int32_t(g2.size()), &lag, &conf);
        double lag2 = lag;
        std::vector<float> pad(size_t(60 * SR)); for (size_t i = 0; i < pad.size(); ++i) pad[i] = 0.3f * float(std::sin(2 * M_PI * 110 * i / SR));
        djc_grid_offset(pad.data(), int64_t(pad.size()), SR, g0.data(), int32_t(g0.size()), &lag, &conf);
        double confPad = conf;
        CHECK(std::fabs(lag0) <= 0.0015 && std::fabs(lag1 - 0.026) <= 0.0015 && std::fabs(lag2 + 0.013) <= 0.0015 && conf0 > 3 && conf1 > 3 && confPad < 2,
              "grid offset check: exact grid %+.1f ms, grid 26 ms early -> %+.1f ms, 13 ms late -> %+.1f ms, beatless pad conf %.1f",
              lag0 * 1000, lag1 * 1000, lag2 * 1000, confPad);
    }
    // 8n. master EQ: low shelf cuts a 50 Hz tone, high shelf boosts a 12 kHz tone, flat = untouched
    {
        auto lvl = [&](double hz, double lowDb, double hiDb) {
            djc_engine *e = djc_create(SR, 4096);
            route4(e);
            djc_cmd c{DJC_LOAD, 0, 0, 0, 0, sineTrack(hz, 10)}; djc_send(e, 0, &c);
            post(e, DJC_FADER, 0, 0, 1.0); post(e, DJC_MASTER_LEVEL, 0, 0, 0.84);
            post(e, DJC_MASTER_EQ, 0, 0, lowDb); post(e, DJC_MASTER_EQ, 0, 2, hiDb);
            post(e, DJC_PLAY, 0); render(e, 0.5);
            std::vector<float> L; render(e, 1.0, &L);
            djc_destroy(e);
            double a = 0; for (float v : L) a += double(v) * v; return 10 * std::log10(a / double(L.size()) + 1e-24);
        };
        double lowFlat = lvl(50, 0, 0), lowCut = lvl(50, -12, 0), hiFlat = lvl(12000, 0, 0), hiBoost = lvl(12000, 0, 6);
        CHECK(lowFlat - lowCut > 9 && hiBoost - hiFlat > 3.5 && hiBoost - hiFlat < 6.5,
              "master EQ: LOW -12 dB cuts 50 Hz by %.1f dB, HI +6 dB lifts 12 kHz by %.1f dB", lowFlat - lowCut, hiBoost - hiFlat);
    }
    // 8o. phones: master only with MASTER CUE, whatever the MIX knob; drums at unity around 63 % GAIN
    {
        djc_engine *e = djc_create(SR, 4096);
        route4(e);
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, sineTrack(220, 10)}; djc_send(e, 0, &c);
        post(e, DJC_FADER, 0, 0, 1.0); post(e, DJC_PHONES_MIX, 0, 0, 0.5); post(e, DJC_PLAY, 0); render(e, 0.3);
        std::vector<float> P0, P1;
        render(e, 0.3, nullptr, &P0);
        post(e, DJC_MASTER_CUE, 0, 1); render(e, 0.1);
        render(e, 0.3, nullptr, &P1);
        double off = 0, on = 0; for (float v : P0) off = std::max(off, double(std::fabs(v))); for (float v : P1) on = std::max(on, double(std::fabs(v)));
        CHECK(off < 1e-6 && on > 0.05, "phones: nothing cued, MIX centred = silence (%.1e); MASTER CUE on = master (%.3f)", off, on);
        // same track on master and CH CUE: not doubled (owner: it saturated)
        std::vector<float> P2, P3;
        post(e, DJC_CH_CUE, 0, 1); render(e, 0.1); render(e, 0.3, nullptr, &P2);
        double both = 0; for (float v : P2) both = std::max(both, double(std::fabs(v)));
        post(e, DJC_FADER, 0, 0, 0.0); render(e, 0.1); render(e, 0.3, nullptr, &P3);   // fader down: CH CUE is back
        double cueOnly = 0; for (float v : P3) cueOnly = std::max(cueOnly, double(std::fabs(v)));
        CHECK(std::fabs(both - on) < 0.01 && cueOnly > 0.05,
              "MASTER CUE + CH CUE on a live track: phones %.3f = master alone %.3f (no double); fader down -> CH CUE %.3f",
              both, on, cueOnly);
        djc_destroy(e);
    }
    // 9. rekordbox Device Library + ANLZ on real exports (rekordcrate fixtures)
    {
        std::string root = FIXTURES "/rekordcrate/demo_tracks";
        djc_library *l = djc_lib_open_pdb((root + "/PIONEER/rekordbox/export.pdb").c_str());
        CHECK(djc_lib_track_count(l) == 2 && std::string(djc_lib_error(l)).empty(), "demo export: 2 tracks");
        const djc_lib_track *t = djc_lib_track_at(l, 0);
        CHECK(std::string(t->title) == "Demo Track 1" && std::string(t->artist) == "Loopmasters" &&
              std::string(t->key) == "Fm" && std::fabs(t->bpm - 128.0) < 1e-6 && t->duration == 172,
              "track 1: '%s' / %s / key %s / %.2f BPM / %u s", t->title, t->artist, t->key, t->bpm, t->duration);
        CHECK(std::string(t->filePath) == "/Contents/Loopmasters/UnknownAlbum/Demo Track 1.mp3", "file path %s", t->filePath);
        djc_anlz *a = djc_anlz_open((root + t->analyzePath).c_str());
        const djc_beat *b = djc_anlz_beats(a);
        CHECK(djc_anlz_beat_count(a) == 368 && b && b[0].tempo100 == 12800 && b[0].beatNumber == 1,
              "beat grid: %d beats, first %u ms at %.2f BPM", djc_anlz_beat_count(a), b ? b[0].timeMs : 0, b ? b[0].tempo100 / 100.0 : 0);
        CHECK(djc_anlz_wave_source(a) == 7 && std::abs(djc_anlz_wave_count(a) / 150.0 - 172.4) < 0.5,
              "3-band waveform PWV7: %d points = %.1f s", djc_anlz_wave_count(a), djc_anlz_wave_count(a) / 150.0);
        CHECK(djc_anlz_phrase_count(a) == 19 && djc_anlz_mood(a) == 2, "phrases: %d, mood %d", djc_anlz_phrase_count(a), djc_anlz_mood(a));
        {
            int n7 = djc_anlz_wave_len(a, 7), n5 = djc_anlz_wave_len(a, 5), n3 = djc_anlz_wave_len(a, 3);
            const float *h5 = djc_anlz_wave_data(a, 5, 0), *r5 = djc_anlz_wave_data(a, 5, 1);
            float maxH = 0, maxR = 0;
            for (int i = 0; h5 && i < n5; ++i) { maxH = std::max(maxH, h5[i]); maxR = std::max(maxR, r5[i]); }
            CHECK(n7 > 0 && n5 > 0 && n3 > 0 && maxH > 0.3f && maxH <= 1.0f && maxR > 0.0f && maxR <= 1.0f,
                  "raw waveforms: 3-band %d, RGB %d (max h %.2f, max red %.2f), blue %d", n7, n5, maxH, maxR, n3);
            int mem = 0, act = 0;
            for (int i = 0; i < djc_anlz_cue_count(a); ++i) { const djc_cue *c = djc_anlz_cue_at(a, i); if (c->hotCue == 0) mem++; if (c->active) act++; }
            std::printf("     (demo track: %d memory cues, %d active loops)\n", mem, act);
        }
        djc_anlz_close(a);
        djc_lib_close(l);

        l = djc_lib_open_pdb(FIXTURES "/rekordcrate/big/PIONEER/rekordbox/export.pdb");
        int folders = 0, withTracks = 0;
        for (int i = 0; i < djc_lib_playlist_count(l); ++i) {
            const djc_lib_playlist *p = djc_lib_playlist_at(l, i);
            if (p->isFolder) folders++;
            else if (djc_lib_playlist_tracks(l, p->id, nullptr, 0) > 0) withTracks++;
        }
        CHECK(djc_lib_track_count(l) == 3886 && djc_lib_playlist_count(l) == 104,
              "large export: %d tracks, %d playlists (%d folders, %d non-empty)", djc_lib_track_count(l),
              djc_lib_playlist_count(l), folders, withTracks);
        djc_lib_close(l);
        l = djc_lib_open_pdb(FIXTURES "/does/not/exist.pdb");
        CHECK(djc_lib_track_count(l) == 0 && std::string(djc_lib_error(l)).size() > 0, "missing file reports an error, no crash");
        djc_lib_close(l);
    }
    // 10. rekordbox cues in the engine + SHIFT handling on the GRV6
    {
        djc_engine *e = djc_create(SR, 4096);
        djc_midi *m = djc_midi_create(e);
        auto in = [&](uint8_t a, uint8_t b, uint8_t c) { uint8_t msg[3] = {a, b, c}; djc_midi_in(m, msg, 3); };
        djc_cmd c{DJC_LOAD, 0, 0, 0, 0, clickTrack(120, 0.0, 60)};
        djc_send(e, 0, &c);
        djc_cmd hc{DJC_SET_HOTCUE, 0, 2, 8.0, 10.0, nullptr}; djc_send(e, 0, &hc);   // hot loop C 8-10 s
        djc_cmd cue{DJC_SET_CUE, 0, 0, 4.0, 0, nullptr}; djc_send(e, 0, &cue);
        render(e, 0.02);
        djc_deck_state s; djc_get_deck_state(e, 0, &s);
        CHECK(std::fabs(s.cuePoint - 4.0) < 1e-6 && std::fabs(s.position - 4.0) < 1e-6, "rekordbox memory cue sets cue + parks deck");
        post(e, DJC_HOTCUE, 0, 2, 1); render(e, 3.0);
        djc_get_deck_state(e, 0, &s);
        CHECK(s.loopActive && s.position >= 8.0 && s.position < 10.0, "hot loop C plays 8-10 s loop (%.3f)", s.position);
        double r0 = s.tempoRange;
        in(0x90, 0x3F, 0x7F); in(0x90, 0x60, 0x7F); in(0x90, 0x60, 0x00); in(0x90, 0x3F, 0x00); // SHIFT + MASTER TEMPO (9n 60)
        render(e, 0.02);
        djc_get_deck_state(e, 0, &s);
        CHECK(!s.keylock && s.tempoRange > r0, "SHIFT+MASTER TEMPO changes range %.2f -> %.2f, keylock untouched", r0, s.tempoRange);
        in(0x90, 0x3F, 0x7F); in(0x90, 0x51, 0x7F); in(0x90, 0x3F, 0x00);           // SHIFT + CALL < halves
        render(e, 0.02);
        djc_get_deck_state(e, 0, &s);
        CHECK(std::fabs((s.loopOut - s.loopIn) - 1.0) < 1e-3, "SHIFT+CALL < halves the loop (%.3f s)", s.loopOut - s.loopIn);
        // the unit's own SHIFT notes: MASTER TEMPO +SHIFT 0x60 = range, KEY SYNC +SHIFT 0x64 = key reset
        r0 = s.tempoRange;
        post(e, DJC_KEY_SHIFT, 0, 0, 3); render(e, 0.02);
        in(0x90, 0x60, 0x7F); render(e, 0.02);
        djc_get_deck_state(e, 0, &s);
        CHECK(s.tempoRange != r0 && s.keyShift == 3, "0x60 (SHIFT+MASTER TEMPO) cycles range %.2f -> %.2f, key kept", r0, s.tempoRange);
        r0 = s.tempoRange;
        in(0x90, 0x64, 0x7F); render(e, 0.02);
        djc_get_deck_state(e, 0, &s);
        CHECK(s.keyShift == 0 && s.tempoRange == r0, "0x64 (SHIFT+KEY SYNC) resets key, range kept");
        // browse buttons from the official list
        in(0x92, 0x00, 0x00); // (no-op)
        in(0x96, 0x65, 0x7F); in(0x96, 0x66, 0x7F); in(0x96, 0x7A, 0x7F); in(0x96, 0x38, 0x7F); in(0x96, 0x2E, 0x7F);
        djc_ui_event ev; int back = 0, top = 0, view = 0, tf = 0, tr = 0;
        while (djc_midi_poll_event(m, &ev)) {
            if (ev.kind == DJC_EV_BROWSE_BACK && ev.value == 1) back++;
            if (ev.kind == DJC_EV_BROWSE_BACK && ev.value == 2) top++;
            if (ev.kind == DJC_EV_VIEW) view++;
            if (ev.kind == DJC_EV_TILT && ev.value == 1) tf++;
            if (ev.kind == DJC_EV_TILT && ev.value == 4) tr++;
        }
        CHECK(back == 1 && top == 1 && view == 1 && tf == 1 && tr == 1, "BACK, SHIFT+BACK, VIEW, tilt forward/right reach the UI");
        in(0x96, 0x35, 0x7F); in(0x96, 0x35, 0x00); in(0x96, 0x68, 0x7F); in(0x96, 0x68, 0x00);
        int disc = 0, src = 0;
        while (djc_midi_poll_event(m, &ev)) { if (ev.kind == DJC_EV_VIEW) disc++; if (ev.kind == DJC_EV_SOURCE) src++; }
        djc_get_deck_state(e, 0, &s);
        CHECK(disc == 1 && src == 1, "DISCOVER (96 35) = SOURCE, SHIFT+DISCOVER (96 68) = BROWSE; deck QUANTIZE on 9n 35 unaffected");
        // active memory loop engages when the playhead reaches it; plain memory loop does not
        in(0x90, 0x4D, 0x7F); render(e, 0.02);                          // exit hot loop
        djc_cmd ml{DJC_SET_MEMCUE, 0, 0 | 0x100, 20.0, 22.0, nullptr}; djc_send(e, 0, &ml);
        djc_cmd pl{DJC_SET_MEMCUE, 0, 1, 15.0, 16.0, nullptr}; djc_send(e, 0, &pl);
        post(e, DJC_SEEK, 0, 0, 14.0); render(e, 8.0);
        djc_get_deck_state(e, 0, &s);
        CHECK(s.playing && s.loopActive && std::fabs(s.loopIn - 20.0) < 1e-3 && s.position >= 20.0 && s.position < 22.0,
              "active loop 20-22 s engaged by playback (pos %.3f), plain loop at 15 s skipped", s.position);
        // backspin inside an active loop stays inside the loop (wraps to OUT)
        {
            post(e, DJC_BEAT_LOOP, 0, 0, 4); render(e, 0.1);
            djc_deck_state l; djc_get_deck_state(e, 0, &l);
            double lin = l.loopIn, lout = l.loopOut, minPos = 1e9, maxPos = -1;
            in(0x90, 0x36, 0x7F);                       // touch platter (vinyl)
            for (int k = 0; k < 200; ++k) {
                in(0xB0, 0x22, 0x40 - 20); render(e, 0.006);
                djc_get_deck_state(e, 0, &l);
                minPos = std::min(minPos, l.position); maxPos = std::max(maxPos, l.position);
            }
            in(0x90, 0x36, 0x00); render(e, 0.05);
            CHECK(l.loopActive && minPos >= lin - 0.02 && maxPos <= lout + 0.02,
                  "backspin in a %.2f-%.2f s loop stays inside (%.2f..%.2f)", lin, lout, minPos, maxPos);
            in(0x90, 0x4D, 0x7F); render(e, 0.02);      // exit loop
        }
        // nudge: fast spin of the wheel side is capped at +30 %, BPM display ignores it
        double bpm0 = s.bpm;
        for (int k = 0; k < 20; ++k) { in(0xB0, 0x21, 0x40 + 30); render(e, 0.006); }
        djc_get_deck_state(e, 0, &s);
        CHECK(s.rate <= s.bpm / s.trackBpm * 1.08 + 1e-6 && s.rate > s.bpm / s.trackBpm && std::fabs(s.bpm - bpm0) < 1e-6,
              "nudge bends rate to %.3f (cap 1.08x), BPM stays %.2f", s.rate / (s.bpm / s.trackBpm), s.bpm);
        djc_midi_destroy(m);
        djc_destroy(e);
    }
    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
