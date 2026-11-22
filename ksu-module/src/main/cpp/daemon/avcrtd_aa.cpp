// EXPERIMENTAL / NOT A PRODUCTION BACKEND.
// This legacy AAudio variant is excluded from CMakeLists.txt and the module package.
// Its topology restoration is device-specific; do not use as generic crash recovery.
// avcrtd_aa — cellular voice-changer daemon, AAudio capture variant.
//
// Capture path uses AAudio (AHAL-managed, VOICE_COMMUNICATION preset) instead of raw
// tinyalsa, because during a call the AHAL only exposes the in-call mic via a proper
// capture usecase. Inject path stays raw tinyalsa (Incall_Music_2 -> MM9 / pcm 0,23).
//
//   avcrtd_aa [pitch] [gain_db] [--dmic0] [--preset vc|unproc|camcorder|voicerec]
//     --dmic0 : route the call uplink to DMIC0 (real voice -> SILENT) on start, and
//               cross-pair-restore DMIC1/DMIC3 on exit. With a heavy gain on the (faint)
//               DMIC0-mode capture this aims for a CLEAN replace (far end = pitched only).
//     default (no --dmic0): additive (far end = real + pitched).
//
// Run as root, DURING an active cellular call. Ctrl-C/SIGTERM restores everything.

#include <aaudio/AAudio.h>
#include <tinyalsa/asoundlib.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>

#include "avcrt_engine.h"
#include "avcrt_config.h"

using avcrt::DspConfig;
using avcrt::Engine;

namespace {

constexpr unsigned kCard      = 0;
constexpr unsigned kInjDevice = 23;   // MultiMedia9 (pcm 0,23) playback
constexpr int32_t  kRate      = 16000;
constexpr int32_t  kChannels  = 1;
constexpr unsigned kPeriod    = 256;
constexpr unsigned kPeriods   = 4;

const char* kInjRoute = "Incall_Music_2 Audio Mixer MultiMedia9";

std::atomic<bool> g_run{true};
struct mixer*     g_mixer  = nullptr;
bool              g_dmic0  = false;

void on_signal(int) { g_run.store(false); }

void mx_set(const char* name, int value) {
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) { fprintf(stderr, "[aa] mixer ctl not found: %s\n", name); return; }
    int n = mixer_ctl_get_num_values(c);
    for (int i = 0; i < n; ++i) mixer_ctl_set_value(c, i, value);
}

void mx_enum(const char* name, const char* val) {
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) { fprintf(stderr, "[aa] mixer ctl not found: %s\n", name); return; }
    if (mixer_ctl_set_enum_by_string(c, val) != 0)
        fprintf(stderr, "[aa] enum set failed: %s=%s\n", name, val);
}

void set_call_dmic0() {
    mx_enum("TX DMIC MUX1", "DMIC0");
    mx_enum("TX DMIC MUX2", "DMIC0");
}

// Clean cross-pair restore: bounce each mux through a different-pair DMIC so the
// target clock pair fully cycles and re-selects the channel (a plain write-back
// leaves the main mic dead until an AHAL re-route).
void restore_call_mic() {
    mx_enum("TX DMIC MUX2", "DMIC3");
    mx_enum("TX DMIC MUX1", "DMIC2");
    usleep(1000 * 1000);
    mx_enum("TX DMIC MUX1", "DMIC1");
}

aaudio_input_preset_t parse_preset(const char* s) {
    if (!strcmp(s, "unproc"))    return AAUDIO_INPUT_PRESET_UNPROCESSED;
    if (!strcmp(s, "camcorder")) return AAUDIO_INPUT_PRESET_CAMCORDER;
    if (!strcmp(s, "voicerec"))  return AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
    return AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION;
}

pcm_config make_inj_config() {
    pcm_config c; memset(&c, 0, sizeof(c));
    c.channels = kChannels; c.rate = kRate;
    c.period_size = kPeriod; c.period_count = kPeriods;
    c.format = PCM_FORMAT_S16_LE;
    c.start_threshold = kPeriod; c.stop_threshold = kPeriod * kPeriods;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    double pitch = -5.0, gain_db = 24.0;
    const char* presetName = "vc";
    int posarg = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dmic0")) g_dmic0 = true;
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) presetName = argv[++i];
        else if (posarg++ == 0) pitch = atof(argv[i]);
        else gain_db = atof(argv[i]);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_mixer = mixer_open(kCard);
    if (!g_mixer) { fprintf(stderr, "[aa] mixer_open failed\n"); return 1; }

    fprintf(stderr, "[aa] pitch=%.2f gain=%.1fdB preset=%s dmic0=%d\n",
            pitch, gain_db, presetName, (int)g_dmic0);

    // --- AAudio capture stream ---
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { fprintf(stderr, "[aa] builder fail\n"); return 1; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, kChannels);
    AAudioStreamBuilder_setSampleRate(b, kRate);
    AAudioStreamBuilder_setInputPreset(b, parse_preset(presetName));
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStream* cap = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &cap);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "[aa] open capture: %s\n", AAudio_convertResultToText(r)); return 2; }
    fprintf(stderr, "[aa] capture deviceId=%d rate=%d ch=%d\n",
            AAudioStream_getDeviceId(cap), AAudioStream_getSampleRate(cap), AAudioStream_getChannelCount(cap));

    // --- inject (tinyalsa MM9) ---
    if (g_dmic0) { fprintf(stderr, "[aa] call uplink -> DMIC0 (silent)\n"); set_call_dmic0(); }
    mx_set(kInjRoute, 1);
    pcm_config icfg = make_inj_config();
    struct pcm* inj = pcm_open(kCard, kInjDevice, PCM_OUT, &icfg);
    if (!inj || !pcm_is_ready(inj)) {
        fprintf(stderr, "[aa] inject pcm_open failed: %s\n", inj ? pcm_get_error(inj) : "null");
        if (g_dmic0) restore_call_mic();
        mx_set(kInjRoute, 0);
        return 3;
    }
    mx_set("Playback 23 Volume", 8192);

    if (AAudioStream_requestStart(cap) != AAUDIO_OK) { fprintf(stderr, "[aa] cap start fail\n"); }

    Engine engine(kRate, kChannels);
    DspConfig cfg;
    cfg.enabled = true; cfg.pitch_semitones = pitch; cfg.formant_semitones = 0.0;
    cfg.time_ratio = 1.0; cfg.gain_db = gain_db; cfg.preserve_formants = true; cfg.revision = 1;
    engine.update(cfg);

    const int32_t frames = kPeriod;
    int16_t* buf = new int16_t[frames * kChannels];
    fprintf(stderr, "[aa] running. Ctrl-C to stop.\n");
    unsigned long n = 0;
    while (g_run.load()) {
        int32_t got = AAudioStream_read(cap, buf, frames, 200LL * 1000 * 1000);
        if (got < 0) { fprintf(stderr, "[aa] read err: %s\n", AAudio_convertResultToText(got)); break; }
        if (got == 0) continue;
        engine.process(buf, got);
        if (pcm_write(inj, buf, got * kChannels * sizeof(int16_t)) != 0) {
            fprintf(stderr, "[aa] pcm_write err: %s\n", pcm_get_error(inj)); break;
        }
        if ((++n % 200) == 0) fprintf(stderr, "[aa] %lu periods\n", n);
    }
    delete[] buf;

    fprintf(stderr, "[aa] stopping, restoring\n");
    AAudioStream_requestStop(cap);
    AAudioStream_close(cap);
    pcm_close(inj);
    mx_set(kInjRoute, 0);
    if (g_dmic0) restore_call_mic();
    mixer_close(g_mixer);
    return 0;
}
