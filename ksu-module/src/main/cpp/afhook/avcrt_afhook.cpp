// avcrt_afhook — universal, system-wide voice change at the AudioFlinger HAL-read layer.
//
// Loaded (via root ptrace remote-dlopen) into the audioserver process. INLINE-hooks the ONE
// chokepoint every mic capture passes through, regardless of which app/process owns it:
//
//     android::AudioStreamIn::read(void* buffer, size_t bytes, size_t* read)
//        in /system/lib64/libaudioflinger_datapath.so
//
// AudioFlinger's RecordThread::threadLoop pulls the raw mic from the input HAL through this for
// EVERY input stream, at the device's native format, BEFORE distributing to per-track consumers.
// Pitching the buffer here changes the voice for EVERYTHING — apps (AudioRecord/OpenSL/AAudio),
// MediaRecorder captures that run in mediaserver (OnePlus camera video, screen recorders, …), the
// lot — in a single place. This is what the per-process Zygisk hook can't reach (mediaserver is a
// non-zygote init service).
//
// Format/rate/channels: AudioStreamIn's own getters aren't exported, but
//     android::AudioStreamIn::getAudioProperties() const   (exported)
// returns audio_config_base_t{sample_rate, channel_mask, format} by value — we query it ONCE per
// stream object and cache it.
//
// Reuses the proven avcrt::Engine (SoundTouch + the Phase-6 effect chain) and the self-checking
// avcrt_inlinehook.h (skips gracefully if a ROM's prologue isn't copy-safe — never crashes).
// Config comes from the avcrt.dsp.* system properties, same as the Zygisk hook.
//
// IMPORTANT: when this hook is active it pitches the shared HAL mic, so the Zygisk obtainBuffer
// hook MUST be disabled (else apps get double-pitched). This hook supersedes Zygisk.

#include <dlfcn.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <android/log.h>

#include "avcrt_inlinehook.h"
#include "avcrt_engine.h"
#include "avcrt_config.h"   // DspConfig

#define LOG_TAG "avcrtaf"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

using avcrt::DspConfig;
using avcrt::Engine;

namespace {

// ---- live config snapshot (mirrored from avcrt.dsp.* props), identical layout to the Zygisk hook ----
struct WireConfig {
    int32_t enabled;
    int32_t revision;
    double  pitch;
    double  formant;
    double  gain;
    int32_t preserve_formants;
    int32_t crispness;
    DspConfig fx{};
};

std::mutex g_cfg_mu;
WireConfig g_cfg = {0, -1, 0, 0, 0, 1, 5};

double prop_d(const char *key, double def) {
    char buf[48] = {0};
    if (__system_property_get(key, buf) <= 0) return def;
    return strtod(buf, nullptr);
}
int prop_i(const char *key, int def) {
    char buf[48] = {0};
    if (__system_property_get(key, buf) <= 0) return def;
    return (int)strtol(buf, nullptr, 10);
}
int prop_revision() { return prop_i("avcrt.config.revision", -1); }

int parse_csv(const char *key, double *out, int maxn) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(key, buf) <= 0) return 0;
    const char *p = buf;
    int i = 0;
    while (i < maxn && *p) {
        char *end;
        double v = strtod(p, &end);
        if (end == p) break;
        out[i++] = v;
        p = end;
        if (*p == ',') p++;
    }
    return i;
}

// Parse the grouped avcrt.dsp.fx* CSV props into the effect sub-structs. Field order is the
// CONTRACT shared with the Kotlin packer (HalEngine.writeConfig) — kept identical to zygisk_avcrt.
void parse_fx_props(DspConfig &fx) {
    double a[8];
    int n;
    n = parse_csv("avcrt.dsp.fxgate", a, 4);
    if (n >= 4) { fx.gate = {a[0] != 0, a[1], a[2], a[3]}; }
    n = parse_csv("avcrt.dsp.fxeq", a, 5);
    if (n >= 4) { fx.eq = {a[0] != 0, a[1], a[2], a[3]}; }
    if (n >= 5) { fx.telephone.enabled = a[4] != 0; }
    n = parse_csv("avcrt.dsp.fxmod", a, 7);
    if (n >= 3) { fx.ringmod = {a[0] != 0, a[1], a[2]}; }
    if (n >= 7) { fx.tremolo = {a[3] != 0, a[4], a[5], static_cast<int>(a[6])}; }
    n = parse_csv("avcrt.dsp.fxdrive", a, 3);
    if (n >= 3) { fx.distortion = {a[0] != 0, a[1], a[2]}; }
    n = parse_csv("avcrt.dsp.fxtime", a, 4);
    if (n >= 4) { fx.echo = {a[0] != 0, a[1], a[2], a[3]}; }
    n = parse_csv("avcrt.dsp.fxverb", a, 5);
    if (n >= 5) { fx.reverb = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    n = parse_csv("avcrt.dsp.fxfilter", a, 4);
    if (n >= 4) { fx.filter = {a[0] != 0, a[1], a[2], a[3]}; }
    n = parse_csv("avcrt.dsp.fxmod2", a, 7);
    if (n >= 3) { fx.vibrato = {a[0] != 0, a[1], a[2]}; }
    if (n >= 7) { fx.chorus  = {a[3] != 0, a[4], a[5], a[6]}; }
    n = parse_csv("avcrt.dsp.fxflanger", a, 5);
    if (n >= 5) { fx.flanger = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    n = parse_csv("avcrt.dsp.fxphaser", a, 5);
    if (n >= 5) { fx.phaser = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    n = parse_csv("avcrt.dsp.fxwah", a, 5);
    if (n >= 5) { fx.autowah = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    n = parse_csv("avcrt.dsp.fxcrush", a, 4);
    if (n >= 4) { fx.bitcrush = {a[0] != 0, a[1], a[2], a[3]}; }
    n = parse_csv("avcrt.dsp.fxcomp", a, 6);
    if (n >= 6) { fx.compressor = {a[0] != 0, a[1], a[2], a[3], a[4], a[5]}; }
    n = parse_csv("avcrt.dsp.fxformant", a, 2);
    if (n >= 2) { fx.formant = {a[0] != 0, a[1]}; }
    n = parse_csv("avcrt.dsp.fxvocoder", a, 4);
    if (n >= 4) { fx.vocoder = {a[0] != 0, a[1], static_cast<int>(a[2]), a[3]}; }
    n = parse_csv("avcrt.dsp.fxwhisper", a, 2);
    if (n >= 2) { fx.whisper = {a[0] != 0, a[1]}; }
    n = parse_csv("avcrt.dsp.fxspectral", a, 6);
    if (n >= 6) { fx.spectral = {a[0] != 0, a[1] != 0, a[2] != 0, a[3] != 0, a[4], a[5]}; }
    n = parse_csv("avcrt.dsp.fxautotune", a, 2);
    if (n >= 2) { fx.autotune = {a[0] != 0, a[1]}; }
    n = parse_csv("avcrt.dsp.fxharmon", a, 4);
    if (n >= 4) { fx.harmonizer = {a[0] != 0, static_cast<int>(a[1]), static_cast<int>(a[2]), a[3]}; }
}

void refresh_config() {
    WireConfig wc{};
    wc.revision = prop_i("avcrt.config.revision", -1);
    wc.enabled  = prop_i("avcrt.dsp.enabled", 0);
    wc.pitch    = prop_d("avcrt.dsp.pitch", 0.0);
    wc.formant  = prop_d("avcrt.dsp.formant", 0.0);
    wc.gain     = prop_d("avcrt.dsp.gain", 0.0);
    wc.preserve_formants = prop_i("avcrt.dsp.preserve_formants", 1);
    wc.crispness = prop_i("avcrt.dsp.crispness", 5);
    parse_fx_props(wc.fx);
    static int dbg = 0;
    if (dbg < 3) { LOGI("cfg from props: enabled=%d rev=%d pitch=%.1f formant=%.1f gain=%.1f",
                        wc.enabled, wc.revision, wc.pitch, wc.formant, wc.gain); dbg++; }
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    g_cfg = wc;
}

WireConfig current_config() {
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    return g_cfg;
}

void maybe_refresh_config() {
    static std::atomic<int> since{100000};
    int rev = prop_revision();
    WireConfig cur = current_config();
    if (cur.revision < 0 || (rev >= 0 && rev != cur.revision) || since.fetch_add(1) > 300) {
        refresh_config();
        since.store(0);
    }
}

// ---- format query: AudioStreamIn::getAudioProperties() const returns audio_config_base_t by value
// (sample_rate@0, channel_mask@4, format@8; 12 bytes → AAPCS64 returns it in x0:x1, no sret). ----
struct AfConfigBase { uint32_t sample_rate; uint32_t channel_mask; uint32_t format; };
using getprops_fn = AfConfigBase (*)(const void *);
getprops_fn g_getAudioProperties = nullptr;

// audio_format_t (stable ABI): PCM_16_BIT=0x1, PCM_FLOAT=0x5. We pitch 16-bit and float; else skip.
int bps_for_format(uint32_t fmt) {
    if (fmt == 0x1) return 2;
    if (fmt == 0x5) return 4;
    return 0;
}
uint32_t channels_from_mask(uint32_t mask) {
    uint32_t c = static_cast<uint32_t>(__builtin_popcount(mask & 0x3FFFFFu));
    if (c < 1) c = 1;
    if (c > 8) c = 8;
    return c;
}

// ---- per-stream pitch engine, keyed by AudioStreamIn* ----
struct StreamCtx {
    Engine  *engine = nullptr;
    uint32_t rate = 0;
    uint32_t channels = 0;
    int      bps = 0;            // 2=int16, 4=float, 0=passthrough (probe failed / unsupported fmt)
    int64_t  applied_rev = -2;
    bool     probed = false;
    std::vector<int16_t> i16;   // scratch for float<->int16
};
std::mutex g_streams_mu;
std::map<const void *, StreamCtx *> g_streams;

StreamCtx *ctx_for(const void *self) {
    std::lock_guard<std::mutex> lk(g_streams_mu);
    auto it = g_streams.find(self);
    StreamCtx *c;
    if (it == g_streams.end()) { c = new StreamCtx(); g_streams[self] = c; }
    else                        { c = it->second; }
    if (!c->probed) {
        c->probed = true;       // probe exactly once per stream (getAudioProperties is a live HAL query)
        AfConfigBase ac{0, 0, 0};
        if (g_getAudioProperties) ac = g_getAudioProperties(self);
        uint32_t rate = ac.sample_rate;
        uint32_t ch   = channels_from_mask(ac.channel_mask);
        int bps       = bps_for_format(ac.format);
        if (rate < 4000 || rate > 192000) { rate = 0; bps = 0; }  // nonsense → passthrough
        c->rate = rate; c->channels = ch; c->bps = bps;
        if (bps != 0 && rate != 0) {
            c->engine = new Engine(rate, ch);
            c->applied_rev = -2;
        }
        static int dbg = 0;
        if (dbg < 8) { LOGI("stream %p props: rate=%u ch=%u fmt=0x%x -> bps=%d %s",
                            self, ac.sample_rate, ch, ac.format, bps,
                            c->engine ? "PITCHED" : "passthrough"); dbg++; }
    }
    return c;
}

void apply_cfg(StreamCtx *c, const WireConfig &cfg) {
    if (c->applied_rev == cfg.revision) return;
    DspConfig dc = cfg.fx;
    dc.enabled           = true;
    dc.pitch_semitones   = cfg.pitch;
    dc.formant_semitones = cfg.formant;
    dc.time_ratio        = 1.0;
    dc.gain_db           = cfg.gain;
    dc.preserve_formants = cfg.preserve_formants != 0;
    dc.crispness         = cfg.crispness;
    dc.revision          = cfg.revision;
    c->engine->update(dc);
    c->applied_rev = cfg.revision;
}

// Pitch `frames` of interleaved PCM in place. bps=2 → int16; bps=4 → 32-bit float (via int16 scratch).
void process_pcm(StreamCtx *c, void *raw, size_t frames, uint32_t channels, int bps) {
    if (frames == 0 || !c->engine) return;
    if (bps == 2) {
        c->engine->process(static_cast<int16_t *>(raw), frames);
    } else {  // bps == 4 → float PCM
        size_t n = frames * channels;
        if (c->i16.size() < n) c->i16.resize(n);
        float *fb = static_cast<float *>(raw);
        for (size_t i = 0; i < n; i++) {
            float v = fb[i] * 32768.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            c->i16[i] = static_cast<int16_t>(lrintf(v));
        }
        c->engine->process(c->i16.data(), frames);
        for (size_t i = 0; i < n; i++) fb[i] = static_cast<float>(c->i16[i]) * (1.0f / 32768.0f);
    }
}

// ---- the hook: AudioStreamIn::read(void* buffer, size_t bytes, size_t* read) ----
using read_fn = int (*)(void * /*this*/, void * /*buffer*/, size_t /*bytes*/, size_t * /*read*/);
read_fn orig_read = nullptr;

int hooked_read(void *self, void *buffer, size_t bytes, size_t *read) {
    int st = orig_read(self, buffer, bytes, read);
    if (st != 0 || buffer == nullptr || read == nullptr) return st;  // data valid only on NO_ERROR(0)
    size_t got = *read;
    if (got == 0) return st;

    StreamCtx *c = ctx_for(self);
    if (c->bps != 2 && c->bps != 4) return st;   // unsupported format / probe failed → passthrough

    maybe_refresh_config();
    WireConfig cfg = current_config();
    if (!cfg.enabled) return st;

    uint32_t channels = c->channels ? c->channels : 1;
    size_t frame_size = static_cast<size_t>(channels) * static_cast<size_t>(c->bps);
    if (frame_size == 0) return st;
    size_t frames = got / frame_size;
    if (frames == 0) return st;

    static int dbg = 0;
    if (dbg < 5) { LOGI("read FIRED self=%p bytes=%zu frames=%zu ch=%u bps=%d en=%d rev=%d",
                        self, got, frames, channels, c->bps, cfg.enabled, cfg.revision); dbg++; }

    apply_cfg(c, cfg);
    process_pcm(c, buffer, frames, channels, c->bps);
    return st;
}

// ---- hook installer ----
std::atomic<bool> g_hooked{false};

void try_install() {
    if (g_hooked.load()) return;
    void *h = dlopen("libaudioflinger_datapath.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return;
    void *addr = dlsym(h, "_ZN7android13AudioStreamIn4readEPvmPm");
    g_getAudioProperties = (getprops_fn)dlsym(h, "_ZNK7android13AudioStreamIn18getAudioPropertiesEv");
    if (!addr) { LOGW("AudioStreamIn::read symbol not found"); return; }
    g_hooked.store(true);
    refresh_config();
    int reason = 0;
    void *tr = avcrt::avcrt_inline_hook(addr, (void *)hooked_read, &reason);
    if (tr) { orig_read = (read_fn)tr;
              LOGI("inline-hooked AudioStreamIn::read @%p (getProps=%p)", addr, (void *)g_getAudioProperties); }
    else    LOGW("AudioStreamIn::read hook SKIPPED @%p reason=%d (1=prologue 2=write 3=mem)", addr, reason);
}

void *install_thread(void *) {
    // libaudioflinger_datapath is always loaded in audioserver; retry briefly just in case.
    for (int i = 0; i < 50 && !g_hooked.load(); i++) { try_install(); usleep(100000); }
    return nullptr;
}

}  // namespace

// Runs when the injector dlopen's us into audioserver. Do the work off the linker lock (a detached
// thread) to avoid any dlopen/dlsym re-entrancy under the loader.
__attribute__((constructor)) static void avcrt_afhook_init() {
    LOGI("avcrt afhook loaded (pid=%d)", getpid());
    pthread_t t;
    if (pthread_create(&t, nullptr, install_thread, nullptr) == 0) pthread_detach(t);
}
