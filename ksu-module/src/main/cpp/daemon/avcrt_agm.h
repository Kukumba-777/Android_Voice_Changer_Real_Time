#pragma once
#include <cstdint>
#include <string>
#include <sys/types.h>

#include "avcrt_mic_state.h"

namespace avcrt::agm {
// Passive startup detection. A candidate is not an eligible replacement backend.
bool candidate();
// Returns an identified codec or USB capture route. Replacement needs a provider.
bool started_call(std::string& generation, bool* usb_capture = nullptr);
enum class WorkerExit { Stopped = 0, Refused = 1, Unclean = 2, Recoverable = 3, ColdUplink = 4, LeaseLost = 5, MicWait = 6 };

// Read-only Android checks. A killed dumpsys that has not exited yet prevents
// another probe from spawning; never wait indefinitely for it in the supervisor.
class MicProbe {
public:
    MicProbe() = default;
    MicProbe(const MicProbe&) = delete;
    MicProbe& operator=(const MicProbe&) = delete;
    MicStatus sample(bool log_veto, bool* privacy_clear = nullptr);
    bool reap();
private:
    pid_t retired_ = -1;
};
int run_cellular(const char* generation, int64_t revision, bool (*running)());
#ifdef AVCRT_AGM_LAB
enum class Probe { Capabilities, VaTags, VaLevels, Tone, Speech, CaptureLevels, CaptureReplay, Overlap, Calibrate, Realtime, LivePcm, ReplacePcm, TxRoute };
// Active probes require explicit consent. Speech and LivePcm take PCM from stdin.
// ReplacePcm additionally requires an operator-reviewed backend and TX AIF.
int run(Probe probe, bool (*stock_mic)(), const char* tx_backend = nullptr, unsigned tx_aif = 0);
// Diagnostic only: prepare private graphs and query APM topology, without PCM or mute.
int probe_paths(bool (*stock_mic)(), unsigned codec_tx, unsigned encoder);
#endif
#ifdef AVCRT_USB_CAPTURE_LAB
int usb_capture_levels();
// Host ACDB output must match the live START for the one-shot cutoff test.
// No mode starts capture/playback or writes PCM except usb_capture_levels.
struct UsbPathCandidate {
    uint32_t source, source_sg, cutoff_sg, cutoff, stream_mute, encoder, music;
};
enum class UsbReadbackMode { Paths, AndroidMute, Cutoff };
int usb_readback(const UsbPathCandidate& candidate, UsbReadbackMode mode);
#endif
#ifdef AVCRT_VA_TEST_PRIVATE_PCM
enum class VaCallMode { Levels, BriefMute, Replace };
int va_call_levels(const char* tx_backend, unsigned tx_aif, unsigned long long epoch, unsigned vsid, VaCallMode mode);
#endif
}
