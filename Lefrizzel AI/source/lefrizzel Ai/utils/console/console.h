#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>

// Debug-build console + log file. Release: all no-ops (zero cost).
//
// Line format (Debug):
//   HH:MM:SS.mmm  LVL  message
//   HH:MM:SS.mmm  LVL  [tag] message
// Indented detail (same event):
//               |  key   value
//
// Usage:
//   Con::Ok / Info / Warn / Error / Trace / Seh
//   Con::Tag("hooks", Level::Ok, "CreateMove @ %p", p)
//   Con::Rate / RateAt(level)
//   Con::Detail("tick  %d  frac %.3f", t, f)   // indented follow-up
//   Con::OffsetMiss / PatternMiss
//   Con::Once / Hex / Section / ScopedTimer / Stats

#ifdef _DEBUG

namespace Con {

enum class Level : int {
	Trace = 0, // hot-path / verbose (off unless SetMinLevel Trace)
	Ok    = 1,
	Info  = 2,
	Warn  = 3,
	Error = 4,
	Seh   = 5,
};

void Init(HANDLE console, FILE* logFile);
void Shutdown();

void SetMinLevel(Level level);
Level GetMinLevel();
void SetAlwaysFlush(bool on);

// Show thread id in header (default off — cleaner logs)
void SetShowTid(bool on);

void Print(Level level, const char* fmt, ...);
void VPrint(Level level, const char* fmt, va_list args);

// Same as Print but prefixes "[tag] " when tag non-null
void Tag(const char* tag, Level level, const char* fmt, ...);
void VTag(const char* tag, Level level, const char* fmt, va_list args);

// Indented detail line under last event (no level prefix noise)
//   "               |  key   value"
void Detail(const char* fmt, ...);
void VDetail(const char* fmt, va_list args);

void Trace(const char* fmt, ...);
void Ok(const char* fmt, ...);
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

void Seh(const char* where, DWORD code);

void OffsetMiss(const char* name, uintptr_t value = 0);
void PatternMiss(const char* module, const char* pattern);

void Once(const char* key, const char* fmt, ...);
void Rate(const char* key, DWORD intervalMs, const char* fmt, ...);
// Rate-limit at a specific level (seed WAIT=Info, FIRE=Ok, etc.)
void RateAt(const char* key, DWORD intervalMs, Level level, const char* fmt, ...);

void Hex(const char* label, const void* data, size_t len);
void Section(const char* title);
void Stats();

struct ScopedTimer {
	const char* name;
	ULONGLONG   t0;
	DWORD       slowMs;
	explicit ScopedTimer(const char* n, DWORD slowThresholdMs = 5) noexcept;
	~ScopedTimer();
	ScopedTimer(const ScopedTimer&) = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;
};

} // namespace Con

#define TW_TRACE(...)  ::Con::Trace(__VA_ARGS__)
#define TW_OK(...)     ::Con::Ok(__VA_ARGS__)
#define TW_INFO(...)   ::Con::Info(__VA_ARGS__)
#define TW_WARN(...)   ::Con::Warn(__VA_ARGS__)
#define TW_ERR(...)    ::Con::Error(__VA_ARGS__)
#define TW_SEH(w, c)   ::Con::Seh((w), (c))
#define TW_ONCE(k, ...) ::Con::Once((k), __VA_ARGS__)
#define TW_RATE(k, ms, ...) ::Con::Rate((k), (ms), __VA_ARGS__)
#define TW_TAG(tag, lvl, ...) ::Con::Tag((tag), (lvl), __VA_ARGS__)
#define TW_DETAIL(...) ::Con::Detail(__VA_ARGS__)
#define TW_FN_ENTER()  ::Con::Trace(">> %s", __FUNCTION__)
#define TW_FN_LEAVE()  ::Con::Trace("<< %s", __FUNCTION__)
#define TW_TIMER(name) ::Con::ScopedTimer _tw_timer_##__LINE__((name))

#else // Release — all no-ops

namespace Con {

enum class Level : int {
	Trace = 0, Ok = 1, Info = 2, Warn = 3, Error = 4, Seh = 5,
};

inline void Init(HANDLE, FILE*) {}
inline void Shutdown() {}
inline void SetMinLevel(Level) {}
inline Level GetMinLevel() { return Level::Ok; }
inline void SetAlwaysFlush(bool) {}
inline void SetShowTid(bool) {}
inline void Print(Level, const char*, ...) {}
inline void VPrint(Level, const char*, va_list) {}
inline void Tag(const char*, Level, const char*, ...) {}
inline void VTag(const char*, Level, const char*, va_list) {}
inline void Detail(const char*, ...) {}
inline void VDetail(const char*, va_list) {}
inline void Trace(const char*, ...) {}
inline void Ok(const char*, ...) {}
inline void Info(const char*, ...) {}
inline void Warn(const char*, ...) {}
inline void Error(const char*, ...) {}
inline void Seh(const char*, DWORD) {}
inline void OffsetMiss(const char*, uintptr_t = 0) {}
inline void PatternMiss(const char*, const char*) {}
inline void Once(const char*, const char*, ...) {}
inline void Rate(const char*, DWORD, const char*, ...) {}
inline void RateAt(const char*, DWORD, Level, const char*, ...) {}
inline void Hex(const char*, const void*, size_t) {}
inline void Section(const char*) {}
inline void Stats() {}

struct ScopedTimer {
	explicit ScopedTimer(const char*, DWORD = 5) noexcept {}
	~ScopedTimer() = default;
	ScopedTimer(const ScopedTimer&) = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;
};

} // namespace Con

#define TW_TRACE(...)      ((void)0)
#define TW_OK(...)         ((void)0)
#define TW_INFO(...)       ((void)0)
#define TW_WARN(...)       ((void)0)
#define TW_ERR(...)        ((void)0)
#define TW_SEH(w, c)       ((void)0)
#define TW_ONCE(k, ...)    ((void)0)
#define TW_RATE(k, ms, ...) ((void)0)
#define TW_TAG(tag, lvl, ...) ((void)0)
#define TW_DETAIL(...)     ((void)0)
#define TW_FN_ENTER()      ((void)0)
#define TW_FN_LEAVE()      ((void)0)
#define TW_TIMER(name)     ((void)0)

#endif
