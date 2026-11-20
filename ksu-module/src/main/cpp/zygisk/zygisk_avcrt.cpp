// zygisk_avcrt — system-wide voice change for in-app audio capture.
//
// Loaded by Zygisk into every app process. INLINE-hooks the native capture chokepoints in
// libaudioclient/libaaudio and pitch-shifts the captured buffer in place via avcrt::Engine, at the
// app's native rate/format — best quality (no codec round-trip, no cellular vocoder).
//
// TWO hooks for full capture-API coverage:
//   1. android::AudioRecord::obtainBuffer(Buffer*, const timespec*, ...) in libaudioclient — the
//      ring-buffer access point that read() (MediaRecorder, voice messages, recorders) AND the
//      internal AudioRecordThread callback path (Telegram VOIP via OpenSL) both funnel through.
//      Covers Java AudioRecord + OpenSL ES + AAudio-LEGACY (shared mode goes through AudioRecord).
//   2. AAudioStream_read in libaaudio — adds AAudio-MMAP-EXCLUSIVE capture, which bypasses
//      android::AudioRecord entirely (direct shared-memory ring buffer with the HAL). A thread-local
//      guard makes legacy-AAudio (which calls AudioRecord underneath) pitch ONCE, at the AAudio level.
//
// WHY our own inline hooker (zygisk/avcrt_inlinehook.h): shadowhook is dead on Android 16 and Dobby
// doesn't compile for Android. Our hooker self-checks the prologue and gracefully SKIPS (never
// crashes) on a target it can't safely patch — so it's portable across devices/ROMs.
//
// The active preset lives at active_preset.json which app processes cannot read (SELinux); the
// app/daemon mirrors it to avcrt.dsp.* system properties (app-readable) as root, which we poll.
// We only re-read when the cheap avcrt.config.revision system property changes.

#include "zygisk.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <type_traits>

#include <android/log.h>

#include "avcrt_inlinehook.h"

#include "avcrt_engine.h"
#include "avcrt_config.h"   // DspConfig struct

#define LOG_TAG "avcrtzy"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

using avcrt::DspConfig;
using avcrt::Engine;

namespace {

// ---- live config snapshot (mirrored from avcrt.dsp.* props) ----
// Core voice fields (the proven path) + a DspConfig carrying the Phase-6 effect sub-structs (parsed
// from the grouped avcrt.dsp.fx* CSV props; only its effect members are used here).
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

// aaudio_format_t (NDK AAudio.h enum, sequential): I16=1, FLOAT=2, I24_PACKED=3, I32=4.
constexpr int32_t kAAudioI16   = 1;
constexpr int32_t kAAudioFloat = 2;

// ---- hooked-function trampolines ----
// status_t AudioRecord::obtainBuffer(Buffer*, const timespec* requested, timespec* elapsed,
//   size_t* nonContig)  — x0=this,x1=Buffer*,x2..x4; returns status_t (0=NO_ERROR).
using obtain_fn = int (*)(void * /*this*/, void * /*Buffer*/, const void *, void *, void *);
obtain_fn orig_obtain = nullptr;
// int32_t AAudioStream_read(AAudioStream*, void* buffer, int32_t numFrames, int64_t timeoutNanos)
//   — returns frames read (>=0) or a negative aaudio_result_t. (Blocking-mode AAudio capture.)
using aaudio_read_fn = int32_t (*)(void * /*stream*/, void *, int32_t, int64_t);
aaudio_read_fn orig_aaudio_read = nullptr;
// void AAudioStreamBuilder_setDataCallback(builder, AAudioStream_dataCallback, void* userData) —
// we wrap the app's data callback so CALLBACK-mode capture (incl. MMAP-exclusive, used by games) is
// pitched too. The callback ABI: int32_t cb(stream, userData, audioData, numFrames).
using aaudio_data_cb  = int32_t (*)(void * /*stream*/, void * /*userData*/, void * /*audioData*/, int32_t);
using set_data_cb_fn  = void (*)(void * /*builder*/, void * /*callback*/, void * /*userData*/);
set_data_cb_fn orig_set_data_cb = nullptr;
constexpr int32_t kAAudioInput = 1;  // AAUDIO_DIRECTION_INPUT (OUTPUT=0, INPUT=1)
struct CbCtx { aaudio_data_cb app_cb; void *app_ud; };

// AudioRecord::Buffer layout (stable for years): frameCount@0, size(bytes)@8, raw(union)@16.
constexpr size_t kBufOffFrameCount = 0;
constexpr size_t kBufOffSize       = 8;
constexpr size_t kBufOffRaw        = 16;

// ---- getters ----
using getter_u32 = uint32_t (*)(const void *);
using getter_i32 = int32_t  (*)(const void *);
getter_u32 g_getHalSampleRate   = nullptr;
getter_u32 g_getHalChannelCount = nullptr;
getter_i32 g_getHalFormat       = nullptr;   // resolved but unused for format (see hooked_obtain)
using aa_i32 = int32_t (*)(void *);
aa_i32 g_aa_getFormat       = nullptr;
aa_i32 g_aa_getChannelCount = nullptr;
aa_i32 g_aa_getSampleRate   = nullptr;
aa_i32 g_aa_getDirection    = nullptr;

// Guard: legacy AAudio's AAudioStream_read internally calls AudioRecord (→ obtainBuffer). When set,
// the obtainBuffer hook passes through so we pitch the capture exactly once (at the AAudio level).
thread_local int g_in_aaudio = 0;
thread_local bool g_in_java_read = false;
// Per-callback-cycle flag: obtainBuffer sets it when it pitches (legacy callback-mode reads go
// through AudioRecord). The data-callback wrapper consumes it: if set, the underlying obtainBuffer
// already pitched → skip (don't double); if clear (MMAP path, no AudioRecord) → pitch here.
thread_local bool g_obtained = false;

// ---- shared config ----
std::mutex          g_cfg_mu;
WireConfig          g_cfg = {0, -1, 0, 0, 0, 1, 5};
zygisk::Api        *g_api = nullptr;

// When the universal audioserver-level hook (afhook) is active, it ALREADY pitches every capture that
// flows through AudioFlinger's RecordThread (i.e. all AudioRecord/OpenSL/legacy-AAudio + mediaserver).
// In that case this in-process Zygisk hook must NOT pitch the same buffer again (double-pitch). We keep
// pitching ONLY the paths afhook physically can't see: AAudio-MMAP (exclusive/blocking) capture, which
// bypasses RecordThread via a direct DMA buffer. g_obtained marks "this capture went through
// android::AudioRecord (→ AudioFlinger → afhook already pitched it)" so we know what to skip.
std::atomic<bool>   g_afhook_active{false};

// per-capture-object pitch engines (keyed by AudioRecord* OR AAudioStream* — distinct pointers).
struct StreamCtx {
    Engine   *engine = nullptr;
    uint32_t  rate = 0;
    uint32_t  channels = 0;
    int64_t   applied_rev = -2;
    std::vector<int16_t> i16;   // scratch for FLOAT<->int16 conversion
    // De-overlap state (obtainBuffer only): the consumer can re-obtain the unconsumed tail of a
    // previous chunk; we pitch each frame exactly once by tracking the byte range already done.
    uintptr_t last_raw = 0;
    uintptr_t last_end = 0;
};
std::mutex                    g_streams_mu;
std::map<const void*, StreamCtx*> g_streams;

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

// Parse a comma-separated prop ("a,b,c,…") into up to maxn doubles. Returns count parsed.
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
// CONTRACT shared with the Kotlin packer (HalEngine.writeConfig). Missing/short props → defaults.
void parse_fx_props(DspConfig &fx) {
    double a[8];
    int n;
    // gate: en,thresholdDb,attackMs,releaseMs
    n = parse_csv("avcrt.dsp.fxgate", a, 4);
    if (n >= 4) { fx.gate = {a[0] != 0, a[1], a[2], a[3]}; }
    // eq: en,lowDb,midDb,highDb,telephoneEn
    n = parse_csv("avcrt.dsp.fxeq", a, 5);
    if (n >= 4) { fx.eq = {a[0] != 0, a[1], a[2], a[3]}; }
    if (n >= 5) { fx.telephone.enabled = a[4] != 0; }
    // mod: ringEn,carrierHz,ringMix,tremEn,tremRate,tremDepth,tremShape
    n = parse_csv("avcrt.dsp.fxmod", a, 7);
    if (n >= 3) { fx.ringmod = {a[0] != 0, a[1], a[2]}; }
    if (n >= 7) { fx.tremolo = {a[3] != 0, a[4], a[5], static_cast<int>(a[6])}; }
    // drive: distEn,drive,distMix
    n = parse_csv("avcrt.dsp.fxdrive", a, 3);
    if (n >= 3) { fx.distortion = {a[0] != 0, a[1], a[2]}; }
    // time: echoEn,timeMs,feedback,mix
    n = parse_csv("avcrt.dsp.fxtime", a, 4);
    if (n >= 4) { fx.echo = {a[0] != 0, a[1], a[2], a[3]}; }
    // verb: verbEn,roomSize,damping,wet,width
    n = parse_csv("avcrt.dsp.fxverb", a, 5);
    if (n >= 5) { fx.reverb = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    // ---- Tier-2 ----
    // filter: filterEn,hpHz,lpHz,q
    n = parse_csv("avcrt.dsp.fxfilter", a, 4);
    if (n >= 4) { fx.filter = {a[0] != 0, a[1], a[2], a[3]}; }
    // mod2: vibEn,vibRate,vibDepth,chorEn,chorRate,chorDepth,chorMix
    n = parse_csv("avcrt.dsp.fxmod2", a, 7);
    if (n >= 3) { fx.vibrato = {a[0] != 0, a[1], a[2]}; }
    if (n >= 7) { fx.chorus  = {a[3] != 0, a[4], a[5], a[6]}; }
    // flanger: flanEn,flanRate,flanDepth,flanFb,flanMix
    n = parse_csv("avcrt.dsp.fxflanger", a, 5);
    if (n >= 5) { fx.flanger = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    // phaser: phasEn,phasRate,phasDepth,phasFb,phasMix
    n = parse_csv("avcrt.dsp.fxphaser", a, 5);
    if (n >= 5) { fx.phaser = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    // wah: wahEn,sensitivity,baseHz,rangeHz,mix
    n = parse_csv("avcrt.dsp.fxwah", a, 5);
    if (n >= 5) { fx.autowah = {a[0] != 0, a[1], a[2], a[3], a[4]}; }
    // crush: crushEn,bits,downsample,mix
    n = parse_csv("avcrt.dsp.fxcrush", a, 4);
    if (n >= 4) { fx.bitcrush = {a[0] != 0, a[1], a[2], a[3]}; }
    // comp: compEn,thresholdDb,ratio,attackMs,releaseMs,makeupDb
    n = parse_csv("avcrt.dsp.fxcomp", a, 6);
    if (n >= 6) { fx.compressor = {a[0] != 0, a[1], a[2], a[3], a[4], a[5]}; }
    // ---- Tier-3 ----
    // formant: en,shift
    n = parse_csv("avcrt.dsp.fxformant", a, 2);
    if (n >= 2) { fx.formant = {a[0] != 0, a[1]}; }
    // vocoder: en,carrierHz,carrierType,mix
    n = parse_csv("avcrt.dsp.fxvocoder", a, 4);
    if (n >= 4) { fx.vocoder = {a[0] != 0, a[1], static_cast<int>(a[2]), a[3]}; }
    // whisper: en,mix
    n = parse_csv("avcrt.dsp.fxwhisper", a, 2);
    if (n >= 2) { fx.whisper = {a[0] != 0, a[1]}; }
    // ---- Phase 7 experimental spectral: robotEn,whisperEn,freezeEn,blurEn,blurAmt,specMix ----
    n = parse_csv("avcrt.dsp.fxspectral", a, 6);
    if (n >= 6) { fx.spectral = {a[0] != 0, a[1] != 0, a[2] != 0, a[3] != 0, a[4], a[5]}; }
    // autotune: en,strength
    n = parse_csv("avcrt.dsp.fxautotune", a, 2);
    if (n >= 2) { fx.autotune = {a[0] != 0, a[1]}; }
    // harmonizer: en,interval1,interval2,mix
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
    g_afhook_active.store(prop_i("avcrt.afhook.active", 0) != 0);
    parse_fx_props(wc.fx);
    static int dbg = 0;
    if (dbg < 3) { LOGI("cfg from props: enabled=%d rev=%d pitch=%.1f formant=%.1f gain=%.1f "
                        "fx[gate=%d eq=%d ring=%d trem=%d dist=%d echo=%d verb=%d]",
                        wc.enabled, wc.revision, wc.pitch, wc.formant, wc.gain,
                        wc.fx.gate.enabled, wc.fx.eq.enabled, wc.fx.ringmod.enabled,
                        wc.fx.tremolo.enabled, wc.fx.distortion.enabled, wc.fx.echo.enabled,
                        wc.fx.reverb.enabled); dbg++; }
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    g_cfg = wc;
}

WireConfig current_config() {
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    return g_cfg;
}

// Re-read props when primed (revision<0), on a revision change, or every ~300 calls.
void maybe_refresh_config() {
    static std::atomic<int> since{100000};
    int rev = prop_revision();
    WireConfig cur = current_config();
    if (cur.revision < 0 || (rev >= 0 && rev != cur.revision) || since.fetch_add(1) > 300) {
        refresh_config();
        since.store(0);
    }
}

StreamCtx *ctx_for(const void *self, uint32_t rate, uint32_t channels) {
    std::lock_guard<std::mutex> lk(g_streams_mu);
    auto it = g_streams.find(self);
    StreamCtx *c;
    if (it == g_streams.end()) { c = new StreamCtx(); g_streams[self] = c; }
    else                        { c = it->second; }
    if (!c->engine || c->rate != rate || c->channels != channels) {
        delete c->engine;
        c->engine = new Engine((int)rate, (int)channels);
        c->rate = rate; c->channels = channels; c->applied_rev = -2;
        c->last_raw = 0; c->last_end = 0;
    }
    return c;
}

void apply_cfg(StreamCtx *c, const WireConfig &cfg) {
    if (c->applied_rev == cfg.revision) return;
    DspConfig dc = cfg.fx;            // carries the effect sub-structs (Phase 6)
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
    if (frames == 0) return;
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

// ===== Hook 1: android::AudioRecord::obtainBuffer =====
int hooked_obtain(void *self, void *audioBuffer, const void *requested, void *elapsed, void *nonContig) {
    int st = orig_obtain(self, audioBuffer, requested, elapsed, nonContig);
    if (st != 0 || audioBuffer == nullptr) return st;   // data valid only on NO_ERROR(0)
    // Mark that this capture went through android::AudioRecord — i.e. through AudioFlinger's RecordThread,
    // where afhook (if active) already pitched it, and the path H3's data-callback wrapper must NOT
    // re-pitch. Set unconditionally (even before the gates / under the AAudio guard) so it's an honest
    // "this is an AudioFlinger-routed capture" marker.
    g_obtained = true;
    if (g_in_aaudio > 0 || g_in_java_read) return st;    // outer read owns this buffer
    if (g_afhook_active.load()) return st;               // afhook already pitched it at the HAL read

    uint8_t *b = static_cast<uint8_t *>(audioBuffer);
    size_t frames = *reinterpret_cast<size_t *>(b + kBufOffFrameCount);
    size_t bytes  = *reinterpret_cast<size_t *>(b + kBufOffSize);
    void  *raw    = *reinterpret_cast<void **>(b + kBufOffRaw);
    if (frames == 0 || raw == nullptr || bytes == 0) return st;

    uint32_t rate     = g_getHalSampleRate   ? g_getHalSampleRate(self)   : 16000;
    uint32_t channels = g_getHalChannelCount ? g_getHalChannelCount(self) : 1;
    if (rate == 0) rate = 16000;
    if (channels == 0) channels = 1;

    // Client sample format from the buffer itself: frame_size = size/frameCount, bps = frame_size/ch.
    // getHalChannelCount is the channel disambiguator (client ch == HAL ch in ~all mic cases); a rare
    // channel mismatch yields bps∉{2,4} → safe passthrough (no crash, no garbage).
    if (bytes % frames != 0) return st;
    size_t frame_size = bytes / frames;
    if (frame_size == 0 || frame_size % channels != 0) return st;
    size_t bps = frame_size / channels;

    static int dbg = 0;
    if (dbg < 5) {
        WireConfig c = current_config();
        LOGI("obtain FIRED frames=%zu bytes=%zu frameSz=%zu bps=%zu rate=%u ch=%u en=%d rev=%d",
             frames, bytes, frame_size, bps, rate, channels, c.enabled, c.revision);
        dbg++;
    }

    maybe_refresh_config();
    WireConfig cfg = current_config();
    if (!cfg.enabled) return st;
    if (bps != 2 && bps != 4) return st;

    StreamCtx *c = ctx_for(self, rate, channels);
    apply_cfg(c, cfg);

    // De-overlap: obtainBuffer re-returns the unconsumed tail of a prior chunk (obtain 960, consume
    // 192, release 192, re-obtain 768, …) — pitching the whole chunk each time re-pitches the overlap
    // 2–5× → "wind". Pitch only frames past the previously-processed end.
    uintptr_t start = reinterpret_cast<uintptr_t>(raw);
    uintptr_t end   = start + static_cast<uintptr_t>(frames) * frame_size;
    uintptr_t proc  = start;
    if (start >= c->last_raw && start < c->last_end) {
        proc = (c->last_end < end) ? c->last_end : end;  // skip already-pitched prefix
    }
    c->last_raw = start;
    c->last_end = end;
    if (proc >= end) return st;
    size_t proc_frames = static_cast<size_t>(end - proc) / frame_size;
    process_pcm(c, reinterpret_cast<void *>(proc), proc_frames, channels, static_cast<int>(bps));
    return st;
}

// Java AudioRecord copies samples into the caller's buffer. Hook its registered JNI
// methods through Zygisk; this does not rewrite the native AudioRecord entry point.
template <typename Array>
using java_read_fn = jint (*)(JNIEnv*, jobject, Array, jint, jint, jboolean);
template <typename Array> java_read_fn<Array> orig_java_read = nullptr;
using java_direct_fn = jint (*)(JNIEnv*, jobject, jobject, jint, jboolean);
java_direct_fn orig_java_direct = nullptr;
jmethodID java_rate, java_channels, java_format;

struct JavaReadScope {
    bool previous = g_in_java_read;
    JavaReadScope() { g_in_java_read = true; }
    ~JavaReadScope() { g_in_java_read = previous; }
};

struct JavaStream {
    jweak owner = nullptr;
    StreamCtx ctx;
    ~JavaStream() { delete ctx.engine; }
};
std::mutex java_mu;
std::vector<std::unique_ptr<JavaStream>> java_streams;

template <typename Process>
void process_java(JNIEnv* env, jobject self, size_t samples, int bps, Process process) {
    if (env->ExceptionCheck() || !samples || g_in_java_read) return;
    maybe_refresh_config();
    auto cfg = current_config();
    if (!cfg.enabled || g_afhook_active.load()) return;
    const jint rate = env->CallIntMethod(self, java_rate);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    const jint channels = env->CallIntMethod(self, java_channels);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    const jint format = env->CallIntMethod(self, java_format);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    // android.media.AudioFormat: ENCODING_PCM_16BIT and ENCODING_PCM_FLOAT.
    if (rate <= 0 || channels <= 0 || samples % channels ||
        (bps == 2 ? format != 2 : format != 4)) return;
    std::lock_guard<std::mutex> lock(java_mu);
    JavaStream* stream = nullptr;
    for (auto it = java_streams.begin(); it != java_streams.end();) {
        if (env->IsSameObject((*it)->owner, nullptr)) {
            env->DeleteWeakGlobalRef((*it)->owner);
            it = java_streams.erase(it);
        } else {
            if (env->IsSameObject((*it)->owner, self)) stream = it->get();
            ++it;
        }
    }
    if (!stream) {
        jweak owner = env->NewWeakGlobalRef(self);
        if (!owner) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
        auto entry = std::make_unique<JavaStream>();
        entry->owner = owner;
        stream = entry.get();
        java_streams.push_back(std::move(entry));
    }
    StreamCtx& ctx = stream->ctx;
    if (!ctx.engine || ctx.rate != static_cast<uint32_t>(rate) ||
        ctx.channels != static_cast<uint32_t>(channels)) {
        delete ctx.engine;
        ctx.engine = new Engine(rate, channels);
        ctx.rate = rate;
        ctx.channels = channels;
        ctx.applied_rev = -2;
    }
    apply_cfg(&ctx, cfg);
    const size_t frames = samples / channels;
    if (process(&ctx, frames, channels)) {
        static std::atomic<int> logged{0};
        if (logged.load() < 5 && logged.fetch_add(1) < 5)
            LOGI("java-read FIRED frames=%zu rate=%d ch=%d rev=%d", frames, rate, channels, cfg.revision);
    }
}

template <typename Array>
jint hooked_java_read(JNIEnv* env, jobject self, Array array, jint offset, jint count, jboolean blocking) {
    jint got;
    { JavaReadScope scope; got = orig_java_read<Array>(env, self, array, offset, count, blocking); }
    if (env->ExceptionCheck() || !array || got <= 0 || offset < 0 || got > count ||
        offset > env->GetArrayLength(array) - got) return got;
    constexpr bool bytes = std::is_same_v<Array, jbyteArray>;
    constexpr bool floats = std::is_same_v<Array, jfloatArray>;
    if (bytes && got % 2) return got;
    process_java(env, self, bytes ? got / 2 : got, floats ? 4 : 2,
        [&](StreamCtx* ctx, size_t frames, unsigned channels) {
            // Work on a copy, not a pinned Java array or an unaligned byte-array offset.
            if constexpr (floats) {
                std::vector<jfloat> data(got);
                env->GetFloatArrayRegion(array, offset, got, data.data());
                if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
                process_pcm(ctx, data.data(), frames, channels, 4);
                env->SetFloatArrayRegion(array, offset, got, data.data());
            } else {
                std::vector<jshort> data(bytes ? got / 2 : got);
                if constexpr (bytes) env->GetByteArrayRegion(array, offset, got, reinterpret_cast<jbyte*>(data.data()));
                else env->GetShortArrayRegion(array, offset, got, data.data());
                if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
                process_pcm(ctx, data.data(), frames, channels, 2);
                if constexpr (bytes) env->SetByteArrayRegion(array, offset, got, reinterpret_cast<jbyte*>(data.data()));
                else env->SetShortArrayRegion(array, offset, got, data.data());
            }
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
            return true;
        });
    return got;
}

jint hooked_java_direct(JNIEnv* env, jobject self, jobject buffer, jint count, jboolean blocking) {
    jint got;
    { JavaReadScope scope; got = orig_java_direct(env, self, buffer, count, blocking); }
    if (env->ExceptionCheck() || !buffer || got <= 0 || got > count ||
        env->GetDirectBufferCapacity(buffer) < got) return got;
    void* data = env->GetDirectBufferAddress(buffer);
    if (!data) return got;
    const jint format = env->CallIntMethod(self, java_format);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return got; }
    const int bps = format == 2 ? 2 : format == 4 ? 4 : 0;
    if (!bps || got % bps || reinterpret_cast<uintptr_t>(data) % bps) return got;
    process_java(env, self, got / bps, bps, [&](StreamCtx* ctx, size_t frames, unsigned channels) {
        process_pcm(ctx, data, frames, channels, bps);
        return true;
    });
    return got;
}

void install_java_read(zygisk::Api* api, JNIEnv* env) {
    jclass cls = env->FindClass("android/media/AudioRecord");
    if (!cls) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    auto getter = [&](const char* name) {
        auto id = env->GetMethodID(cls, name, "()I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        return id;
    };
    java_rate = getter("getSampleRate");
    java_channels = getter("getChannelCount");
    java_format = getter("getAudioFormat");
    env->DeleteLocalRef(cls);
    if (!java_rate || !java_channels || !java_format) return;
    JNINativeMethod methods[] = {
        {"native_read_in_byte_array", "([BIIZ)I", reinterpret_cast<void*>(hooked_java_read<jbyteArray>)},
        {"native_read_in_short_array", "([SIIZ)I", reinterpret_cast<void*>(hooked_java_read<jshortArray>)},
        {"native_read_in_float_array", "([FIIZ)I", reinterpret_cast<void*>(hooked_java_read<jfloatArray>)},
        {"native_read_in_direct_buffer", "(Ljava/lang/Object;IZ)I", reinterpret_cast<void*>(hooked_java_direct)},
    };
    if (!api->hookJniNativeMethods(env, "android/media/AudioRecord", methods, 4)) return;
    orig_java_read<jbyteArray> = reinterpret_cast<java_read_fn<jbyteArray>>(methods[0].fnPtr);
    orig_java_read<jshortArray> = reinterpret_cast<java_read_fn<jshortArray>>(methods[1].fnPtr);
    orig_java_read<jfloatArray> = reinterpret_cast<java_read_fn<jfloatArray>>(methods[2].fnPtr);
    orig_java_direct = reinterpret_cast<java_direct_fn>(methods[3].fnPtr);
    LOGI("AudioRecord JNI hooks: byte=%d short=%d float=%d direct=%d", !!orig_java_read<jbyteArray>,
         !!orig_java_read<jshortArray>, !!orig_java_read<jfloatArray>, !!orig_java_direct);
}

// Pitch an AAudio capture buffer (stream's client format from the public getters). No overlap —
// AAudio fills a fresh buffer each read/callback.
void process_aaudio_stream(void *stream, void *buffer, int32_t frames, const WireConfig &cfg) {
    int32_t fmt = g_aa_getFormat ? g_aa_getFormat(stream) : -1;
    int bps = (fmt == kAAudioI16) ? 2 : (fmt == kAAudioFloat) ? 4 : 0;
    if (bps == 0) return;  // I24_PACKED/I32/other → leave untouched (we only pitch 16-bit/float)
    uint32_t channels = g_aa_getChannelCount ? static_cast<uint32_t>(g_aa_getChannelCount(stream)) : 1;
    uint32_t rate     = g_aa_getSampleRate   ? static_cast<uint32_t>(g_aa_getSampleRate(stream))   : 16000;
    if (channels == 0) channels = 1;
    if (rate == 0) rate = 16000;
    StreamCtx *c = ctx_for(stream, rate, channels);
    apply_cfg(c, cfg);
    process_pcm(c, buffer, static_cast<size_t>(frames), channels, bps);
}

// ===== Hook 2: AAudioStream_read — AAudio BLOCKING-mode capture (incl. MMAP-blocking) =====
int32_t hooked_aaudio_read(void *stream, void *buffer, int32_t numFrames, int64_t timeoutNs) {
    g_obtained = false;    // reset: tells us afterwards whether orig went through android::AudioRecord
    g_in_aaudio++;
    int32_t got = orig_aaudio_read(stream, buffer, numFrames, timeoutNs);
    g_in_aaudio--;
    if (got <= 0 || buffer == nullptr) return got;
    maybe_refresh_config();
    WireConfig cfg = current_config();
    if (!cfg.enabled) return got;
    // If afhook is active AND this read went through AudioRecord (g_obtained → legacy AAudio →
    // AudioFlinger), afhook already pitched it at the HAL read → skip. Only MMAP-blocking AAudio
    // (which bypasses AudioFlinger, so g_obtained stays false) is pitched here when afhook is on.
    if (g_afhook_active.load() && g_obtained) return got;
    static int dbg = 0;
    if (dbg < 5) { LOGI("aaudio-read FIRED frames=%d en=%d rev=%d", got, cfg.enabled, cfg.revision); dbg++; }
    process_aaudio_stream(stream, buffer, got, cfg);
    return got;
}

// ===== Hook 3: AAudio data-callback wrapper — CALLBACK-mode capture (incl. MMAP-exclusive/games) =====
// Installed by wrapping the app's callback at AAudioStreamBuilder_setDataCallback time.
int32_t my_data_cb(void *stream, void *userData, void *audioData, int32_t numFrames) {
    CbCtx *ctx = static_cast<CbCtx *>(userData);
    bool done = g_obtained; g_obtained = false;   // consume the per-cycle flag (always clear)
    // Gate on INPUT FIRST (cheap getter) so OUTPUT/playback callbacks — which we never touch — pay
    // NO property read / config work in their real-time thread; pitch only the MMAP capture path
    // (done==true means the legacy path already pitched via obtainBuffer → don't double).
    if (numFrames > 0 && audioData && !done &&
        g_aa_getDirection && g_aa_getDirection(stream) == kAAudioInput) {
        maybe_refresh_config();
        WireConfig cfg = current_config();
        if (cfg.enabled) {
            static int dbg = 0;
            if (dbg < 5) { LOGI("aaudio-cb FIRED frames=%d en=%d rev=%d", numFrames, cfg.enabled, cfg.revision); dbg++; }
            process_aaudio_stream(stream, audioData, numFrames, cfg);
        }
    }
    return ctx->app_cb(stream, ctx->app_ud, audioData, numFrames);
}

void hooked_set_data_callback(void *builder, void *callback, void *userData) {
    if (callback) {
        CbCtx *ctx = new CbCtx{ reinterpret_cast<aaudio_data_cb>(callback), userData };
        orig_set_data_cb(builder, reinterpret_cast<void *>(my_data_cb), ctx);
    } else {
        orig_set_data_cb(builder, callback, userData);   // clearing the callback — pass through
    }
}

void resolve_ar_getters(void *h) {
    g_getHalSampleRate   = (getter_u32)dlsym(h, "_ZNK7android11AudioRecord16getHalSampleRateEv");
    g_getHalChannelCount = (getter_u32)dlsym(h, "_ZNK7android11AudioRecord18getHalChannelCountEv");
    g_getHalFormat       = (getter_i32)dlsym(h, "_ZNK7android11AudioRecord12getHalFormatEv");
}

// AAudioStream_read is often a 4-byte B-thunk to the real impl; follow an unconditional B so we hook
// the real function (which has a full, patchable prologue), not the un-patchable thunk.
void *follow_b_thunk(void *p) {
    if (!p) return p;
    uint32_t insn = *reinterpret_cast<uint32_t *>(p);
    if ((insn & 0xFC000000u) == 0x14000000u) {          // unconditional B
        // sign-extend the 26-bit imm: shift it up to bit31, then arithmetic-shift back down.
        int32_t imm26 = static_cast<int32_t>((insn & 0x03FFFFFFu) << 6) >> 6;
        return static_cast<uint8_t *>(p) + (static_cast<intptr_t>(imm26) << 2);
    }
    return p;
}

// ---- inline-hook installers ----
// libaudioclient/libaaudio load AFTER fork (first audio use). A poll thread installs each hook once
// its lib appears. No linker monitor, no helper .so, no dlopen storm.
std::atomic<bool> g_obtain_hooked{false};
std::atomic<bool> g_aaudio_hooked{false};

void try_hooks() {
    if (!g_obtain_hooked.load()) {
        void *h = dlopen("libaudioclient.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            void *addr = dlsym(h, "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pm");
            if (addr) {
                g_obtain_hooked.store(true);   // attempt once per resolve
                resolve_ar_getters(h);
                int reason = 0;
                const char *detail = nullptr;
                void *tr = avcrt::avcrt_inline_hook(addr, (void *)hooked_obtain, &reason, &detail);
                if (tr) { orig_obtain = (obtain_fn)tr; LOGI("hooked AudioRecord::obtainBuffer @%p", addr); }
                else    LOGW("obtainBuffer hook SKIPPED @%p reason=%d detail=%s", addr, reason, detail ? detail : "n/a");
            }
        }
    }
    if (!g_aaudio_hooked.load()) {
        void *ha = dlopen("libaaudio.so", RTLD_NOW | RTLD_NOLOAD);
        if (ha) {
            void *rd = dlsym(ha, "AAudioStream_read");
            void *sc = dlsym(ha, "AAudioStreamBuilder_setDataCallback");
            if (rd && sc) {
                g_aaudio_hooked.store(true);
                g_aa_getFormat       = (aa_i32)dlsym(ha, "AAudioStream_getFormat");
                g_aa_getChannelCount = (aa_i32)dlsym(ha, "AAudioStream_getChannelCount");
                g_aa_getSampleRate   = (aa_i32)dlsym(ha, "AAudioStream_getSampleRate");
                g_aa_getDirection    = (aa_i32)dlsym(ha, "AAudioStream_getDirection");
                int reason = 0;
                const char *detail = nullptr;
                // Hook 2: blocking AAudio read (follow the B-thunk to the real impl).
                void *rd_real = follow_b_thunk(rd);
                void *tr = avcrt::avcrt_inline_hook(rd_real, (void *)hooked_aaudio_read, &reason, &detail);
                if (tr) { orig_aaudio_read = (aaudio_read_fn)tr; LOGI("hooked AAudioStream_read @%p (real %p)", rd, rd_real); }
                else    LOGW("AAudioStream_read hook SKIPPED @%p reason=%d detail=%s", rd_real, reason, detail ? detail : "n/a");
                // Hook 3: data-callback registration (wrap the app callback for callback-mode capture).
                void *sc_real = follow_b_thunk(sc);
                reason = 0;
                void *tr2 = avcrt::avcrt_inline_hook(sc_real, (void *)hooked_set_data_callback, &reason, &detail);
                if (tr2) { orig_set_data_cb = (set_data_cb_fn)tr2; LOGI("hooked setDataCallback @%p (real %p)", sc, sc_real); }
                else     LOGW("setDataCallback hook SKIPPED @%p reason=%d detail=%s", sc_real, reason, detail ? detail : "n/a");
            }
        }
    }
}

void *hook_poller(void *) {
    for (int i = 0; i < 300 && !(g_obtain_hooked.load() && g_aaudio_hooked.load()); i++) {
        try_hooks();
        usleep(200000);  // 200 ms; ~60 s budget
    }
    return nullptr;
}

class AvcrtModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { g_api = api; env_ = env; }

    void preAppSpecialize(zygisk::AppSpecializeArgs *) override { install_java_read(g_api, env_); }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        refresh_config();   // prime (prop may be unset on a fresh boot)
        try_hooks();        // hook now if the libs are already loaded…
        if (!(g_obtain_hooked.load() && g_aaudio_hooked.load())) {  // …else poll until they load
            pthread_t t;
            if (pthread_create(&t, nullptr, hook_poller, nullptr) == 0) pthread_detach(t);
        }
        LOGI("avcrt zygisk init");
        // The module .so must stay mapped (our hooks + trampolines live in it) — no DLCLOSE.
    }
private:
    JNIEnv* env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(AvcrtModule)
