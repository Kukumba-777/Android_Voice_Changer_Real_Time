#include "avcrt_agm_uplink.h"
#include "avcrt_cellular_backend.h"
#include "avcrt_engine.h"
#include "avcrt_log.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <fstream>

namespace avcrt::log {
void i(const char*, ...) {} // No Android logging dependency on the host.
}

using namespace avcrt;
using namespace avcrt::cellular;
using namespace std::chrono_literals;

namespace {
constexpr unsigned kRate = 48000;
using Period = std::array<int16_t, kRate / 100>;

struct SoundboardFiles {
    char dir[sizeof("./avcrt-engine.XXXXXX")] = "./avcrt-engine.XXXXXX";
    int previous = -1;

    SoundboardFiles() {
        previous = open(".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        assert(previous >= 0 && mkdtemp(dir) && chdir(dir) == 0);
    }

    void publish() const {
        std::vector<int16_t> samples(4 * kRate, 1000);
        std::ofstream clip(paths::kSoundboardClip, std::ios::binary);
        clip.write(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(samples[0]));
        clip.close();
        struct stat64 st{};
        assert(clip && stat64(paths::kSoundboardClip, &st) == 0);
        const soundboard::Header header{soundboard::kMagic, soundboard::boot_ms(), 100, st.st_ino};
        std::ofstream marker("playback.tmp", std::ios::binary);
        marker.write(reinterpret_cast<const char*>(&header), sizeof(header));
        marker.close();
        assert(marker && std::rename("playback.tmp", paths::kSoundboardPlayback) == 0);
    }

    ~SoundboardFiles() {
        assert(unlink(paths::kSoundboardPlayback) == 0 && unlink(paths::kSoundboardClip) == 0);
        assert(fchdir(previous) == 0 && close(previous) == 0 && rmdir(dir) == 0);
    }
};

struct Lease {
    unsigned writes = 0, renewals = 0, releases = 0;
    bool renew() { assert(writes == renewals + 1); ++renewals; return true; }
    bool release() { ++releases; return true; }
};
}

int main() {
    SoundboardFiles files;
    Engine engine(kRate, 1, true);
    DspConfig preset;
    preset.enabled = true;
    preset.revision = 1;
    preset.bypass_voice = true;
    preset.pitch_semitones = 7.0;
    preset.gain_db = -60;
    preset.telephone.enabled = true;
    engine.update(preset);

    Period pcm{};
    bool full = false;
    pcm.fill(-2345);
    assert(engine.process(pcm.data(), pcm.size(), &full) && full);
    assert(std::all_of(pcm.begin(), pcm.end(), [](int16_t s) { return s == -2345; }));

    files.publish();
    const auto wait = std::chrono::steady_clock::now() + 2s;
    while (pcm.front() == -2345 && std::chrono::steady_clock::now() < wait) {
        std::this_thread::sleep_for(10ms);
        pcm.fill(-2345);
        assert(engine.process(pcm.data(), pcm.size(), &full) && full);
    }

    Lease lease;
    ReplacementSession session(lease);
    const auto process = [&] {
        if (!engine.process(pcm.data(), pcm.size(), &full)) return Block::Failed;
        return full ? Block::Full : Block::Pending;
    };
    assert(session.step([] { return PeriodCheck::Ready; }, [&] { pcm.fill(-2345); return Block::Full; }, process,
                        [&] {
                            for (auto s : pcm) assert(s == -2345 + 650);
                            ++lease.writes;
                            return Block::Full;
                        }) == Step::Replaced);
    pcm.fill(32760);
    assert(engine.process(pcm.data(), pcm.size(), &full) && full);
    for (auto s : pcm) assert(s == 32767);

    // Leave Original voice while TX is already leased. The new preset needs
    // warm-up, but even its soundboard mix must not leak into the zero blocks.
    avcrt::agm::UplinkSilence silence;
    ++preset.revision;
    preset.bypass_voice = false;
    preset.pitch_semitones = -6.0;
    preset.gain_db = 0.0;
    engine.update(preset);
    unsigned warmup = 0;
    do {
        assert(session.step([&] { return preset.enabled ? PeriodCheck::Ready : PeriodCheck::Invalid; }, [&] {
            pcm.fill(-1234);
            return Block::Full;
        }, [&] {
            assert(engine.process(pcm.data(), pcm.size(), &full));
            return Block::Full;
        }, [&] {
            assert(silence.prepare(session.periods() != 0, full, pcm));
            if (!full) {
                assert(std::all_of(pcm.begin(), pcm.end(), [](int16_t s) { return s == 0; }));
                ++warmup;
            }
            ++lease.writes;
            return Block::Full;
        }) == Step::Replaced);
        assert(!lease.releases);
    } while (!full);
    assert(warmup && lease.renewals == warmup + 2);

    preset.enabled = false;
    ++preset.revision;
    engine.update(preset);
    pcm.fill(12);
    assert(!engine.process(pcm.data(), pcm.size(), &full) && !full);
    for (auto s : pcm) assert(s == 12);
    assert(session.step([&] { return preset.enabled ? PeriodCheck::Ready : PeriodCheck::Invalid; },
                        [] { assert(false); return Block::Failed; },
                         process, [] { assert(false); return Block::Failed; }) == Step::Invalidated);
    assert(lease.writes == warmup + 2 && lease.renewals == lease.writes && lease.releases == 1);

    ++preset.revision;
    preset.enabled = true;
    preset.bypass_voice = false;
    engine.update(preset);
    Lease warming;
    ReplacementSession cold(warming);
    assert(cold.step([] { return PeriodCheck::Ready; }, [&] { pcm.fill(0); return Block::Full; }, process,
                      [] { assert(false); return Block::Failed; }) == Step::Priming);
    assert(warming.writes == 0 && warming.renewals == 0 && warming.releases == 0);
}
