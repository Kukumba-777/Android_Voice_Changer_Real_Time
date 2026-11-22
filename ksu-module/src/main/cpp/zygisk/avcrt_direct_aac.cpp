#include "avcrt_direct_aac.h"

#include "avcrt_direct_aac_stream.h"
#include "avcrt_engine.h"
#include "avcrt_stock_capture.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "avcrt-direct-aac", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "avcrt-direct-aac", __VA_ARGS__)

namespace avcrt::direct_aac {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Mode = Journal::Mode;
constexpr int kCamcorder = 5, kAacLc = 10, kPcm16 = 2, kBuiltinMic = 15;
constexpr size_t kMaxTracked = 8;
constexpr unsigned kMaxReads = 8;
constexpr size_t kMaxInFlight = 16;
constexpr jint kInvalidOperation = -3;
constexpr char kMime[] = "audio/mp4a-latm";

bool clear_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

struct Frame {
    JNIEnv* env;
    bool ok;
    explicit Frame(JNIEnv* e) : env(e), ok(e->PushLocalFrame(20) == 0) {}
    ~Frame() { if (ok) env->PopLocalFrame(nullptr); }
};

struct Java {
    jclass activity{};
    jmethodID application{}, service{}, config{}, equals{}, muted{}, silenced{};
    jmethodID format{}, source{}, rate{}, channels{}, port{}, client_port{}, client_source{};
    jmethodID client_format{}, config_format{}, config_rate{}, config_channels{};
    jmethodID device{}, device_type{}, device_id{};
} j;

bool resolve(JNIEnv* env, jclass audio) {
    Frame frame(env);
    if (!frame.ok) return false;
#define CLASS(var, name) const jclass var = env->FindClass(name); if (!var) return false
#define METHOD(field, cls, name, sig) if (!(j.field = env->GetMethodID(cls, name, sig))) return false
    CLASS(activity, "android/app/ActivityThread");
    CLASS(context, "android/content/Context");
    CLASS(manager, "android/media/AudioManager");
    CLASS(config, "android/media/AudioRecordingConfiguration");
    CLASS(format, "android/media/AudioFormat");
    CLASS(device, "android/media/AudioDeviceInfo");
    j.application = env->GetStaticMethodID(activity, "currentApplication", "()Landroid/app/Application;");
    if (!j.application) return false;
    METHOD(service, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    METHOD(format, audio, "getAudioFormat", "()I");
    METHOD(source, audio, "getAudioSource", "()I");
    METHOD(rate, audio, "getSampleRate", "()I");
    METHOD(channels, audio, "getChannelCount", "()I");
    METHOD(port, audio, "getPortId", "()I");
    METHOD(config, audio, "getActiveRecordingConfiguration", "()Landroid/media/AudioRecordingConfiguration;");
    METHOD(equals, config, "equals", "(Ljava/lang/Object;)Z");
    METHOD(client_port, config, "getClientPortId", "()I");
    METHOD(client_source, config, "getClientAudioSource", "()I");
    METHOD(client_format, config, "getClientFormat", "()Landroid/media/AudioFormat;");
    METHOD(config_format, format, "getEncoding", "()I");
    METHOD(config_rate, format, "getSampleRate", "()I");
    METHOD(config_channels, format, "getChannelCount", "()I");
    METHOD(silenced, config, "isClientSilenced", "()Z");
    METHOD(device, config, "getAudioDevice", "()Landroid/media/AudioDeviceInfo;");
    METHOD(device_id, device, "getId", "()I");
    METHOD(device_type, device, "getType", "()I");
    METHOD(muted, manager, "isMicrophoneMute", "()Z");
#undef METHOD
#undef CLASS
    j.activity = static_cast<jclass>(env->NewGlobalRef(activity));
    return j.activity != nullptr;
}

struct CodecDelete { void operator()(AMediaCodec* ptr) const { AMediaCodec_delete(ptr); } };
struct FormatDelete { void operator()(AMediaFormat* ptr) const { AMediaFormat_delete(ptr); } };
using Codec = std::unique_ptr<AMediaCodec, CodecDelete>;
using Format = std::unique_ptr<AMediaFormat, FormatDelete>;

struct Transcode {
    const int rate, channels;
    Codec decoder, encoder;
    std::unique_ptr<Engine> engine;
    Bytes<2 * adts::kMaxFrame> input, output;
    std::array<int16_t, 2 * adts::kSamples> pcm{};
    bool decoded = false, decoder_format = false, encoder_format = false, encoder_config = false;
    uint64_t submitted = 0, forwarded = 0, encoded = 0;
    int64_t revision = -1;
    const char* error = nullptr;

    Transcode(int r, int c) : rate(r), channels(c) {}

    bool start(size_t frame) {
        const int index = adts::rate_index(rate);
        const uint8_t asc[] = {uint8_t((2 << 3) | (index >> 1)),
                               uint8_t(((index & 1) << 7) | (channels << 3))};
        // Estimate a starting bitrate from the original access unit duration
        // and payload. The encoder negotiates the final bitrate; no vendor
        // AudioSystem parameters or muxer metadata are rewritten.
        const int bitrate = std::clamp(int((frame - adts::kHeader) * 8 * rate / adts::kSamples),
                                       16000, 320000);
        auto make = [&](bool encode) -> Codec {
            Format format(AMediaFormat_new());
            if (!format) return {};
            AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, kMime);
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, rate);
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_PCM_ENCODING, kPcm16);
            if (encode) {
                AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_AAC_PROFILE, 2);
                AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
            } else {
                AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, adts::kMaxFrame);
                AMediaFormat_setBuffer(format.get(), "csd-0", asc, sizeof asc);
            }
            Codec codec(encode ? AMediaCodec_createEncoderByType(kMime) : AMediaCodec_createDecoderByType(kMime));
            if (!codec || AMediaCodec_configure(codec.get(), format.get(), nullptr, nullptr,
                                              encode ? AMEDIACODEC_CONFIGURE_FLAG_ENCODE : 0) != AMEDIA_OK ||
                AMediaCodec_start(codec.get()) != AMEDIA_OK) return {};
            return codec;
        };
        decoder = make(false);
        if (decoder) encoder = make(true);
        return decoder && encoder;
    }

    uint64_t time_us(uint64_t n) const { return n * adts::kSamples * 1000000 / rate; }

    bool fail(const char* why) { error = why; return false; }

    enum class Result { Again, Done, Error };

    Result queue(AMediaCodec* codec, const void* bytes, size_t count, uint64_t n) {
        const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return Result::Again;
        if (idx < 0) return Result::Error;
        size_t capacity = 0;
        auto* dst = AMediaCodec_getInputBuffer(codec, idx, &capacity);
        if (!dst || count > capacity) return Result::Error;
        std::memcpy(dst, bytes, count);
        return AMediaCodec_queueInputBuffer(codec, idx, 0, count, time_us(n), 0) == AMEDIA_OK
            ? Result::Done : Result::Error;
    }

    bool format(AMediaCodec* codec, bool encode) {
        Format fmt(AMediaCodec_getOutputFormat(codec));
        int32_t r = 0, c = 0, pcm_encoding = kPcm16; // Android codec default if not advertised.
        const char* mime = nullptr;
        if (!fmt || !AMediaFormat_getString(fmt.get(), AMEDIAFORMAT_KEY_MIME, &mime) || !mime ||
            std::strcmp(mime, encode ? kMime : "audio/raw") != 0 ||
            !AMediaFormat_getInt32(fmt.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, &r) || r != rate ||
            !AMediaFormat_getInt32(fmt.get(), AMEDIAFORMAT_KEY_CHANNEL_COUNT, &c) || c != channels) return false;
        if (!encode) {
            AMediaFormat_getInt32(fmt.get(), AMEDIAFORMAT_KEY_PCM_ENCODING, &pcm_encoding);
            return pcm_encoding == kPcm16;
        }
        void* data = nullptr;
        size_t size = 0;
        if (!AMediaFormat_getBuffer(fmt.get(), "csd-0", &data, &size)) return true;
        return encoder_config = adts::matches_asc(static_cast<const uint8_t*>(data), size, rate, channels);
    }

    bool drain(bool encode, bool& progress) {
        if (encode ? output.room() < adts::kMaxFrame : decoded) return true;
        auto* codec = encode ? encoder.get() : decoder.get();
        AMediaCodecBufferInfo info{};
        const auto idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        progress = true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            const bool ok = format(codec, encode);
            (encode ? encoder_format : decoder_format) = ok;
            return ok || fail(encode ? "encoder output format" : "decoder output format");
        }
        if (idx < 0) return fail(encode ? "encoder dequeue" : "decoder dequeue");
        const bool config = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
        bool ok = info.size >= 0 && !(info.flags & ~(AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG |
                                                    AMEDIACODEC_BUFFER_FLAG_KEY_FRAME));
        if (ok && info.size > 0) {
            size_t capacity = 0;
            const auto* data = AMediaCodec_getOutputBuffer(codec, idx, &capacity);
            // NDK buffers may return a range-adjusted pointer. This bridge
            // intentionally supports only the unambiguous zero-offset case.
            ok = data && info.offset == 0 && size_t(info.size) <= capacity;
            if (ok && encode && config) {
                encoder_config = ok = adts::matches_asc(data, info.size, rate, channels);
            } else if (ok && encode) {
                uint8_t header[adts::kHeader];
                ok = encoder_format && encoder_config && encoded < forwarded &&
                    adts::header(header, info.size, rate, channels) &&
                    output.append(header, sizeof header) && output.append(data, info.size);
                if (ok) ++encoded;
            } else if (ok) {
                ok = !config && decoder_format && forwarded < submitted &&
                    info.presentationTimeUs == static_cast<int64_t>(time_us(forwarded)) &&
                    size_t(info.size) == channels * adts::kSamples * sizeof(int16_t);
                if (ok) { std::memcpy(pcm.data(), data, info.size); decoded = true; }
            }
        }
        if (AMediaCodec_releaseOutputBuffer(codec, idx, false) != AMEDIA_OK) ok = false;
        return ok || fail(encode ? "unsupported encoder buffer or AAC config" : "unsupported decoder PCM buffer");
    }

    bool pump(const DspConfig& cfg) {
        // Codec calls use zero dequeue timeouts. Bound both queue sizes and
        // the amount of work performed before handing control back to the app.
        for (int step = 0; step < 32; ++step) {
            bool progress = false;
            if (!drain(true, progress) || !drain(false, progress)) return false;
            if (decoded) {
                const size_t n = channels * adts::kSamples;
                const ssize_t idx = AMediaCodec_dequeueInputBuffer(encoder.get(), 0);
                if (idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                    if (idx < 0) return fail("encoder input dequeue");
                    size_t capacity = 0;
                    auto* dst = AMediaCodec_getInputBuffer(encoder.get(), idx, &capacity);
                    if (!dst || capacity < n * sizeof(int16_t)) return fail("encoder input bounds");
                    if (std::all_of(pcm.begin(), pcm.begin() + n, [](int16_t s) { return s == 0; })) {
                        engine.reset(); // Don't turn silence into buffered speech or an effect tail.
                    } else {
                        if (!engine || cfg.revision != revision) {
                            engine = std::make_unique<Engine>(rate, channels, /*soundboard=*/true);
                            engine->update(cfg);
                            revision = cfg.revision;
                        }
                        if (!engine->process(pcm.data(), adts::kSamples)) return fail("DSP declined PCM");
                    }
                    std::memcpy(dst, pcm.data(), n * sizeof(int16_t));
                    if (AMediaCodec_queueInputBuffer(encoder.get(), idx, 0, n * sizeof(int16_t),
                                                    time_us(forwarded), 0) != AMEDIA_OK)
                        return fail("encoder input queue");
                    ++forwarded;
                    progress = true;
                    decoded = false;
                }
            }
            size_t frame = 0;
            const Parse parsed = frame_size(input.data(), input.size(), rate, channels, frame);
            if (parsed == Parse::Unsupported) return fail("input is not matching AAC-LC/ADTS");
            if (parsed == Parse::Complete && submitted - encoded < kMaxInFlight) {
                const auto result = queue(decoder.get(), input.data() + adts::kHeader,
                                          frame - adts::kHeader, submitted);
                if (result == Result::Error) return fail("decoder input queue");
                if (result == Result::Done) {
                    input.discard(frame);
                    ++submitted;
                    progress = true;
                }
            }
            if (!progress || output.size() != 0) return true;
        }
        return true;
    }
};

struct Session {
    JavaVM* vm{};
    jobject manager{}, initial_config{};
    int rate = 0, channels = 0, port = 0, route = 0;
    std::atomic<bool> ended{false};
    std::mutex mu;
    std::mutex publication_mu; // Serializes the final copy/commit with stop, never a codec or stock read.
    Journal journal;
    std::unique_ptr<Transcode> codec;
    Clock::time_point started{};
    unsigned probe_reads = 0;
    uint64_t published = 0;
    std::optional<jint> deferred_error;
    std::optional<int64_t> revision;

    // All owners are JNI calls. A reader can finish after stop/release; it
    // keeps the state alive without making those calls wait for the codec.
    ~Session() {
        if (!initial_config && !manager) return;
        JNIEnv* env = nullptr;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(initial_config);
            env->DeleteGlobalRef(manager);
        }
    }

    void clear(JNIEnv* env) {
        if (initial_config) env->DeleteGlobalRef(initial_config);
        if (manager) env->DeleteGlobalRef(manager);
        initial_config = manager = nullptr;
        codec.reset();
    }

    void decline(JNIEnv* env, const char* reason) {
        LOGI("port=%d declined: %s; leaving original audio", port, reason);
        journal.decline();
        codec.reset();
        // Retain the exact pre-read observation until the last stock byte
        // has been replayed; a fresh snapshot alone can't validate old bytes.
        if (!journal.pending()) clear(env);
    }

    void fail(JNIEnv* env, const char* reason) {
        LOGW("port=%d stopped replacement: %s (published AUs=%llu)", port, reason,
             static_cast<unsigned long long>(published));
        journal.fail();
        clear(env);
    }

    enum class Safe { Yes, Unavailable, Unsupported, Unsafe };

    Safe observe(JNIEnv* env, jobject self) {
        Frame frame(env);
        if (!frame.ok) { clear_exception(env); return Safe::Unavailable; }
        if (!manager) {
            jobject app = env->CallStaticObjectMethod(j.activity, j.application);
            if (clear_exception(env) || !app) return Safe::Unavailable;
            jstring key = env->NewStringUTF("audio");
            if (clear_exception(env) || !key) return Safe::Unavailable;
            jobject service = env->CallObjectMethod(app, j.service, key);
            if (clear_exception(env) || !service) return Safe::Unavailable;
            manager = env->NewGlobalRef(service);
            if (clear_exception(env) || !manager) return Safe::Unavailable;
        }
        const bool mute = env->CallBooleanMethod(manager, j.muted);
        if (clear_exception(env)) return Safe::Unavailable;
        if (mute || ended.load()) return Safe::Unsafe;
        jobject cfg = env->CallObjectMethod(self, j.config);
        if (clear_exception(env) || !cfg) return Safe::Unavailable;
        const bool silent = env->CallBooleanMethod(cfg, j.silenced);
        if (clear_exception(env)) return Safe::Unavailable;
        if (silent) return Safe::Unsafe;
        auto is_int = [&](jobject obj, jmethodID method, int expected) {
            const int actual = env->CallIntMethod(obj, method);
            return !clear_exception(env) && actual == expected;
        };
        const auto mismatch = initial_config ? Safe::Unsafe : Safe::Unsupported;
        if (!is_int(cfg, j.client_port, port) || !is_int(cfg, j.client_source, kCamcorder)) return mismatch;
        jobject device = env->CallObjectMethod(cfg, j.device);
        if (clear_exception(env) || !device) return Safe::Unavailable;
        if (!is_int(device, j.device_type, kBuiltinMic)) return mismatch;
        const int id = env->CallIntMethod(device, j.device_id);
        if (clear_exception(env) || id <= 0) return Safe::Unavailable;
        if (initial_config) {
            const bool same = env->CallBooleanMethod(cfg, j.equals, initial_config);
            return !clear_exception(env) && same && id == route ? Safe::Yes : Safe::Unsafe;
        }
        jobject format = env->CallObjectMethod(cfg, j.client_format);
        if (clear_exception(env) || !format) return Safe::Unavailable;
        if (!is_int(format, j.config_format, kAacLc) || !is_int(format, j.config_rate, rate) ||
            !is_int(format, j.config_channels, channels)) return mismatch;
        initial_config = env->NewGlobalRef(cfg);
        if (clear_exception(env) || !initial_config) return Safe::Unavailable;
        route = id;
        return Safe::Yes;
    }
};

Read raw_read{};
Config config{};
bool installed = false;
using Start = jint (*)(JNIEnv*, jobject, jint, jint);
using End = void (*)(JNIEnv*, jobject);
Start stock_start{};
End stock_stop{}, stock_release{};
std::mutex sessions_mu;
std::mutex lifecycle_mu;
struct Entry { jweak owner; std::shared_ptr<Session> active; };
std::vector<Entry> sessions;

int integer(JNIEnv* env, jobject object, jmethodID method) {
    const int result = env->CallIntMethod(object, method);
    return clear_exception(env) ? -1 : result;
}

// A matched entry with no active state is a stopping/stopped track. Do not
// let a concurrent reader fall through to a stock read on that track.
std::optional<std::shared_ptr<Session>> find(JNIEnv* env, jobject self) {
    std::lock_guard lock(sessions_mu);
    for (const auto& entry : sessions) if (env->IsSameObject(entry.owner, self)) return entry.active;
    return {};
}

std::shared_ptr<Session> detach(JNIEnv* env, jobject self) {
    auto s = std::shared_ptr<Session>{};
    std::lock_guard lock(sessions_mu);
    for (auto& entry : sessions) {
        if (!env->IsSameObject(entry.owner, self)) continue;
        s = std::move(entry.active);
        if (s) {
            std::lock_guard publish_lock(s->publication_mu);
            s->ended.store(true);
            if (s->published) LOGI("port=%d closed, published AUs=%llu", s->port,
                                  static_cast<unsigned long long>(s->published));
        }
        break;
    }
    return s;
}

jint start(JNIEnv* env, jobject self, jint event, jint sync_session) {
    if (!stock_start) return kInvalidOperation;
    if (!installed) return stock_start(env, self, event, sync_session);
    std::lock_guard lifecycle_lock(lifecycle_mu);
    const int ret = stock_start(env, self, event, sync_session);
    if (ret || env->ExceptionCheck()) return ret;
    {
        std::array<std::shared_ptr<Session>, kMaxTracked> retired;
        size_t n = 0;
        std::lock_guard lock(sessions_mu);
        for (auto it = sessions.begin(); it != sessions.end();) {
            const bool same = env->IsSameObject(it->owner, self);
            if (same && it->active) return ret; // Redundant start on a live track.
            if (same || env->IsSameObject(it->owner, nullptr)) {
                env->DeleteWeakGlobalRef(it->owner);
                retired[n++] = std::move(it->active);
                it = sessions.erase(it);
            } else ++it;
        }
    }
    try {
        if (!stock_capture::aac_allowed() || event != 0 || integer(env, self, j.format) != kAacLc ||
            integer(env, self, j.source) != kCamcorder) return ret;
        const int rate = integer(env, self, j.rate), channels = integer(env, self, j.channels);
        const int port = integer(env, self, j.port);
        if (port <= 0 || rate < 8000 || rate > 48000 || adts::rate_index(rate) < 0 ||
            (channels != 1 && channels != 2)) return ret;
        DspConfig cfg;
        if (!config(cfg)) return ret;
        auto s = std::make_shared<Session>();
        if (env->GetJavaVM(&s->vm) != JNI_OK) return ret;
        s->rate = rate;
        s->channels = channels;
        s->port = port;
        std::lock_guard lock(sessions_mu);
        if (sessions.size() >= kMaxTracked) return ret;
        sessions.reserve(sessions.size() + 1);
        const auto owner = env->NewWeakGlobalRef(self);
        if (clear_exception(env) || !owner) return ret;
        sessions.push_back({owner, std::move(s)});
        LOGI("port=%d probing CAMCORDER/AAC-LC rate=%d ch=%d", port, rate, channels);
    } catch (...) {} // No audio bytes have been intercepted.
    return ret;
}

template<End& stock>
void end(JNIEnv* env, jobject self) {
    if (!stock) return;
    if (!installed) return stock(env, self);
    std::shared_ptr<Session> retired;
    {
        std::lock_guard lifecycle_lock(lifecycle_mu);
        retired = detach(env, self);
        stock(env, self); // Stop first to unblock a stock blocking read.
    }
}

// Drain accepted stock bytes on errors to keep the original capture lifecycle
// and timing. Don't publish those bytes or splice them into the encoded stream.
jint suppress(JNIEnv* env, jobject self, jobject buf, uint8_t* dst, jint count, bool blocking) {
    const int got = raw_read(env, self, buf, count, blocking);
    if (got > 0) std::memset(dst, 0, std::min(got, count));
    if (blocking && got <= 0 && !env->ExceptionCheck()) std::this_thread::sleep_for(10ms);
    return got > 0 ? 0 : got;
}

size_t publish(Session& s, uint8_t* dst, size_t count) {
    auto& out = s.codec->output;
    const auto prefix = whole_frames(out.data(), out.size(), count, s.rate, s.channels);
    if (!prefix.bytes) return 0;
    if (s.journal.mode() == Mode::Probe) {
        s.journal.commit();
        LOGI("port=%d committed AAC-LC/ADTS rate=%d ch=%d route=%d", s.port, s.rate, s.channels, s.route);
    }
    std::memcpy(dst, out.data(), prefix.bytes);
    out.discard(prefix.bytes);
    s.published += prefix.frames;
    return prefix.bytes;
}

jint read_locked(JNIEnv* env, jobject self, jobject buf, uint8_t* dst, jint count, bool blocking, Session& s) {
    auto original = [&]() -> jint {
        if (s.journal.pending()) {
            if (s.observe(env, self) != Session::Safe::Yes) {
                s.fail(env, "cannot replay audio after capture state changed");
                return 0;
            }
            std::lock_guard publish_lock(s.publication_mu);
            if (s.ended.load()) return kInvalidOperation;
            const auto replay = static_cast<jint>(s.journal.replay(dst, count));
            if (!s.journal.pending()) s.clear(env);
            return replay;
        }
        if (s.ended.load()) return kInvalidOperation;
        if (s.deferred_error) {
            const jint result = *s.deferred_error;
            s.deferred_error.reset();
            return result;
        }
        const jint result = raw_read(env, self, buf, count, blocking);
        std::lock_guard publish_lock(s.publication_mu);
        if (!s.ended.load()) return result;
        if (result > 0 && result <= count) std::memset(dst, 0, result);
        return kInvalidOperation;
    };
    if (s.ended.load()) return kInvalidOperation;
    if (s.journal.mode() == Mode::Original) return original();
    if (s.journal.mode() == Mode::Failed) return suppress(env, self, buf, dst, count, blocking);

    auto decline_or_fail = [&](const char* why) -> jint {
        if (s.journal.mode() == Mode::Probe) { s.decline(env, why); return original(); }
        s.fail(env, why);
        return 0;
    };
    DspConfig cfg;
    if (!config(cfg)) return decline_or_fail("disabled or conflicting capture backend");
    if (s.revision && cfg.revision != *s.revision) return decline_or_fail("DSP preset changed");
    s.revision = cfg.revision;
    if (!s.codec) {
        s.codec = std::make_unique<Transcode>(s.rate, s.channels);
        s.started = Clock::now();
    }
    auto& x = *s.codec;
    auto deadline = Clock::now() + 12ms; // Codec polling, not a deadline for stock AudioRecord.read().
    bool pulled = false;
    for (;;) {
        if (s.ended.load()) return kInvalidOperation;
        if (x.decoder) {
            if (!x.pump(cfg)) return decline_or_fail(x.error);
            if (x.output.size() != 0) {
                const auto safe = s.observe(env, self);
                if (safe != Session::Safe::Yes) {
                    s.fail(env, "mute, silencing, route or configuration changed");
                    return 0;
                }
                if (!config(cfg)) return decline_or_fail("disabled or conflicting capture backend");
                if (cfg.revision != *s.revision) return decline_or_fail("DSP preset changed");
                size_t n;
                {
                    std::lock_guard publish_lock(s.publication_mu);
                    if (s.ended.load()) return kInvalidOperation;
                    n = publish(s, dst, count);
                }
                return n ? jint(n) : decline_or_fail("output access unit exceeds destination");
            }
        }
        if (s.journal.mode() == Mode::Probe && (s.probe_reads >= kMaxReads || Clock::now() - s.started > 1s))
            return decline_or_fail("no verified AAC output in probe window");
        if (pulled || x.submitted - x.encoded >= kMaxInFlight) {
            if (!blocking || Clock::now() >= deadline) {
                if (x.submitted - x.encoded >= kMaxInFlight) return decline_or_fail("codec queue stalled");
                return 0;
            }
            std::this_thread::sleep_for(1ms);
            continue;
        }
        auto before = s.observe(env, self);
        // Recording configuration can appear just after native_start returns.
        // Don't consume any speculative data while waiting for the snapshot.
        const auto observation_deadline = std::min(s.started + 100ms, Clock::now() + 100ms);
        while (!s.initial_config && blocking && before == Session::Safe::Unavailable &&
               Clock::now() < observation_deadline) {
            std::this_thread::sleep_for(2ms);
            before = s.observe(env, self);
        }
        if (before != Session::Safe::Yes) {
            if (s.journal.mode() == Mode::Probe && !s.initial_config) return decline_or_fail(
                before == Session::Safe::Unsupported ? "unsupported capture configuration or route" :
                "no safe initial recording configuration");
            s.fail(env, "mute, silencing, route or configuration changed before read");
            return 0;
        }
        const size_t room = s.journal.mode() == Mode::Probe ? std::min(s.journal.room(), x.input.room()) : x.input.room();
        const int request = std::min(size_t(count), room);
        if (request <= 0) return decline_or_fail("capture queue full");
        const int got = raw_read(env, self, buf, request, blocking);
        if (env->ExceptionCheck() || got > request || s.ended.load()) {
            if (got > 0) std::memset(dst, 0, std::min(got, count));
            s.fail(env, "invalid or cancelled capture read");
            return kInvalidOperation;
        }
        if (got <= 0) {
            if (got < 0) {
                if (s.journal.mode() == Mode::Probe) {
                    s.deferred_error = got;
                    return decline_or_fail("stock read error during probe");
                }
                s.fail(env, "stock read failed");
            }
            return got;
        }
        // Copy only bytes reported fresh by AudioRecord, before the app can
        // reuse its direct buffer. The journal is live only before commit.
        const auto after = s.observe(env, self);
        if (after != Session::Safe::Yes) {
            std::memset(dst, 0, got);
            s.fail(env, "mute, silencing, route or configuration changed after read");
            return 0;
        }
        if (s.journal.mode() == Mode::Probe && !s.journal.note(dst, got)) {
            std::memset(dst, 0, got);
            s.fail(env, "capture journal overflow");
            return 0;
        }
        const bool appended = x.input.append(dst, got);
        std::memset(dst, 0, got);
        if (!appended) return decline_or_fail("capture queue overflow");
        ++s.probe_reads;
        if (!x.decoder) {
            size_t frame = 0;
            const auto parsed = frame_size(x.input.data(), x.input.size(), s.rate, s.channels, frame);
            if (parsed == Parse::Unsupported) return decline_or_fail("input is not matching AAC-LC/ADTS");
            if (parsed == Parse::NeedBytes) return 0;
            if (!x.start(frame)) return decline_or_fail("AAC codec negotiation failed");
        }
        pulled = true;
        deadline = Clock::now() + 12ms;
    }
}

} // namespace

std::optional<jint> read(JNIEnv* env, jobject self, jobject buffer, jint count, jboolean blocking) {
    if (!installed || !buffer || count <= 0) return {};
    const auto entry = find(env, self);
    if (!entry) return {};
    const auto& s = *entry;
    if (!s) return kInvalidOperation;
    auto* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (!dst || capacity <= 0) return capacity == 0 && dst ? 0 : -2;
    count = static_cast<jint>(std::min<jlong>(capacity, count));
    std::lock_guard lock(s->mu);
    try {
        return read_locked(env, self, buffer, dst, count, blocking, *s);
    } catch (...) {
        if (s->journal.mode() == Mode::Probe) {
            s->decline(env, "transcode allocation failed");
            return 0;
        }
        s->fail(env, "transcode allocation failed");
        return 0;
    }
}

void install(zygisk::Api* api, JNIEnv* env, Read reader, Config provider) {
    if (!reader || !provider) return;
    Frame frame(env);
    const auto audio = frame.ok ? env->FindClass("android/media/AudioRecord") : nullptr;
    if (!audio || !resolve(env, audio)) {
        clear_exception(env);
        return;
    }
    JNINativeMethod methods[] = {
        {"native_start", "(II)I", reinterpret_cast<void*>(start)},
        {"native_stop", "()V", reinterpret_cast<void*>(end<stock_stop>)},
        {"native_release", "()V", reinterpret_cast<void*>(end<stock_release>)},
    };
    using stock_capture::Registration;
    const auto registration = stock_capture::install_required_jni(api, env, audio, "android/media/AudioRecord", methods);
    if (registration != Registration::Ready) {
        env->DeleteGlobalRef(j.activity);
        j.activity = nullptr;
        if (registration == Registration::Unavailable) return;
    }
    stock_start = reinterpret_cast<Start>(methods[0].fnPtr);
    stock_stop = reinterpret_cast<End>(methods[1].fnPtr);
    stock_release = reinterpret_cast<End>(methods[2].fnPtr);
    raw_read = reader;
    config = provider;
    installed = registration == Registration::Ready;
    if (!installed) LOGW("Incomplete Direct AAC JNI registration; stock only");
}

} // namespace avcrt::direct_aac
