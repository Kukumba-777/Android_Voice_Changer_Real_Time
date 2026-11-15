// Compact subset of the Android Audio Effects Framework ABI.
//
// Combines just-enough types from AOSP's `system/media/audio/include/system/audio_effect.h`
// and `hardware/audio_effect.h` so we can build against NDK without dragging in the
// full AOSP source tree. License of the underlying definitions: Apache-2.0.
//
// Keep this header tiny — only the symbols `avcrt_effect.cpp` actually uses.

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

#define EFFECT_MAKE_API_VERSION(M, m)   ((((M) & 0xFFFF) << 16) | ((m) & 0xFFFF))
#define EFFECT_LIBRARY_API_VERSION      EFFECT_MAKE_API_VERSION(3, 0)
#define EFFECT_CONTROL_API_VERSION      EFFECT_MAKE_API_VERSION(2, 0)
#define AUDIO_EFFECT_LIBRARY_TAG        ((((uint32_t)'A') << 24) | (((uint32_t)'E') << 16) | (((uint32_t)'L') << 8) | ((uint32_t)'T'))
// Name of the exported library-info symbol. The effects factory finds our library via
// dlsym(handle, "AELI") — so the exported symbol MUST be named AELI, NOT the literal macro.
// (Distinct from the TAG above: tag value is 'AELT', the symbol name is 'AELI'.)
#define AUDIO_EFFECT_LIBRARY_INFO_SYM        AELI
#define AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR "AELI"

#define EFFECT_FLAG_TYPE_PRE_PROC       0x00000003u  // bits 0-2: type
#define EFFECT_FLAG_INSERT_FIRST        0x00000010u  // bits 3-5: insert mode

#ifndef EINVAL
#  define EINVAL  22
#endif
#ifndef ENOENT
#  define ENOENT   2
#endif
#ifndef ENOMEM
#  define ENOMEM  12
#endif

typedef struct effect_uuid_s {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t clockSeq;
    uint8_t  node[6];
} effect_uuid_t;

typedef struct effect_descriptor_s {
    effect_uuid_t type;
    effect_uuid_t uuid;
    uint32_t      apiVersion;
    uint32_t      flags;
    uint16_t      cpuLoad;
    uint16_t      memoryUsage;
    char          name[64];
    char          implementor[64];
} effect_descriptor_t;

typedef struct audio_buffer_s {
    size_t frameCount;
    union {
        void*    raw;
        float*   f32;
        int32_t* s32;
        int16_t* s16;
        uint8_t* u8;
    };
} audio_buffer_t;

typedef struct buffer_config_s {
    audio_buffer_t buffer;
    uint32_t       samplingRate;
    uint32_t       channels;     // audio_channel_mask_t (a bitmask, NOT a count — convert via channel_count_from_in_mask)
    uint8_t        format;
    uint8_t        accessMode;
    uint16_t       mask;
} buffer_config_t;

typedef struct effect_config_s {
    buffer_config_t inputCfg;
    buffer_config_t outputCfg;
} effect_config_t;

struct effect_interface_s;
typedef struct effect_interface_s** effect_handle_t;

typedef int32_t (*effect_process_func)(effect_handle_t self, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer);
typedef int32_t (*effect_command_func)(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize, void* cmdData, uint32_t* replySize, void* replyData);
typedef int32_t (*effect_get_descriptor_func)(effect_handle_t self, effect_descriptor_t* desc);
typedef int32_t (*effect_process_reverse_func)(effect_handle_t self, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer);

typedef struct effect_interface_s {
    effect_process_func         process;
    effect_command_func         command;
    effect_get_descriptor_func  get_descriptor;
    effect_process_reverse_func process_reverse;
} effect_interface_t;

enum effect_command_e {
    EFFECT_CMD_INIT                          = 0,
    EFFECT_CMD_SET_CONFIG                    = 1,
    EFFECT_CMD_RESET                         = 2,
    EFFECT_CMD_ENABLE                        = 3,
    EFFECT_CMD_DISABLE                       = 4,
    EFFECT_CMD_SET_PARAM                     = 5,
    EFFECT_CMD_SET_PARAM_DEFERRED            = 6,
    EFFECT_CMD_SET_PARAM_COMMIT              = 7,
    EFFECT_CMD_GET_PARAM                     = 8,
    EFFECT_CMD_SET_DEVICE                    = 9,
    EFFECT_CMD_SET_VOLUME                    = 10,
    EFFECT_CMD_SET_AUDIO_MODE                = 11,
    EFFECT_CMD_SET_CONFIG_REVERSE            = 12,
    EFFECT_CMD_SET_INPUT_DEVICE              = 13,
    EFFECT_CMD_GET_CONFIG                    = 14,
    EFFECT_CMD_GET_CONFIG_REVERSE            = 15,
    EFFECT_CMD_GET_FEATURE_SUPPORTED_CONFIGS = 16,
    EFFECT_CMD_GET_FEATURE_CONFIG            = 17,
    EFFECT_CMD_SET_FEATURE_CONFIG            = 18,
    EFFECT_CMD_SET_AUDIO_SOURCE              = 19,
    EFFECT_CMD_OFFLOAD                       = 20,
    EFFECT_CMD_DUMP                          = 21,
    EFFECT_CMD_FIRST_PROPRIETARY             = 0x10000,
};

typedef struct audio_effect_library_s {
    uint32_t    tag;
    uint32_t    version;
    const char* name;
    const char* implementor;
    int32_t (*create_effect)(const effect_uuid_t* uuid, int32_t sessionId, int32_t ioId, effect_handle_t* pHandle);
    int32_t (*release_effect)(effect_handle_t handle);
    int32_t (*get_descriptor)(const effect_uuid_t* uuid, effect_descriptor_t* pDescriptor);
} audio_effect_library_t;

#ifdef __cplusplus
}  // extern "C"
#endif
