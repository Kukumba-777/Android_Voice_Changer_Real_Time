#pragma once

#include <array>
#include <cstddef>

#include "avcrt_af_backend.h"

namespace avcrt::stock_capture {

enum class Registration { Unavailable, Partial, Ready };
enum class RouteQuery { Unsupported, Scalar, Array };

inline constexpr char kRouteId[] = "native_getRoutedDeviceId";
inline constexpr char kRouteIds[] = "native_getRoutedDeviceIds";
inline constexpr char kRouteIdSignature[] = "()I";
inline constexpr char kRouteIdsSignature[] = "()[I";

template <class Env, class Class>
auto find_instance_method(Env* env, Class cls, const char* name, const char* signature) {
    auto method = env->GetMethodID(cls, name, signature);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        method = {};
    }
    return method;
}

// Only claim the reviewed scalar or array contract. If both are exposed,
// replacing just one would leave an uncovered query after takeover.
template <class Env, class Class>
RouteQuery route_query(Env* env, Class cls) {
    const bool scalar = find_instance_method(env, cls, kRouteId, kRouteIdSignature) != nullptr;
    const bool array = find_instance_method(env, cls, kRouteIds, kRouteIdsSignature) != nullptr;
    if (scalar == array) return RouteQuery::Unsupported;
    return array ? RouteQuery::Array : RouteQuery::Scalar;
}

constexpr bool single_route_matches(int observed, size_t count, int actual) {
    return observed > 0 && count == 1 && actual == observed;
}

// Check the full instance-method surface before asking Zygisk to replace any
// methods. A Java declaration alone is not proof of a registered native.
// The API must also return a callable original for every method. On partial
// installs only the successfully hooked entries have originals to forward to.
template <class Api, class Env, class Class, class Method, size_t N>
Registration install_required_jni(Api* api, Env* env, Class cls, const char* name, Method (&methods)[N]) {
    std::array<void*, N> replacements;
    for (size_t i = 0; i < N; ++i) {
        const auto& m = methods[i];
        if (!find_instance_method(env, cls, m.name, m.signature)) return Registration::Unavailable;
        replacements[i] = m.fnPtr;
    }
    if (!api->hookJniNativeMethods(env, name, methods, N)) return Registration::Unavailable;
    bool complete = !env->ExceptionCheck();
    if (!complete) env->ExceptionClear();
    for (size_t i = 0; i < N; ++i) {
        if (methods[i].fnPtr == replacements[i]) methods[i].fnPtr = nullptr;
        if (!methods[i].fnPtr) complete = false;
    }
    return complete ? Registration::Ready : Registration::Partial;
}

// Sample the live master switch even before the cached DSP revision changes.
template <typename Enabled>
bool aac_allowed(Enabled enabled) {
    return enabled("avcrt.dsp.enabled") && !enabled(af_backend::kProperty) &&
        !enabled("debug.avcrt.af_capture") && !enabled("debug.avcrt.stagefright");
}

bool aac_allowed();

} // namespace avcrt::stock_capture
