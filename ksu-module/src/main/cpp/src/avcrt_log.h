#pragma once
//
// Thin wrapper around android/log.h with a shared tag. Everything logged here
// flows into logcat as `[avcrtfx]`. The Kotlin LogSink in :core:common cannot
// see these messages — that's intentional, the in-app log buffer is for the
// app's own lifecycle, not for cross-process audio HAL spam.
//
namespace avcrt::log {
void v(const char* fmt, ...);
void d(const char* fmt, ...);
void i(const char* fmt, ...);
void w(const char* fmt, ...);
void e(const char* fmt, ...);
}  // namespace avcrt::log
