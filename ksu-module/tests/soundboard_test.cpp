#include "avcrt_soundboard.h"

#include <array>
#include <cassert>
#include <cmath>

using namespace avcrt::soundboard;

int main() {
    Cursor cursor;
    Window clip{{100, kRate / 10}, 0, std::vector<int16_t>(kRate / 10, 1000)};
    clip.pcm[0] = 2000;
    clip.pcm[3] = 3000;
    clip.pcm[6] = -1000;

    std::array<float, 3> output{};
    const auto add = [&](size_t frame, float value) { output[frame] += value; };

    // Before the clip and after it expires, the mic buffer must be untouched.
    output.fill(42);
    mix(clip, cursor, 90, output.size(), 16000, add);
    for (auto sample : output) assert(sample == 42);

    // 48 kHz clip into the 16 kHz cellular path: every third source frame.
    mix(clip, cursor, 100, output.size(), 16000, add);
    assert(output[0] == 42 + 2000 * kClipGain);
    assert(output[1] == 42 + 3000 * kClipGain);
    assert(output[2] == 42 - 1000 * kClipGain);

    output.fill(42);
    mix(clip, cursor, 200, output.size(), 16000, add);
    for (auto sample : output) assert(sample == 42);

    // Replacing the sound resets the cursor; conversion interpolates at a fractional rate.
    Window next{{300, 3}, 0, {2000, 3000, 4000}};
    output.fill(0);
    mix(next, cursor, 300, output.size(), 96000, add);
    assert(output[0] == 2000 * kClipGain);
    assert(std::abs(output[1] - 2500 * kClipGain) < 0.01f);
    assert(output[2] == 3000 * kClipGain);
}
