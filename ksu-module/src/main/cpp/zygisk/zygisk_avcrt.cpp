// zygisk_avcrt — system-wide voice change for in-app audio capture.
//
// Loaded by Zygisk into every app process. INLINE-hooks the native capture chokepoints in
// libaudioclient/libaaudio and pitch-shifts the captured buffer in place via avcrt::Engine, at the
// app's native rate/format — best quality (no codec round-trip, no cellular vocoder).
//
// Optional native capture hooks, in addition to Java AudioRecord JNI:
//   1. android::AudioRecord::obtainBuffer(Buffer*, const timespec*, ...) in libaudioclient — the
//      ring-buffer access point that read() (MediaRecorder, voice messages, recorders) AND the
//      internal AudioRecordThread callback path (Telegram VOIP via OpenSL) both funnel through.
//      Requires the supported private ABI and exported client PCM getters.
//   2. AAudioStream_read in libaaudio — adds AAudio-MMAP-EXCLUSIVE capture, which bypasses
//      android::AudioRecord entirely (direct shared-memory ring buffer with the HAL). A thread-local
//      guard makes legacy-AAudio (which calls AudioRecord underneath) pitch ONCE, at the AAudio level.
//
// Native entry points must pass the AArch64 prologue checks in avcrt_inlinehook.h.
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
#include <new>
#include <vector>
#include <type_traits>

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <STTypes.h>

#include "avcrt_inlinehook.h"

#include "avcrt_engine.h"
#include "avcrt_paths.h"
#include "avcrt_config.h"   // DspConfig struct
#include "avcrt_capture_contract.h"
#include "avcrt_direct_aac.h"
#include "avcrt_pcm24.h"
#include "avcrt_recorder.h"
#include "avcrt_stock_capture.h"
#include "avcrt_af_backend.h"

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

namespace pcm = avcrt::capture_contract;

// ---- hooked-function trampolines ----
// status_t AudioRecord::obtainBuffer(Buffer*, const timespec* requested, timespec* elapsed,
//   size_t* nonContig)  — x0=this,x1=Buffer*,x2..x4; returns status_t (0=NO_ERROR).
using obtain_fn = int (*)(void * /*this*/, void * /*Buffer*/, const void *, void *, void *);
std::atomic<obtain_fn> orig_obtain{nullptr};
// int32_t AAudioStream_read(AAudioStream*, void* buffer, int32_t numFrames, int64_t timeoutNanos)
//   — returns frames read (>=0) or a negative aaudio_result_t. (Blocking-mode AAudio capture.)
using aaudio_read_fn = decltype(&AAudioStream_read);
std::atomic<aaudio_read_fn> orig_aaudio_read{nullptr};
// void AAudioStreamBuilder_setDataCallback(builder, AAudioStream_dataCallback, void* userData) —
// we wrap the app's data callback so CALLBACK-mode capture (incl. MMAP-exclusive, used by games) is
// pitched too. The callback ABI: int32_t cb(stream, userData, audioData, numFrames).
using set_data_cb_fn = decltype(&AAudioStreamBuilder_setDataCallback);
std::atomic<set_data_cb_fn> orig_set_data_cb{nullptr};
struct CbCtx { AAudioStream_dataCallback app_cb; void *app_ud; };

// Leading fields of the supported 64-bit AOSP AudioRecord::Buffer ABI. This
// private layout is not a public guarantee; matching exports alone cannot prove it.
struct AudioRecordBufferPrefix { size_t frames; size_t bytes; void* raw; };

// ---- getters ----
using getter_u32 = uint32_t (*)(const void *);
using getter_i32 = int32_t  (*)(const void *);
getter_u32 g_ar_rate = nullptr;
getter_u32 g_ar_channels = nullptr;
getter_i32 g_ar_format = nullptr;
decltype(&AAudioStream_getFormat) g_aa_getFormat = nullptr;
decltype(&AAudioStream_getChannelCount) g_aa_getChannelCount = nullptr;
decltype(&AAudioStream_getSampleRate) g_aa_getSampleRate = nullptr;
decltype(&AAudioStream_getDirection) g_aa_getDirection = nullptr;

// Guard: legacy AAudio's AAudioStream_read internally calls AudioRecord (→ obtainBuffer). When set,
// the obtainBuffer hook passes through so we pitch the capture exactly once (at the AAudio level).
thread_local int g_in_aaudio = 0;
thread_local bool g_in_java_read = false;
// Per-callback-cycle flag: obtainBuffer sets it when it owns the samples (legacy callback-mode reads go
// through AudioRecord). The data-callback wrapper consumes it: if set, the underlying obtainBuffer
// already pitched → skip (don't double); if clear (MMAP path, no AudioRecord) → pitch here.
thread_local bool g_obtained = false;

// ---- shared config ----
std::mutex          g_cfg_mu;
WireConfig          g_cfg = {0, -1, 0, 0, 0, 1, 5};
zygisk::Api        *g_api = nullptr;

// Keep the app-process DSP separate from server/diagnostic backends.

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
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(key, buf) <= 0) return def;
    return strtod(buf, nullptr);
}
int prop_i(const char *key, int def) {
    char buf[PROP_VALUE_MAX] = {0};
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
    parse_fx_props(wc.fx);
    wc.fx.bypass_voice = prop_i(avcrt::paths::kBypassVoiceProperty, 0) != 0;
    static std::atomic<unsigned> dbg{0};
    if (dbg.load() < 3 && dbg.fetch_add(1) < 3) { LOGI("cfg from props: enabled=%d rev=%d pitch=%.1f formant=%.1f gain=%.1f "
                        "fx[gate=%d eq=%d ring=%d trem=%d dist=%d echo=%d verb=%d]",
                        wc.enabled, wc.revision, wc.pitch, wc.formant, wc.gain,
                        wc.fx.gate.enabled, wc.fx.eq.enabled, wc.fx.ringmod.enabled,
                        wc.fx.tremolo.enabled, wc.fx.distortion.enabled, wc.fx.echo.enabled,
                         wc.fx.reverb.enabled); }
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    g_cfg = wc;
}

WireConfig current_config() {
    std::lock_guard<std::mutex> lk(g_cfg_mu);
    return g_cfg;
}

// Ownership is independent of the preset revision. Check it at the DSP gate
// instead of leaving a stale app backend enabled until the next preset refresh.
bool server_capture_active() {
    return avcrt::af_backend::claimed() || prop_i("debug.avcrt.af_capture", 0) != 0 ||
           prop_i("debug.avcrt.stagefright", 0) != 0;
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
        c->engine = new Engine(rate, channels, /*soundboard=*/true);
        c->rate = rate; c->channels = channels; c->applied_rev = -2;
        c->last_raw = 0; c->last_end = 0;
    }
    return c;
}

DspConfig engine_config(const WireConfig& cfg) {
    DspConfig dc = cfg.fx;            // carries the effect sub-structs (Phase 6)
    dc.enabled           = true;
    dc.pitch_semitones   = cfg.pitch;
    dc.formant_semitones = cfg.formant;
    dc.time_ratio        = 1.0;
    dc.gain_db           = cfg.gain;
    dc.preserve_formants = cfg.preserve_formants != 0;
    dc.crispness         = cfg.crispness;
    dc.revision          = cfg.revision;
    return dc;
}

void apply_cfg(StreamCtx *c, const WireConfig &cfg) {
    if (c->applied_rev == cfg.revision) return;
    const auto dc = engine_config(cfg);
    c->engine->update(dc);
    c->applied_rev = cfg.revision;
}

// Pitch `frames` of interleaved PCM in place. bps=2 → int16; bps=4 → 32-bit float (via int16 scratch).
void process_pcm(StreamCtx *c, void *raw, size_t frames, uint32_t channels, int bps) {
    if (frames == 0) return;
    if (bps == 2) {
        c->engine->process(static_cast<int16_t *>(raw), frames);
    } else if (c->engine->voiceBypassed()) {
        c->engine->mixSound(static_cast<float*>(raw), frames);
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

void process_packed(StreamCtx* c, uint8_t* packed, size_t frames, uint32_t channels) {
    if (c->engine->voiceBypassed()) {
        c->engine->mixSoundPacked24(packed, frames);
        return;
    }
    const size_t count = frames * channels;
    if (c->i16.size() < count) c->i16.resize(count);
    for (size_t i = 0; i < count; ++i) c->i16[i] = avcrt::pcm24_to_s16(packed + i * 3);
    c->engine->process(c->i16.data(), frames);
    for (size_t i = 0; i < count; ++i) avcrt::s16_to_pcm24(c->i16[i], packed + i * 3);
}

// ===== Hook 1: android::AudioRecord::obtainBuffer =====
int hooked_obtain(void *self, void *audioBuffer, const void *requested, void *elapsed, void *nonContig) {
    const int st = orig_obtain.load()(self, audioBuffer, requested, elapsed, nonContig);
    if (st != 0 || !audioBuffer || g_in_aaudio > 0 || g_in_java_read) return st;
    maybe_refresh_config();
    WireConfig cfg = current_config();
    if (!cfg.enabled || server_capture_active()) return st;

    AudioRecordBufferPrefix buffer{};
    std::memcpy(&buffer, audioBuffer, sizeof(buffer));
    const auto rate = g_ar_rate(self);
    const auto channels = g_ar_channels(self);
    const auto sample = pcm::audio_record_sample(g_ar_format(self));
    const auto span = pcm::audio_record_span(buffer.raw, buffer.frames, buffer.bytes, rate,
                                             channels, sample, SOUNDTOUCH_MAX_CHANNELS);
    if (!span.bytes) return st;
    const auto frame_size = span.frame_bytes;
    StreamCtx *c = ctx_for(self, rate, channels);
    apply_cfg(c, cfg);

    // De-overlap: obtainBuffer re-returns the unconsumed tail of a prior chunk (obtain 960, consume
    // 192, release 192, re-obtain 768, …) — pitching the whole chunk each time re-pitches the overlap
    // 2–5× → "wind". Pitch only frames past the previously-processed end.
    uintptr_t start = reinterpret_cast<uintptr_t>(buffer.raw);
    uintptr_t end   = start + span.bytes;
    uintptr_t proc  = start;
    if (start >= c->last_raw && start < c->last_end) {
        proc = (c->last_end < end) ? c->last_end : end;  // skip already-pitched prefix
    }
    if ((proc - start) % frame_size) return st;
    c->last_raw = start;
    c->last_end = end;
    g_obtained = true; // The legacy callback must not process the same samples again.
    if (proc >= end) return st;
    size_t proc_frames = static_cast<size_t>(end - proc) / frame_size;
    process_pcm(c, reinterpret_cast<void *>(proc), proc_frames, channels, pcm::sample_bytes(sample));
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

jint recorder_read(JNIEnv* env, jobject self, jmethodID method, jshortArray buf, jint count) {
    JavaReadScope scope;
    return env->CallIntMethod(self, method, buf, 0, count, 1); // AudioRecord.READ_NON_BLOCKING.
}

bool stock_aac_config(DspConfig& result) {
    if (!avcrt::stock_capture::aac_allowed()) return false;
    maybe_refresh_config();
    const auto cfg = current_config();
    result = engine_config(cfg);
    return cfg.enabled != 0;
}

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
    if (!cfg.enabled || server_capture_active()) return;
    const jint rate = env->CallIntMethod(self, java_rate);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    const jint channels = env->CallIntMethod(self, java_channels);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    const jint format = env->CallIntMethod(self, java_format);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    // android.media.AudioFormat: ENCODING_PCM_16BIT, ENCODING_PCM_FLOAT,
    // and ENCODING_PCM_24BIT_PACKED (21). Packed PCM24 is handled below by
    // converting complete signed little-endian 24-bit samples through the
    // engine's native S16 interface; other encodings remain untouched.
    if (!pcm::supported_dimensions(rate, channels, SOUNDTOUCH_MAX_CHANNELS) || samples % channels ||
        (bps == 2 ? format != 2 : bps == 3 ? format != 21 : bps != 4 || format != 4)) return;
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
        ctx.engine = new Engine(rate, channels, /*soundboard=*/true);
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
    if (!orig_java_read<Array>) return -3; // No valid original; never recurse into this wrapper.
    jint got;
    { JavaReadScope scope; got = orig_java_read<Array>(env, self, array, offset, count, blocking); }
    if (env->ExceptionCheck() || !array || got <= 0 || offset < 0 || got > count ||
        offset > env->GetArrayLength(array) - got) return got;
    constexpr bool bytes = std::is_same_v<Array, jbyteArray>;
    constexpr bool floats = std::is_same_v<Array, jfloatArray>;
    const jint format = env->CallIntMethod(self, java_format);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return got; }
    const int bps = floats ? 4 : bytes ? (format == 21 ? 3 : 2) : 2;
    if ((bytes && got % bps) || (!bytes && format == 21)) return got;
    process_java(env, self, bytes ? got / bps : got, bps,
        [&](StreamCtx* ctx, size_t frames, unsigned channels) {
            // Work on a copy, not a pinned Java array or an unaligned byte-array offset.
            if constexpr (floats) {
                std::vector<jfloat> data(got);
                env->GetFloatArrayRegion(array, offset, got, data.data());
                if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
                process_pcm(ctx, data.data(), frames, channels, 4);
                env->SetFloatArrayRegion(array, offset, got, data.data());
            } else if constexpr (bytes) {
                if (bps == 3) {
                    std::vector<jbyte> packed(got);
                    env->GetByteArrayRegion(array, offset, got, packed.data());
                    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
                    process_packed(ctx, reinterpret_cast<uint8_t*>(packed.data()), frames, channels);
                    env->SetByteArrayRegion(array, offset, got, packed.data());
                } else {
                    std::vector<jshort> data(got / 2);
                    env->GetByteArrayRegion(array, offset, got, reinterpret_cast<jbyte*>(data.data()));
                    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
                    process_pcm(ctx, data.data(), frames, channels, 2);
                    env->SetByteArrayRegion(array, offset, got, reinterpret_cast<jbyte*>(data.data()));
                }
            } else {
                std::vector<jshort> data(got);
                env->GetShortArrayRegion(array, offset, got, data.data());
                if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
                process_pcm(ctx, data.data(), frames, channels, 2);
                env->SetShortArrayRegion(array, offset, got, data.data());
            }
            if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
            return true;
        });
    return got;
}

jint stock_java_direct(JNIEnv* env, jobject self, jobject buffer, jint count, jboolean blocking) {
    JavaReadScope scope;
    return orig_java_direct ? orig_java_direct(env, self, buffer, count, blocking) : -3;
}

jint hooked_java_direct(JNIEnv* env, jobject self, jobject buffer, jint count, jboolean blocking) {
    if (!g_in_java_read) {
        if (const auto replaced = avcrt::direct_aac::read(env, self, buffer, count, blocking)) return *replaced;
    }
    const jint got = stock_java_direct(env, self, buffer, count, blocking);
    if (env->ExceptionCheck() || !buffer || got <= 0 || got > count ||
        env->GetDirectBufferCapacity(buffer) < got) return got;
    void* data = env->GetDirectBufferAddress(buffer);
    if (!data) return got;
    const jint format = env->CallIntMethod(self, java_format);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return got; }
    const int bps = format == 2 ? 2 : format == 4 ? 4 : format == 21 ? 3 : 0;
    if (!bps || got % bps || (bps != 3 && reinterpret_cast<uintptr_t>(data) % bps)) return got;
    process_java(env, self, got / bps, bps, [&](StreamCtx* ctx, size_t frames, unsigned channels) {
        if (bps == 3) {
            process_packed(ctx, static_cast<uint8_t*>(data), frames, channels);
        } else process_pcm(ctx, data, frames, channels, bps);
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
    if (orig_java_read<jbyteArray> == hooked_java_read<jbyteArray>) orig_java_read<jbyteArray> = nullptr;
    if (orig_java_read<jshortArray> == hooked_java_read<jshortArray>) orig_java_read<jshortArray> = nullptr;
    if (orig_java_read<jfloatArray> == hooked_java_read<jfloatArray>) orig_java_read<jfloatArray> = nullptr;
    if (orig_java_direct == hooked_java_direct) orig_java_direct = nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGI("AudioRecord JNI hooks: byte=%d short=%d float=%d direct=%d", !!orig_java_read<jbyteArray>,
         !!orig_java_read<jshortArray>, !!orig_java_read<jfloatArray>, !!orig_java_direct);
}

// Pitch an AAudio capture buffer (stream's client format from the public getters). No overlap —
// AAudio fills a fresh buffer each read/callback.
void process_aaudio_stream(AAudioStream *stream, void *buffer, int32_t frames, const WireConfig &cfg) {
    const auto fmt = g_aa_getFormat(stream);
    const auto sample = fmt == AAUDIO_FORMAT_PCM_I16 ? pcm::Sample::S16 :
                        fmt == AAUDIO_FORMAT_PCM_FLOAT ? pcm::Sample::Float : pcm::Sample::Unsupported;
    const auto channels = g_aa_getChannelCount(stream);
    const auto rate = g_aa_getSampleRate(stream);
    if (!pcm::pcm_span(buffer, frames, rate, channels, sample, SOUNDTOUCH_MAX_CHANNELS).bytes) return;
    StreamCtx *c = ctx_for(stream, rate, channels);
    apply_cfg(c, cfg);
    process_pcm(c, buffer, frames, channels, pcm::sample_bytes(sample));
}

// ===== Hook 2: AAudioStream_read — AAudio BLOCKING-mode capture (incl. MMAP-blocking) =====
aaudio_result_t hooked_aaudio_read(AAudioStream *stream, void *buffer, int32_t numFrames, int64_t timeoutNs) {
    g_in_aaudio++;
    const auto got = orig_aaudio_read.load()(stream, buffer, numFrames, timeoutNs);
    g_in_aaudio--;
    if (!pcm::valid_read_result(got, numFrames) || !stream || !buffer ||
        g_aa_getDirection(stream) != AAUDIO_DIRECTION_INPUT) return got;
    maybe_refresh_config();
    WireConfig cfg = current_config();
    if (!cfg.enabled || server_capture_active()) return got;
    process_aaudio_stream(stream, buffer, got, cfg);
    return got;
}

// ===== Hook 3: AAudio data-callback wrapper — CALLBACK-mode capture (incl. MMAP-exclusive/games) =====
// Installed by wrapping the app's callback at AAudioStreamBuilder_setDataCallback time.
aaudio_data_callback_result_t my_data_cb(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames) {
    CbCtx *ctx = static_cast<CbCtx *>(userData);
    bool done = g_obtained; g_obtained = false;   // consume the per-cycle flag (always clear)
    // Gate on INPUT FIRST (cheap getter) so OUTPUT/playback callbacks — which we never touch — pay
    // no property/config work. Skip a legacy buffer already processed by obtainBuffer.
    if (numFrames > 0 && stream && audioData && !done &&
        g_aa_getDirection(stream) == AAUDIO_DIRECTION_INPUT) {
        maybe_refresh_config();
        WireConfig cfg = current_config();
        if (cfg.enabled && !server_capture_active()) {
            process_aaudio_stream(stream, audioData, numFrames, cfg);
        }
    }
    return ctx->app_cb(stream, ctx->app_ud, audioData, numFrames);
}

void hooked_set_data_callback(AAudioStreamBuilder *builder, AAudioStream_dataCallback callback, void *userData) {
    if (callback) {
        auto* ctx = new (std::nothrow) CbCtx{callback, userData};
        if (ctx) {
            orig_set_data_cb.load()(builder, my_data_cb, ctx);
            return;
        }
    }
    orig_set_data_cb.load()(builder, callback, userData);
}

bool resolve_ar_getters(void *h) {
    g_ar_rate = reinterpret_cast<getter_u32>(dlsym(h, "_ZNK7android11AudioRecord13getSampleRateEv"));
    g_ar_channels = reinterpret_cast<getter_u32>(dlsym(h, "_ZNK7android11AudioRecord12channelCountEv"));
    g_ar_format = reinterpret_cast<getter_i32>(dlsym(h, "_ZNK7android11AudioRecord6formatEv"));
    return g_ar_rate && g_ar_channels && g_ar_format;
}

// AAudioStream_read is often a 4-byte B-thunk to the real impl; follow an unconditional B so we hook
// the real function (which has a full, patchable prologue), not the un-patchable thunk.
void *follow_b_thunk(void *p) {
    if (!p) return p;
    uint32_t insn = *reinterpret_cast<uint32_t *>(p);
    if ((insn & 0xFC000000u) == 0x14000000u) {          // unconditional B
        // sign-extend the 26-bit imm: shift it up to bit31, then arithmetic-shift back down.
        int32_t imm26 = static_cast<int32_t>((insn & 0x03FFFFFFu) << 6) >> 6;
        return static_cast<uint8_t *>(p) + static_cast<intptr_t>(imm26) * 4;
    }
    return p;
}

// ---- inline-hook installers ----
// libaudioclient/libaaudio load AFTER fork (first audio use). A poll thread installs each hook once
// its lib appears. No linker monitor, no helper .so, no dlopen storm.
std::atomic<bool> g_obtain_checked{false};
std::atomic<bool> g_aaudio_checked{false};

template <typename Fn, std::atomic<Fn>& Original>
void install_native(const char* name, void* target, Fn replacement) {
    int reason = 0;
    const char* detail = nullptr;
    const auto trampoline = avcrt::avcrt_inline_hook(target, reinterpret_cast<void*>(replacement),
        &reason, &detail, [](void* original) { Original.store(reinterpret_cast<Fn>(original)); });
    if (trampoline) LOGI("hooked %s @%p", name, target);
    else LOGW("%s hook skipped: reason=%d detail=%s", name, reason, detail ? detail : "n/a");
}

void try_hooks() {
    if (!g_obtain_checked.load()) {
        void *h = dlopen("libaudioclient.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            void *addr = dlsym(h, "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pm");
            g_obtain_checked.store(true);
            if (addr && resolve_ar_getters(h)) {
                install_native<obtain_fn, orig_obtain>("AudioRecord::obtainBuffer", addr, hooked_obtain);
            } else LOGI("native AudioRecord skipped: client PCM ABI unavailable");
            if (!orig_obtain.load()) dlclose(h);
        }
    }
    if (!g_aaudio_checked.load()) {
        void *ha = dlopen("libaaudio.so", RTLD_NOW | RTLD_NOLOAD);
        if (ha) {
            g_aaudio_checked.store(true);
            g_aa_getFormat = reinterpret_cast<decltype(g_aa_getFormat)>(dlsym(ha, "AAudioStream_getFormat"));
            g_aa_getChannelCount = reinterpret_cast<decltype(g_aa_getChannelCount)>(dlsym(ha, "AAudioStream_getChannelCount"));
            g_aa_getSampleRate = reinterpret_cast<decltype(g_aa_getSampleRate)>(dlsym(ha, "AAudioStream_getSampleRate"));
            g_aa_getDirection = reinterpret_cast<decltype(g_aa_getDirection)>(dlsym(ha, "AAudioStream_getDirection"));
            if (g_aa_getFormat && g_aa_getChannelCount && g_aa_getSampleRate && g_aa_getDirection) {
                if (void* rd = dlsym(ha, "AAudioStream_read")) {
                    install_native<aaudio_read_fn, orig_aaudio_read>("AAudioStream_read", follow_b_thunk(rd), hooked_aaudio_read);
                }
                if (void* sc = dlsym(ha, "AAudioStreamBuilder_setDataCallback")) {
                    install_native<set_data_cb_fn, orig_set_data_cb>("AAudioStreamBuilder_setDataCallback",
                        follow_b_thunk(sc), hooked_set_data_callback);
                }
            } else LOGI("AAudio skipped: stream getters unavailable");
            if (!orig_aaudio_read.load() && !orig_set_data_cb.load()) dlclose(ha);
        }
    }
}

void *hook_poller(void *) {
    for (int i = 0; i < 300 && !(g_obtain_checked.load() && g_aaudio_checked.load()); i++) {
        try_hooks();
        usleep(200000);  // 200 ms; ~60 s budget
    }
    return nullptr;
}

class AvcrtModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { g_api = api; env_ = env; }

    void preAppSpecialize(zygisk::AppSpecializeArgs *) override {
        if (env_->PushLocalFrame(32) != JNI_OK) {
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            return;
        }
        install_java_read(g_api, env_);
        // Probe independently in Zygisk app processes, including when DSP is
        // off. The live switch and each session's contract govern replacement.
        if (orig_java_direct) {
            avcrt::direct_aac::install(g_api, env_, stock_java_direct, stock_aac_config);
        }
        avcrt::recorder::install(g_api, env_, recorder_read, stock_aac_config);
        env_->PopLocalFrame(nullptr);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        refresh_config();   // prime (prop may be unset on a fresh boot)
        try_hooks();        // hook now if the libs are already loaded…
        if (!(g_obtain_checked.load() && g_aaudio_checked.load())) {  // …else poll within the discovery budget
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

bool avcrt::stock_capture::aac_allowed() {
    return aac_allowed([](const char* key) {
        if (std::strcmp(key, avcrt::af_backend::kProperty) == 0) return avcrt::af_backend::claimed();
        return prop_i(key, 0) != 0;
    });
}

REGISTER_ZYGISK_MODULE(AvcrtModule)
