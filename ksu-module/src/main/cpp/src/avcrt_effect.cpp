//
// Android Audio Effects Framework adapter — glues the engine to the
// `effect_descriptor_t` / `audio_effect_library_t` ABI that the system
// audio HAL discovers via /system/etc/audio_effects.conf.
//
// We only implement the pre-processing path (input source effect). For each
// AudioRecord that uses one of the input sources we're attached to (mic,
// voice_communication, voice_call), the audio HAL instantiates one effect
// instance, calls process() per buffer, and releases it on stop.
//

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// `<hardware/audio_effect.h>` is AOSP-internal — not in the NDK — so we ship a
// trimmed copy of just the types & constants we need.
#include "effect_api.h"

#include "avcrt_config.h"
#include "avcrt_engine.h"
#include "avcrt_log.h"

namespace {

// Stable UUID identifying the AVCRT pre-processing effect.
// Keep in sync with audio_effects.conf.
constexpr effect_uuid_t kAvcrtEffectUuid = {
    0x9b8a0001, 0xa07c, 0x4a87, 0x9c8b,
    {0xa0, 0x4c, 0xa2, 0x7d, 0x4f, 0x10},
};

// Pseudo-type UUID — we present ourselves as a generic pre-processing effect.
// Using NULL_UUID type means "uncategorized pre-processing", which is what
// custom system-wide effects typically use.
constexpr effect_uuid_t kAvcrtTypeUuid = {
    0x9b8a0000, 0xa07c, 0x4a87, 0x9c8b,
    {0xa0, 0x4c, 0xa2, 0x7d, 0x4f, 0x00},
};

constexpr effect_descriptor_t kAvcrtDescriptor = {
    /*type=*/      kAvcrtTypeUuid,
    /*uuid=*/      kAvcrtEffectUuid,
    /*apiVersion=*/EFFECT_CONTROL_API_VERSION,
    /*flags=*/     EFFECT_FLAG_TYPE_PRE_PROC | EFFECT_FLAG_INSERT_FIRST,
    /*cpuLoad=*/   30,
    /*memoryUsage=*/64,
    /*name=*/      "AVCRT Voice Changer",
    /*implementor=*/"AVCRT",
};

// buffer_config_t.channels carries an audio_channel_mask_t (AUDIO_CHANNEL_IN_MONO = 0x10,
// AUDIO_CHANNEL_IN_STEREO = 0xC), NOT a channel count. SoundTouch and our interleave math
// need the COUNT, so derive it via popcount (floored to 1). Feeding the raw mask as a
// count (e.g. 16 for mono) multiplies every frame index by 16 → buffer over-read/over-write
// and a mis-configured SoundTouch → silent/garbled capture.
inline uint32_t channel_count_from_in_mask(uint32_t mask) {
    const uint32_t n = static_cast<uint32_t>(__builtin_popcount(mask));
    return n ? n : 1u;
}

// Per-instance state. The audio framework calls process()/command() on
// arbitrary threads — each instance owns its own engine.
struct EffectContext {
    effect_interface_s itfe;       // first field; the framework treats
                                   // `effect_handle_t` as a pointer to this.
    effect_config_t    config{};
    uint32_t           channelCount = 1;  // derived from inputCfg.channels mask (see helper)
    bool               enabled = false;
    std::unique_ptr<avcrt::Engine> engine;
    std::shared_ptr<avcrt::ConfigStore> store;
};

// One config store shared across all instances — file polling stays cheap.
std::mutex g_store_mu;
std::weak_ptr<avcrt::ConfigStore> g_store_weak;

std::shared_ptr<avcrt::ConfigStore> acquire_store() {
    std::lock_guard<std::mutex> lock(g_store_mu);
    auto s = g_store_weak.lock();
    if (!s) {
        s = std::make_shared<avcrt::ConfigStore>();
        g_store_weak = s;
    }
    return s;
}

// ---- effect_interface_s callbacks ---------------------------------------

int32_t avcrt_process(effect_handle_t self, audio_buffer_t* in, audio_buffer_t* out) {
    auto* ctx = reinterpret_cast<EffectContext*>(self);
    if (!ctx || !ctx->engine) return -EINVAL;
    if (!in || !out || !in->raw || !out->raw) return -EINVAL;
    if (in->frameCount != out->frameCount) return -EINVAL;

    auto cfg = ctx->store->snapshot();
    ctx->engine->update(cfg);

    if (out->raw != in->raw) {
        std::memcpy(out->raw, in->raw, in->frameCount * ctx->channelCount * sizeof(int16_t));
    }

    if (ctx->enabled && cfg.enabled) {
        ctx->engine->process(reinterpret_cast<int16_t*>(out->raw), in->frameCount);
    }
    return 0;
}

int32_t avcrt_command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                      void* cmdData, uint32_t* replySize, void* replyData) {
    auto* ctx = reinterpret_cast<EffectContext*>(self);
    if (!ctx) return -EINVAL;

    switch (cmdCode) {
        case EFFECT_CMD_INIT: {
            if (!replyData || !replySize || *replySize < sizeof(int32_t)) return -EINVAL;
            *static_cast<int32_t*>(replyData) = 0;
            return 0;
        }
        case EFFECT_CMD_SET_CONFIG: {
            if (!cmdData || cmdSize < sizeof(effect_config_t)) return -EINVAL;
            std::memcpy(&ctx->config, cmdData, sizeof(effect_config_t));
            ctx->channelCount = channel_count_from_in_mask(ctx->config.inputCfg.channels);
            avcrt::log::i("SET_CONFIG: rate=%u channelMask=0x%x -> channels=%u",
                          ctx->config.inputCfg.samplingRate,
                          ctx->config.inputCfg.channels, ctx->channelCount);
            ctx->engine = std::make_unique<avcrt::Engine>(
                ctx->config.inputCfg.samplingRate,
                ctx->channelCount);
            if (replyData && replySize && *replySize >= sizeof(int32_t)) {
                *static_cast<int32_t*>(replyData) = 0;
            }
            return 0;
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (!replyData || !replySize || *replySize < sizeof(effect_config_t)) return -EINVAL;
            std::memcpy(replyData, &ctx->config, sizeof(effect_config_t));
            *replySize = sizeof(effect_config_t);
            return 0;
        }
        case EFFECT_CMD_ENABLE: {
            ctx->enabled = true;
            if (replyData && replySize && *replySize >= sizeof(int32_t)) {
                *static_cast<int32_t*>(replyData) = 0;
            }
            return 0;
        }
        case EFFECT_CMD_DISABLE: {
            ctx->enabled = false;
            if (replyData && replySize && *replySize >= sizeof(int32_t)) {
                *static_cast<int32_t*>(replyData) = 0;
            }
            return 0;
        }
        case EFFECT_CMD_RESET: {
            if (ctx->engine) {
                ctx->engine = std::make_unique<avcrt::Engine>(
                    ctx->config.inputCfg.samplingRate,
                    ctx->channelCount);
            }
            return 0;
        }
        case EFFECT_CMD_SET_PARAM:
        case EFFECT_CMD_GET_PARAM:
        case EFFECT_CMD_SET_AUDIO_MODE:
        case EFFECT_CMD_SET_AUDIO_SOURCE: {
            if (replyData && replySize && *replySize >= sizeof(int32_t)) {
                *static_cast<int32_t*>(replyData) = 0;
            }
            return 0;
        }
        default:
            return 0;
    }
}

int32_t avcrt_get_descriptor(effect_handle_t /*self*/, effect_descriptor_t* desc) {
    if (!desc) return -EINVAL;
    *desc = kAvcrtDescriptor;
    return 0;
}

constexpr effect_interface_s kAvcrtInterface = {
    /*process=*/        avcrt_process,
    /*command=*/        avcrt_command,
    /*get_descriptor=*/ avcrt_get_descriptor,
    /*process_reverse=*/nullptr,
};

// ---- audio_effect_library_t entry points ---------------------------------

int32_t avcrt_create(const effect_uuid_t* uuid, int32_t /*sessionId*/, int32_t /*ioId*/,
                     effect_handle_t* handle) {
    if (!uuid || !handle) return -EINVAL;
    if (std::memcmp(uuid, &kAvcrtEffectUuid, sizeof(effect_uuid_t)) != 0) {
        return -ENOENT;
    }
    auto* ctx = new (std::nothrow) EffectContext();
    if (!ctx) return -ENOMEM;
    ctx->itfe = kAvcrtInterface;
    ctx->store = acquire_store();
    *handle = reinterpret_cast<effect_handle_t>(ctx);
    avcrt::log::i("avcrt effect instance created");
    return 0;
}

int32_t avcrt_release(effect_handle_t handle) {
    auto* ctx = reinterpret_cast<EffectContext*>(handle);
    delete ctx;
    return 0;
}

int32_t avcrt_get_descriptor_lib(const effect_uuid_t* uuid, effect_descriptor_t* desc) {
    // Log on EVERY call, BEFORE the uuid check, so we can tell "factory never
    // queried our lib at all" (no log) from "queried with some other effect's
    // uuid" (logs a non-matching timeLow, then early -ENOENT).
    avcrt::log::i("avcrt_get_descriptor_lib called: uuid.timeLow=0x%08x",
                  uuid ? uuid->timeLow : 0u);
    if (!uuid || !desc) return -EINVAL;
    if (std::memcmp(uuid, &kAvcrtEffectUuid, sizeof(effect_uuid_t)) != 0) {
        return -ENOENT;
    }
    avcrt::log::i("avcrt_get_descriptor_lib: uuid MATCH — returning our descriptor");
    *desc = kAvcrtDescriptor;
    return 0;
}

}  // namespace

// Forward-declared so the load-probe constructor can print the exact bytes the
// effects factory validates right after dlopen.
extern "C" audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM;

// Fires the instant the audio HAL / effect factory dlopen()s this .so, before any
// descriptor query. We KNOW (from on-device logs) the ctor runs but get_descriptor
// never does → the factory rejects the library at the tag/version check and skips
// its effects. So log the LIVE struct fields the factory reads: expect
// tag=0x41454c54 ('AELT'), version=0x00030000 (3.0), non-null fn-ptrs. Any
// mismatch here is the rejection cause; if all correct, the reject is a vendor
// factory quirk, not our struct.
__attribute__((constructor))
static void avcrt_on_dlopen() {
    avcrt::log::i("libavcrtfx dlopen'd; INFO_SYM tag=0x%08x version=0x%08x create=%p release=%p get_descriptor=%p",
                  AUDIO_EFFECT_LIBRARY_INFO_SYM.tag,
                  AUDIO_EFFECT_LIBRARY_INFO_SYM.version,
                  (void*)AUDIO_EFFECT_LIBRARY_INFO_SYM.create_effect,
                  (void*)AUDIO_EFFECT_LIBRARY_INFO_SYM.release_effect,
                  (void*)AUDIO_EFFECT_LIBRARY_INFO_SYM.get_descriptor);
}

extern "C" __attribute__((visibility("default"))) audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    /*tag=*/             AUDIO_EFFECT_LIBRARY_TAG,
    /*version=*/         EFFECT_LIBRARY_API_VERSION,
    /*name=*/            "AVCRT Audio Effects",
    /*implementor=*/     "AVCRT",
    /*create_effect=*/   avcrt_create,
    /*release_effect=*/  avcrt_release,
    /*get_descriptor=*/  avcrt_get_descriptor_lib,
};

// ---- Why this .so is built WITHOUT CFI but WITH -z nodelete -------------------
// The platform effects factory (/vendor/lib64/libeffects.so) is CFI-instrumented and makes a
// CFI-checked indirect call into our get_descriptor/create entry points. That is FINE for a
// non-CFI library: bionic's linker marks a loaded library that exports no __cfi_check as
// "unchecked" in the CFI shadow (linker_cfi.cpp: `if (cfi_check == 0) AddUnchecked(...)`), so
// the factory's call passes. The crash we chased for a whole session (__loader_cfi_fail /
// SIGABRT at Effect_GetDescriptor on the record path) was NOT a CFI type-id problem — it was a
// USE-AFTER-UNLOAD: the factory dlopen's us, reads the descriptor, dlclose's us (bionic's
// BeforeUnload then AddInvalid's our pages → kInvalid), and a later createEffect calls a stale
// get_descriptor pointer into the now-unloaded library → kInvalid → abort. The fix is to keep
// the .so permanently mapped via `-Wl,-z,nodelete` (see CMakeLists): dlclose becomes a no-op,
// our pages stay valid/unchecked, and every factory call — boot enumeration and record-time
// create alike — resolves cleanly. Trying to satisfy CFI from our side (hand-rolled or compiler
// __cfi_check, recover, generalize-pointers, static-libsan) was a dead end — it bootlooped and
// never addressed the real unload-lifecycle issue.
