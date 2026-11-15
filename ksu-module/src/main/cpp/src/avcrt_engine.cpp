#include "avcrt_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <SoundTouch.h>

#include "avcrt_log.h"

namespace avcrt {

namespace {
constexpr float  kInt16Scale = 32767.0f;
constexpr size_t kInitialScratchFrames = 4096;
}  // namespace

Engine::Engine(uint32_t sample_rate, uint32_t channels)
    : sample_rate_(sample_rate),
      channels_(channels),
      st_(std::make_unique<soundtouch::SoundTouch>()) {

    scratch_in_.reserve(kInitialScratchFrames * channels);
    scratch_out_.reserve(kInitialScratchFrames * channels);
    out_.reserve(kInitialScratchFrames * channels);
    // Allocate effect buffers ONCE (delay lines, reverb banks). The Engine is recreated when the
    // stream's rate/channels change, so this is the right (and only) place to allocate.
    chain_.prepare(sample_rate_, channels_);
    rebuild();
}

Engine::~Engine() = default;

void Engine::update(const DspConfig& cfg) {
    if (cfg.revision == applied_revision_) return;
    active_cfg_ = cfg;
    applied_revision_ = cfg.revision;
    rebuild();
}

void Engine::rebuild() {
    st_->clear();
    fifo_.clear();
    st_->setSampleRate(sample_rate_);
    st_->setChannels(channels_);

    // SoundTouch has no real formant separation — when the user asks for it,
    // we bias the pitch slightly toward zero so a heavy shift sounds less
    // chipmunk-y. Not a substitute for a proper phase vocoder; see header.
    double effective_pitch = active_cfg_.pitch_semitones;
    if (active_cfg_.preserve_formants && std::abs(effective_pitch) > 1.0) {
        const double formant_bias = active_cfg_.formant_semitones * 0.5;
        effective_pitch = effective_pitch - formant_bias;
    }

    st_->setPitchSemiTones(static_cast<float>(effective_pitch));
    st_->setTempo(static_cast<float>(active_cfg_.time_ratio));
    st_->setRate(1.0f);

    // crispness (0..6) maps SoundTouch's window sizes: lower = smoother, higher = crisper (sharper
    // transients). Index 5 == the previous fixed 28/12/8, so every existing preset (default crispness
    // 5) is byte-identical = zero regression. All rows stay well above the 15/7/5 that underran the
    // continuous cellular feed (choppy), so even crispness 6 is safe on the GSM path.
    static const int kSeq[7]  = {50, 44, 40, 34, 30, 28, 24};
    static const int kSeek[7] = {22, 19, 17, 15, 13, 12, 11};
    static const int kOvl[7]  = {10,  9,  9,  8,  8,  8,  7};
    int ci = active_cfg_.crispness;
    if (ci < 0) ci = 0; else if (ci > 6) ci = 6;
    st_->setSetting(SETTING_USE_QUICKSEEK, 1);
    st_->setSetting(SETTING_USE_AA_FILTER, 1);
    st_->setSetting(SETTING_SEQUENCE_MS,   kSeq[ci]);
    st_->setSetting(SETTING_SEEKWINDOW_MS, kSeek[ci]);
    st_->setSetting(SETTING_OVERLAP_MS,    kOvl[ci]);

    // Re-apply effect params (cheap; no realloc, keeps reverb/echo tails). prepare() already ran
    // in the ctor.
    chain_.updateParams(active_cfg_);

    log::i("engine rebuilt: pitch=%.2f (effective=%.2f) formant=%.2f preserveF=%d "
           "fx[gate=%d eq=%d tel=%d ring=%d trem=%d dist=%d echo=%d verb=%d]",
           active_cfg_.pitch_semitones, effective_pitch,
           active_cfg_.formant_semitones, active_cfg_.preserve_formants,
           active_cfg_.gate.enabled, active_cfg_.eq.enabled, active_cfg_.telephone.enabled,
           active_cfg_.ringmod.enabled, active_cfg_.tremolo.enabled, active_cfg_.distortion.enabled,
           active_cfg_.echo.enabled, active_cfg_.reverb.enabled);
}

bool Engine::process(int16_t* samples, size_t frames) {
    if (!active_cfg_.enabled || !st_) return false;
    if (frames == 0) return true;

    const size_t total = frames * channels_;

    if (scratch_in_.size() < total) scratch_in_.resize(total);
    if (out_.size() < total) out_.resize(total);

    // ---- int16 -> float (interleaved) ----
    for (size_t i = 0; i < total; ++i) {
        scratch_in_[i] = samples[i] / kInt16Scale;
    }

    // ---- PRE-pitch effects (gate / EQ / telephone), on the input. No-op if none enabled. ----
    chain_.processPre(scratch_in_.data(), frames);

    // ---- feed SoundTouch, drain ALL it has into the output FIFO ----
    st_->putSamples(scratch_in_.data(), static_cast<unsigned int>(frames));

    const unsigned int chunk = 1024;
    if (scratch_out_.size() < chunk * channels_) scratch_out_.resize(chunk * channels_);
    unsigned int got;
    do {
        got = st_->receiveSamples(scratch_out_.data(), chunk);
        if (got > 0)
            fifo_.insert(fifo_.end(), scratch_out_.data(),
                         scratch_out_.data() + (size_t)got * channels_);
    } while (got == chunk);

    // ---- build the output buffer of `total` samples from the FIFO ----
    // If the FIFO has a full period, take it; if not (warm-up / transient underrun) emit silence
    // for THIS period — no "prefill" latch (that starved the continuous cellular feed). We STILL
    // run post-effects on the silence so time-based tails (echo/reverb) keep ringing.
    if (fifo_.size() >= total) {
        std::memcpy(out_.data(), fifo_.data(), total * sizeof(float));
        fifo_.erase(fifo_.begin(), fifo_.begin() + total);
    } else {
        std::memset(out_.data(), 0, total * sizeof(float));
    }

    // ---- POST-pitch effects (ring-mod / tremolo / distortion / echo / reverb). No-op if none. ----
    chain_.processPost(out_.data(), frames);

    // ---- makeup gain + soft-knee limiter + float -> int16 ----
    // Transparent below ±0.95 (linear → clean pitch is unchanged), then tanh-rounded into a hard
    // ±1.0 ceiling. Avoids the harsh hard-clip when effects (reverb/echo wet added on top of dry,
    // distortion, compressor makeup, gain) push peaks past 0 dBFS — that clip was a big source of the
    // "strange/raspy" sound on the effect-heavy presets. Asymptotic to 1.0 so int16 never overflows.
    const float gain = std::pow(10.0f, static_cast<float>(active_cfg_.gain_db) / 20.0f);
    for (size_t i = 0; i < total; ++i) {
        float v = out_[i] * gain;
        if (v > 0.95f)       v =  0.95f + 0.05f * std::tanh((v - 0.95f) * 20.0f);
        else if (v < -0.95f) v = -0.95f + 0.05f * std::tanh((v + 0.95f) * 20.0f);
        samples[i] = static_cast<int16_t>(v * kInt16Scale);
    }
    return true;
}

}  // namespace avcrt
