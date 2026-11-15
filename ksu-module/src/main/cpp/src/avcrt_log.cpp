#include "avcrt_log.h"

#include <android/log.h>
#include <cstdarg>
#include <cstdio>

#include "avcrt_paths.h"

namespace avcrt::log {

namespace {
constexpr size_t kMaxLineLen = 1024;

void emit(int prio, const char* fmt, va_list args) {
    char buf[kMaxLineLen];
    vsnprintf(buf, sizeof(buf), fmt, args);
    __android_log_print(prio, avcrt::paths::kLogTag, "%s", buf);
}
}  // namespace

void v(const char* fmt, ...) { va_list a; va_start(a, fmt); emit(ANDROID_LOG_VERBOSE, fmt, a); va_end(a); }
void d(const char* fmt, ...) { va_list a; va_start(a, fmt); emit(ANDROID_LOG_DEBUG,   fmt, a); va_end(a); }
void i(const char* fmt, ...) { va_list a; va_start(a, fmt); emit(ANDROID_LOG_INFO,    fmt, a); va_end(a); }
void w(const char* fmt, ...) { va_list a; va_start(a, fmt); emit(ANDROID_LOG_WARN,    fmt, a); va_end(a); }
void e(const char* fmt, ...) { va_list a; va_start(a, fmt); emit(ANDROID_LOG_ERROR,   fmt, a); va_end(a); }

}  // namespace avcrt::log
