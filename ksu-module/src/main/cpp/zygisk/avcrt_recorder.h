#pragma once

#include "zygisk.hpp"
#include "avcrt_config.h"

namespace avcrt::recorder {

// A read owned by the replacement must bypass both existing AVCRT read hooks.
using Read = jint (*)(JNIEnv*, jobject, jmethodID, jshortArray, jint);
using Config = bool (*)(DspConfig&);

void install(zygisk::Api*, JNIEnv*, Read, Config);

} // namespace avcrt::recorder
