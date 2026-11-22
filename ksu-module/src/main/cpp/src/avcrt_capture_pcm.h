#pragma once

#include "avcrt_engine.h"

#include <array>
#include <cstring>

namespace avcrt {

// Owned by a record track; only its producer thread may access the processor.
// Epochs are supplied by the framework at buffer acquisition and publication.
class CapturePcm {
public:
    static bool supports(uint32_t rate, uint32_t channels) {
        return rate >= 8000 && rate <= 192000 && (channels == 1 || channels == 2);
    }

    void bypass() noexcept {
        engine_.reset();
    }

    template <class StillOwned, class Commit>
    bool process(void* raw, size_t frames, uint32_t rate, uint32_t channels,
                 uint64_t epoch, uint32_t route, uint64_t micEpoch, const DspConfig& config,
                 StillOwned&& stillOwned, Commit&& commit) noexcept {
        if (!config.enabled || !raw || !frames || frames > kMaxFrames || !epoch || !route || !micEpoch ||
                !supports(rate, channels) || !stillOwned()) {
            bypass();
            return false;
        }
        if (epoch != epoch_ || route != route_ || micEpoch != mic_epoch_ ||
                rate != rate_ || channels != channels_ ||
                config.revision != revision_) {
            bypass();
            epoch_ = epoch;
            route_ = route;
            mic_epoch_ = micEpoch;
            rate_ = rate;
            channels_ = channels;
            revision_ = config.revision;
            failed_ = false;
        }
        if (failed_) return false;

        const size_t bytes = frames * channels * sizeof(int16_t);
        std::memcpy(scratch_.data(), raw, bytes);
        try {
            if (!engine_) engine_ = std::make_unique<Engine>(rate, channels);
            engine_->update(config);
            if (!engine_->process(scratch_.data(), frames) || !commit(scratch_.data(), bytes)) {
                bypass();
                return false;
            }
        } catch (...) {
            bypass();
            failed_ = true;
            return false;
        }
        return true;
    }

private:
    static constexpr size_t kMaxFrames = 4096;
    uint64_t epoch_ = 0, mic_epoch_ = 0;
    uint32_t route_ = 0, rate_ = 0, channels_ = 0;
    int64_t revision_ = -1;
    bool failed_ = false;
    std::array<int16_t, kMaxFrames * 2> scratch_;
    std::unique_ptr<Engine> engine_;
};

}  // namespace avcrt
