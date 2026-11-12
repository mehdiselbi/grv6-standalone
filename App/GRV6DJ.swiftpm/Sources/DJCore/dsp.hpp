// Small DSP building blocks. Header-only, allocation-free.
#pragma once
#include <cmath>
#include <algorithm>

namespace djc {

static constexpr double kPi = 3.14159265358979323846;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// RBJ biquad, transposed direct form II, one channel.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    void lowpass(double fc, double sr, double q = 0.70710678) {
        double w = 2 * kPi * fc / sr, c = std::cos(w), s = std::sin(w), al = s / (2 * q), a0 = 1 + al;
        b0 = float((1 - c) / 2 / a0); b1 = float((1 - c) / a0); b2 = b0;
        a1 = float(-2 * c / a0); a2 = float((1 - al) / a0);
    }
    void highpass(double fc, double sr, double q = 0.70710678) {
        double w = 2 * kPi * fc / sr, c = std::cos(w), s = std::sin(w), al = s / (2 * q), a0 = 1 + al;
        b0 = float((1 + c) / 2 / a0); b1 = float(-(1 + c) / a0); b2 = b0;
        a1 = float(-2 * c / a0); a2 = float((1 - al) / a0);
    }
    // RBJ cookbook shelves / peak (gain in dB)
    void lowShelf(double fc, double sr, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2 * kPi * fc / sr, c = std::cos(w), s = std::sin(w);
        double al = s / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
        double a0 = (A + 1) + (A - 1) * c + sa;
        b0 = float(A * ((A + 1) - (A - 1) * c + sa) / a0); b1 = float(2 * A * ((A - 1) - (A + 1) * c) / a0);
        b2 = float(A * ((A + 1) - (A - 1) * c - sa) / a0);
        a1 = float(-2 * ((A - 1) + (A + 1) * c) / a0); a2 = float(((A + 1) + (A - 1) * c - sa) / a0);
    }
    void highShelf(double fc, double sr, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2 * kPi * fc / sr, c = std::cos(w), s = std::sin(w);
        double al = s / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
        double a0 = (A + 1) - (A - 1) * c + sa;
        b0 = float(A * ((A + 1) + (A - 1) * c + sa) / a0); b1 = float(-2 * A * ((A - 1) + (A + 1) * c) / a0);
        b2 = float(A * ((A + 1) + (A - 1) * c - sa) / a0);
        a1 = float(2 * ((A - 1) - (A + 1) * c) / a0); a2 = float(((A + 1) - (A - 1) * c - sa) / a0);
    }
    void peak(double fc, double sr, double q, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2 * kPi * fc / sr, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
        double a0 = 1 + al / A;
        b0 = float((1 + al * A) / a0); b1 = float(-2 * c / a0); b2 = float((1 - al * A) / a0);
        a1 = float(-2 * c / a0); a2 = float((1 - al / A) / a0);
    }
    inline float tick(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// Linkwitz-Riley 4th order = two cascaded Butterworth 2nd order sections.
struct LR4 {
    Biquad s1, s2;
    void lowpass(double fc, double sr) { s1.lowpass(fc, sr); s2.lowpass(fc, sr); }
    void highpass(double fc, double sr) { s1.highpass(fc, sr); s2.highpass(fc, sr); }
    inline float tick(float x) { return s2.tick(s1.tick(x)); }
    void reset() { s1.reset(); s2.reset(); }
};

// 3-band isolator. Bands sum to an allpass (flat magnitude) at unity gains.
struct Isolator {
    LR4 lpLo, hpLo, lpHi, hpHi, apLp, apHp; // apLp+apHp = allpass matching the hi crossover on the low path
    void setup(double sr, double fLo = 250.0, double fHi = 3000.0) {
        lpLo.lowpass(fLo, sr); hpLo.highpass(fLo, sr);
        lpHi.lowpass(fHi, sr); hpHi.highpass(fHi, sr);
        apLp.lowpass(fHi, sr); apHp.highpass(fHi, sr);
    }
    inline float tick(float x, float gLow, float gMid, float gHigh) {
        float lo = lpLo.tick(x);
        lo = apLp.tick(lo) + apHp.tick(lo);
        float rest = hpLo.tick(x);
        float mid = lpHi.tick(rest);
        float hi = hpHi.tick(rest);
        return gLow * lo + gMid * mid + gHigh * hi;
    }
};

// Cytomic TPT state variable filter, one channel.
struct Svf {
    float ic1 = 0, ic2 = 0;
    inline void tick(float v0, float a1, float a2, float a3, float &low, float &band) {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        low = v2;
        band = v1;
    }
};

struct SvfCoeffs {
    float a1 = 1, a2 = 0, a3 = 0, k = 1.4f;
    void set(double fc, double sr, double q) {
        double g = std::tan(kPi * std::min(fc, sr * 0.49) / sr);
        k = float(1.0 / q);
        a1 = float(1.0 / (1.0 + g * (g + k)));
        a2 = float(g * a1);
        a3 = float(g * a2);
    }
};

// 4-point cubic Hermite interpolation.
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

} // namespace djc
