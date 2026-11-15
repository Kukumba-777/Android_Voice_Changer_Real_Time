#pragma once

// avcrt_dsp — header-only real-time DSP building blocks (float, dependency-free).
//
// All blocks are: causal, streaming, block-agnostic (process per-sample or per-buffer),
// stateful across calls, O(1)/sample, and allocate their buffers UP FRONT (prepare()) so the
// audio thread never allocates. Used by avcrt_effects.h to build the per-stream effect chain.
//
// Conventions: mono helpers operate on one channel's sample stream; interleaved multi-channel
// effects keep one state instance per channel. Sample values are normalized float (~[-1,1]).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace avcrt::dsp {

inline constexpr float kPi  = 3.14159265358979323846f;
inline constexpr float k2Pi = 6.28318530717958647692f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- one-pole parameter smoother (anti-zipper for slider moves mid-stream) ----
class OnePole {
public:
    void setTarget(float t) { target_ = t; }
    void snap(float v) { target_ = cur_ = v; }
    // coeff in (0,1]; smaller = slower. Derive from a time constant if needed.
    void setCoeff(float c) { coeff_ = c; }
    float next() { cur_ += coeff_ * (target_ - cur_); return cur_; }
    float value() const { return cur_; }
private:
    float cur_ = 0.f, target_ = 0.f, coeff_ = 0.05f;
};

// ---- RBJ biquad (Direct Form I) — shelves, peaks, bandpass, lp/hp ----
class Biquad {
public:
    void reset() { x1_ = x2_ = y1_ = y2_ = 0.f; }
    float process(float x) {
        float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x; y2_ = y1_; y1_ = y;
        return y;
    }
    void setLowShelf(float fs, float f0, float gain_db) {
        float A = std::pow(10.f, gain_db / 40.f);
        float w0 = k2Pi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        float S = 1.f, alpha = sw / 2.f * std::sqrt((A + 1.f / A) * (1.f / S - 1.f) + 2.f);
        float tsa = 2.f * std::sqrt(A) * alpha;
        float b0 = A * ((A + 1.f) - (A - 1.f) * cw + tsa);
        float b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cw);
        float b2 = A * ((A + 1.f) - (A - 1.f) * cw - tsa);
        float a0 = (A + 1.f) + (A - 1.f) * cw + tsa;
        float a1 = -2.f * ((A - 1.f) + (A + 1.f) * cw);
        float a2 = (A + 1.f) + (A - 1.f) * cw - tsa;
        normalize(b0, b1, b2, a0, a1, a2);
    }
    void setHighShelf(float fs, float f0, float gain_db) {
        float A = std::pow(10.f, gain_db / 40.f);
        float w0 = k2Pi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        float S = 1.f, alpha = sw / 2.f * std::sqrt((A + 1.f / A) * (1.f / S - 1.f) + 2.f);
        float tsa = 2.f * std::sqrt(A) * alpha;
        float b0 = A * ((A + 1.f) + (A - 1.f) * cw + tsa);
        float b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cw);
        float b2 = A * ((A + 1.f) + (A - 1.f) * cw - tsa);
        float a0 = (A + 1.f) - (A - 1.f) * cw + tsa;
        float a1 = 2.f * ((A - 1.f) - (A + 1.f) * cw);
        float a2 = (A + 1.f) - (A - 1.f) * cw - tsa;
        normalize(b0, b1, b2, a0, a1, a2);
    }
    void setPeaking(float fs, float f0, float Q, float gain_db) {
        float A = std::pow(10.f, gain_db / 40.f);
        float w0 = k2Pi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.f * Q);
        float b0 = 1.f + alpha * A, b1 = -2.f * cw, b2 = 1.f - alpha * A;
        float a0 = 1.f + alpha / A, a1 = -2.f * cw, a2 = 1.f - alpha / A;
        normalize(b0, b1, b2, a0, a1, a2);
    }
    void setBandpass(float fs, float f0, float Q) {  // constant 0 dB peak (CSG)
        float w0 = k2Pi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.f * Q);
        float b0 = alpha, b1 = 0.f, b2 = -alpha;
        float a0 = 1.f + alpha, a1 = -2.f * cw, a2 = 1.f - alpha;
        normalize(b0, b1, b2, a0, a1, a2);
    }
    void setLowpass(float fs, float f0, float Q) {
        float w0 = k2Pi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.f * Q);
        float b0 = (1.f - cw) / 2.f, b1 = 1.f - cw, b2 = (1.f - cw) / 2.f;
        float a0 = 1.f + alpha, a1 = -2.f * cw, a2 = 1.f - alpha;
        normalize(b0, b1, b2, a0, a1, a2);
    }
    void setHighpass(float fs, float f0, float Q) {
        float w0 = k2Pi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.f * Q);
        float b0 = (1.f + cw) / 2.f, b1 = -(1.f + cw), b2 = (1.f + cw) / 2.f;
        float a0 = 1.f + alpha, a1 = -2.f * cw, a2 = 1.f - alpha;
        normalize(b0, b1, b2, a0, a1, a2);
    }
private:
    void normalize(float b0, float b1, float b2, float a0, float a1, float a2) {
        b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
    }
    float b0_ = 1.f, b1_ = 0.f, b2_ = 0.f, a1_ = 0.f, a2_ = 0.f;
    float x1_ = 0.f, x2_ = 0.f, y1_ = 0.f, y2_ = 0.f;
};

// ---- first-order all-pass (phaser stage), frequency-tunable coefficient ----
// Transfer: y = a*x + x[-1] - a*y[-1]. `a` is derived from a (modulated) center frequency.
inline float allpassCoeff(float fs, float fc) {
    float t = std::tan(kPi * clampf(fc, 20.f, fs * 0.49f) / fs);
    return (t - 1.f) / (t + 1.f);
}
class ModAllpass {
public:
    void reset() { xm1_ = ym1_ = 0.f; }
    void setCoeff(float a) { a_ = a; }
    float process(float x) {
        float y = a_ * x + xm1_ - a_ * ym1_;
        xm1_ = x; ym1_ = y;
        return y;
    }
private:
    float a_ = 0.f, xm1_ = 0.f, ym1_ = 0.f;
};

// ---- LFO (sine / triangle / square) ----
class Lfo {
public:
    void prepare(float fs) { fs_ = fs; phase_ = 0.f; }
    void setRate(float hz) { inc_ = hz / fs_; }
    // shape: 0=sine, 1=triangle, 2=square. Returns unipolar [0,1].
    float nextUni(int shape) {
        float p = phase_;
        phase_ += inc_;
        if (phase_ >= 1.f) phase_ -= 1.f;
        switch (shape) {
            case 1:  return p < 0.5f ? (p * 2.f) : (2.f - p * 2.f);      // triangle
            case 2:  return p < 0.5f ? 1.f : 0.f;                         // square
            default: return 0.5f + 0.5f * std::sin(k2Pi * p);            // sine
        }
    }
    // bipolar [-1,1] sine (for vibrato/ring-mod-ish modulators)
    float nextBiSine() {
        float s = std::sin(k2Pi * phase_);
        phase_ += inc_;
        if (phase_ >= 1.f) phase_ -= 1.f;
        return s;
    }
private:
    float fs_ = 48000.f, phase_ = 0.f, inc_ = 0.f;
};

// ---- phase accumulator for a free-running carrier (ring modulator) ----
class Phasor {
public:
    void prepare(float fs) { fs_ = fs; phase_ = 0.f; }
    void setFreq(float hz) { inc_ = hz / fs_; }
    float nextSine() {
        float s = std::sin(k2Pi * phase_);
        phase_ += inc_;
        if (phase_ >= 1.f) phase_ -= 1.f;
        return s;
    }
private:
    float fs_ = 48000.f, phase_ = 0.f, inc_ = 0.f;
};

// ---- circular delay line with linear-interpolated fractional read (echo / mod delays) ----
class DelayLine {
public:
    void prepare(size_t max_samples) {
        buf_.assign(max_samples > 1 ? max_samples : 1, 0.f);
        w_ = 0;
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.f); w_ = 0; }
    void write(float x) { buf_[w_] = x; if (++w_ >= buf_.size()) w_ = 0; }
    // read `delay` samples back (fractional). delay must be < buffer size.
    float readFrac(float delay) const {
        float rp = static_cast<float>(w_) - delay;
        while (rp < 0.f) rp += static_cast<float>(buf_.size());
        size_t i0 = static_cast<size_t>(rp);
        float frac = rp - static_cast<float>(i0);
        size_t i1 = i0 + 1; if (i1 >= buf_.size()) i1 = 0;
        return buf_[i0] + frac * (buf_[i1] - buf_[i0]);
    }
    size_t capacity() const { return buf_.size(); }
private:
    std::vector<float> buf_;
    size_t w_ = 0;
};

// ---- envelope follower (peak, attack/release) ----
class EnvFollower {
public:
    void prepare(float fs) { fs_ = fs; env_ = 0.f; }
    void setTimes(float attack_ms, float release_ms) {
        atk_ = std::exp(-1.f / (fs_ * (attack_ms  * 0.001f) + 1e-9f));
        rel_ = std::exp(-1.f / (fs_ * (release_ms * 0.001f) + 1e-9f));
    }
    float process(float x) {
        float r = std::fabs(x);
        float c = (r > env_) ? atk_ : rel_;
        env_ = c * (env_ - r) + r;
        return env_;
    }
    float value() const { return env_; }
private:
    float fs_ = 48000.f, env_ = 0.f, atk_ = 0.f, rel_ = 0.f;
};

inline float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
inline float linToDb(float v)  { return 20.f * std::log10(v + 1e-9f); }

// soft saturator (tanh), normalized so unity drive ≈ unity gain at low level.
inline float softClip(float x, float drive) {
    float d = drive < 1.f ? 1.f : drive;
    return std::tanh(d * x) / std::tanh(d * 0.999f + 1e-6f);
}

// ============================ Freeverb (Jezar, public domain) ============================
// Mono reverb bank (8 lowpass-combs + 4 allpass). One bank per channel. Tunings scaled to fs.
class CombFilter {
public:
    void prepare(size_t size) { buf_.assign(size > 1 ? size : 1, 0.f); idx_ = 0; store_ = 0.f; }
    void setFeedback(float f) { feedback_ = f; }
    void setDamp(float d) { damp1_ = d; damp2_ = 1.f - d; }
    float process(float in) {
        float out = buf_[idx_];
        store_ = out * damp2_ + store_ * damp1_;
        buf_[idx_] = in + store_ * feedback_;
        if (++idx_ >= buf_.size()) idx_ = 0;
        return out;
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.f); store_ = 0.f; idx_ = 0; }
private:
    std::vector<float> buf_;
    size_t idx_ = 0;
    float feedback_ = 0.f, damp1_ = 0.f, damp2_ = 1.f, store_ = 0.f;
};

class AllpassFilter {
public:
    void prepare(size_t size) { buf_.assign(size > 1 ? size : 1, 0.f); idx_ = 0; }
    float process(float in) {
        float bufout = buf_[idx_];
        float out = -in + bufout;
        buf_[idx_] = in + bufout * 0.5f;  // fixed allpass feedback
        if (++idx_ >= buf_.size()) idx_ = 0;
        return out;
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.f); idx_ = 0; }
private:
    std::vector<float> buf_;
    size_t idx_ = 0;
};

class Freeverb {
public:
    static constexpr int kNumCombs    = 8;
    static constexpr int kNumAllpass  = 4;
    void prepare(float fs) {
        static const int comb[kNumCombs]   = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int allp[kNumAllpass] = {556, 441, 341, 225};
        float scale = fs / 44100.f;
        for (int i = 0; i < kNumCombs; ++i)
            comb_[i].prepare(static_cast<size_t>(comb[i] * scale) + 1);
        for (int i = 0; i < kNumAllpass; ++i)
            allp_[i].prepare(static_cast<size_t>(allp[i] * scale) + 1);
    }
    // room 0..1, damp 0..1
    void setParams(float room, float damp) {
        float fb = room * 0.28f + 0.7f;
        for (int i = 0; i < kNumCombs; ++i) { comb_[i].setFeedback(fb); comb_[i].setDamp(damp * 0.4f); }
    }
    void clear() { for (auto& c : comb_) c.clear(); for (auto& a : allp_) a.clear(); }
    float process(float in) {
        float input = in * 0.015f;  // fixedgain
        float out = 0.f;
        for (int i = 0; i < kNumCombs; ++i) out += comb_[i].process(input);
        for (int i = 0; i < kNumAllpass; ++i) out = allp_[i].process(out);
        return out;
    }
private:
    CombFilter   comb_[kNumCombs];
    AllpassFilter allp_[kNumAllpass];
};

// ---- xorshift32 white-noise RNG (cheap, no <random> dep; per-stream state) ----
class Rng {
public:
    explicit Rng(uint32_t seed = 0x1234567u) : s_(seed ? seed : 0x1234567u) {}
    // uniform white noise in [-1, 1]
    float next() {
        s_ ^= s_ << 13; s_ ^= s_ >> 17; s_ ^= s_ << 5;
        return (static_cast<float>(s_) * (1.0f / 2147483648.0f)) - 1.0f;
    }
private:
    uint32_t s_;
};

// ---- carrier oscillator for the vocoder (saw / pulse), phase-accumulator ----
class Osc {
public:
    void prepare(float fs) { fs_ = fs; phase_ = 0.f; }
    void setFreq(float hz) { inc_ = hz / fs_; }
    // type: 0=saw, 1=pulse(square); (noise is handled by Rng at the call site)
    float next(int type) {
        float p = phase_;
        phase_ += inc_;
        if (phase_ >= 1.f) phase_ -= 1.f;
        return (type == 1) ? (p < 0.5f ? 1.f : -1.f) : (2.f * p - 1.f);
    }
private:
    float fs_ = 48000.f, phase_ = 0.f, inc_ = 0.f;
};

// ============================ Channel vocoder filter bank ============================
// ONE primitive powering 3 Tier-3 effects (formant shift / vocoder / whisper). A bank of band-pass
// "analysis" filters measures the per-band energy (the spectral envelope = formants) of the input;
// a parallel "synthesis" bank re-imposes that envelope onto a chosen EXCITATION:
//   - formant shift: excitation = the (pre-emphasis-whitened) input, synth bands placed at cf*shift
//                    → formants move by `shift`, the excitation's pitch is preserved.
//   - vocoder:       excitation = an internal carrier (saw/pulse/noise), synth bands at cf
//                    → classic talking-robot.
//   - whisper:       excitation = white noise, synth bands at cf → voiced→unvoiced breath.
// Pure biquads + envelope followers → causal, per-sample, ZERO added latency, NO FFT. One bank per
// channel per effect (state is not shared). Bands above fs*0.45 are clamped (low-rate call streams).
class VocoderBank {
public:
    static constexpr int kBands = 24;

    void prepare(float fs) {
        fs_ = fs;
        for (int i = 0; i < kBands; ++i) {
            float t = (kBands > 1) ? static_cast<float>(i) / (kBands - 1) : 0.f;
            cf_[i] = 150.f * std::pow(7000.f / 150.f, t);   // log-spaced 150..7000 Hz
            ana_[i].setBandpass(fs_, clampCf(cf_[i]), kQ);
            syn_[i].setBandpass(fs_, clampCf(cf_[i]), kQ);
            env_[i].prepare(fs_);
            env_[i].setTimes(5.f, 15.f);                    // voice-rate envelope
        }
        inEnv_ = 0.f;
        outEnv_ = 0.f;
    }

    // Re-place the SYNTHESIS bands at cf*shift (formant shift). shift==1 → identity. Cheap enough to
    // call on a config change (not per sample).
    void setSynthShift(float shift) {
        for (int i = 0; i < kBands; ++i) syn_[i].setBandpass(fs_, clampCf(cf_[i] * shift), kQ);
    }

    // One sample: update per-band envelopes from `in`, re-synthesise `excitation` through the synth
    // bank gained by those envelopes, AUTO-GAINED to the input's level.
    // The summed band gain depends on the signal/effect (a hand-tuned makeup was unreliable — formant
    // came out ~35 dB too quiet, vocoder/whisper uneven). Instead we track the input vs output level
    // (one-pole on |x|, ~10 ms) and scale the output to match the input → stable, audible level for
    // all three effects on any material; the engine's soft limiter still backs us up.
    float process(float in, float excitation) {
        float out = 0.f;
        for (int i = 0; i < kBands; ++i) {
            float e = env_[i].process(ana_[i].process(in));
            out += syn_[i].process(excitation) * e;
        }
        inEnv_  += kGainTrack * (std::fabs(in)  - inEnv_);
        outEnv_ += kGainTrack * (std::fabs(out) - outEnv_);
        float g = (outEnv_ > 1e-5f) ? (inEnv_ / outEnv_) : 1.f;
        return out * clampf(g, 0.f, 8.f);
    }

private:
    float clampCf(float cf) const {
        float hi = fs_ * 0.45f;
        return cf < 40.f ? 40.f : (cf > hi ? hi : cf);
    }
    static constexpr float kQ = 5.f;
    static constexpr float kGainTrack = 0.002f;   // ~10 ms level-match follower
    float fs_ = 48000.f;
    float inEnv_ = 0.f, outEnv_ = 0.f;
    float cf_[kBands] = {0};
    Biquad ana_[kBands], syn_[kBands];
    EnvFollower env_[kBands];
};

// ============================ FFT + streaming STFT (Phase 7, experimental) ============================
// Hand-written radix-2 iterative Cooley-Tukey FFT (no external dep). Powers the experimental spectral
// effects. NOTE: STFT adds latency = window size → only used behind the "experimental" toggle.
class Fft {
public:
    void prepare(int n) {
        n_ = n; levels_ = 0; for (int t = n; t > 1; t >>= 1) ++levels_;
        rev_.resize(n);
        for (int i = 0; i < n; ++i) { int r = 0, x = i; for (int b = 0; b < levels_; ++b) { r = (r << 1) | (x & 1); x >>= 1; } rev_[i] = r; }
        cos_.resize(n / 2); sin_.resize(n / 2);
        for (int i = 0; i < n / 2; ++i) { cos_[i] = std::cos(-k2Pi * i / n); sin_[i] = std::sin(-k2Pi * i / n); }
    }
    // in-place complex FFT (re[],im[] length n). inverse=true → IFFT (1/n scaled).
    void transform(float* re, float* im, bool inverse) const {
        const int n = n_;
        for (int i = 0; i < n; ++i) { int j = rev_[i]; if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); } }
        for (int size = 2; size <= n; size <<= 1) {
            const int half = size >> 1, step = n / size;
            for (int i = 0; i < n; i += size) {
                for (int j = i, k = 0; j < i + half; ++j, k += step) {
                    const float c = cos_[k], s = inverse ? -sin_[k] : sin_[k];
                    const float tre = re[j + half] * c - im[j + half] * s;
                    const float tim = re[j + half] * s + im[j + half] * c;
                    re[j + half] = re[j] - tre; im[j + half] = im[j] - tim;
                    re[j] += tre;              im[j] += tim;
                }
            }
        }
        if (inverse) { const float inv = 1.f / n; for (int i = 0; i < n; ++i) { re[i] *= inv; im[i] *= inv; } }
    }
    int size() const { return n_; }
private:
    int n_ = 0, levels_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

// Streaming short-time Fourier transform with Hann window + 75% overlap-add. Per sample: tick(in)
// returns one output sample delayed by the window size. Every `hop` samples it windows the last N
// input samples, FFTs, calls the subclass-free `frameOp` (set via a function pointer / lambda stored
// by the owner — here we expose the spectral buffers and let the EffectChain fill applyFrame), IFFTs,
// windows again and overlap-adds. COLA-normalised so an identity frameOp reconstructs the input.
class StftProcessor {
public:
    void prepare(int n, float fs) {
        n_ = n; hop_ = n / 4; fs_ = fs;
        fft_.prepare(n);
        win_.resize(n);
        for (int i = 0; i < n; ++i) win_[i] = 0.5f - 0.5f * std::cos(k2Pi * i / (n - 1));  // Hann
        // COLA gain for analysis*synthesis Hann at hop=N/4: sum of squared windows over the overlaps.
        float acc = 0.f; for (int m = 0; m * hop_ < n; ++m) { int idx = m * hop_; acc += win_[idx] * win_[idx]; }
        colaScale_ = acc > 1e-6f ? 1.f / acc : 1.f;
        inRing_.assign(n, 0.f); outRing_.assign(n, 0.f);
        re_.assign(n, 0.f); im_.assign(n, 0.f);
        inIdx_ = 0; outIdx_ = 0; hopCount_ = 0; frozen_ = false;
        freezeMag_.assign(n, 0.f); blurMag_.assign(n, 0.f);
        // phase-vocoder (auto-tune / harmonizer) scratch + per-bin phase state
        mag_.assign(n, 0.f); tf_.assign(n, 0.f);
        lastPhase_.assign(n, 0.f); sumPhase_.assign(n, 0.f);
        sumPhaseH1_.assign(n, 0.f); sumPhaseH2_.assign(n, 0.f);
        outMag_.assign(n, 0.f); outFreq_.assign(n, 0.f);
        accRe_.assign(n, 0.f); accIm_.assign(n, 0.f);
        dryRe_.assign(n, 0.f); dryIm_.assign(n, 0.f);
    }
    void clear() {
        std::fill(inRing_.begin(), inRing_.end(), 0.f);
        std::fill(outRing_.begin(), outRing_.end(), 0.f);
        std::fill(blurMag_.begin(), blurMag_.end(), 0.f);
        inIdx_ = outIdx_ = hopCount_ = 0; frozen_ = false;
    }
    // Spectral params (set by the owner each config change). amounts 0..1.
    struct Params {
        bool robot=false, whisper=false, freeze=false, blur=false; float blurAmt=0.5f, specMix=1.f;
        bool autotune=false;  float atStrength=1.f;          // 0..1 correction depth
        bool harmon=false;    int hInt1=4, hInt2=7; float hMix=0.5f;  // semitone intervals + voice mix
    };
    void setParams(const Params& p) { p_ = p; if (!p.freeze) frozen_ = false; }
    bool active() const { return p_.robot || p_.whisper || p_.freeze || p_.blur || p_.autotune || p_.harmon; }

    float tick(float in) {
        inRing_[inIdx_] = in;
        inIdx_ = (inIdx_ + 1) % n_;
        float out = outRing_[outIdx_];
        outRing_[outIdx_] = 0.f;
        outIdx_ = (outIdx_ + 1) % n_;
        if (++hopCount_ >= hop_) { hopCount_ = 0; doFrame(); }
        // auto-gain to the input level (robot/freeze come out very quiet otherwise; ~10 ms follower,
        // delayed by the window but the slow follower smooths that; identity passthrough → g≈1).
        inEnv_  += 0.002f * (std::fabs(in)  - inEnv_);
        outEnv_ += 0.002f * (std::fabs(out) - outEnv_);
        float g = (outEnv_ > 1e-5f) ? clampf(inEnv_ / outEnv_, 0.f, 8.f) : 1.f;
        return out * g;
    }
private:
    void doFrame() {
        const int n = n_;
        for (int k = 0; k < n; ++k) { re_[k] = inRing_[(inIdx_ + k) % n] * win_[k]; im_[k] = 0.f; }
        fft_.transform(re_.data(), im_.data(), false);
        applyFrame();
        fft_.transform(re_.data(), im_.data(), true);
        // overlap-add the windowed IFFT into the output ring, aligned to the current read pointer.
        for (int k = 0; k < n; ++k) outRing_[(outIdx_ + k) % n] += re_[k] * win_[k] * colaScale_;
    }
    void applyFrame() {
        const int n = n_, half = n / 2;
        // Phase-vocoder pitch effects FIRST (they rewrite the spectrum); spectral mods then layer on.
        if (p_.autotune) { analyze(); pvAutotune(); }
        else if (p_.harmon) { analyze(); pvHarmonize(); }
        // wet/dry blend for the spectral ops: snapshot the (pre-spectral) spectrum so specMix can
        // dial their strength back toward the input.
        const bool specMod = p_.robot || p_.whisper || p_.freeze || p_.blur;
        const bool doBlend = specMod && p_.specMix < 0.999f;
        if (doBlend) { for (int k = 0; k < n; ++k) { dryRe_[k] = re_[k]; dryIm_[k] = im_[k]; } }
        if (p_.blur) {  // temporal smear of magnitudes (phase kept)
            const float a = 0.02f + 0.18f * (1.f - p_.blurAmt);  // higher amt → slower follow → more smear
            for (int k = 0; k < n; ++k) {
                float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
                blurMag_[k] += a * (mag - blurMag_[k]);
                float ph = std::atan2(im_[k], re_[k]);
                re_[k] = blurMag_[k] * std::cos(ph); im_[k] = blurMag_[k] * std::sin(ph);
            }
        }
        if (p_.freeze) {  // hold the first magnitude frame, let phase run
            if (!frozen_) { for (int k = 0; k < n; ++k) freezeMag_[k] = std::sqrt(re_[k]*re_[k]+im_[k]*im_[k]); frozen_ = true; }
            for (int k = 0; k < n; ++k) { float ph = std::atan2(im_[k], re_[k]); re_[k]=freezeMag_[k]*std::cos(ph); im_[k]=freezeMag_[k]*std::sin(ph); }
        }
        if (p_.robot) {  // zero phase → monotone robot (keep DC..Nyquist real, mirror)
            for (int k = 0; k <= half; ++k) { float mag = std::sqrt(re_[k]*re_[k]+im_[k]*im_[k]); re_[k]=mag; im_[k]=0.f; if (k>0 && k<half){ re_[n-k]=mag; im_[n-k]=0.f; } }
        }
        if (p_.whisper) {  // randomise phase → breath
            for (int k = 1; k < half; ++k) { float mag = std::sqrt(re_[k]*re_[k]+im_[k]*im_[k]); float ph = k2Pi * rng_.next() * 0.5f; float c=std::cos(ph), s=std::sin(ph); re_[k]=mag*c; im_[k]=mag*s; re_[n-k]=mag*c; im_[n-k]=-mag*s; }
        }
        if (doBlend) { float m = p_.specMix; for (int k = 0; k < n; ++k) { re_[k] = dryRe_[k]*(1.f-m) + re_[k]*m; im_[k] = dryIm_[k]*(1.f-m) + im_[k]*m; } }
    }

    // ---- phase-vocoder (auto-tune / harmonizer) ----
    // analysis: per bin magnitude + true (instantaneous) frequency in bin units, via phase difference.
    void analyze() {
        const int n = n_, half = n / 2;
        const float expF = k2Pi * hop_ / n;
        for (int k = 0; k <= half; ++k) {
            mag_[k] = std::sqrt(re_[k]*re_[k] + im_[k]*im_[k]);
            float phase = std::atan2(im_[k], re_[k]);
            float dev = (phase - lastPhase_[k]) - expF * k;
            lastPhase_[k] = phase;
            dev -= k2Pi * std::round(dev / k2Pi);          // wrap to [-π,π]
            tf_[k] = k + dev / expF;                        // true bin frequency
        }
    }
    // shift the analysed spectrum by ratio r into re_/im_ (replace), accumulating synthesis phase in sp.
    void pvShiftReplace(float r, std::vector<float>& sp) {
        const int n = n_, half = n / 2;
        std::fill(outMag_.begin(), outMag_.end(), 0.f);
        std::fill(outFreq_.begin(), outFreq_.end(), 0.f);
        for (int k = 0; k <= half; ++k) { int j = static_cast<int>(std::lround(k * r)); if (j >= 0 && j <= half) { outMag_[j] += mag_[k]; outFreq_[j] = tf_[k] * r; } }
        const float synF = k2Pi * hop_ / n;
        for (int k = 0; k < n; ++k) { re_[k] = 0.f; im_[k] = 0.f; }
        for (int j = 0; j <= half; ++j) {
            sp[j] += synF * outFreq_[j];
            float c = std::cos(sp[j]), s = std::sin(sp[j]);
            re_[j] = outMag_[j] * c; im_[j] = outMag_[j] * s;
            if (j > 0 && j < half) { re_[n-j] = re_[j]; im_[n-j] = -im_[j]; }
        }
    }
    // add a pitch-shifted voice (ratio r) into the acc spectrum at `gain`, accumulating phase in sp.
    void addVoice(float r, std::vector<float>& sp, float gain) {
        const int n = n_, half = n / 2;
        std::fill(outMag_.begin(), outMag_.end(), 0.f);
        std::fill(outFreq_.begin(), outFreq_.end(), 0.f);
        for (int k = 0; k <= half; ++k) { int j = static_cast<int>(std::lround(k * r)); if (j >= 0 && j <= half) { outMag_[j] += mag_[k]; outFreq_[j] = tf_[k] * r; } }
        const float synF = k2Pi * hop_ / n;
        for (int j = 0; j <= half; ++j) {
            sp[j] += synF * outFreq_[j];
            float m = outMag_[j] * gain, c = std::cos(sp[j]), s = std::sin(sp[j]);
            accRe_[j] += m * c; accIm_[j] += m * s;
            if (j > 0 && j < half) { accRe_[n-j] += m * c; accIm_[n-j] += -m * s; }
        }
    }
    void pvAutotune() {
        // The autocorrelation pitch detector is the costliest op; pitch changes slowly, so only
        // re-detect every few frames and reuse the cached correction ratio in between.
        if (--atCounter_ <= 0) {
            float f0 = detectPitch();
            atR_ = 1.f;
            if (f0 > 0.f) { float tgt = nearestNote(f0); float r = tgt / f0; atR_ = 1.f + p_.atStrength * (r - 1.f); }
            atCounter_ = 4;
        }
        pvShiftReplace(atR_, sumPhase_);
    }
    void pvHarmonize() {
        const int n = n_;
        for (int k = 0; k < n; ++k) { accRe_[k] = re_[k]; accIm_[k] = im_[k]; }   // dry
        addVoice(std::pow(2.f, p_.hInt1 / 12.f), sumPhaseH1_, p_.hMix);
        addVoice(std::pow(2.f, p_.hInt2 / 12.f), sumPhaseH2_, p_.hMix);
        for (int k = 0; k < n; ++k) { re_[k] = accRe_[k]; im_[k] = accIm_[k]; }
    }
    // autocorrelation pitch detector on the time-domain ring (70..500 Hz voice range).
    float detectPitch() {
        const int n = n_;
        int minLag = static_cast<int>(fs_ / 500.f), maxLag = static_cast<int>(fs_ / 70.f);
        if (maxLag >= n) maxLag = n - 1;
        float best = 0.f; int bestLag = 0;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            float s = 0.f, e0 = 0.f, e1 = 0.f;
            for (int i = 0; i + lag < n; ++i) {
                float a = inRing_[(inIdx_ + i) % n], b = inRing_[(inIdx_ + i + lag) % n];
                s += a * b; e0 += a * a; e1 += b * b;
            }
            float nc = s / (std::sqrt(e0 * e1) + 1e-9f);
            if (nc > best) { best = nc; bestLag = lag; }
        }
        return (best < 0.5f || bestLag == 0) ? 0.f : fs_ / bestLag;
    }
    static float nearestNote(float f) {   // snap to nearest 12-TET semitone (A4=440)
        float midi = 69.f + 12.f * std::log2(f / 440.f);
        return 440.f * std::pow(2.f, (std::round(midi) - 69.f) / 12.f);
    }
    Fft fft_;
    int n_ = 0, hop_ = 0, inIdx_ = 0, outIdx_ = 0, hopCount_ = 0;
    float fs_ = 48000.f;
    float colaScale_ = 1.f;
    float inEnv_ = 0.f, outEnv_ = 0.f;
    int   atCounter_ = 0;     // auto-tune re-detect throttle
    float atR_ = 1.f;         // cached auto-tune correction ratio
    bool frozen_ = false;
    Params p_;
    Rng rng_;
    std::vector<float> win_, inRing_, outRing_, re_, im_, freezeMag_, blurMag_;
    // phase-vocoder scratch + per-bin phase state (auto-tune / harmonizer)
    std::vector<float> mag_, tf_, lastPhase_, sumPhase_, sumPhaseH1_, sumPhaseH2_,
                       outMag_, outFreq_, accRe_, accIm_, dryRe_, dryIm_;
};

}  // namespace avcrt::dsp
