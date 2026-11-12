// Offline analysis helpers (not real-time). Phase 1 stand-ins until Phase 3 reads rekordbox grids.
#include "djcore.h"
#include "dsp.hpp"

#include <vector>
#include <cmath>
#include <algorithm>

using namespace djc;

static std::vector<float> onsetEnvelope(const float *x, int64_t frames, double sr, double &fps) {
    // high-passed energy (kick + hats), log-flux, ~200 frames per second
    int hop = std::max(1, int(std::lround(sr / 200.0)));
    fps = sr / hop;
    Biquad hp;
    hp.highpass(40.0, sr);
    int64_t nHop = frames / hop;
    std::vector<float> env(size_t(std::max<int64_t>(nHop, 1)), 0.0f);
    float prev = 0;
    for (int64_t h = 0; h < nHop; ++h) {
        double e = 0;
        for (int k = 0; k < hop; ++k) {
            float s = hp.tick(x[h * hop + k]);
            e += double(s) * s;
        }
        float le = float(std::log(1e-9 + e / hop));
        env[size_t(h)] = std::max(0.0f, le - prev);
        prev = le;
    }
    // remove local mean
    std::vector<float> out(env.size());
    int w = int(fps * 0.1);
    double acc = 0;
    for (size_t i = 0; i < env.size(); ++i) {
        acc += env[i];
        if (i >= size_t(2 * w)) acc -= env[i - 2 * w];
        float mean = float(acc / std::min<size_t>(i + 1, size_t(2 * w)));
        out[i] = std::max(0.0f, env[i] - mean);
    }
    return out;
}

static double acfAt(const std::vector<float> &e, double lag) {
    int64_t L = int64_t(std::floor(lag));
    double f = lag - L;
    double s = 0;
    for (size_t t = 0; t + size_t(L) + 1 < e.size(); ++t)
        s += e[t] * ((1 - f) * e[t + size_t(L)] + f * e[t + size_t(L) + 1]);
    return s;
}

extern "C" void djc_estimate_bpm(const float *mono, int64_t frames, double sr, double *bpmOut, double *firstBeatOut) {
    *bpmOut = 0;
    *firstBeatOut = 0;
    if (frames < sr * 4) return;
    double fps;
    auto env = onsetEnvelope(mono, frames, sr, fps);
    auto score = [&](double bpm) {
        double lag = 60.0 * fps / bpm, s = 0;
        for (int k = 1; k <= 4; ++k) s += acfAt(env, lag * k) / k;
        double oct = std::log2(bpm / 122.0); // mild prior toward house tempos
        return s * std::exp(-0.5 * oct * oct / (0.6 * 0.6));
    };
    double best = 0, bestS = -1;
    for (double b = 70; b <= 180; b += 0.5) {
        double s = score(b);
        if (s > bestS) { bestS = s; best = b; }
    }
    for (double b = best - 0.6; b <= best + 0.6; b += 0.01) {
        double s = score(b);
        if (s > bestS) { bestS = s; best = b; }
    }
    double r = std::round(best);
    if (std::fabs(best - r) < 0.06) best = r; // most club tracks have integer tempo
    // phase: offset maximising the comb sum
    double period = 60.0 * fps / best;
    int steps = int(period);
    double bestP = 0, bestPS = -1;
    for (int p = 0; p < steps; ++p) {
        double s = 0;
        for (double t = p; t < double(env.size()); t += period) s += env[size_t(t)];
        if (s > bestPS) { bestPS = s; bestP = p; }
    }
    *bpmOut = best;
    *firstBeatOut = bestP / fps;
}

extern "C" void djc_overview(const float *mono, int64_t frames, double sr, int32_t bins, float *low, float *mid,
                             float *high) {
    if (bins <= 0) return;
    Biquad lp, hp;
    lp.lowpass(250.0, sr);
    hp.highpass(3000.0, sr);
    float mx[3] = {1e-9f, 1e-9f, 1e-9f};
    for (int b = 0; b < bins; ++b) {
        int64_t s0 = frames * b / bins, s1 = frames * (b + 1) / bins;
        float pl = 0, pm = 0, ph = 0;
        for (int64_t i = s0; i < s1; ++i) {
            float x = mono[i];
            float l = lp.tick(x), h = hp.tick(x), m = x - l - h;
            pl = std::max(pl, std::fabs(l));
            pm = std::max(pm, std::fabs(m));
            ph = std::max(ph, std::fabs(h));
        }
        low[b] = pl; mid[b] = pm; high[b] = ph;
        mx[0] = std::max(mx[0], pl); mx[1] = std::max(mx[1], pm); mx[2] = std::max(mx[2], ph);
    }
    float norm = std::max(mx[0], std::max(mx[1], mx[2]));
    for (int b = 0; b < bins; ++b) { low[b] /= norm; mid[b] /= norm; high[b] /= norm; }
}

// ---- rekordbox grid vs decoded audio --------------------------------------------------------------------------
// Some MP3 / AAC files decode with a different start offset than rekordbox used (encoder / decoder delay,
// ~26 ms on a few percent of files, see pestrela's offset research). That shifts every beat, cue and loop
// relative to what you hear: invisible on the waveform, audible when the drum roller plays on top.
// We measure it from the audio: a 1 kHz low-band onset envelope (kick attacks), and the lag in -60..+60 ms
// that puts the most onset energy on the rekordbox beats. Returns the lag in seconds (positive = the audio
// is later than the grid) and a confidence (peak / median score over all lags).
extern "C" void djc_grid_offset(const float *mono, int64_t frames, double sr, const double *beats, int32_t count,
                                double *lagOut, double *confidenceOut) {
    *lagOut = 0; *confidenceOut = 0;
    if (!mono || frames <= 0 || !beats || count < 16) return;
    const int hop = std::max(1, int(std::lround(sr / 1000.0)));      // 1 ms resolution
    const double fps = sr / hop;
    Biquad lp1, lp2;
    lp1.lowpass(160.0, sr); lp2.lowpass(160.0, sr);                   // kick band
    int64_t nHop = std::min<int64_t>(frames / hop, int64_t(fps * 240)); // first 4 minutes is plenty
    std::vector<float> e(size_t(std::max<int64_t>(nHop, 1)), 0.0f);
    float prev = 0;
    for (int64_t h = 0; h < nHop; ++h) {
        double acc = 0;
        for (int k = 0; k < hop; ++k) { float v = lp2.tick(lp1.tick(mono[h * hop + k])); acc += double(v) * v; }
        float le = float(std::log(1e-10 + acc / hop));
        e[size_t(h)] = std::max(0.0f, le - prev);                     // rising energy = attack
        prev = le;
    }
    const int maxLag = int(0.060 * fps);
    std::vector<double> score(size_t(2 * maxLag + 1), 0.0);
    int used = 0;
    for (int32_t b = 0; b < count && used < 400; ++b) {
        double t = beats[b] * fps;
        if (t - maxLag < 1 || t + maxLag + 2 >= double(nHop)) continue;
        ++used;
        for (int l = -maxLag; l <= maxLag; ++l) {
            // sum over a 3 ms window: the attack spreads over a couple of 1 ms frames
            int64_t i = int64_t(std::lround(t)) + l;
            score[size_t(l + maxLag)] += e[size_t(i)] + 0.5 * (e[size_t(i - 1)] + e[size_t(i + 1)]);
        }
    }
    if (used < 16) return;
    size_t best = 0;
    for (size_t i = 1; i < score.size(); ++i) if (score[i] > score[best]) best = i;
    std::vector<double> sorted = score;
    std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
    double med = sorted[sorted.size() / 2];
    *lagOut = (double(best) - maxLag) / fps - 0.001; // the rising-energy frame lands ~1 ms after the attack starts
    *confidenceOut = med > 0 ? score[best] / med : 0;
}
