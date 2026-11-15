#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "avcrt_config.h"
#include "avcrt_effects.h"

namespace soundtouch {
class SoundTouch;
}

namespace avcrt {

/**
 * Thin DSP front-end on top of SoundTouch.
 *
 * One instance per AudioRecord — owned by EffectContext in avcrt_effect.cpp.
 * Knows only about int16 PCM frames in the system audio format, nothing about
 * the Android Audio Effect ABI.
 *
 * NOTE on formant preservation:
 *   SoundTouch shifts pitch by time-stretching + resampling, which natively
 *   moves formants with pitch (the classic “chipmunk” effect at strong
 *   shifts). `DspConfig.preserve_formants` is honored only as a best-effort
 *   hint here — true formant separation needs Rubber Band / phase-vocoder
 *   code that the MVP intentionally doesn’t pull in. See README for the
 *   upgrade path.
 */
class Engine {
public:
    Engine(uint32_t sample_rate, uint32_t channels);
    ~Engine();

    /** Re-applies cfg to the underlying SoundTouch if it changed since the
     *  last call. Cheap to call every process(). */
    void update(const DspConfig& cfg);

    /** Processes a frame of S16LE PCM in-place. Returns false if the engine
     *  is disabled / unconfigured (caller should pass through). */
    bool process(int16_t* samples, size_t frames);

private:
    void rebuild();

    uint32_t  sample_rate_;
    uint32_t  channels_;
    DspConfig active_cfg_;
    int64_t   applied_revision_{-1};

    std::unique_ptr<soundtouch::SoundTouch> st_;

    // Phase 6 effect chain (gate/EQ/telephone pre-pitch; ring-mod/tremolo/distortion/echo/reverb
    // post-pitch). Allocated once per Engine (Engine is recreated on rate/channel change); params
    // re-applied on each revision via updateParams (no realloc, keeps reverb/echo tails alive).
    EffectChain chain_;

    // Pre-allocated float scratch — SoundTouch wants float in/out and we
    // receive int16 from the audio HAL, so we convert each buffer.
    std::vector<float> scratch_in_;
    std::vector<float> scratch_out_;
    std::vector<float> out_;   // post-pitch output buffer (drained from fifo_, then post-effects)

    // Output FIFO (interleaved float). SoundTouch has pipeline latency and does
    // NOT return the same frame count it was fed; we buffer its output here and
    // emit `frames` per call. If a period isn't ready yet (warm-up or a transient
    // underrun) we emit silence for that period only — no prefill latch (which
    // starved the continuous cellular feed into near-constant silence).
    std::vector<float> fifo_;
};

}  // namespace avcrt
