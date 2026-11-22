// EXPERIMENTAL / NOT A PRODUCTION BACKEND.
// This legacy fixed-topology daemon is excluded from CMakeLists.txt and the module package.
// Do not use it as a generic device backend or as a crash-recovery tool.
// avcrtd_clean — CLEAN cellular voice replace daemon.
//
// Far end hears ONLY the pitched voice; the real voice is removed from the modem
// uplink. Requires the "AVCRT Dec Mute" kernel kcontrol (tx-macro.c patch).
//
// Mechanism (all proven on-device):
//   - The call's modem path is codec decimators DEC1+DEC2 -> TX_AIF1 -> TX_CDC_DMA_TX_3
//     -> CVP -> modem. Muting those decimators (AVCRT Dec Mute bitmask, TX_PATH_CTL bit
//     0x10) makes the modem uplink SILENT (real voice gone) — the mute is per-decimator.
//   - A spare FREE decimator (DEC0), UNMUTED, taps the same mic data -> TX_AIF2 ->
//     TX_CDC_DMA_TX_4 -> pcm 0,0: the daemon captures the LOUD real voice.
//   - avcrt::Engine pitch -> inject via Incall_Music_2 / MM9 / pcm 0,23 into the CVS:
//     far end hears the pitched voice.
//
//   avcrtd_clean [pitch_semitones] [gain_db]
// Run as root, DURING an active cellular call. Ctrl-C/SIGTERM unmutes + restores.

#include <tinyalsa/asoundlib.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#include "avcrt_engine.h"
#include "avcrt_config.h"

using avcrt::DspConfig;
using avcrt::Engine;

namespace {

constexpr unsigned kCard      = 0;
constexpr unsigned kCapDevice = 0;    // MultiMedia1 (pcm 0,0) capture
constexpr unsigned kInjDevice = 23;   // MultiMedia9 (pcm 0,23) playback
constexpr unsigned kRate      = 16000;
constexpr unsigned kChannels  = 1;
constexpr unsigned kPeriod    = 256;
constexpr unsigned kPeriods   = 4;

const char* kInjRoute   = "Incall_Music_2 Audio Mixer MultiMedia9";
const char* kDecMuteCtl = "AVCRT Dec Mute";
// spare capture path (free decimator DEC0 -> AIF2 -> TX_CDC_DMA_TX_4):
const char* kSpareDmicMux = "TX DMIC MUX0";
const char* kSpareDecMux  = "TX DEC0 MUX";
const char* kSpareAif     = "TX_AIF2_CAP Mixer DEC0";
const char* kCapFromTx4   = "MultiMedia1 Mixer TX_CDC_DMA_TX_4";
const char* kCapFromTx3   = "MultiMedia1 Mixer TX_CDC_DMA_TX_3";

std::atomic<bool> g_run{true};
struct mixer*     g_mixer = nullptr;
int               g_mute_mask = 0;

void on_signal(int) { g_run.store(false); }

void mx_set(const char* name, int value) {
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) { fprintf(stderr, "[clean] ctl not found: %s\n", name); return; }
    int n = mixer_ctl_get_num_values(c);
    for (int i = 0; i < n; ++i) mixer_ctl_set_value(c, i, value);
}

void mx_enum(const char* name, const char* val) {
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) { fprintf(stderr, "[clean] ctl not found: %s\n", name); return; }
    if (mixer_ctl_set_enum_by_string(c, val) != 0)
        fprintf(stderr, "[clean] enum set failed: %s=%s\n", name, val);
}

int mx_get(const char* name) {
    if (!g_mixer) return 0;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return 0;
    return mixer_ctl_get_value(c, 0);
}

// Build the mute mask from the call's active modem decimators (TX_AIF1_CAP Mixer DECn).
int detect_modem_mask() {
    int mask = 0;
    char name[40];
    for (int i = 0; i < 8; ++i) {
        snprintf(name, sizeof(name), "TX_AIF1_CAP Mixer DEC%d", i);
        if (mx_get(name)) mask |= (1 << i);
    }
    return mask;
}

pcm_config make_cfg() {
    pcm_config c; memset(&c, 0, sizeof(c));
    c.channels = kChannels; c.rate = kRate;
    c.period_size = kPeriod; c.period_count = kPeriods;
    c.format = PCM_FORMAT_S16_LE;
    c.start_threshold = kPeriod; c.stop_threshold = kPeriod * kPeriods;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    double pitch = -5.0, gain_db = 26.0;
    if (argc > 1) pitch = atof(argv[1]);
    if (argc > 2) gain_db = atof(argv[2]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_mixer = mixer_open(kCard);
    if (!g_mixer) { fprintf(stderr, "[clean] mixer_open failed\n"); return 1; }

    g_mute_mask = detect_modem_mask();
    if (g_mute_mask == 0) {
        fprintf(stderr, "[clean] no active modem decimators (TX_AIF1_CAP) — is a call up? "
                        "defaulting mask=6 (DEC1+DEC2)\n");
        g_mute_mask = 6;
    }
    fprintf(stderr, "[clean] pitch=%.2f gain=%.1fdB modem-dec mask=0x%x\n",
            pitch, gain_db, g_mute_mask);

    // 1) spare UNMUTED capture path: DEC0 <- mic -> TX_AIF2 -> TX_CDC_DMA_TX_4 -> pcm 0,0
    mx_enum(kSpareDmicMux, "DMIC1");
    mx_enum(kSpareDecMux,  "MSM_DMIC");
    mx_set(kSpareAif, 1);
    mx_set(kCapFromTx3, 0);   // MM1 must read TX_4 (spare), not the muted TX_3
    mx_set(kCapFromTx4, 1);

    // 2) inject path on
    mx_set(kInjRoute, 1);

    pcm_config ccfg = make_cfg(), icfg = make_cfg();
    struct pcm* cap = pcm_open(kCard, kCapDevice, PCM_IN,  &ccfg);
    struct pcm* inj = pcm_open(kCard, kInjDevice, PCM_OUT, &icfg);

    int rc = 0;
    if (!cap || !pcm_is_ready(cap)) {
        fprintf(stderr, "[clean] capture pcm_open failed: %s\n", cap ? pcm_get_error(cap) : "null");
        rc = 2; goto done;
    }
    if (!inj || !pcm_is_ready(inj)) {
        fprintf(stderr, "[clean] inject pcm_open failed: %s\n", inj ? pcm_get_error(inj) : "null");
        rc = 2; goto done;
    }

    {
        mx_set("Playback 23 Volume", 8192);

        // 3) MUTE the modem decimators -> real voice removed from the uplink.
        fprintf(stderr, "[clean] muting modem decimators: %s = 0x%x\n", kDecMuteCtl, g_mute_mask);
        mx_set(kDecMuteCtl, g_mute_mask);

        Engine engine(kRate, kChannels);
        DspConfig cfg;
        cfg.enabled = true; cfg.pitch_semitones = pitch; cfg.formant_semitones = 0.0;
        cfg.time_ratio = 1.0; cfg.gain_db = gain_db; cfg.preserve_formants = true; cfg.revision = 1;
        engine.update(cfg);

        const size_t frames = kPeriod;
        const size_t bytes  = frames * kChannels * sizeof(int16_t);
        int16_t* buf = new int16_t[frames * kChannels];

        fprintf(stderr, "[clean] running — far end should hear ONLY the pitched voice. Ctrl-C to stop.\n");
        unsigned long n = 0;
        while (g_run.load()) {
            if (pcm_read(cap, buf, bytes) != 0) {
                fprintf(stderr, "[clean] pcm_read err: %s\n", pcm_get_error(cap)); break;
            }
            engine.process(buf, frames);
            if (pcm_write(inj, buf, bytes) != 0) {
                fprintf(stderr, "[clean] pcm_write err: %s\n", pcm_get_error(inj)); break;
            }
            if ((++n % 200) == 0) fprintf(stderr, "[clean] %lu periods\n", n);
        }
        delete[] buf;
    }

done:
    fprintf(stderr, "[clean] stopping — unmute + restore\n");
    mx_set(kDecMuteCtl, 0);          // unmute modem decimators (real voice back)
    if (inj) pcm_close(inj);
    if (cap) pcm_close(cap);
    mx_set(kInjRoute, 0);
    mx_set(kSpareAif, 0);
    mx_set(kCapFromTx4, 0);
    mx_set(kCapFromTx3, 1);          // restore MM1 default binding
    mx_enum(kSpareDmicMux, "ZERO");
    if (g_mixer) mixer_close(g_mixer);
    return rc;
}
