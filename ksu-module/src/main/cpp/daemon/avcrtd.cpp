// avcrtd — AVCRT cellular voice-changer service (clean replace).
//
// A persistent root daemon. It watches the engine on/off + preset (via ConfigStore,
// the same active_preset.json + avcrt.config.revision the app already writes) and the
// live cellular-call state, and when both are active it engages a CLEAN voice replace:
// the far end hears ONLY the processed (pitched) voice; the real voice is removed.
//
// Mechanism (fully adaptive — no hardcoded session/decimator/port):
//   * The kernel "AVCRT Dec Mute" kcontrol (tx-macro.c patch) mutes, at the codec, the
//     decimators that feed the call's modem uplink -> real voice removed.
//   * A spare FREE decimator (unmuted) taps the same mic -> a free capture port -> the
//     daemon reads the LOUD real voice, pitches it (avcrt::Engine), and injects it via
//     incall_music into the call's CVS -> far end hears only the processed voice.
//
// Self-managing: engages when (engine on && call up), disengages + fully restores when
// the call ends or the engine is turned off. Run as root, persistently.

#include <tinyalsa/asoundlib.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "avcrt_engine.h"
#include "avcrt_config.h"

using avcrt::ConfigStore;
using avcrt::DspConfig;
using avcrt::Engine;

namespace {

// Select a card by its exposed voice routing capabilities, never by card order.
int            g_card      = -1;
int            g_usb_card  = -1;       // USB headset sound card index (can be 0!); -1 = none
int g_cap_device = -1;
int g_inj_device = -1;
int g_usb_cap_device = -1;
constexpr unsigned kRate      = 16000;
constexpr unsigned kChannels  = 1;
constexpr unsigned kPeriod    = 256;
constexpr unsigned kPeriods   = 4;
constexpr unsigned kUsbRate   = 48000; // USB headset mic native rate (S16 mono); decimated 3:1 -> 16k
constexpr unsigned kUsbDecim  = kUsbRate / kRate;        // 3
constexpr unsigned kUsbPeriod = kPeriod * kUsbDecim;     // 768 USB frames -> 256 @16k
constexpr int      kIdlePollMs = 400;  // how often we look for a call while idle

// Capture/engage mode for the current call.
//  MODE_BT = a Bluetooth (HFP/SCO) headset call — the uplink sources from INT_BT_SCO_TX
//  (the BT chip <-> ADSP SCO bridge), handled like MODE_HEADSET (reroute to bolero + capture
//  the freed mic + incall_music inject), but the BT mic is on the LITO card (not a USB card).
enum Mode { MODE_NONE = 0, MODE_BUILTIN = 1, MODE_HEADSET = 2, MODE_BT = 3 };

// The incall_music inject path is structurally quieter than the real mic leg, so
// we add a fixed compensation on top of the preset's artistic gain to land the
// processed voice at roughly original loudness on the far end. Empirically ~+22 dB.
constexpr double   kInjectGainCompDb = 22.0;

const char* kDecMuteCtl = "AVCRT Dec Mute";
// In-kernel surgical vocproc (CVP/CVS) rebuild for the live call — flushes a stale CVP Tx-device
// latch WITHOUT restarting audioserver (no USB re-enumeration flicker). Replaces the "hammer" for
// every accessory transition when the AVCRT unified kernel patch is flashed. Default-off kcontrol.
const char* kCvpRebuildCtl = "AVCRT CVP Rebuild";

std::atomic<bool> g_run{true};
struct mixer*     g_mixer = nullptr;
bool              g_patch_present = false;   // "AVCRT Dec Mute" kcontrol exists (AVCRT kernel)
bool              g_cvp_rebuild_present = false;   // "AVCRT CVP Rebuild" kcontrol exists (unified patch)

void on_signal(int) { g_run.store(false); }

void sleep_ms(int ms) {
    struct timespec ts { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

// Resolve a named front end (or a unique USB capture endpoint) with the required direction.
int find_pcm(int card, const char* fe, bool capture) {
    FILE* f = fopen("/proc/asound/pcm", "re");
    if (!f) return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        int c, d, n = 0;
        if (sscanf(line, "%d-%d: %n", &c, &d, &n) != 2 || !n || c != card) continue;
        const char* end = strchr(line + n, ':');
        if (!end || !strstr(end, capture ? " : capture " : " : playback ")) continue;
        if (fe) {
            const size_t len = strlen(fe);
            if (strncmp(line + n, fe, len) || (line[n + len] != ' ' && line[n + len] != ':')) continue;
        }
        if (found >= 0) { found = -1; break; }
        found = d;
    }
    fclose(f);
    return found;
}

int find_voice_card() {
    FILE* f = fopen("/proc/asound/cards", "re");
    if (!f) return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        int n, consumed = 0;
        if (sscanf(line, " %d [%n", &n, &consumed) != 1 || !consumed || n < 0) continue;
        if (find_pcm(n, "MultiMedia1", true) < 0 || find_pcm(n, "MultiMedia9", false) < 0) continue;
        mixer* m = mixer_open(n);
        if (!m) continue;
        const bool compatible = mixer_get_ctl_by_name(m, "MultiMedia1 Mixer TX_CDC_DMA_TX_3") &&
                                mixer_get_ctl_by_name(m, "MultiMedia1 Mixer TX_CDC_DMA_TX_4") &&
                                mixer_get_ctl_by_name(m, "Incall_Music Audio Mixer MultiMedia9") &&
                                mixer_get_ctl_by_name(m, "VoiceMMode1_Tx Mixer TX_CDC_DMA_TX_3_MMode1");
        mixer_close(m);
        if (!compatible) continue;
        if (found >= 0) { found = -1; break; }
        found = n;
    }
    fclose(f);
    return found;
}

// Find a USB-Audio sound card (a USB/USB-C headset or a USB-C->jack DAC adapter) in
// /proc/asound/cards. Returns the index, or -1 if no USB card is present (no headset).
// Re-scanned each idle poll since a headset can be plugged/unplugged at runtime.
int find_usb_card() {
    FILE* f = fopen("/proc/asound/cards", "re");
    if (!f) return -1;
    char line[256];
    int idx = -1, found = -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        if (sscanf(line, " %d [", &n) == 1) idx = n;
        // USB-Audio cards advertise "USB-Audio" / "USB Audio" on the description line.
        if (idx >= 0 && (strstr(line, "USB-Audio") || strstr(line, "USB Audio"))) {
            found = idx; break;
        }
    }
    fclose(f);
    return found;
}

void resolve_cards() {
    const int card = find_voice_card();
    if (card != g_card || (!g_mixer && card >= 0)) {
        if (g_mixer) { mixer_close(g_mixer); g_mixer = nullptr; }
        g_card = card;
        if (card >= 0) g_mixer = mixer_open(card);
        fprintf(stderr, "[avcrtd] cellular mixer: card=%d %s\n", card, g_mixer ? "ready" : "unavailable");
    }
    g_cap_device = card >= 0 ? find_pcm(card, "MultiMedia1", true) : -1;
    g_inj_device = card >= 0 ? find_pcm(card, "MultiMedia9", false) : -1;
    g_patch_present = g_mixer && mixer_get_ctl_by_name(g_mixer, kDecMuteCtl);
    g_cvp_rebuild_present = g_mixer && mixer_get_ctl_by_name(g_mixer, kCvpRebuildCtl);
    g_usb_card = find_usb_card();
    if (g_usb_card == g_card) g_usb_card = -1;
    g_usb_cap_device = g_usb_card >= 0 ? find_pcm(g_usb_card, nullptr, true) : -1;
}

bool mx_set(const char* name, int value) {
    if (!g_mixer) return false;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) { fprintf(stderr, "[avcrtd] ctl not found: %s\n", name); return false; }
    int n = mixer_ctl_get_num_values(c);
    bool ok = n > 0;
    for (int i = 0; i < n; ++i) {
        int rc = mixer_ctl_set_value(c, i, value);
        if (rc != 0) {
            fprintf(stderr, "[avcrtd] mixer write failed: %s[%d]=%d rc=%d\n", name, i, value, rc);
            ok = false;
        }
    }
    return ok;
}

bool mx_enum(const char* name, const char* val) {
    if (!g_mixer) return false;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return false;
    int rc = mixer_ctl_set_enum_by_string(c, val);
    if (rc != 0) fprintf(stderr, "[avcrtd] mixer enum write failed: %s=%s rc=%d\n", name, val, rc);
    return rc == 0;
}

// Returns the first value of a control, or -1 if absent.
int mx_get(const char* name) {
    if (!g_mixer) return -1;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return -1;
    return mixer_ctl_get_value(c, 0);
}

// Current STRING value of an ENUM control (e.g. a mux source). Writes into out (size sz);
// out is "" if the control is absent / not an enum. Used to DISCOVER, not assume, the mic
// source feeding the call's modem decimator (digital DMICx vs analog ADCx via SoundWire).
void mx_get_enum(const char* name, char* out, size_t sz) {
    if (sz) out[0] = '\0';
    if (!g_mixer) return;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return;
    int idx = mixer_ctl_get_value(c, 0);
    const char* s = mixer_ctl_get_enum_string(c, idx);
    if (s) snprintf(out, sz, "%s", s);
}

// Any value of a control non-zero?
bool mx_any_on(const char* name) {
    if (!g_mixer) return false;
    struct mixer_ctl* c = mixer_get_ctl_by_name(g_mixer, name);
    if (!c) return false;
    int n = mixer_ctl_get_num_values(c);
    for (int i = 0; i < n; ++i)
        if (mixer_ctl_get_value(c, i) > 0) return true;
    return false;
}

// ---- adaptive call/session/decimator detection -----------------------------

// Is the device in an ACTIVE call? On WiFi-Calling (VoWiFi) devices the CVD voice
// session (pcm15 / VoiceMMode Tx mixer) stays RUNNING permanently even when idle,
// so the mixer state is NOT a reliable "call up" signal. Audio mode is also no good
// on this device: `mode (internal)` stays NORMAL even during a real call, and
// `Actual mode` can stay stale IN_CALL with no call. The reliable signal is the
// telephony call state: `dumpsys telephony.registry` reports mCallState=2 (OFFHOOK)
// for an active call on a SIM (0=idle, 1=ringing). Cached ~1s (cheap).
bool is_in_call_uncached() {
    // CS (circuit-switched) call: telephony mCallState=2 (OFFHOOK).
    FILE* p = popen("dumpsys telephony.registry 2>/dev/null", "r");
    if (p) {
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            const char* m = strstr(line, "mCallState=");
            if (m && m[11] == '2') { pclose(p); return true; }
        }
        pclose(p);
    }
    // VoLTE/IMS call: mCallState often stays 0; the audio HAL's "Actual mode = MODE_IN_CALL" catches
    // it. A STALE Actual-mode after a call ends is NOT a false-engage risk because detect_mode also
    // requires the voice Tx mixer to be routed (which is Off once the call really ends).
    p = popen("dumpsys audio 2>/dev/null", "r");
    if (p) {
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            if (strstr(line, "Actual mode") && strstr(line, "MODE_IN_CALL")) { pclose(p); return true; }
        }
        pclose(p);
    }
    return false;
}

// Call state is polled by a BACKGROUND thread (dumpsys forks a process and blocks
// ~100ms — doing that in the audio loop between pcm_read/pcm_write underruns the
// stream → choppy audio + engage/disengage flapping). The audio loop only reads
// this atomic, which is instant.
std::atomic<bool> g_in_call{false};

void* call_monitor(void*) {
    while (g_run.load()) {
        g_in_call.store(is_in_call_uncached());
        // poll ~once a second; break the sleep into chunks so we exit promptly
        for (int i = 0; i < 10 && g_run.load(); ++i) sleep_ms(100);
    }
    return nullptr;
}

bool is_in_call() { return g_in_call.load(); }

// Active VoiceMMode session: 1, 2, or 0 if no cellular call is up. Requires BOTH
// the voice Tx mixer route AND a real in-call audio mode (VoWiFi-safe).
// ---- no-hardcode codec mic-path discovery ----------------------------------
// The call's modem-uplink mic is NOT always the built-in DMIC on TX_AIF1/TX_CDC_DMA_TX_3.
// CONFIRMED on device: built-in mic = DMIC -> DECn -> TX_AIF1 -> TX_CDC_DMA_TX_3; an ANALOG
// headset (Type-C->3.5mm adapter) = ADC1 -> SWR_MIC -> DEC5 -> TX_AIF2 -> TX_CDC_DMA_TX_4.
// So we DISCOVER the live path each call (which VoiceMMode_Tx source, which AIF, which
// decimator, which mic) and replicate the SAME source on a spare decimator — no hardcoded
// TX_3 / AIF1 / DMIC1. One code path covers built-in + analog-headset (+ anything else codec).

// A decimator's mic source, so we can replicate it on a spare decimator for capture.
struct MicSrc {
    char dec_mux[16] = {0};   // "MSM_DMIC" (digital) or "SWR_MIC" (analog/SoundWire ADC)
    char sub_mux[16] = {0};   // DMICx (digital) or ADCx / SWR_DMICx (analog)
    bool valid = false;
};

// What mic source is decimator d reading right now?
MicSrc discover_mic_source(int d) {
    MicSrc m;
    char dec[20], dmic[20], smic[20], decv[16];
    snprintf(dec,  sizeof(dec),  "TX DEC%d MUX",  d);
    snprintf(dmic, sizeof(dmic), "TX DMIC MUX%d", d);
    snprintf(smic, sizeof(smic), "TX SMIC MUX%d", d);
    mx_get_enum(dec, decv, sizeof(decv));
    if (strcmp(decv, "SWR_MIC") == 0) {
        snprintf(m.dec_mux, sizeof(m.dec_mux), "SWR_MIC");
        mx_get_enum(smic, m.sub_mux, sizeof(m.sub_mux));     // ADCx / SWR_DMICx
    } else if (strcmp(decv, "MSM_DMIC") == 0) {
        snprintf(m.dec_mux, sizeof(m.dec_mux), "MSM_DMIC");
        mx_get_enum(dmic, m.sub_mux, sizeof(m.sub_mux));     // DMICx
    }
    m.valid = m.sub_mux[0] && strcmp(m.sub_mux, "ZERO") != 0;
    return m;
}

// Bitmask of decimators with a REAL (non-ZERO) mic source routed into TX_AIF<aif>_CAP.
int decs_in_aif(int aif) {
    int mask = 0;
    char name[40];
    for (int i = 0; i < 8; ++i) {
        snprintf(name, sizeof(name), "TX_AIF%d_CAP Mixer DEC%d", aif, i);
        if (mx_get(name) > 0 && discover_mic_source(i).valid) mask |= (1 << i);
    }
    return mask;
}

// Which VoiceMMode<n>_Tx codec backend is the modem reading? Sets session (1/2) + the AIF
// that carries the mic. Only the codec pairs understood by this backend are
// supported; never guess the AIF for an unknown or ambiguous uplink.
bool discover_codec_uplink(int& session, int& aif) {
    char name[64];
    bool found = false;
    for (int n = 1; n <= 2; ++n) {
        for (int tx = 0; tx <= 5; ++tx) {
            snprintf(name, sizeof(name), "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_%d_MMode%d", n, tx, n);
            if (mx_any_on(name)) {
                if (found || (tx != 3 && tx != 4)) return false;
                found = true;
                session = n;
                if      (tx == 3) aif = 1;
                else if (tx == 4) aif = 2;
            }
        }
    }
    return found;
}

// A free spare decimator (not a modem dec) for our parallel capture. Prefer HIGH indices
// (DEC3+) so it can't collide with the modem's typically-low decimators or a transition latch.
int pick_spare_dec(int excluded) {
    for (int i = 3; i < 11; ++i) {
        const int dec_index = i % 8;
        if (excluded & (1 << dec_index)) continue;
        bool free = true;
        for (unsigned j = 0; j < mixer_get_num_ctls(g_mixer); ++j) {
            mixer_ctl* ctl = mixer_get_ctl(g_mixer, j);
            int aif, dec, end = 0;
            const char* name = mixer_ctl_get_name(ctl);
            if (name && sscanf(name, "TX_AIF%d_CAP Mixer DEC%d%n", &aif, &dec, &end) == 2 &&
                end && !name[end] && dec == dec_index && mixer_ctl_get_value(ctl, 0) != 0) {
                free = false;
                break;
            }
        }
        if (free) return dec_index;
    }
    return -1;
}

// Decide which clean-replace flow this call supports + the VoiceMMode session (1/2).
//  - USB-card headset on the uplink (VoiceMMode*_Tx <- USB_AUDIO_TX) -> MODE_HEADSET (reroute flow).
//  - Codec mic on the uplink (built-in DMIC *or* analog-headset ADC, DISCOVERED) -> MODE_BUILTIN.
//    Clean-replaced via AVCRT Dec Mute -> needs the AVCRT kernel patch.
//  - else MODE_NONE (idle).
Mode detect_mode(int& session) {
    session = 0;
    if (!is_in_call() || !g_mixer || g_cap_device < 0 || g_inj_device < 0) return MODE_NONE;
    if (mx_any_on("VoiceMMode2_Tx Mixer USB_AUDIO_TX_MMode2")) { session = 2; return MODE_HEADSET; }
    if (mx_any_on("VoiceMMode1_Tx Mixer USB_AUDIO_TX_MMode1")) { session = 1; return MODE_HEADSET; }
    // Bluetooth (HFP/SCO) headset call: the uplink sources from INT_BT_SCO_TX. Detected like USB but
    // a BT device is NOT a USB sound card, so this is checked before the USB-card guard below and the
    // BT flow (engage_bt) frees + captures the BT mic on the lito card.
    if (mx_any_on("VoiceMMode2_Tx Mixer INT_BT_SCO_TX_MMode2")) { session = 2; return MODE_BT; }
    if (mx_any_on("VoiceMMode1_Tx Mixer INT_BT_SCO_TX_MMode1")) { session = 1; return MODE_BT; }
    // A USB-Audio CARD is present but the uplink isn't on USB yet (AHAL mid-transition / leftover
    // reroute) -> stay idle until the USB uplink appears (don't run the codec flow with a USB headset
    // = the double-voice regression). NOTE: an analog Type-C->jack adapter is NOT a USB card, so it
    // correctly falls through to the codec flow below.
    if (find_usb_card() >= 0) return MODE_NONE;
    int aif = 0;
    if (g_patch_present && discover_codec_uplink(session, aif)) return MODE_BUILTIN;
    return MODE_NONE;
}

pcm_config make_cfg() {
    pcm_config c; memset(&c, 0, sizeof(c));
    c.channels = kChannels; c.rate = kRate;
    c.period_size = kPeriod; c.period_count = kPeriods;
    c.format = PCM_FORMAT_S16_LE;
    c.start_threshold = kPeriod; c.stop_threshold = kPeriod * kPeriods;
    return c;
}

// USB headset mic: 48k mono, 768-frame periods (== 256 @16k after 3:1 decimation).
pcm_config make_usb_cfg() {
    pcm_config c; memset(&c, 0, sizeof(c));
    c.channels = 1; c.rate = kUsbRate;
    c.period_size = kUsbPeriod; c.period_count = kPeriods;
    c.format = PCM_FORMAT_S16_LE;
    c.start_threshold = kUsbPeriod; c.stop_threshold = kUsbPeriod * kPeriods;
    return c;
}

// ---- engage / disengage ----------------------------------------------------

struct MixerEdit {
    std::string name;
    int before;
    int written;
    bool spare;
};

struct Session {
    int   mode       = MODE_NONE;
    int   session    = 0;          // VoiceMMode 1 or 2
    int   modem_mask = 0;          // DECs the AHAL shows in the modem leg (discovered AIF)
    int   mute_mask  = 0;          // DECs we actually mute
    int   modem_aif  = 1;          // AIF carrying the modem mic (1=TX_3, 2=TX_4) — DISCOVERED
    int   spare_dec  = 0;
    MicSrc mic;                    // the modem mic source we replicate on the spare (DMIC/ADC)
    char  inj_mixer[48] = {0};     // Incall_Music[_2] Audio Mixer MultiMedia9
    char  cap_from[48]  = {0};     // MultiMedia1 Mixer TX_CDC_DMA_TX_<cap> = our capture FE source
    char  spare_dmic_mux[20] = {0};
    char  spare_smic_mux[20] = {0};
    char  spare_dec_mux[20] = {0};
    char  spare_aif[28] = {0};       // TX_AIF<cap>_CAP Mixer DEC<spare> (our capture leg)
    char  spare_aif1[28] = {0};      // TX_AIF<modem>_CAP Mixer DEC<spare> (the MODEM leg — must stay 0)
    struct pcm* cap = nullptr;     // built-in: spare-DEC capture (pcm <lito>,0)
    struct pcm* inj = nullptr;     // both: incall_music inject (pcm <lito>,23)
    // ---- headset mode only ----
    struct pcm* usb_cap   = nullptr;   // the freed USB headset mic (pcm <usb>,0, 48k mono)
    struct pcm* be_keeper = nullptr;   // keeps the bolero placeholder codec chain powered (pcm <lito>,0)
    char  vtx_usb[44] = {0};           // VoiceMMode<n>_Tx Mixer USB_AUDIO_TX_MMode<n>
    char  vtx_bt[48] = {0};            // VoiceMMode<n>_Tx Mixer INT_BT_SCO_TX_MMode<n> (BT mode)
    char  vtx_bolero[48] = {0};        // VoiceMMode<n>_Tx Mixer TX_CDC_DMA_TX_3_MMode<n>
    int usb_card = -1;
    int saved_tx3 = -1;
    int saved_tx4 = -1;
    bool route_touched = false;
    bool uplink_touched = false;
    bool inject_touched = false;
    bool mute_touched = false;
    bool  engaged = false;
    std::vector<MixerEdit> codec_edits;
};

const char* kCapFromTx4 = "MultiMedia1 Mixer TX_CDC_DMA_TX_4";
const char* kCapFromTx3 = "MultiMedia1 Mixer TX_CDC_DMA_TX_3";
bool g_mute_clear_pending = false;
bool g_codec_rollback_failed = false;

bool codec_write(Session& s, const char* name, int value, bool spare = false) {
    mixer_ctl* ctl = mixer_get_ctl_by_name(g_mixer, name);
    if (!ctl || mixer_ctl_get_num_values(ctl) != 1 || value < 0) return false;
    const int before = mixer_ctl_get_value(ctl, 0);
    if (before < 0) return false;
    if (before == value) return true;
    s.codec_edits.push_back({name, before, value, spare});
    return mx_set(name, value);
}

bool codec_enum(Session& s, const char* name, const char* value) {
    mixer_ctl* ctl = mixer_get_ctl_by_name(g_mixer, name);
    if (!ctl || mixer_ctl_get_type(ctl) != MIXER_CTL_TYPE_ENUM) return false;
    for (unsigned i = 0; i < mixer_ctl_get_num_enums(ctl); ++i) {
        const char* item = mixer_ctl_get_enum_string(ctl, i);
        if (item && !strcmp(item, value)) return codec_write(s, name, i, true);
    }
    return false;
}

void rollback_codec(Session& s) {
    // If HAL has changed the spare's source or routed it into another AIF, do not
    // put our old mux state back over the new route. These are value checks, not ALSA locks.
    MicSrc live = discover_mic_source(s.spare_dec);
    const bool attached = mx_get(s.spare_aif) != 0;
    const bool reused = mx_get(s.spare_aif1) != 0 || (attached && (!live.valid ||
        strcmp(live.dec_mux, s.mic.dec_mux) || strcmp(live.sub_mux, s.mic.sub_mux)));
    int session = 0, aif = 0;
    const bool sole_uplink = attached && is_in_call() && discover_codec_uplink(session, aif) &&
        aif != s.modem_aif && decs_in_aif(aif) == (1 << s.spare_dec);
    for (auto it = s.codec_edits.rbegin(); it != s.codec_edits.rend(); ++it) {
        const char* name = it->name.c_str();
        const int live_value = mx_get(name);
        if (it->spare && (reused || sole_uplink)) {
            g_codec_rollback_failed = true;
            fprintf(stderr, "[avcrtd] gsm rollback: spare ownership ambiguous for '%s'; leaving it to HAL\n", name);
            continue;
        }
        if (live_value < 0 || (live_value == it->written && !mx_set(name, it->before))) {
            g_codec_rollback_failed = true;
            fprintf(stderr, "[avcrtd] gsm rollback: cannot restore '%s'; cellular processing suspended\n", name);
            if (it->spare) break;  // Don't change a source if its edge couldn't be detached.
        } else if (live_value == it->written) {
            fprintf(stderr, "[avcrtd] gsm rollback: restored '%s'\n", name);
        }
    }
}

bool set_session_mute(Session& s, int mask) {
    // A failed multi-value write may still have changed part of the control.
    s.mute_touched = true;
    bool ok = mx_set(kDecMuteCtl, mask);
    int cached = mx_get(kDecMuteCtl);
    fprintf(stderr, "[avcrtd] gsm mute: mode=%d session=%d requested=0x%x cached=%d write=%s\n",
            s.mode, s.session, mask, cached, ok ? "ok" : "failed");
    return ok && cached == mask;
}

bool prime_inject(Session& s) {
    // Check that the inject PCM accepts a period before committing codec mute.
    int16_t silence[kPeriod * kChannels] = {};
    if (pcm_write(s.inj, silence, sizeof(silence)) == 0) return true;
    fprintf(stderr, "[avcrtd] gsm inject prime failed: mode=%d session=%d: %s\n",
            s.mode, s.session, pcm_get_error(s.inj));
    return false;
}

bool enable_inject(Session& s) {
    s.inject_touched = true;
    return mx_set(s.inj_mixer, 1);
}

void save_capture_route(Session& s) {
    s.saved_tx3 = mx_get(kCapFromTx3);
    s.saved_tx4 = mx_get(kCapFromTx4);
    s.route_touched = true;
}

// Do not overwrite a concurrent HAL update that changed a mixer away from our value.
void restore_capture_ctl(const char* name, int saved, int ours) {
    if (saved >= 0 && saved != ours && mx_get(name) == ours) mx_set(name, saved);
}

// Brings up the clean-replace routing + opens the PCMs. Returns false on failure (caller
// disengages). Assumes a codec call is up. NO HARDCODE: DISCOVERS the modem's mic path
// (which AIF, which decimator, which DMIC/ADC source) and replicates the SAME source on a
// spare decimator routed to the OTHER (free) AIF/DMA for capture. Built-in DMIC and an analog
// Type-C->jack headset (ADC via SWR_MIC) are handled by the exact same code.
bool engage(Session& s, int session) {
    s.mode = MODE_BUILTIN;
    s.session = session;

    // DISCOVER the modem's uplink AIF (1=TX_CDC_DMA_TX_3, 2=TX_4) for this session.
    int dsess = session, aif = 1;
    if (!discover_codec_uplink(dsess, aif) || dsess != session) return false;
    s.modem_aif  = aif;
    s.modem_mask = decs_in_aif(aif);                        // the real-source decs feeding the modem
    if (s.modem_mask == 0) return false;                    // not really a call
    // ALSO defensively mute DEC0. After a headset flow (which uses DEC0 as its bolero placeholder)
    // and a swap (type-c→...→codec), the codec DMA / CVP can stay LATCHED feeding the modem's TX
    // port from DEC0 even though TX_AIF only shows the new modem dec → the real voice leaks = the
    // swap ADDITIVE. DEC0 is unused on a fresh codec call → muting it is harmless (proven), and the
    // spare picker avoids the mute set. (This restores the pre-discovery-rewrite belt-and-suspenders
    // that was wrongly dropped — it was for exactly this headset→codec latch, not morph-specific.)
    s.mute_mask  = s.modem_mask | (1 << 0);

    // The modem's PRIMARY decimator + its mic source (DMICx digital / ADCx analog) — to replicate.
    int primary = __builtin_ctz((unsigned)s.modem_mask);
    s.mic = discover_mic_source(primary);
    if (!s.mic.valid) return false;                         // couldn't read the call's mic source
    // Multiple digital mics are normal for the handset. A mixed digital/analog
    // route is not a proven tap source: do not pick the lowest DEC by accident.
    for (unsigned bits = static_cast<unsigned>(s.modem_mask); bits; bits &= bits - 1) {
        MicSrc other = discover_mic_source(__builtin_ctz(bits));
        if (!other.valid || strcmp(other.dec_mux, s.mic.dec_mux) ||
            (!strcmp(s.mic.dec_mux, "SWR_MIC") && strcmp(other.sub_mux, s.mic.sub_mux))) return false;
    }

    // Capture via the OTHER AIF/DMA (the modem holds one of the two codec capture pairs; we use
    // the free one: modem on AIF1/TX_3 -> we capture AIF2/TX_4, and vice-versa).
    int cap_aif = (aif == 1) ? 2 : 1;
    const char* cap_dma = (cap_aif == 1) ? kCapFromTx3 : kCapFromTx4;
    const char* off_dma = (cap_aif == 1) ? kCapFromTx4 : kCapFromTx3;
    s.spare_dec = pick_spare_dec(s.mute_mask);
    if (s.spare_dec < 0) return false;

    snprintf(s.inj_mixer, sizeof(s.inj_mixer),
             session == 1 ? "Incall_Music Audio Mixer MultiMedia9"
                          : "Incall_Music_2 Audio Mixer MultiMedia9");
    snprintf(s.cap_from,       sizeof(s.cap_from),       "%s", cap_dma);
    snprintf(s.spare_dec_mux,  sizeof(s.spare_dec_mux),  "TX DEC%d MUX",  s.spare_dec);
    snprintf(s.spare_dmic_mux, sizeof(s.spare_dmic_mux), "TX DMIC MUX%d", s.spare_dec);
    snprintf(s.spare_smic_mux, sizeof(s.spare_smic_mux), "TX SMIC MUX%d", s.spare_dec);
    snprintf(s.spare_aif,      sizeof(s.spare_aif),      "TX_AIF%d_CAP Mixer DEC%d", cap_aif, s.spare_dec);
    snprintf(s.spare_aif1,     sizeof(s.spare_aif1),     "TX_AIF%d_CAP Mixer DEC%d", aif,     s.spare_dec);

    fprintf(stderr, "[avcrtd] engage: session=%d modem_aif=%d modem_mask=0x%x mic=%s/%s spare_dec=%d cap_aif=%d inject='%s'\n",
            session, aif, s.modem_mask, s.mic.dec_mux, s.mic.sub_mux, s.spare_dec, cap_aif, s.inj_mixer);

    // 1) spare UNMUTED capture: DEC<spare> reads the SAME mic as the modem (replicated source),
    //    routed to the free AIF/DMA and discovered capture PCM. It must feed ONLY our capture leg, NEVER
    //    the modem leg (else the real voice leaks to the modem AND we pitch it = ADDITIVE).
    const char* source_ctl = strcmp(s.mic.dec_mux, "SWR_MIC") == 0 ? s.spare_smic_mux : s.spare_dmic_mux;
    if (decs_in_aif(cap_aif) || mx_get(s.spare_aif1) != 0 || mx_get(s.spare_aif) != 0 ||
        mx_get(off_dma) != 0 || mx_get(cap_dma) != 0 || mx_get(s.inj_mixer) != 0) return false;
    if (!codec_enum(s, s.spare_dec_mux, s.mic.dec_mux) || !codec_enum(s, source_ctl, s.mic.sub_mux) ||
        !codec_write(s, s.spare_aif, 1, true) || !codec_write(s, cap_dma, 1)) return false;

    pcm_config ccfg = make_cfg(), icfg = make_cfg();
    s.cap = pcm_open(g_card, g_cap_device, PCM_IN, &ccfg);
    if (!s.cap || !pcm_is_ready(s.cap)) {
        fprintf(stderr, "[avcrtd] gsm capture open failed: %s\n", s.cap ? pcm_get_error(s.cap) : "null");
        return false;
    }
    int16_t first[kPeriod * kChannels];
    if (pcm_read(s.cap, first, sizeof(first)) != 0) {
        fprintf(stderr, "[avcrtd] gsm capture prime failed: session=%d: %s\n",
                session, pcm_get_error(s.cap));
        return false;
    }
    if (!codec_write(s, s.inj_mixer, 1)) return false;
    s.inj = pcm_open(g_card, g_inj_device, PCM_OUT, &icfg);
    if (!s.inj || !pcm_is_ready(s.inj) || !prime_inject(s)) return false;
    int live_session = 0, live_aif = 0;
    if (!g_run.load() || !is_in_call() || find_usb_card() >= 0 ||
        !discover_codec_uplink(live_session, live_aif) || live_session != session ||
        live_aif != aif || decs_in_aif(aif) != s.modem_mask) {
        fprintf(stderr, "[avcrtd] gsm codec route changed during prime — abort before mute\n");
        return false;
    }
    MicSrc live_mic = discover_mic_source(primary);
    if (!live_mic.valid || strcmp(live_mic.dec_mux, s.mic.dec_mux) ||
        strcmp(live_mic.sub_mux, s.mic.sub_mux)) return false;
    if (!set_session_mute(s, s.mute_mask)) return false;
    s.engaged = true;
    return true;
}

// Decimate 48k mono -> 16k mono (3:1) by averaging each group of 3 (cheap anti-alias).
// in: kUsbPeriod (768) samples; out: kPeriod (256) samples.
void decimate_3to1(const int16_t* in, int16_t* out, size_t out_frames) {
    for (size_t i = 0; i < out_frames; ++i) {
        int32_t a = in[i * 3] + in[i * 3 + 1] + in[i * 3 + 2];
        out[i] = (int16_t)(a / 3);
    }
}

// ───── HEADSET clean-replace (USB/USB-C-adapter headset, NO kernel patch required) ─────
// Reroute the call uplink USB→bolero (so the modem reads the built-in mic), silence the bolero
// placeholder (DMIC0 silent line; + AVCRT Dec Mute if the patch is present), which FREES the USB
// headset mic; capture the freed headset mic, pitch, and inject via incall_music. The user talks
// into the HEADSET, the far end hears ONLY the pitched headset voice. All stock mechanisms.
bool engage_headset(Session& s, int session) {
    if (g_usb_card < 0 || g_usb_cap_device < 0) return false;
    s.mode = MODE_HEADSET;
    s.session = session;
    s.usb_card = g_usb_card;

    snprintf(s.inj_mixer, sizeof(s.inj_mixer),
             session == 1 ? "Incall_Music Audio Mixer MultiMedia9"
                          : "Incall_Music_2 Audio Mixer MultiMedia9");
    snprintf(s.vtx_usb, sizeof(s.vtx_usb),
             "VoiceMMode%d_Tx Mixer USB_AUDIO_TX_MMode%d", session, session);
    snprintf(s.vtx_bolero, sizeof(s.vtx_bolero),
             "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_3_MMode%d", session, session);

    fprintf(stderr, "[avcrtd] engage_headset: session=%d usb_card=%d inject='%s'\n",
            session, g_usb_card, s.inj_mixer);

    // 1) bolero PLACEHOLDER mic: DEC0 ← DMIC0 (silent line) → TX_AIF1 → TX_CDC_DMA_TX_3.
    save_capture_route(s);
    if (!mx_enum("TX DMIC MUX0", "DMIC0") || !mx_enum("TX DEC0 MUX", "MSM_DMIC") ||
        !mx_set("TX_AIF1_CAP Mixer DEC0", 1) || !mx_set(kCapFromTx3, 1)) return false;

    // 2) BE-keeper: open a capture on TX_CDC_DMA_TX_3 to power the codec chain AND make the BE
    //    'active' so the voice-Tx reroute fires voc_set_device_config. Read one period to start it.
    pcm_config bcfg = make_cfg();
    s.be_keeper = pcm_open(g_card, g_cap_device, PCM_IN, &bcfg);
    if (!s.be_keeper || !pcm_is_ready(s.be_keeper)) {
        fprintf(stderr, "[avcrtd] engage_headset: BE-keeper open failed: %s\n",
                s.be_keeper ? pcm_get_error(s.be_keeper) : "null");
        return false;
    }
    {
        int16_t tmp[kPeriod];
        if (pcm_read(s.be_keeper, tmp, sizeof(tmp)) != 0) return false;
    }

    // 3) reroute (ORDER CRITICAL): USB OFF first (voc_disable_device), bolero ON last
    //    (voc_set_device_config(bolero)+voc_enable_device) → modem reads the silent bolero mic.
    if (!is_in_call() || find_usb_card() != s.usb_card || !mx_any_on(s.vtx_usb)) return false;
    s.uplink_touched = true;
    if (!mx_set(s.vtx_usb, 0) || !mx_set(s.vtx_bolero, 1)) return false;
    // On a patched kernel, hard-mute ALL codec decimators (0xFF). In headset mode the modem must hear
    // ONLY our incall_music inject — the real voice comes from the type-c mic (captured via usb_cap),
    // so NO codec decimator should reach the uplink. Muting only DEC0 (the placeholder) left a leak on
    // a SWAP (analog→type-c): the transient bottom-mic moment latches the CVP/DMA onto DMIC1's dec, and
    // the visible routing (modem←DEC0 muted) looked clean yet the latched dec leaked = additive. Muting
    // all decs (they stay CLOCKED-but-silent) kills any latched dec; harmless on a fresh call (the
    // placeholder DEC0 is muted either way, the rest are unused). type-c capture is unaffected (it's the
    // USB card, not a codec dec).

    // 4) the USB headset mic is now FREE — open + PRIME it (card<usb>,0, 48k mono). The reroute
    //    releases the mic from the call's ADSP offload ASYNCHRONOUSLY, so the first start can EPIPE
    //    ("cannot start channel: Broken pipe") when the headset was plugged MID-CALL (the endpoint
    //    isn't ready yet). Retry open+start a few times before giving up — do NOT misread a startup
    //    race as "headset gone" (that fell back to built-in → the phone's bottom mic leaked = additive).
    //    A headset plugged BEFORE the call usually starts on the first try.
    pcm_config ucfg = make_usb_cfg();
    bool usb_started = false;
    for (int attempt = 0; attempt < 12 && g_run.load(); ++attempt) {
        sleep_ms(120);                              // let the ADSP release the USB capture endpoint
        if (find_usb_card() != s.usb_card) {
            fprintf(stderr, "[avcrtd] engage_headset: headset vanished during usb_cap prime\n");
            return false;
        }
        if (!s.usb_cap) s.usb_cap = pcm_open(s.usb_card, g_usb_cap_device, PCM_IN, &ucfg);
        if (!s.usb_cap || !pcm_is_ready(s.usb_cap)) {
            if (s.usb_cap) { pcm_close(s.usb_cap); s.usb_cap = nullptr; }
            continue;
        }
        int16_t tmp[kUsbPeriod];
        if (pcm_read(s.usb_cap, tmp, sizeof(tmp)) == 0) { usb_started = true; break; }
        // start failed (EPIPE) — close + reopen for a clean retry next attempt
        pcm_close(s.usb_cap); s.usb_cap = nullptr;
    }
    if (!usb_started) {
        fprintf(stderr, "[avcrtd] engage_headset: usb_cap failed to start after retries (headset present) — bail\n");
        return false;
    }
    fprintf(stderr, "[avcrtd] engage_headset: usb_cap started\n");

    // 5) inject path.
    if (!enable_inject(s)) return false;
    pcm_config icfg = make_cfg();
    s.inj = pcm_open(g_card, g_inj_device, PCM_OUT, &icfg);
    if (!s.inj || !pcm_is_ready(s.inj)) {
        fprintf(stderr, "[avcrtd] engage_headset: inj open failed: %s\n",
                s.inj ? pcm_get_error(s.inj) : "null");
        return false;
    }

    if (!prime_inject(s) || !g_run.load() || !is_in_call() || find_usb_card() != s.usb_card ||
        mx_get(s.vtx_usb) != 0 || !mx_any_on(s.vtx_bolero)) return false;
    if (g_patch_present && !set_session_mute(s, 0xFF)) return false;
    s.engaged = true;
    return true;
}

// ───── BLUETOOTH (HFP/SCO) clean-replace — mirrors the USB headset flow ─────
// A BT-routed GSM call sources its uplink from INT_BT_SCO_TX (the BT chip <-> ADSP SCO bridge, the
// BT analogue of USB_AUDIO_TX). Same idea as engage_headset: reroute the uplink INT_BT_SCO_TX->bolero
// (the modem reads the silent bolero placeholder), which should FREE the BT SCO mic; capture the freed
// BT mic via MM1 <- INTERNAL_BT_SCO_TX, pitch (avcrt::Engine), inject via incall_music. The BT mic is on
// the LITO card (not a separate USB card), so MM1 is reused SEQUENTIALLY: first as the BE-keeper to fire
// the reroute, then re-pointed to the BT mic.
//
// ⚠️ LIVE-VALIDATION PATH — needs a real BT (HFP) GSM call to confirm two unknowns the USB flow did NOT
// have: (1) whether the SCO link SURVIVES the uplink reroute off it (the BT stack owns the HFP link and
// may tear the SCO mic down), and (2) whether INTERNAL_BT_SCO_TX is AP-capturable once the call is off it
// (the same ADSP-bridge concurrency question USB had). engage_bt logs each step and BAILS SAFELY (returns
// false -> the daemon idles for this BT call, the call itself is unaffected) if the capture can't start.
bool engage_bt(Session& s, int session) {
    s.mode = MODE_BT;
    s.session = session;

    snprintf(s.inj_mixer, sizeof(s.inj_mixer),
             session == 1 ? "Incall_Music Audio Mixer MultiMedia9"
                          : "Incall_Music_2 Audio Mixer MultiMedia9");
    snprintf(s.vtx_bt, sizeof(s.vtx_bt),
             "VoiceMMode%d_Tx Mixer INT_BT_SCO_TX_MMode%d", session, session);
    snprintf(s.vtx_bolero, sizeof(s.vtx_bolero),
             "VoiceMMode%d_Tx Mixer TX_CDC_DMA_TX_3_MMode%d", session, session);

    fprintf(stderr, "[avcrtd] engage_bt: session=%d inject='%s' (BT GSM — live-validation path)\n",
            session, s.inj_mixer);

    save_capture_route(s);

    // 1) bolero PLACEHOLDER mic: DEC0 <- DMIC0 (clocked-but-silent line) -> TX_AIF1 -> TX_CDC_DMA_TX_3.
    if (!mx_enum("TX DMIC MUX0", "DMIC0") || !mx_enum("TX DEC0 MUX", "MSM_DMIC") ||
        !mx_set("TX_AIF1_CAP Mixer DEC0", 1) || !mx_set(kCapFromTx3, 1)) return false;

    // 2) BE-keeper on MM1 -> makes the bolero BE active so the voice-Tx reroute fires voc_set_device_config.
    pcm_config bcfg = make_cfg();
    s.be_keeper = pcm_open(g_card, g_cap_device, PCM_IN, &bcfg);
    if (!s.be_keeper || !pcm_is_ready(s.be_keeper)) {
        fprintf(stderr, "[avcrtd] engage_bt: BE-keeper open failed: %s\n",
                s.be_keeper ? pcm_get_error(s.be_keeper) : "null");
        return false;
    }
    { int16_t tmp[kPeriod]; pcm_read(s.be_keeper, tmp, sizeof(tmp)); }

    // 3) reroute (ORDER CRITICAL): BT off first (voc_disable_device), bolero on last
    //    (voc_set_device_config(bolero)+voc_enable_device) -> modem reads the silent bolero placeholder.
    if (!is_in_call() || !mx_any_on(s.vtx_bt)) return false;
    s.uplink_touched = true;
    if (!mx_set(s.vtx_bt, 0) || !mx_set(s.vtx_bolero, 1)) return false;

    // 4) close the BE-keeper (the CVP/modem now holds the bolero BE) and RE-POINT MM1 to capture the
    //    freed BT SCO mic (16k mono; the FE SRCs from the 8k/16k SCO BE). Retry the start — the SCO
    //    endpoint may need a moment to free after the reroute.
    pcm_close(s.be_keeper); s.be_keeper = nullptr;
    mx_set("MultiMedia1 Mixer TX_CDC_DMA_TX_3", 0);
    mx_set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 1);
    pcm_config ccfg = make_cfg();
    bool started = false;
    for (int attempt = 0; attempt < 12 && g_run.load(); ++attempt) {
        sleep_ms(120);
        if (!mx_any_on(s.vtx_bolero)) {              // reroute didn't hold / call moved -> bail
            fprintf(stderr, "[avcrtd] engage_bt: bolero reroute didn't hold — bail\n");
            return false;
        }
        if (!s.cap) s.cap = pcm_open(g_card, g_cap_device, PCM_IN, &ccfg);
        if (!s.cap || !pcm_is_ready(s.cap)) {
            if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
            continue;
        }
        int16_t tmp[kPeriod];
        if (pcm_read(s.cap, tmp, sizeof(tmp)) == 0) { started = true; break; }
        pcm_close(s.cap); s.cap = nullptr;           // start failed (EPIPE) — clean retry
    }
    if (!started) {
        fprintf(stderr, "[avcrtd] engage_bt: BT mic capture failed to start — INTERNAL_BT_SCO_TX not "
                        "AP-capturable after reroute (likely needs kernel/AHAL work) — bail\n");
        return false;
    }
    fprintf(stderr, "[avcrtd] engage_bt: BT mic capture started (MM1 <- INTERNAL_BT_SCO_TX)\n");

    // 5) inject path on.
    if (!enable_inject(s)) return false;
    pcm_config icfg = make_cfg();
    s.inj = pcm_open(g_card, g_inj_device, PCM_OUT, &icfg);
    if (!s.inj || !pcm_is_ready(s.inj)) {
        fprintf(stderr, "[avcrtd] engage_bt: inj open failed: %s\n",
                s.inj ? pcm_get_error(s.inj) : "null");
        return false;
    }

    if (!prime_inject(s) || !g_run.load() || !is_in_call() ||
        mx_get(s.vtx_bt) != 0 || !mx_any_on(s.vtx_bolero)) return false;
    if (g_patch_present && !set_session_mute(s, 0xFF)) return false;
    s.engaged = true;
    return true;
}

void disengage(Session& s, bool headset_gone = false) {
    if (!s.route_touched && s.codec_edits.empty() && !s.engaged && !s.cap && !s.inj && !s.usb_cap && !s.be_keeper) {
        s = Session{};
        return;
    }
    if (s.mute_touched) g_mute_clear_pending = !set_session_mute(s, 0);

    if (s.mode == MODE_BT) {
        // Bluetooth teardown — mirror of the headset path. 'headset_gone' here = BT route/device gone.
        fprintf(stderr, "[avcrtd] disengage_bt: teardown (gone=%d)\n", (int)headset_gone);
        bool call_up = is_in_call();
        if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
        if (s.uplink_touched) mx_set(s.vtx_bolero, 0);
        // restore the BT uplink only if the call is CONTINUING + BT is still the route (engine toggled
        // off mid-call). On a call-end / BT gone, leave voice Tx for the AHAL (don't strand a dead route).
        if (call_up && !headset_gone && s.uplink_touched) mx_set(s.vtx_bt, 1);
        if (s.inj) { pcm_close(s.inj); s.inj = nullptr; }
        if (s.be_keeper) { pcm_close(s.be_keeper); s.be_keeper = nullptr; }
        if (s.inject_touched) mx_set(s.inj_mixer, 0);
        mx_set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 0);   // drop our BT-mic capture tap
        mx_set("TX_AIF1_CAP Mixer DEC0", 0);                 // tear down the silent placeholder DEC
        mx_enum("TX DMIC MUX0", "ZERO");
        restore_capture_ctl(kCapFromTx3, s.saved_tx3, s.uplink_touched ? 0 : 1);
        Session fresh; s = fresh;
        return;
    }

    if (s.mode == MODE_HEADSET) {
        fprintf(stderr, "[avcrtd] disengage_headset: teardown (headset_gone=%d)\n", (int)headset_gone);
        // Voice-Tx restore is CONDITIONAL on the disengage REASON (NOT find_usb_card — that races the
        // async USB de-registration on unplug and wrongly reports "present"):
        //  - headset_gone (unplug / capture broke): the AHAL is moving the call to the built-in mic.
        //    Do NOT touch the voice Tx (forcing USB-on to a dead card confuses the CVP → additive leak).
        //  - else (call-end / engine-off, headset still there): restore the USB uplink so the call
        //    returns to the headset mic (bolero OFF, USB ON last).
        // Voice Tx is the AHAL's to own. We ALWAYS drop our bolero reroute. We only RESTORE the USB
        // uplink when the call is CONTINUING (engine toggled OFF mid-call) and the headset is still
        // present — so it keeps working with the headset mic. On a CALL-END we set NOTHING (let the
        // AHAL clear voice Tx for the ended call): setting USB here left a DEAD USB route that broke the
        // NEXT call once the headset was unplugged (the user's silence). NEVER restore USB when the
        // headset is gone (dead card).
        bool call_up = is_in_call();
        // Release our USB capture before handing the endpoint back to the HAL.
        if (s.usb_cap)   { pcm_close(s.usb_cap);   s.usb_cap = nullptr; }
        if (s.uplink_touched) mx_set(s.vtx_bolero, 0);
        if (call_up && !headset_gone && s.uplink_touched && find_usb_card() == s.usb_card)
            mx_set(s.vtx_usb, 1);
        if (s.inj)       { pcm_close(s.inj);       s.inj = nullptr; }
        if (s.be_keeper) { pcm_close(s.be_keeper); s.be_keeper = nullptr; }
        if (s.inject_touched) mx_set(s.inj_mixer, 0);
        // The HAL may already own DEC0 on the replacement route. Only remove our DMIC0 placeholder.
        char source[16];
        mx_get_enum("TX DMIC MUX0", source, sizeof(source));
        if (strcmp(source, "DMIC0") == 0) {
            mx_set("TX_AIF1_CAP Mixer DEC0", 0);
            mx_enum("TX DMIC MUX0", "ZERO");
        }
        // On unplug the HAL may already have reused the capture leg for the new route.
        if (!headset_gone) restore_capture_ctl(kCapFromTx3, s.saved_tx3, 1);
        Session fresh; s = fresh;
        return;
    }

    // built-in (bolero) mode
    fprintf(stderr, "[avcrtd] disengage: unmute + restore\n");
    if (s.inj) { pcm_close(s.inj); s.inj = nullptr; }
    if (s.cap) { pcm_close(s.cap); s.cap = nullptr; }
    if (!s.codec_edits.empty()) rollback_codec(s);
    Session fresh;
    s = fresh;
}

bool engine_enabled(ConfigStore& cfg, DspConfig& out) {
    out = cfg.snapshot();
    return out.enabled;
}

// Compare the observed mic path with the last engaged route.
// The old recovery path distinguished a mixer route change (the AHAL re-routed
// the call to a new mic → full rebuild) vs a TRANSIENT blip (same route → just retry). NO hardcode:
// compares the live discovered path to the engaged Session. This is what lets a capture I/O error
// distinguish "headset plugged/unplugged" (→ hammer) from a startup/xrun blip (→ retry, no needless
// audioserver restart / call-start glitch).
//   ACC_SAME    = same accessory + same modem mic decimators
//   ACC_CHANGED = USB plugged/unplugged, OR the codec modem AIF / decimators moved (analog jack
//                 plug/unplug, built-in<->analog)
//   ACC_UNKNOWN = codec uplink momentarily unreadable (mid-transition) — caller decides
enum AccCheck { ACC_SAME, ACC_CHANGED, ACC_UNKNOWN };
AccCheck check_accessory(const Session& s) {
    int usb_now = find_usb_card();
    if (s.mode == MODE_HEADSET) return usb_now == s.usb_card ? ACC_SAME : ACC_CHANGED;
    if (usb_now >= 0 && s.mode == MODE_BUILTIN) return ACC_CHANGED;
    if (s.mode == MODE_BUILTIN) {
        int ds = 0, na = 0;
        if (!discover_codec_uplink(ds, na)) return ACC_UNKNOWN;    // uplink gone for a moment
        if (ds != s.session || na != s.modem_aif) return ACC_CHANGED;
        int dmask = decs_in_aif(na);
        if (!dmask) return ACC_UNKNOWN;
        if (dmask != s.modem_mask) return ACC_CHANGED;
        MicSrc mic = discover_mic_source(__builtin_ctz((unsigned)dmask));
        if (!mic.valid) return ACC_UNKNOWN;
        if (strcmp(mic.dec_mux, s.mic.dec_mux) || strcmp(mic.sub_mux, s.mic.sub_mux)) return ACC_CHANGED;
    }
    return ACC_SAME;
}

struct RouteSample {
    Mode mode = MODE_NONE;
    int session = 0;
    int aif = 0;
    int mask = 0;
    int usb = -1;
    MicSrc mic;

    bool operator==(const RouteSample& r) const {
        return mode == r.mode && session == r.session && aif == r.aif && mask == r.mask &&
               usb == r.usb && !strcmp(mic.dec_mux, r.mic.dec_mux) && !strcmp(mic.sub_mux, r.mic.sub_mux);
    }
};

RouteSample sample_route() {
    RouteSample r;
    r.mode = detect_mode(r.session);
    if (r.mode == MODE_HEADSET) {
        r.usb = g_usb_card;
        if (r.usb < 0) return {};
    } else if (r.mode == MODE_BUILTIN) {
        int session = 0;
        if (!discover_codec_uplink(session, r.aif) || session != r.session) return {};
        r.mask = decs_in_aif(r.aif);
        if (!r.mask) return {};
        r.mic = discover_mic_source(__builtin_ctz((unsigned)r.mask));
        if (!r.mic.valid) return {};
    }
    return r;
}

}  // namespace

// The dedicated AVCRT control is not part of HAL routing. After a crash we
// cannot infer ownership of any topology controls from their values alone.
int run_unmute() {
    int fd = open("/data/local/tmp/avcrtd.lock", O_CREAT | O_RDWR, 0644);
    if (fd < 0) return 1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return 1; }
    resolve_cards();
    const int rc = g_patch_present && mx_get(kDecMuteCtl) != 0 && !mx_set(kDecMuteCtl, 0) ? 1 : 0;
    if (g_mixer) mixer_close(g_mixer);
    close(fd);
    return rc;
}

// Probe: does the running kernel expose the "AVCRT Dec Mute" kcontrol (i.e. is the
// AVCRT kernel patch flashed)? Exit 0 if present, 1 if not. Used by customize.sh.
int run_probe_kcontrol() {
    const int card = find_voice_card();
    struct mixer* m = card >= 0 ? mixer_open(card) : nullptr;
    if (!m) return 1;
    struct mixer_ctl* c = mixer_get_ctl_by_name(m, kDecMuteCtl);
    int ok = (c != nullptr) ? 0 : 1;
    mixer_close(m);
    return ok;
}

// Read-only probe: a missing mixer is an inspection error, not an absent control.
int run_probe_cvp() {
    const int card = find_voice_card();
    if (card < 0) return 1;
    struct mixer* m = mixer_open(card);
    if (!m) return 2;
    const bool present = mixer_get_ctl_by_name(m, kCvpRebuildCtl) != nullptr;
    mixer_close(m);
    return present ? 0 : 1;
}

// ---- afhook auto-injection (the universal AudioFlinger-level voice hook) -----
//
// The afhook .so (libavcrt_afhook.so) pitches EVERY mic capture that flows through
// AudioFlinger's RecordThread — apps, mediaserver, the camera — at the HAL read.
// It can't load itself: it must be ptrace-injected into the audioserver process (a
// non-zygote init service). We drive that here, in the already-root, auto-supervised
// daemon. Packaging keeps this experimental path separate from Zygisk. A mapped
// payload does not establish that an inline hook was installed or processes audio.
//
// SAFETY (this runs on a daily driver): if our injected hook were to crash audioserver,
// it would respawn and we'd re-inject → an audio bootloop. So we DISABLE afhook
// after a few quick post-inject
// restarts, leaving audioserver clean. The inject itself is isolated in a short-lived
// child process (the avcrt_inject binary), so a ptrace glitch can't take down avcrtd.

pid_t pidof_one(const char* name) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", name);
    FILE* p = popen(cmd, "r");
    if (!p) return 0;
    pid_t pid = 0;
    if (fscanf(p, "%d", &pid) != 1) pid = 0;   // first pid only (audioserver is singleton)
    pclose(p);
    return pid;
}

bool afhook_in_maps(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "re");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libavcrt_afhook")) { found = true; break; }
    }
    fclose(f);
    return found;
}

// Run the avcrt_inject helper (sibling of this binary) in a child process. Returns 0
// only if it exits 0. Isolated so a ptrace failure never affects the daemon.
int run_injector(pid_t target, const char* payload) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return -1;
    exe[n] = '\0';
    char* slash = strrchr(exe, '/');
    if (!slash) return -1;
    *slash = '\0';
    char inj[600];
    snprintf(inj, sizeof(inj), "%s/avcrt_inject", exe);
    if (access(inj, X_OK) != 0) return -1;
    char pidstr[16];
    snprintf(pidstr, sizeof(pidstr), "%d", target);
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execl(inj, inj, pidstr, payload, (char*)nullptr);
        _exit(127);   // exec failed
    }
    int st = 0;
    if (waitpid(child, &st, 0) < 0) return -1;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
}

void* afhook_monitor(void*) {
    const char* payload = getenv("AVCRT_AF_PAYLOAD");
    if (!payload || payload[0] != '/') {
        fprintf(stderr, "[avcrtd] AudioFlinger: no unique module overlay payload; skipping\n");
        return nullptr;
    }
    pid_t  last_injected = 0;
    time_t last_inject_t = 0;
    int    strikes       = 0;       // quick post-inject restarts (suspected hook crash)
    bool   disabled      = false;
    pid_t  attempt_pid   = 0;
    pid_t  observed_pid  = 0;
    int    attempts      = 0;
    const int kCrashWindowSec = 12; // a restart within this of our inject = suspect us
    const int kMaxStrikes     = 3;
    const int kMaxAttemptsPerPid = 3;

    while (g_run.load()) {
        if (disabled) { for (int i = 0; i < 10 && g_run.load(); ++i) sleep_ms(500); continue; }

        pid_t as = pidof_one("audioserver");
        if (as == 0) { sleep_ms(1000); continue; }                  // not up yet

        // ALREADY injected (by us, or a previous avcrtd instance the supervisor respawned
        // over)? Adopt it — re-injecting a second copy would inline-hook the already-patched
        // prologue and corrupt it. This is the steady state.
        if (afhook_in_maps(as)) {
            observed_pid = as;
            if (as != last_injected) {
                last_injected = as;
                if (last_inject_t == 0) last_inject_t = time(nullptr);
            }
            sleep_ms(2000);
            continue;
        }

        // audioserver is NEW and NOT injected (boot, or a restart that dropped our hook).
        time_t now = time(nullptr);
        const bool new_pid = as != observed_pid;
        if (new_pid) observed_pid = as;
        if (new_pid && as != last_injected && last_inject_t != 0 && now - last_inject_t < kCrashWindowSec) {
            // it respawned right after we injected → our hook likely crashed it.
            if (++strikes >= kMaxStrikes) {
                fprintf(stderr, "[avcrtd] afhook: audioserver restarted %d× shortly after inject — "
                                "DISABLING afhook (audio left clean)\n", strikes);
                disabled = true;
                continue;
            }
        } else if (new_pid && as != last_injected) {
            strikes = 0;   // healthy/independent restart
        }

        sleep_ms(1500);   // let audioserver finish initializing
        if (!g_run.load()) break;
        as = pidof_one("audioserver");
        if (as == 0) continue;
        if (as != attempt_pid) { attempt_pid = as; attempts = 0; }
        if (attempts >= kMaxAttemptsPerPid) { sleep_ms(2000); continue; }
        const std::string target_path = "/proc/" + std::to_string(as) + "/root" + payload;
        struct stat st{};
        if (stat(target_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            attempts = kMaxAttemptsPerPid;
            fprintf(stderr, "[avcrtd] AudioFlinger: overlay not visible to pid=%d; skipping without attaching\n", as);
            continue;
        }
        ++attempts;

        if (run_injector(as, payload) == 0) {
            sleep_ms(300);
            if (afhook_in_maps(as)) {
                observed_pid = as;
                last_injected = as;
                last_inject_t = time(nullptr);
                fprintf(stderr, "[avcrtd] AudioFlinger payload mapped in pid=%d; see avcrtaf for hook status\n", as);
                continue;
            }
        }
        fprintf(stderr, "[avcrtd] afhook load failed for pid=%d attempt=%d/%d%s\n",
                as, attempts, kMaxAttemptsPerPid,
                attempts == kMaxAttemptsPerPid ? " — waiting for new audioserver PID" : "");
        sleep_ms(2000);
    }
    return nullptr;
}

// The "HAMMER" for an accessory change mid-call. Instead of surgically adapting our routing
// to each plug/unplug (the fragile morph/re-assert web — too many {built-in,analog,USB} ×
// {plug,unplug} combinations, each with different AHAL behavior), we RESET: the caller first
// disengages (clears all our mixer hacks), then this restarts audioserver so the AHAL rebuilds
// the call's routing from scratch on the NEW device. PROVEN on device: the modem call survives;
// the voice session drops ~3-5s then re-establishes. Afterwards the idle loop re-engages from a
// clean slate via detect_mode/engage — which self-gate on the voice route being back, so every
// transition reduces to the already-working fresh-call (static) case. Deterministic, no per-
// transition special cases. The daemon runs as root (magisk context) → setprop ctl.restart works
// (same context as the verified manual test).
// A crude fingerprint of the current accessory/uplink state — to tell a genuine NEW transition
// (different state → its hammer is legit) from a loop (SAME state re-firing → suppress).
int accessory_fp() {
    int usb = (find_usb_card() >= 0) ? 1 : 0;
    int s = 0, aif = 0;
    discover_codec_uplink(s, aif);            // aif 0 (none) / 1 (TX_3) / 2 (TX_4)
    return usb * 100 + aif;
}

static time_t g_last_hammer = 0;
static int    g_last_hammer_fp = -1;
void restart_audioserver() {
    // RATE-LIMIT — but STATE-AWARE. A hammer is the right tool for each DIFFERENT accessory transition
    // (type-c→built-in→analog = 3 legit hammers). The danger is a LOOP: the SAME state re-firing
    // hammers in a tight cycle → brick audio. So: suppress a hammer ONLY if it's the SAME accessory
    // state as the last hammer AND within ~8s (= a re-fire/loop). A DIFFERENT state (a real new
    // transition) is ALWAYS allowed — even right after another hammer. (The pure time-based limit
    // wrongly suppressed the minijack-after-type-c transition's hammer → the CVP latch wasn't cleared
    // → additive.)
    time_t now = time(nullptr);
    int fp = accessory_fp();
    if (g_last_hammer != 0 && now - g_last_hammer < 8 && fp == g_last_hammer_fp) {
        fprintf(stderr, "[avcrtd] HAMMER suppressed (same state fp=%d, %lds ago) — loop guard, settle only\n",
                fp, (long)(now - g_last_hammer));
        for (int i = 0; i < 15 && g_run.load(); ++i) sleep_ms(200);   // ~3s settle, no restart
        return;
    }
    g_last_hammer = now;
    g_last_hammer_fp = fp;
    fprintf(stderr, "[avcrtd] HAMMER: restart audioserver (rebuild call routing for the new device)\n");
    int rc = system("setprop ctl.restart audioserver");
    (void)rc;
    // wait for audioserver to respawn (init service), then settle for the AHAL to re-establish
    // the voice session. detect_mode won't engage until the voice-Tx route is back, so a modest
    // fixed settle is enough; the idle loop covers any remaining lag.
    for (int i = 0; i < 50 && g_run.load(); ++i) {        // up to ~10s for the new pid
        if (pidof_one("audioserver") > 0) { sleep_ms(200); break; }
        sleep_ms(200);
    }
    for (int i = 0; i < 25 && g_run.load(); ++i) sleep_ms(200);   // ~5s settle for the call audio
    resolve_cards();   // the restart may have RENUMBERED the cards — re-resolve lito + reopen g_mixer
}

// In-kernel CVP/CVS vocproc rebuild for the live call — the AVCRT-unified-kernel replacement for the
// "hammer". Writing the "AVCRT CVP Rebuild" kcontrol runs voc_disable_device()+voc_enable_device() on
// the active voice session: a full vocproc teardown+rebuild (re-sends VSS_IVOCPROC_CMD_SET_DEVICE with
// the current Tx port) that FLUSHES a stale CVP Tx-device latch. Unlike the hammer it does NOT restart
// audioserver, so it (a) is much faster, and (b) does NOT re-enumerate the USB sound card — which is
// exactly what made hammering-with-a-USB-card loop, so this finally fixes the direct minijack->type-c
// swap latch. Called while DISENGAGED (no open capture/inject PCMs to disrupt); the idle loop then
// re-engages cleanly on the settled accessory. Caller gates on g_cvp_rebuild_present.
bool cvp_rebuild() {
    struct mixer_ctl* ctl = g_mixer ? mixer_get_ctl_by_name(g_mixer, kCvpRebuildCtl) : nullptr;
    if (!ctl || mixer_ctl_set_value(ctl, 0, 1) < 0) {
        fprintf(stderr, "[avcrtd] CVP rebuild FAILED — control missing or kernel rejected request; no audioserver restart\n");
        return false;
    }
    fprintf(stderr, "[avcrtd] CVP rebuild request accepted — active-session result unverified\n");
    // let voc_disable_device+voc_enable_device complete (it pauses then restarts the voice path).
    for (int i = 0; i < 12 && g_run.load(); ++i) sleep_ms(100);   // ~1.2s
    resolve_cards();
    return true;
}

// After a mid-call device change / capture failure: clear our hacks, then recover the RIGHT way:
//  - USB accessory present  → the headset (reroute) flow handles it → just settle and let the idle
//    loop run engage_headset. NEVER hammer with a USB card present: restarting audioserver flickers
//    USB enumeration → engage_headset's usb_cap prime fails ("headset vanished") → built-in fallback
//    → the 0.5s check sees the USB card again → hammer → LOOP (the type-c regression).
//  - no USB card (pure codec built-in<->analog transition) → HAMMER (restart audioserver → AHAL
//    rebuilds the call routing cleanly → idle re-engages the codec flow). Proven for minijack.
int main(int argc, char** argv) {
    // Never let a typo/new probe flag silently start the audio-routing daemon.
    if (argc == 2) {
        if (strcmp(argv[1], "--unmute") == 0) return run_unmute();
        if (strcmp(argv[1], "--probe-kcontrol") == 0) return run_probe_kcontrol();
        if (strcmp(argv[1], "--probe-cvp") == 0) return run_probe_cvp();
        if (strcmp(argv[1], "--list-probes") == 0) {
            puts("--probe-kcontrol\n--probe-cvp");
            return 0;
        }
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--unmute|--probe-kcontrol|--probe-cvp|--list-probes]\n", argv[0]);
        return 2;
    }

    // SINGLE-INSTANCE GUARD. Two daemons fight over pcm 0,0 / 0,23 (capture+inject)
    // → "cannot prepare channel" / "Bad address" → flap → torn audio. A second
    // instance (e.g. a supervisor respawn while the old one is still stuck in a
    // D-state pcm syscall) must NOT start. flock an exclusive lock; if held, exit.
    int lock_fd = open("/data/local/tmp/avcrtd.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[avcrtd] another instance is running — exiting\n");
        return 0;
    }

    // Install handlers WITHOUT SA_RESTART so a blocked pcm_read/pcm_write returns
    // EINTR on SIGTERM/SIGINT -> the loop breaks and disengage() runs (unmute).
    // Without this, a stalled inject could hang the daemon while the mic stays
    // muted -> dead mic. This is the clean-stop path.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;                       // no SA_RESTART
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    resolve_cards();
    if (!g_mixer) fprintf(stderr, "[avcrtd] cellular routing unavailable; app capture remains independent\n");

    // GATING (per-flow, NOT a hard cellular kill-switch):
    //  - BUILT-IN mic GSM clean-replace needs the "AVCRT Dec Mute" kcontrol (AVCRT kernel patch).
    //    Absent on a stock kernel → we can't cleanly mute the codec mic → built-in flow disabled.
    //  - HEADSET (USB/adapter) GSM clean-replace needs NO patch (reroute to bolero + capture the freed
    //    headset mic + incall_music inject, all stock) → works on ANY kernel when a headset is on the call.
    // So the daemon RUNS regardless of the patch and engages whichever flow the live call supports.
    fprintf(stderr, "[avcrtd] AVCRT Dec Mute %s; codec GSM %s; headset support requires route validation.\n",
            g_patch_present ? "present" : "ABSENT",
            g_patch_present ? "enabled" : "disabled (stock kernel)");
    fprintf(stderr, "[avcrtd] CVP-rebuild kcontrol %s -> accessory recovery via %s.\n",
            g_cvp_rebuild_present ? "present" : "absent",
            g_cvp_rebuild_present ? "optional in-kernel request after route settles" : "rollback and settle only");

    ConfigStore cfg;
    Engine engine(kRate, kChannels);
    Session s;
    int64_t applied_revision = -1;
    const size_t frames = kPeriod;
    const size_t bytes  = frames * kChannels * sizeof(int16_t);
    int16_t* buf = new int16_t[frames * kChannels];
    int16_t* usbbuf = new int16_t[kUsbPeriod];        // headset 48k read buffer (decimated into buf)

    // Background call-state poller (keeps dumpsys OUT of the audio loop). ALWAYS runs — both the
    // built-in and headset flows need is_in_call().
    pthread_t mon;
    bool mon_ok = pthread_create(&mon, nullptr, call_monitor, nullptr) == 0;

    // Optional AudioFlinger monitor, separate from cellular capture.
    pthread_t afmon;
    const char* af_payload = getenv("AVCRT_AF_PAYLOAD");
    bool afmon_ok = af_payload && af_payload[0] == '/' &&
                    pthread_create(&afmon, nullptr, afhook_monitor, nullptr) == 0;

    fprintf(stderr, "[avcrtd] service started (idle poll %d ms, patch=%s, af monitor=%s)\n",
            kIdlePollMs, g_patch_present ? "yes" : "no", afmon_ok ? "on" : "off");

    DspConfig dc;
    int  end_misses = 0;                 // consecutive "call not up" checks (×~0.5s)
    int  iters_since_check = 0;
    const int kCheckEvery = 30;          // re-check on/session every ~30 audio iters (~0.5s)
    using Clock = std::chrono::steady_clock;
    RouteSample observed;
    int failures = 0;
    int good_periods = 0;
    int off_polls = 0;
    bool bypass = false;
    RouteSample rejected_codec;
    bool cvp_pending = false;
    auto retry_at = Clock::time_point{};
    auto last_cvp = Clock::time_point{};

    auto apply_preset = [&]() {
        avcrt::DspConfig ec = dc;
        ec.enabled = true;
        ec.time_ratio = 1.0;
        ec.gain_db = dc.gain_db + kInjectGainCompDb;
        engine.update(ec);
        applied_revision = dc.revision;
    };

    auto rollback = [&](const char* reason, AccCheck accessory, bool retry_failure) {
        const int usb_now = find_usb_card();
        const bool usb_gone = s.mode == MODE_HEADSET && usb_now != s.usb_card;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        fprintf(stderr, "[avcrtd] gsm rollback: t_ms=%lld reason=%s route=%s mode=%d session=%d aif=%d mask=0x%x usb=%d->%d\n",
                static_cast<long long>(ms), reason,
                accessory == ACC_CHANGED ? "changed" : accessory == ACC_SAME ? "same" : "unknown",
                s.mode, s.session, s.modem_aif, s.modem_mask, s.usb_card, usb_now);
        const int previous_mode = s.mode;
        disengage(s, usb_gone);
        observed = {};
        end_misses = 0;
        good_periods = 0;
        retry_at = Clock::now() + std::chrono::milliseconds(600);
        if (retry_failure && ++failures >= 3) {
            bypass = true;
            fprintf(stderr, "[avcrtd] gsm bypass: repeated failures; no re-engage until engine off or call ends\n");
        }
        // First let HAL settle the codec-to-codec transition on its own.
        cvp_pending = !bypass && previous_mode != MODE_BUILTIN && g_cvp_rebuild_present && accessory == ACC_CHANGED;
    };

    while (g_run.load()) {
        // ---- engaged: stay in the tight audio loop; only re-check call/engine
        //      state every ~0.5s (NOT every iteration — those reads add jitter), and
        //      require kEndHysteresis consecutive misses before disengaging so a
        //      momentary mCallState/mixer blip doesn't flap us. ----
        if (s.engaged) {
            if (++iters_since_check >= kCheckEvery) {
                iters_since_check = 0;
                bool on = engine_enabled(cfg, dc);
                // While engaged we do NOT re-run detect_mode(): in headset mode our own reroute
                // (bolero on / USB off) would make it misread the mode → flap. Just check the call
                // is still up + the engine is still on.
                bool call_up = is_in_call();

                AccCheck accessory = call_up && on ? check_accessory(s) : ACC_SAME;
                if (accessory != ACC_SAME) {
                    rollback("route-check", accessory, accessory == ACC_UNKNOWN);
                    continue;
                }
                if (call_up && on && s.uplink_touched && !mx_any_on(s.vtx_bolero) &&
                    !mx_any_on(s.mode == MODE_HEADSET ? s.vtx_usb : s.vtx_bt)) {
                    rollback("external uplink unavailable", ACC_UNKNOWN, true);
                    continue;
                }

                if (!on || !call_up) {
                    if (!on || ++end_misses >= 3) {
                        fprintf(stderr, "[avcrtd] call/engine ended (on=%d call=%d) — disengage\n",
                                (int)on, (int)call_up);
                        rollback("engine-or-call-off", ACC_SAME, false);
                        continue;
                    }
                } else {
                    end_misses = 0;
                    // HEADSET: if the AHAL re-applied USB routing (our reroute got wiped → the real
                    // USB mic is back in the uplink = additive), re-assert the reroute.
                    if (s.mode == MODE_HEADSET && s.vtx_usb[0] && mx_any_on(s.vtx_usb)) {
                        fprintf(stderr, "[avcrtd] headset: AHAL re-applied USB uplink — re-asserting reroute\n");
                        if (!mx_set(s.vtx_usb, 0) || !mx_set(s.vtx_bolero, 1) ||
                            (g_patch_present && !set_session_mute(s, 0xFF))) {
                            rollback("USB reroute write", ACC_SAME, true);
                            continue;
                        }
                    }
                    // BT: same re-assert — if the AHAL put the BT SCO mic back on the uplink, re-reroute.
                    if (s.mode == MODE_BT && s.vtx_bt[0] && mx_any_on(s.vtx_bt)) {
                        fprintf(stderr, "[avcrtd] bt: AHAL re-applied BT uplink — re-asserting reroute\n");
                        if (!mx_set(s.vtx_bt, 0) || !mx_set(s.vtx_bolero, 1) ||
                            (g_patch_present && !set_session_mute(s, 0xFF))) {
                            rollback("BT reroute write", ACC_SAME, true);
                            continue;
                        }
                    }
                }
                // apply preset on revision change
                if (on && dc.revision != applied_revision) apply_preset();
            }
            // ---- audio iteration ----
            if (s.mode == MODE_HEADSET) {
                // Drain the BE-keeper (keeps the bolero placeholder powered). BEST-EFFORT — a read
                // error here is NOT "headset gone": tearing down on it left the call rerouted to a
                // dead placeholder = 0 sound (the mid-call-plug bug). Reopen once; if it still fails,
                // DROP it and carry on — the real capture (usb_cap) + incall_music inject deliver the
                // pitched voice regardless. (Seen on a built-in→headset transition: the bolero MM1
                // capture EFAULTs when the voice path takes over TX_CDC_DMA_TX_3.)
                if (s.be_keeper) {
                    int16_t bk[kPeriod];
                    if (pcm_read(s.be_keeper, bk, sizeof(bk)) != 0) {
                        fprintf(stderr, "[avcrtd] be_keeper read err: %s — reopen (non-fatal)\n", pcm_get_error(s.be_keeper));
                        pcm_close(s.be_keeper); s.be_keeper = nullptr;
                        pcm_config bcfg = make_cfg();
                        struct pcm* bk2 = pcm_open(g_card, g_cap_device, PCM_IN, &bcfg);
                        if (bk2 && pcm_is_ready(bk2)) {
                            int16_t t[kPeriod];
                            if (pcm_read(bk2, t, sizeof(t)) == 0) s.be_keeper = bk2;
                            else pcm_close(bk2);
                        } else if (bk2) { pcm_close(bk2); }
                        // s.be_keeper may stay null → we simply stop draining (placeholder stays
                        // configured; the inject still reaches the uplink). Do NOT disengage.
                    }
                }
                if (pcm_read(s.usb_cap, usbbuf, kUsbPeriod * sizeof(int16_t)) != 0) {
                    fprintf(stderr, "[avcrtd] usb_cap read err: %s\n", pcm_get_error(s.usb_cap));
                    AccCheck result = check_accessory(s);
                    rollback("USB capture", result, result != ACC_CHANGED);
                    continue;
                }
                decimate_3to1(usbbuf, buf, frames);     // 48k 768 -> 16k 256
                engine.process(buf, frames);
            } else if (pcm_read(s.cap, buf, bytes) != 0) {
                fprintf(stderr, "[avcrtd] cap read err: %s\n", pcm_get_error(s.cap));
                AccCheck result = check_accessory(s);
                rollback("codec/BT capture", result, result != ACC_CHANGED);
                continue;
            } else {
                engine.process(buf, frames);
            }
            if (pcm_write(s.inj, buf, bytes) != 0) {
                fprintf(stderr, "[avcrtd] inj write err: %s\n", pcm_get_error(s.inj));
                AccCheck result = check_accessory(s);
                rollback("inject write", result, result != ACC_CHANGED);
                continue;
            }
            if (good_periods < 2 * kCheckEvery && ++good_periods == 2 * kCheckEvery) failures = 0;
            continue;
        }

        // ---- idle: poll for (engine on && a supported call), then engage once ----
        bool on = engine_enabled(cfg, dc);
        if (!on || !is_in_call()) {
            observed = {};
            if (!on || ++off_polls >= 3) {
                failures = 0;
                bypass = false;
                cvp_pending = false;
                rejected_codec = {};
            }
        } else {
            off_polls = 0;
        }
        if (g_mute_clear_pending) {
            Session cleanup;
            g_mute_clear_pending = !set_session_mute(cleanup, 0);
            sleep_ms(kIdlePollMs);
            continue;
        }
        if (!on || !is_in_call() || bypass || g_codec_rollback_failed || Clock::now() < retry_at) {
            sleep_ms(kIdlePollMs);
            continue;
        }
        resolve_cards();
        RouteSample current = sample_route();
        if (current.mode == MODE_NONE || !(current == observed)) {
            observed = current;
            sleep_ms(kIdlePollMs);
            continue;
        }
        if (current == rejected_codec) { sleep_ms(kIdlePollMs); continue; }
        rejected_codec = {};

        if (cvp_pending) {
            cvp_pending = false;
            if (last_cvp == Clock::time_point{} || Clock::now() - last_cvp >= std::chrono::seconds(5)) {
                last_cvp = Clock::now();
                if (!cvp_rebuild()) {
                    bypass = true;
                    fprintf(stderr, "[avcrtd] gsm bypass: CVP request rejected; no re-engage until engine off or call ends\n");
                }
                observed = {};  // Require a fresh pair of samples after the kernel request.
                continue;
            }
        }
        Mode m = current.mode;
        int session = current.session;
        fprintf(stderr, "[avcrtd] gsm route stable: mode=%d session=%d aif=%d mask=0x%x usb=%d mic=%s/%s\n",
                m, session, current.aif, current.mask, current.usb, current.mic.dec_mux, current.mic.sub_mux);

        apply_preset();
        bool ok = (m == MODE_BT)      ? engage_bt(s, session)
                : (m == MODE_HEADSET) ? engage_headset(s, session)
                                      : engage(s, session);
        if (!ok) {
            if (m == MODE_BUILTIN) {
                rejected_codec = current;
                fprintf(stderr, "[avcrtd] gsm codec route rejected; leaving stock until route changes or engine is toggled\n");
            }
            rollback("engage failed", ACC_UNKNOWN, m != MODE_BUILTIN);
            continue;
        }
        end_misses = 0;
        iters_since_check = 0;
    }

    disengage(s);
    if (mon_ok) pthread_join(mon, nullptr);
    if (afmon_ok) pthread_join(afmon, nullptr);
    delete[] buf;
    delete[] usbbuf;
    if (g_mixer) mixer_close(g_mixer);
    fprintf(stderr, "[avcrtd] service stopped\n");
    return 0;
}
