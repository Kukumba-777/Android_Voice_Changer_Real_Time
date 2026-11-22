#pragma once

#include "avcrt_config.h"
#include "zygisk.hpp"

#include <optional>

namespace avcrt::direct_aac {

using Read = jint (*)(JNIEnv*, jobject, jobject, jint, jboolean);
using Config = bool (*)(DspConfig&);

void install(zygisk::Api*, JNIEnv*, Read, Config);
// Empty means the existing AudioRecord hook should handle the read normally.
std::optional<jint> read(JNIEnv*, jobject, jobject, jint, jboolean);

} // namespace avcrt::direct_aac
