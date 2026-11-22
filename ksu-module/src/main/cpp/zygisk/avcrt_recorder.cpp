#include "avcrt_recorder.h"

#include "avcrt_adts_output.h"
#include "avcrt_engine.h"
#include "avcrt_stock_capture.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "avcrt-mr", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "avcrt-mr", __VA_ARGS__)

namespace avcrt::recorder {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr int kPcm16 = 2;
constexpr int kBuiltinMic = 15; // AudioDeviceInfo.TYPE_BUILTIN_MIC, not a device ID.
constexpr int kEventError = 1, kEventInfo = 2, kErrorUnknown = 1;
constexpr int kChannelsMax = 2;
constexpr int kSamples = adts::kSamples * kChannelsMax;

struct LocalFrame {
    JNIEnv* env;
    bool ok;
    explicit LocalFrame(JNIEnv* e) : env(e), ok(e->PushLocalFrame(32) == 0) {}
    ~LocalFrame() { if (ok) env->PopLocalFrame(nullptr); }
};

bool threw(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

struct Java {
    JavaVM* vm{};
    jclass mr{}, audio{}, state{}, illegal{}, runtime{}, application_class{}, attr_class{};
    jmethodID application{}, service{}, attribution{}, default_attribution{}, attribution_equals{}, device_context{}, pid{};
    jmethodID create_state{}, make_attribution{}, parcel_position{}, parcel_seek{};
    jfieldID creator{}, descriptor{}, surface{};
    jmethodID audio_new{}, min_buffer{}, state_id{}, rate{}, channels{}, encoding{}, source{};
    jmethodID audio_start{}, audio_stop{}, audio_release{}, recording{}, read{}, port{}, audio_route_ids{};
    jmethodID config{}, config_equals{}, silenced{}, client_port{}, client_source{}, audio_device{}, device_id{}, device_type{};
    jmethodID muted{}, event{};
} j;

Read read_raw{};
Config current_config{};
bool installed = false;

bool resolve(JNIEnv* env, stock_capture::RouteQuery& recorder_route) {
    LocalFrame frame(env);
    if (!frame.ok || env->GetJavaVM(&j.vm) != JNI_OK) return false;
    auto klass = [&](jclass& dst, const char* name) {
        dst = env->FindClass(name);
        return dst != nullptr;
    };
    jclass activity, context, attr, parcel, creator, manager, config, device, fd;
#define CLASS(dst, name) if (!klass(dst, name)) return false
#define METHOD(dst, owner, name, sig) if (!(dst = env->GetMethodID(owner, name, sig))) return false
#define STATIC(dst, owner, name, sig) if (!(dst = env->GetStaticMethodID(owner, name, sig))) return false
#define FIELD(dst, owner, name, sig) if (!(dst = env->GetFieldID(owner, name, sig))) return false
    CLASS(j.mr, "android/media/MediaRecorder");
    CLASS(j.audio, "android/media/AudioRecord");
    recorder_route = stock_capture::route_query(env, j.mr);
    if (recorder_route == stock_capture::RouteQuery::Unsupported) return false;
    if (recorder_route == stock_capture::RouteQuery::Array) {
        j.audio_route_ids = stock_capture::find_instance_method(env, j.audio,
            stock_capture::kRouteIds, stock_capture::kRouteIdsSignature);
        if (!j.audio_route_ids) return false;
    }
    CLASS(j.state, "android/content/AttributionSourceState");
    CLASS(j.illegal, "java/lang/IllegalStateException");
    CLASS(j.runtime, "java/lang/RuntimeException");
    CLASS(activity, "android/app/ActivityThread");
    CLASS(context, "android/content/Context");
    CLASS(attr, "android/content/AttributionSource");
    CLASS(parcel, "android/os/Parcel");
    CLASS(creator, "android/os/Parcelable$Creator");
    CLASS(manager, "android/media/AudioManager");
    CLASS(config, "android/media/AudioRecordingConfiguration");
    CLASS(device, "android/media/AudioDeviceInfo");
    CLASS(fd, "java/io/FileDescriptor");
    STATIC(j.application, activity, "currentApplication", "()Landroid/app/Application;");
    METHOD(j.service, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    METHOD(j.attribution, context, "getAttributionSource", "()Landroid/content/AttributionSource;");
    STATIC(j.default_attribution, attr, "myAttributionSource", "()Landroid/content/AttributionSource;");
    METHOD(j.attribution_equals, attr, "equals", "(Ljava/lang/Object;)Z");
    METHOD(j.device_context, attr, "getDeviceId", "()I");
    METHOD(j.pid, attr, "getPid", "()I");
    METHOD(j.make_attribution, attr, "<init>", "(Landroid/content/AttributionSourceState;)V");
    METHOD(j.create_state, creator, "createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;");
    METHOD(j.parcel_position, parcel, "dataPosition", "()I");
    METHOD(j.parcel_seek, parcel, "setDataPosition", "(I)V");
    if (!(j.creator = env->GetStaticFieldID(j.state, "CREATOR", "Landroid/os/Parcelable$Creator;"))) return false;
    FIELD(j.descriptor, fd, "descriptor", "I");
    FIELD(j.surface, j.mr, "mSurface", "Landroid/view/Surface;");
    METHOD(j.audio_new, j.audio, "<init>", "(IIIII)V");
    STATIC(j.min_buffer, j.audio, "getMinBufferSize", "(III)I");
    METHOD(j.state_id, j.audio, "getState", "()I");
    METHOD(j.rate, j.audio, "getSampleRate", "()I");
    METHOD(j.channels, j.audio, "getChannelCount", "()I");
    METHOD(j.encoding, j.audio, "getAudioFormat", "()I");
    METHOD(j.source, j.audio, "getAudioSource", "()I");
    METHOD(j.audio_start, j.audio, "startRecording", "()V");
    METHOD(j.audio_stop, j.audio, "stop", "()V");
    METHOD(j.audio_release, j.audio, "release", "()V");
    METHOD(j.recording, j.audio, "getRecordingState", "()I");
    METHOD(j.read, j.audio, "read", "([SIII)I");
    METHOD(j.port, j.audio, "getPortId", "()I");
    METHOD(j.config, j.audio, "getActiveRecordingConfiguration", "()Landroid/media/AudioRecordingConfiguration;");
    METHOD(j.config_equals, config, "equals", "(Ljava/lang/Object;)Z");
    METHOD(j.silenced, config, "isClientSilenced", "()Z");
    METHOD(j.client_port, config, "getClientPortId", "()I");
    METHOD(j.client_source, config, "getClientAudioSource", "()I");
    METHOD(j.audio_device, config, "getAudioDevice", "()Landroid/media/AudioDeviceInfo;");
    METHOD(j.device_id, device, "getId", "()I");
    METHOD(j.device_type, device, "getType", "()I");
    METHOD(j.muted, manager, "isMicrophoneMute", "()Z");
    STATIC(j.event, j.mr, "postEventFromNative", "(Ljava/lang/Object;IIILjava/lang/Object;)V");
#undef FIELD
#undef STATIC
#undef METHOD
#undef CLASS
    j.application_class = activity;
    j.attr_class = attr;
    const std::array classes{&j.mr, &j.audio, &j.state, &j.illegal, &j.runtime,
                             &j.application_class, &j.attr_class};
    for (size_t i = 0; i < classes.size(); ++i) {
        auto& ref = *classes[i];
        ref = static_cast<jclass>(env->NewGlobalRef(ref));
        if (!ref) {
            for (size_t n = 0; n < i; ++n) env->DeleteGlobalRef(*classes[n]);
            return false;
        }
    }
    return true;
}

void release_classes(JNIEnv* env) {
    for (auto* ref : {&j.mr, &j.audio, &j.state, &j.illegal, &j.runtime, &j.application_class, &j.attr_class}) {
        env->DeleteGlobalRef(*ref);
        *ref = nullptr;
    }
}

// Read the same framework Parcelable as stock JNI and restore the position
// before calling stock. Do not substitute a different attribution context.
jobject same_application(JNIEnv* env, jobject parcel) {
    if (!parcel) return nullptr;
    jobject app = env->CallStaticObjectMethod(j.application_class, j.application);
    if (threw(env) || !app) return nullptr;
    jobject expected = env->CallObjectMethod(app, j.attribution);
    if (threw(env) || !expected) return nullptr;
    const jint pos = env->CallIntMethod(parcel, j.parcel_position);
    if (threw(env)) return nullptr;
    jobject creator = env->GetStaticObjectField(j.state, j.creator);
    if (threw(env) || !creator) return nullptr;
    jobject state = env->CallObjectMethod(creator, j.create_state, parcel);
    const bool bad = threw(env);
    env->CallVoidMethod(parcel, j.parcel_seek, pos);
    if (threw(env) || bad || !state) return nullptr;
    // This is a comparison only. AudioRecord uses its normal application
    // attribution and permission checks; it never receives this parsed object.
    jobject actual = env->NewObject(j.attr_class, j.make_attribution, state);
    if (threw(env) || !actual) return nullptr;
    const bool same = env->CallBooleanMethod(actual, j.attribution_equals, expected);
    if (threw(env) || !same) return nullptr;
    jobject capture = env->CallStaticObjectMethod(j.attr_class, j.default_attribution);
    if (threw(env) || !capture) return nullptr;
    const bool same_capture = env->CallBooleanMethod(actual, j.attribution_equals, capture);
    if (threw(env) || !same_capture) return nullptr;
    for (auto identity : {actual, expected, capture}) {
        const int ctx = env->CallIntMethod(identity, j.device_context);
        if (threw(env) || ctx != 0) return nullptr;
    }
    const int actual_pid = env->CallIntMethod(actual, j.pid);
    if (threw(env)) return nullptr;
    const int own_pid = env->CallIntMethod(expected, j.pid);
    if (threw(env) || actual_pid != own_pid) return nullptr;
    const int capture_pid = env->CallIntMethod(capture, j.pid);
    return !threw(env) && actual_pid == capture_pid ? app : nullptr;
}

struct CodecDelete { void operator()(AMediaCodec* c) const { AMediaCodec_delete(c); } };
struct FormatDelete { void operator()(AMediaFormat* f) const { AMediaFormat_delete(f); } };
using Codec = std::unique_ptr<AMediaCodec, CodecDelete>;
using Format = std::unique_ptr<AMediaFormat, FormatDelete>;

struct Session {
    enum Phase { Configuring, Pending, Prepared, Started, Stopped } phase = Configuring;
    enum Control { Run, Pause, Finish, Abort };
    jweak owner{};
    jobject java_weak{}, manager{}, audio{}, initial_config{};
    adts::Request request;
    Codec codec;
    bool codec_started = false;
    bool audio_started = false;
    std::unique_ptr<Engine> engine;
    int64_t revision = -1;
    int fd = -1;
    long pipe_buf = 0;
    std::thread worker;
    std::atomic<Control> control{Run};
    std::atomic<int> fault{0}, peak{0}, route{0};
    std::atomic<bool> finished{false};
    std::atomic<unsigned> pause_epoch{0};
    std::atomic<uint64_t> written{0};
    std::atomic<int64_t> max_bytes{0}, max_ms{0};
    int port = 0;
    const char* failure = "capture state, codec or output error";

    bool taken() const { return phase != Configuring; }

    void close_output() { if (fd >= 0) { close(fd); fd = -1; } }

    void done(JNIEnv* env) {
        close_output();
        if (initial_config) {
            env->DeleteGlobalRef(initial_config);
            initial_config = nullptr;
        }
        if (audio) {
            if (audio_started) { env->CallVoidMethod(audio, j.audio_stop); threw(env); }
            env->CallVoidMethod(audio, j.audio_release);
            threw(env);
            env->DeleteGlobalRef(audio);
            audio = nullptr;
        }
        audio_started = false;
        if (codec_started) AMediaCodec_stop(codec.get());
        codec_started = false;
        codec.reset();
        engine.reset();
        route.store(0);
    }

    void clear_configuration() {
        request = {};
        phase = Configuring;
        control.store(Run);
        fault.store(0);
        peak.store(0);
        written.store(0);
        finished.store(false);
        pause_epoch.store(0);
    }

    void notify(JNIEnv* env, int what, int arg) {
        env->CallStaticVoidMethod(j.mr, j.event, java_weak, what, arg, 0, nullptr);
        threw(env);
    }

    bool preflight(JNIEnv* env, int output) {
        failure = "AVCRT disabled or capture backend conflict";
        if (!request.supported() || !manager) return false;
        DspConfig cfg;
        if (!current_config(cfg)) return false;
        failure = "unsupported or unavailable output descriptor";
        fd = adts::duplicate_output(output);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0) return false;
        pipe_buf = S_ISFIFO(st.st_mode) ? fpathconf(fd, _PC_PIPE_BUF) : 0;
        if (pipe_buf < 0) return false;
        failure = "AudioRecord configuration unavailable";
        const int mask = request.channels == 1 ? 16 : 12; // AudioFormat input masks.
        const int min = env->CallStaticIntMethod(j.audio, j.min_buffer, request.rate, mask, kPcm16);
        if (threw(env) || min <= 0 || min > 1024 * 1024) return false;
        const int frame_bytes = request.channels * int(sizeof(int16_t));
        const int capacity = std::max(min, 4 * adts::kSamples * frame_bytes);
        jobject ar = env->NewObject(j.audio, j.audio_new, request.source, request.rate, mask, kPcm16,
                                    (capacity + frame_bytes - 1) / frame_bytes * frame_bytes);
        if (threw(env) || !ar) return false;
        audio = env->NewGlobalRef(ar);
        if (threw(env) || !audio) {
            env->CallVoidMethod(ar, j.audio_release);
            threw(env);
            return false;
        }
        auto equals = [&](jmethodID method, int value) {
            const int actual = env->CallIntMethod(audio, method);
            return !threw(env) && actual == value;
        };
        if (!equals(j.state_id, 1) || !equals(j.rate, request.rate) || !equals(j.channels, request.channels) ||
            !equals(j.encoding, kPcm16) || !equals(j.source, request.source)) return false;
        port = env->CallIntMethod(audio, j.port);
        if (threw(env) || port <= 0) return false;
        if (j.audio_route_ids) {
            // Verify that the AudioRecord query actually calls before the
            // original recorder loses the output. An idle route may be empty.
            env->CallObjectMethod(audio, j.audio_route_ids);
            if (threw(env)) return false;
        }
        failure = "microphone muted or mute state unavailable";
        const bool mute = env->CallBooleanMethod(manager, j.muted);
        if (threw(env) || mute) return false;

        failure = "AAC-LC encoder configuration unavailable";
        Format format(AMediaFormat_new());
        if (!format) return false;
        AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, request.rate);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_CHANNEL_COUNT, request.channels);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_BIT_RATE, request.bitrate);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_PCM_ENCODING, kPcm16);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_AAC_PROFILE, 2); // AACObjectLC
        codec.reset(AMediaCodec_createEncoderByType("audio/mp4a-latm"));
        if (!codec || AMediaCodec_configure(codec.get(), format.get(), nullptr, nullptr,
                                          AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
            AMediaCodec_start(codec.get()) != AMEDIA_OK) return false;
        codec_started = true;
        engine = std::make_unique<Engine>(request.rate, request.channels);
        engine->update(cfg);
        revision = cfg.revision;
        max_bytes.store(request.max_bytes);
        max_ms.store(request.max_ms);
        failure = "capture state, codec or output error";
        return true;
    }

    // 0: safe state not available yet; -1: known unsafe; positive: observed route.
    // These Android APIs are snapshots, not capture-buffer metadata.
    int capture_state(JNIEnv* env) {
        LocalFrame frame(env);
        if (!frame.ok) { threw(env); return -1; }
        if (!stock_capture::aac_allowed() || control.load() == Abort || env->IsSameObject(owner, nullptr)) {
            failure = "capture disabled or recorder released";
            return -1;
        }
        failure = "microphone muted or mute state unavailable";
        const bool mute = env->CallBooleanMethod(manager, j.muted);
        if (threw(env) || mute) return -1;
        failure = "recording configuration unavailable or changed";
        jobject cfg = env->CallObjectMethod(audio, j.config);
        if (threw(env)) return -1;
        if (!cfg) return 0;
        const bool silent = env->CallBooleanMethod(cfg, j.silenced);
        if (threw(env) || silent) return -1;
        const int client_port = env->CallIntMethod(cfg, j.client_port);
        if (threw(env) || port != client_port) return -1;
        const int src = env->CallIntMethod(cfg, j.client_source);
        if (threw(env) || src != request.source) return -1;
        if (initial_config) {
            const bool same = env->CallBooleanMethod(cfg, j.config_equals, initial_config);
            if (threw(env) || !same) return -1;
        }
        failure = "input route unavailable or not a built-in microphone";
        jobject dev = env->CallObjectMethod(cfg, j.audio_device);
        if (threw(env)) return -1;
        if (!dev) return 0;
        const int type = env->CallIntMethod(dev, j.device_type);
        if (threw(env) || type != kBuiltinMic) return -1;
        const int id = env->CallIntMethod(dev, j.device_id);
        if (threw(env) || id <= 0) return -1;
        if (j.audio_route_ids) {
            auto ids = static_cast<jintArray>(env->CallObjectMethod(audio, j.audio_route_ids));
            if (threw(env)) return -1;
            if (!ids) return 0;
            const jsize count = env->GetArrayLength(ids);
            if (threw(env)) return -1;
            if (count == 0) return 0;
            if (count != 1) return -1;
            jint actual = 0;
            env->GetIntArrayRegion(ids, 0, 1, &actual);
            if (threw(env) || !stock_capture::single_route_matches(id, count, actual)) return -1;
        }
        if (!initial_config) {
            initial_config = env->NewGlobalRef(cfg);
            if (threw(env) || !initial_config) return -1;
        }
        failure = "capture, encoder or output error";
        return id;
    }

    bool safe(JNIEnv* env) { return route.load() > 0 && capture_state(env) == route.load(); }

    bool check_format() {
        Format format(AMediaCodec_getOutputFormat(codec.get()));
        void* csd = nullptr;
        size_t length = 0;
        return format && AMediaFormat_getBuffer(format.get(), "csd-0", &csd, &length) &&
            adts::matches_asc(static_cast<const uint8_t*>(csd), length, request.rate, request.channels);
    }

    enum class Drain { Again, More, Eos, Limit, Error };
    Drain drain(JNIEnv* env, bool& config_ok, int& event, int64_t timeout) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec.get(), &info, timeout);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return Drain::Again;
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return Drain::More;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // Some encoders provide CSD in a separate codec-config buffer.
            config_ok = check_format();
            return Drain::More;
        }
        if (idx < 0) return Drain::Error;

        Drain result = Drain::More;
        if (info.size > 0) {
            size_t capacity = 0;
            const auto* data = AMediaCodec_getOutputBuffer(codec.get(), idx, &capacity);
            // NdkMediaCodec on AOSP returns data() (already range-offset),
            // not base(). Decline nonzero offsets rather than assuming a
            // vendor implementation uses the same pointer convention.
            if (!data || info.offset != 0 || size_t(info.size) > capacity ||
                (info.flags & AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME)) {
                result = Drain::Error;
            } else if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                config_ok = adts::matches_asc(data, info.size, request.rate, request.channels);
                if (!config_ok) result = Drain::Error;
            } else {
                std::array<uint8_t, adts::kMaxFrame> frame;
                const size_t length = adts::kHeader + info.size;
                const int64_t max_time = max_ms.load(), max_size = max_bytes.load();
                const uint64_t pts = uint64_t(std::max(info.presentationTimeUs, int64_t(0)));
                auto limits = request;
                limits.max_ms = max_time;
                limits.max_bytes = max_size;
                // Round up the codec PTS conversion so integer microsecond truncation
                // does not count a frame just below its requested duration boundary.
                const uint64_t at_frame = (pts / 1000000) * request.rate +
                    ((pts % 1000000) * request.rate + 999999) / 1000000;
                event = adts::limit(limits, written.load(), length, at_frame);
                if (!config_ok || !adts::header(frame.data(), info.size, request.rate, request.channels)) {
                    result = Drain::Error;
                } else if (event) {
                    result = Drain::Limit;
                } else {
                    memcpy(frame.data() + adts::kHeader, data, info.size);
                    if (adts::write_frame(fd, frame.data(), length, pipe_buf, [&] { return safe(env); })) {
                        written.fetch_add(length);
                    } else result = Drain::Error;
                }
            }
        } else if (info.size < 0) result = Drain::Error;
        if (AMediaCodec_releaseOutputBuffer(codec.get(), idx, false) != AMEDIA_OK) return Drain::Error;
        return result == Drain::More && (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            ? Drain::Eos : result;
    }

    bool pump(JNIEnv* env, jshortArray array, int& event) {
        int initial_route = 0;
        const auto ready = Clock::now() + 750ms;
        do {
            if (control.load() == Abort) return false;
            if (control.load() == Finish) return true;
            initial_route = capture_state(env);
            if (initial_route < 0) return false;
            if (!initial_route) std::this_thread::sleep_for(10ms);
        } while (!initial_route && Clock::now() < ready);
        if (initial_route <= 0) return false;
        route.store(initial_route);

        std::array<int16_t, kSamples> pcm{};
        const int count = adts::kSamples * request.channels;
        uint64_t frames = 0;
        int samples = 0;
        // Do not feed audio collected before the initial route was identified.
        bool config_ok = false, eos = false, discard = true;
        bool have_silent_read = false;
        unsigned observed_pause = 0;
        auto deadline = Clock::now() + 2s;
        for (;;) {
            const Control cmd = control.load();
            if (cmd == Abort || !safe(env)) return false;
            const auto epoch = pause_epoch.load();
            if (observed_pause != epoch) {
                observed_pause = epoch;
                discard = true;
                samples = 0;
                have_silent_read = false;
                engine.reset();
            }
            const Drain result = drain(env, config_ok, event, eos ? 5000 : 0);
            if (result == Drain::Limit) return true;
            if (result == Drain::Error || (result == Drain::Eos && !eos)) return false;
            if (result == Drain::Eos) return true;
            if (cmd == Pause || (cmd == Run && discard)) {
                // Drain before the first capture and on pause/resume, even if
                // both lifecycle calls landed between worker iterations.
                const int got = read_raw(env, audio, j.read, array, count);
                if (threw(env) || got < 0 || got > count || !safe(env)) return false;
                if (cmd == Pause || got == 0) deadline = Clock::now() + 2s;
                else if (Clock::now() >= deadline) return false;
                if (got == 0 && cmd == Run) discard = false;
                if (got == 0) std::this_thread::sleep_for(3ms);
                continue;
            }
            if (Clock::now() >= deadline) return false;
            if (eos) continue;

            if (cmd == Run && samples < count) {
                const int got = read_raw(env, audio, j.read, array, count - samples);
                if (threw(env) || got < 0 || got > count - samples || got % request.channels || !safe(env))
                    return false;
                if (got > 0) {
                    env->GetShortArrayRegion(array, 0, got, pcm.data() + samples);
                    if (threw(env)) return false;
                    have_silent_read |= std::all_of(pcm.data() + samples, pcm.data() + samples + got,
                                                   [](int16_t x) { return x == 0; });
                    samples += got;
                } else std::this_thread::sleep_for(3ms);
                if (samples < count) continue;
            }
            if (control.load() == Pause || observed_pause != pause_epoch.load()) continue;
            const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec.get(), 0);
            if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                if (result == Drain::Again) std::this_thread::sleep_for(3ms);
                continue;
            }
            if (idx < 0) return false;
            size_t capacity = 0;
            auto* data = AMediaCodec_getInputBuffer(codec.get(), idx, &capacity);
            const size_t bytes = samples * sizeof(int16_t);
            if (!data || bytes > capacity || !safe(env)) return false;
            if (samples) {
                int p = 0;
                for (int i = 0; i < samples; ++i) p = std::max(p, std::abs(int(pcm[i])));
                int old = peak.load();
                while (old < p && !peak.compare_exchange_weak(old, p)) {}
                DspConfig cfg;
                if (p > 0 && !have_silent_read && current_config(cfg)) {
                    if (!engine || revision != cfg.revision) {
                        engine = std::make_unique<Engine>(request.rate, request.channels);
                        engine->update(cfg);
                        revision = cfg.revision;
                    }
                    engine->process(pcm.data(), samples / request.channels);
                } else {
                    // Never substitute buffered DSP for a wholly zero read,
                    // including when it was part of an assembled AAC block.
                    engine.reset();
                }
                memcpy(data, pcm.data(), bytes);
            }
            if (!safe(env)) return false;
            // An input buffer cannot be returned unqueued. Queue an empty
            // non-EOS buffer if a pause arrived while processing this AU.
            const bool paused = control.load() == Pause || observed_pause != pause_epoch.load();
            if (paused) samples = 0;
            eos = cmd == Finish;
            if (AMediaCodec_queueInputBuffer(codec.get(), idx, 0, paused ? 0 : bytes,
                    frames * 1000000 / request.rate,
                    eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0) != AMEDIA_OK) return false;
            frames += samples / request.channels;
            samples = 0;
            have_silent_read = false;
            deadline = Clock::now() + 2s;
        }
    }

    void run() noexcept {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        const bool blocked = pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
        JNIEnv* env = nullptr;
        if (j.vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
            fault.store(kErrorUnknown);
            finished.store(true);
            return; // Lifecycle caller owns cleanup if attachment failed.
        }
        auto array = env->NewShortArray(kSamples);
        bool ok = false;
        int event = 0;
        if (!threw(env) && array && blocked) {
            try { ok = pump(env, array, event); } catch (...) {}
        }
        done(env);
        finished.store(true);
        if (control.load() != Abort) {
            if (!ok) {
                fault.store(kErrorUnknown);
                LOGW("replacement aborted: %s", failure);
                notify(env, kEventError, kErrorUnknown);
            } else if (event) notify(env, kEventInfo, event);
        }
        LOGI("replacement finished bytes=%llu", static_cast<unsigned long long>(written.load()));
        j.vm->DetachCurrentThread();
    }
};

std::mutex sessions_mu;
std::vector<std::unique_ptr<Session>> sessions;

Session* find(JNIEnv* env, jobject self) {
    if (!installed) return nullptr;
    for (auto& s : sessions) if (env->IsSameObject(s->owner, self)) return s.get();
    return nullptr;
}

void finish(JNIEnv* env, Session& s, Session::Control command) {
    if (s.worker.joinable()) {
        s.control.store(command);
        s.worker.join();
    }
    s.done(env);
}

void remove(JNIEnv* env, Session* s) {
    if (!s) return;
    finish(env, *s, Session::Abort);
    env->DeleteWeakGlobalRef(s->owner);
    env->DeleteGlobalRef(s->java_weak);
    env->DeleteGlobalRef(s->manager);
    sessions.erase(std::find_if(sessions.begin(), sessions.end(), [&](const auto& p) { return p.get() == s; }));
}

// A weak global may already be cleared when Java calls native_finalize.
void reap(JNIEnv* env) {
    for (size_t i = 0; i < sessions.size();) {
        if (env->IsSameObject(sessions[i]->owner, nullptr)) remove(env, sessions[i].get());
        else ++i;
    }
}

template <typename R, typename... Args> using Fn = R (*)(JNIEnv*, jobject, Args...);
Fn<void, jobject, jstring, jobject> stock_setup;
Fn<void, jint> stock_source, stock_format, stock_encoder, stock_duration;
Fn<void, jlong> stock_size;
Fn<void, jstring> stock_parameter;
Fn<void, jobject> stock_output;
Fn<void> stock_prepare, stock_start, stock_stop, stock_pause, stock_resume, stock_reset, stock_release, stock_finalize;
Fn<jint> stock_peak, stock_route, stock_port;
Fn<jintArray> stock_route_ids;

void illegal(JNIEnv* env) { env->ThrowNew(j.illegal, "Invalid operation for AVCRT AAC-ADTS recorder"); }

void setup(JNIEnv* env, jobject self, jobject weak, jstring name, jobject parcel) {
    // A recorder can be constructed while AVCRT is off; the master switch
    // decides at output preflight, not at construction or process startup.
    if (!stock_setup) return;
    if (!installed) return stock_setup(env, self, weak, name, parcel);
    LocalFrame frame(env);
    jobject app = frame.ok ? same_application(env, parcel) : nullptr;
    threw(env);
    stock_setup(env, self, weak, name, parcel);
    if (env->ExceptionCheck() || !app || !weak) return;
    jobject key = env->NewStringUTF("audio");
    if (threw(env) || !key) return;
    jobject manager = env->CallObjectMethod(app, j.service, key);
    if (threw(env) || !manager) return;
    std::lock_guard lock(sessions_mu);
    try {
        reap(env);
        if (sessions.size() >= 32 || find(env, self)) return;
        auto s = std::make_unique<Session>();
        sessions.reserve(sessions.size() + 1);
        s->owner = env->NewWeakGlobalRef(self);
        if (!threw(env) && s->owner) s->java_weak = env->NewGlobalRef(weak);
        if (!threw(env) && s->java_weak) s->manager = env->NewGlobalRef(manager);
        if (threw(env) || !s->manager) {
            if (s->manager) env->DeleteGlobalRef(s->manager);
            if (s->java_weak) env->DeleteGlobalRef(s->java_weak);
            if (s->owner) env->DeleteWeakGlobalRef(s->owner);
            return;
        }
        sessions.push_back(std::move(s));
    } catch (...) { threw(env); }
}

template <int adts::Request::*field, auto& stock>
void integer(JNIEnv* env, jobject self, jint value) {
    if (!stock) return;
    if (!installed) return stock(env, self, value);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (s && s->taken()) return illegal(env);
    stock(env, self, value);
    if (s) {
        if (env->ExceptionCheck()) s->request.excluded = true;
        else s->request.*field = value;
    }
}

template <typename T, std::atomic<int64_t> Session::*active, int64_t adts::Request::*field, auto& stock>
void limit(JNIEnv* env, jobject self, T value) {
    if (!stock) return;
    if (!installed) return stock(env, self, value);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (s && s->taken()) {
        if (s->phase == Session::Stopped) return illegal(env);
        if (!s->finished.load()) {
            // Retain framework parameter validation (the stock recorder has
            // no output and has not started).
            stock(env, self, value);
            if (env->ExceptionCheck()) return;
        }
        (s->*active).store(value);
    } else {
        stock(env, self, value);
        if (s) {
            if (env->ExceptionCheck()) s->request.excluded = true;
            else s->request.*field = value;
        }
    }
}

void parameter(JNIEnv* env, jobject self, jstring value) {
    if (!stock_parameter) return;
    if (!installed) return stock_parameter(env, self, value);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (s && s->taken()) return illegal(env);
    stock_parameter(env, self, value);
    if (!s) return;
    if (env->ExceptionCheck() || !value) { s->request.excluded = true; return; }
    const char* p = env->GetStringUTFChars(value, nullptr);
    if (!p) { threw(env); s->request.excluded = true; return; }
    const std::string_view text(p);
    const auto eq = text.find('=');
    const auto key = text.substr(0, eq);
    int* field = nullptr;
    if (key == "audio-param-sampling-rate") field = &s->request.rate;
    if (key == "audio-param-number-of-channels") field = &s->request.channels;
    if (key == "audio-param-encoding-bitrate") field = &s->request.bitrate;
    int n = 0;
    if (field && eq != text.npos) {
        const auto v = text.substr(eq + 1);
        const auto parsed = std::from_chars(v.data(), v.data() + v.size(), n);
        if (parsed.ec == std::errc{} && parsed.ptr == v.data() + v.size() && n > 0) *field = n;
        else s->request.excluded = true;
    } else s->request.excluded = true;
    env->ReleaseStringUTFChars(value, p);
}

void output(JNIEnv* env, jobject self, jobject descriptor) {
    if (!stock_output) return;
    if (!installed) return stock_output(env, self, descriptor);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s) return stock_output(env, self, descriptor);
    if (s->taken()) return illegal(env);
    if (descriptor && s->request.supported() && stock_capture::aac_allowed() && std::none_of(sessions.begin(), sessions.end(),
        [&](const auto& other) { return other.get() != s && other->taken() && other->phase != Session::Stopped; })) {
        LocalFrame frame(env);
        bool ok = false;
        if (frame.ok) {
            jobject surface = env->GetObjectField(self, j.surface);
            if (!threw(env) && !surface) {
                const int fd = env->GetIntField(descriptor, j.descriptor);
                if (!threw(env)) {
                    try { ok = s->preflight(env, fd); } catch (...) { threw(env); }
                }
            }
        } else threw(env);
        if (ok && stock_capture::aac_allowed()) {
            s->phase = Session::Pending;
            LOGI("takeover committed AAC-LC/ADTS rate=%d ch=%d bitrate=%d output=%s",
                 s->request.rate, s->request.channels, s->request.bitrate, s->pipe_buf > 0 ? "pipe" : "file");
            return;
        }
        if (ok) s->failure = "AVCRT disabled or capture backend conflict before commit";
        s->done(env);
        LOGI("preflight declined: %s; using stock recorder", s->failure);
    } else if (stock_capture::aac_allowed() && s->request.format == 6) {
        LOGI("request declined source=%d encoder=%d rate=%d ch=%d bitrate=%d excluded=%d",
             s->request.source, s->request.encoder, s->request.rate, s->request.channels,
             s->request.bitrate, s->request.excluded);
    }
    s->request.excluded = true; // Never switch a recorder that already supplied output to stock.
    stock_output(env, self, descriptor);
}

void prepare(JNIEnv* env, jobject self) {
    if (!stock_prepare) return;
    if (!installed) return stock_prepare(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s || !s->taken()) {
        if (s) s->request.excluded = true;
        return stock_prepare(env, self);
    }
    if (s->phase != Session::Pending) return illegal(env);
    s->phase = Session::Prepared;
}

void start(JNIEnv* env, jobject self) {
    if (!stock_start) return;
    if (!installed) return stock_start(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s || !s->taken()) {
        if (s) s->request.excluded = true;
        return stock_start(env, self);
    }
    if (s->phase != Session::Prepared) return illegal(env);
    s->phase = Session::Stopped;
    DspConfig cfg;
    jthrowable start_error = nullptr;
    bool ok = current_config(cfg);
    if (ok) {
        env->CallVoidMethod(s->audio, j.audio_start);
        if (env->ExceptionCheck()) {
            start_error = env->ExceptionOccurred();
            env->ExceptionClear();
        }
        ok = !start_error;
        s->audio_started = ok;
        if (ok) {
            const int state = env->CallIntMethod(s->audio, j.recording);
            ok = !threw(env) && state == 3; // AudioRecord.RECORDSTATE_RECORDING.
        }
    }
    if (ok) {
        try {
            s->worker = std::thread([s] { s->run(); });
            s->phase = Session::Started;
            return;
        } catch (...) {}
    }
    s->done(env);
    if (start_error) env->Throw(start_error);
    else env->ThrowNew(j.runtime, "AVCRT AAC-ADTS capture could not start");
}

void stop(JNIEnv* env, jobject self) {
    if (!stock_stop) return;
    if (!installed) return stock_stop(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s || !s->taken()) {
        stock_stop(env, self);
        // A successful stock stop allows a new, independently configured
        // recording on the same object, including one started with DSP on.
        if (s) {
            if (env->ExceptionCheck()) s->request.excluded = true;
            else s->clear_configuration();
        }
        return;
    }
    if (s->phase != Session::Started) return illegal(env);
    finish(env, *s, Session::Finish);
    s->phase = Session::Stopped;
    if (s->fault.load() || !s->written.load()) {
        env->ThrowNew(j.runtime, "AVCRT AAC-ADTS recording failed");
    } else {
        // Stock stop also clears its state for reuse. It never started here,
        // so reset the original object instead of calling its stop().
        stock_reset(env, self);
        if (!env->ExceptionCheck()) s->clear_configuration();
    }
}

template <Session::Control command, auto& stock>
void change_control(JNIEnv* env, jobject self) {
    if (!stock) return;
    if (!installed) return stock(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s || !s->taken()) return stock(env, self);
    if (s->phase != Session::Started || s->finished.load()) return illegal(env);
    if (command == Session::Pause && s->control.load() != Session::Pause) s->pause_epoch.fetch_add(1);
    s->control.store(command);
}

void reset(JNIEnv* env, jobject self) {
    if (!stock_reset) return;
    if (!installed) return stock_reset(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (s) finish(env, *s, Session::Abort);
    stock_reset(env, self);
    if (!s || env->ExceptionCheck()) return;
    s->clear_configuration();
}

template <auto& stock>
void release(JNIEnv* env, jobject self) {
    if (!stock) return;
    if (!installed) return stock(env, self);
    std::lock_guard lock(sessions_mu);
    remove(env, find(env, self));
    reap(env);
    stock(env, self);
}

jint amplitude(JNIEnv* env, jobject self) {
    if (!stock_peak) return 0;
    if (!installed) return stock_peak(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s || !s->taken()) return stock_peak(env, self);
    if (s->phase == Session::Stopped) { illegal(env); return 0; }
    return s->peak.exchange(0);
}

template <auto& stock, bool is_port>
jint routing(JNIEnv* env, jobject self) {
    if (!stock) return 0;
    if (!installed) return stock(env, self);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (!s || !s->taken()) return stock(env, self);
    return is_port ? (s->phase != Session::Stopped ? s->port : 0) : s->route.load();
}

jintArray route_ids(JNIEnv* env, jobject self) {
    if (!stock_route_ids) return nullptr;
    if (!installed) return stock_route_ids(env, self);
    jint id;
    {
        std::lock_guard lock(sessions_mu);
        auto* s = find(env, self);
        if (!s || !s->taken()) return stock_route_ids(env, self);
        id = s->route.load();
    }
    auto result = env->NewIntArray(id > 0 ? 1 : 0);
    if (result && id > 0) env->SetIntArrayRegion(result, 0, 1, &id);
    return result;
}

// Track configuration outside the supported subset without interpreting it. If
// an app asks for it after committing, don't silently apply it to the unused
// stock pipeline. Query hooks are also fail-closed where no equivalent exists.
Fn<void, jint> stock_video_source, stock_video_encoder, stock_video_fps;
Fn<void, jint, jint> stock_video_size;
Fn<void, jobject> stock_camera, stock_surface, stock_next;
Fn<void, jboolean> stock_privacy, stock_route_callback;
Fn<jobject> stock_get_surface, stock_metrics;
Fn<jint, jint> stock_mic_direction;
Fn<jint, jfloat> stock_mic_field;
Fn<jboolean, jint> stock_device;
Fn<jint, jobject> stock_mics;

template <auto& stock, class R, class... Args>
R exclude(JNIEnv* env, jobject self, Args... args) {
    if (!stock) return R();
    if (!installed) return stock(env, self, args...);
    std::lock_guard lock(sessions_mu);
    auto* s = find(env, self);
    if (s) {
        if (s->taken()) {
            illegal(env);
            return R();
        }
        s->request.excluded = true;
    }
    return stock(env, self, args...);
}

} // namespace

void install(zygisk::Api* api, JNIEnv* env, Read read, Config config) {
    if (!read || !config) return;
    stock_capture::RouteQuery route;
    if (!resolve(env, route)) {
        threw(env);
        return;
    }
    // AOSP methods only. No inline patching, field offsets, or hidden API
    // exemptions. Zygisk returns null for missing registrations; any missing
    // original disables takeover for the process, including partial installs.
#define HOOKS(X) \
    X("native_setup", "(Ljava/lang/Object;Ljava/lang/String;Landroid/os/Parcel;)V", stock_setup, setup) \
    X("setAudioSource", "(I)V", stock_source, (integer<&adts::Request::source, stock_source>)) \
    X("setOutputFormat", "(I)V", stock_format, (integer<&adts::Request::format, stock_format>)) \
    X("setAudioEncoder", "(I)V", stock_encoder, (integer<&adts::Request::encoder, stock_encoder>)) \
    X("setMaxDuration", "(I)V", stock_duration, (limit<jint, &Session::max_ms, &adts::Request::max_ms, stock_duration>)) \
    X("setMaxFileSize", "(J)V", stock_size, (limit<jlong, &Session::max_bytes, &adts::Request::max_bytes, stock_size>)) \
    X("setParameter", "(Ljava/lang/String;)V", stock_parameter, parameter) \
    X("_setOutputFile", "(Ljava/io/FileDescriptor;)V", stock_output, output) \
    X("_prepare", "()V", stock_prepare, prepare) \
    X("start", "()V", stock_start, start) \
    X("stop", "()V", stock_stop, stop) \
    X("pause", "()V", stock_pause, (change_control<Session::Pause, stock_pause>)) \
    X("resume", "()V", stock_resume, (change_control<Session::Run, stock_resume>)) \
    X("native_reset", "()V", stock_reset, reset) \
    X("release", "()V", stock_release, (release<stock_release>)) \
    X("native_finalize", "()V", stock_finalize, (release<stock_finalize>)) \
    X("getMaxAmplitude", "()I", stock_peak, amplitude) \
    X("native_getPortId", "()I", stock_port, (routing<stock_port, true>)) \
    X("setVideoSource", "(I)V", stock_video_source, (exclude<stock_video_source, void, jint>)) \
    X("setVideoEncoder", "(I)V", stock_video_encoder, (exclude<stock_video_encoder, void, jint>)) \
    X("setVideoFrameRate", "(I)V", stock_video_fps, (exclude<stock_video_fps, void, jint>)) \
    X("setVideoSize", "(II)V", stock_video_size, (exclude<stock_video_size, void, jint, jint>)) \
    X("setCamera", "(Landroid/hardware/Camera;)V", stock_camera, (exclude<stock_camera, void, jobject>)) \
    X("native_setInputSurface", "(Landroid/view/Surface;)V", stock_surface, (exclude<stock_surface, void, jobject>)) \
    X("_setNextOutputFile", "(Ljava/io/FileDescriptor;)V", stock_next, (exclude<stock_next, void, jobject>)) \
    X("setPrivacySensitive", "(Z)V", stock_privacy, (exclude<stock_privacy, void, jboolean>)) \
    X("native_enableDeviceCallback", "(Z)V", stock_route_callback, (exclude<stock_route_callback, void, jboolean>)) \
    X("getSurface", "()Landroid/view/Surface;", stock_get_surface, (exclude<stock_get_surface, jobject>)) \
    X("native_getMetrics", "()Landroid/os/PersistableBundle;", stock_metrics, (exclude<stock_metrics, jobject>)) \
    X("native_setPreferredMicrophoneDirection", "(I)I", stock_mic_direction, (exclude<stock_mic_direction, jint, jint>)) \
    X("native_setPreferredMicrophoneFieldDimension", "(F)I", stock_mic_field, (exclude<stock_mic_field, jint, jfloat>)) \
    X("native_setInputDevice", "(I)Z", stock_device, (exclude<stock_device, jboolean, jint>)) \
    X("native_getActiveMicrophones", "(Ljava/util/ArrayList;)I", stock_mics, (exclude<stock_mics, jint, jobject>))

#define ENTRY(name, sig, slot, hook) {name, sig, reinterpret_cast<void*>(hook)},
    const bool array_route = route == stock_capture::RouteQuery::Array;
    const JNINativeMethod route_method = array_route
        ? JNINativeMethod{stock_capture::kRouteIds, stock_capture::kRouteIdsSignature,
                          reinterpret_cast<void*>(route_ids)}
        : JNINativeMethod{stock_capture::kRouteId, stock_capture::kRouteIdSignature,
                          reinterpret_cast<void*>(routing<stock_route, false>)};
    JNINativeMethod methods[] = {HOOKS(ENTRY)
        route_method
    };
#undef ENTRY
    LocalFrame frame(env);
    using stock_capture::Registration;
    const auto registration = frame.ok ? stock_capture::install_required_jni(api, env, j.mr,
        "android/media/MediaRecorder", methods) : Registration::Unavailable;
    if (registration != Registration::Ready) {
        threw(env);
        release_classes(env);
        if (registration == Registration::Unavailable) return;
    }
    size_t n = 0;
#define SAVE(name, sig, slot, hook) slot = reinterpret_cast<decltype(slot)>(methods[n++].fnPtr);
    HOOKS(SAVE)
#undef SAVE
#undef HOOKS
    if (array_route) stock_route_ids = reinterpret_cast<decltype(stock_route_ids)>(methods[n].fnPtr);
    else stock_route = reinterpret_cast<decltype(stock_route)>(methods[n].fnPtr);
    read_raw = read;
    current_config = config;
    installed = registration == Registration::Ready;
    if (!installed) LOGW("Incomplete MediaRecorder JNI registration; stock only");
}

} // namespace avcrt::recorder
