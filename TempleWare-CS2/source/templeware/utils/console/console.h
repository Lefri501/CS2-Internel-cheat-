#pragma once
#include <Windows.h>
#include <cstdarg>
#include <cstdint>

// Debug-build console + log file. Release: all no-ops (zero cost).
//
// Usage:
//   Con::Ok / Info / Warn / Error / Trace / Seh
//   Con::OffsetMiss / PatternMiss
//   Con::Once("key", "msg %d", x)     — log once per process
//   Con::Rate("key", 500, "spam")     — rate-limit by key (ms)
//   Con::Hex("label", ptr, len)       — hex dump
//   Con::Section("Hooks")             — visual separator
//   Con::ScopedTimer t("Bones::Init") — RAII duration on scope exit
//   TW_TRACE / TW_OK / TW_INFO / ...  — macros with optional __FUNCTION__

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

// console = STD_OUTPUT_HANDLE; logFile = fopen log (optional)
void Init(HANDLE console, FILE* logFile);

// Minimum level printed (default Ok). Trace requires SetMinLevel(Trace) or env.
void SetMinLevel(Level level);
Level GetMinLevel();

// Force flush after every line (default: Error/Seh always; others every N lines)
void SetAlwaysFlush(bool on);

void Print(Level level, const char* fmt, ...);
void VPrint(Level level, const char* fmt, va_list args);

void Trace(const char* fmt, ...);
void Ok(const char* fmt, ...);
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

// SEH helper — prints named code + hex. Rate-limited per `where`.
void Seh(const char* where, DWORD code);

// Pattern / offset diagnostics (rate-limited)
void OffsetMiss(const char* name, uintptr_t value = 0);
void PatternMiss(const char* module, const char* pattern);

// Log once per key for process lifetime
void Once(const char* key, const char* fmt, ...);

// Rate-limited log (same key suppressed for intervalMs)
void Rate(const char* key, DWORD intervalMs, const char* fmt, ...);

// Hex dump (max 256 bytes shown)
void Hex(const char* label, const void* data, size_t len);

// Visual section banner
void Section(const char* title);

// Stats (errors/warns/seh counts) — call from unload or on demand
void Stats();

// RAII scope timer (logs at Trace on exit, or Info if ms >= slowMs)
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

// Convenience macros (Debug only)
#define TW_TRACE(...)  ::Con::Trace(__VA_ARGS__)
#define TW_OK(...)     ::Con::Ok(__VA_ARGS__)
#define TW_INFO(...)   ::Con::Info(__VA_ARGS__)
#define TW_WARN(...)   ::Con::Warn(__VA_ARGS__)
#define TW_ERR(...)    ::Con::Error(__VA_ARGS__)
#define TW_SEH(w, c)   ::Con::Seh((w), (c))
#define TW_ONCE(k, ...) ::Con::Once((k), __VA_ARGS__)
#define TW_RATE(k, ms, ...) ::Con::Rate((k), (ms), __VA_ARGS__)
#define TW_FN_ENTER()  ::Con::Trace(">> %s", __FUNCTION__)
#define TW_FN_LEAVE()  ::Con::Trace("<< %s", __FUNCTION__)
#define TW_TIMER(name) ::Con::ScopedTimer _tw_timer_##__LINE__((name))

#else // Release — all no-ops

namespace Con {

enum class Level : int {
	Trace = 0, Ok = 1, Info = 2, Warn = 3, Error = 4, Seh = 5,
};

inline void Init(HANDLE, FILE*) {}
inline void SetMinLevel(Level) {}
inline Level GetMinLevel() { return Level::Info; }
inline void SetAlwaysFlush(bool) {}
inline void Print(Level, const char*, ...) {}
inline void VPrint(Level, const char*, va_list) {}
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
#define TW_FN_ENTER()      ((void)0)
#define TW_FN_LEAVE()      ((void)0)
#define TW_TIMER(name)     ((void)0)

#endif
