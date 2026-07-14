#ifdef _DEBUG

#include "console.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>

namespace Con {
namespace {

HANDLE g_console = nullptr;
FILE*  g_log = nullptr;

// Default Ok so success lines show; Trace still opt-in via env / SetMinLevel
Level  g_minLevel = Level::Ok;
bool   g_alwaysFlush = false;
int    g_linesSinceFlush = 0;

// Counters for Stats()
volatile LONG g_cntOk = 0;
volatile LONG g_cntInfo = 0;
volatile LONG g_cntWarn = 0;
volatile LONG g_cntError = 0;
volatile LONG g_cntSeh = 0;
volatile LONG g_cntTrace = 0;
volatile LONG g_cntSuppressed = 0;

// Rate-limit table
struct RateEntry {
	char  key[96];
	DWORD lastTick;
};
constexpr int kRateSlots = 96;
RateEntry g_rate[kRateSlots] = {};

// Once table
struct OnceEntry {
	char key[96];
	bool used;
};
constexpr int kOnceSlots = 64;
OnceEntry g_once[kOnceSlots] = {};

bool RateAllow(const char* key, DWORD intervalMs)
{
	if (!key || !key[0])
		return true;

	const DWORD now = GetTickCount();
	int freeSlot = -1;

	for (int i = 0; i < kRateSlots; ++i) {
		if (g_rate[i].key[0] == '\0') {
			if (freeSlot < 0)
				freeSlot = i;
			continue;
		}
		if (strncmp(g_rate[i].key, key, sizeof(g_rate[i].key) - 1) == 0) {
			if (now - g_rate[i].lastTick < intervalMs) {
				InterlockedIncrement(&g_cntSuppressed);
				return false;
			}
			g_rate[i].lastTick = now;
			return true;
		}
	}

	if (freeSlot >= 0) {
		strncpy_s(g_rate[freeSlot].key, key, _TRUNCATE);
		g_rate[freeSlot].lastTick = now;
		return true;
	}
	// Table full — allow (prefer signal over drop forever)
	return true;
}

bool OnceAllow(const char* key)
{
	if (!key || !key[0])
		return true;

	int freeSlot = -1;
	for (int i = 0; i < kOnceSlots; ++i) {
		if (!g_once[i].used) {
			if (freeSlot < 0)
				freeSlot = i;
			continue;
		}
		if (strncmp(g_once[i].key, key, sizeof(g_once[i].key) - 1) == 0)
			return false;
	}
	if (freeSlot >= 0) {
		strncpy_s(g_once[freeSlot].key, key, _TRUNCATE);
		g_once[freeSlot].used = true;
		return true;
	}
	return true;
}

const char* SehName(DWORD code)
{
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS";
	case EXCEPTION_DATATYPE_MISALIGNMENT:    return "MISALIGNMENT";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIV0";
	case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID";
	case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
	case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
	case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSN";
	case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIV0";
	case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
	case EXCEPTION_INVALID_HANDLE:           return "INVALID_HANDLE";
	case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSN";
	case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
	case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
	case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
	case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
	default:                                 return "EXCEPTION";
	}
}

WORD ColorFor(Level level)
{
	switch (level) {
	case Level::Trace: return FOREGROUND_INTENSITY; // dim gray
	case Level::Ok:    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	case Level::Info:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	case Level::Warn:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	case Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
	case Level::Seh:   return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
	default:           return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	}
}

const char* PrefixFor(Level level)
{
	switch (level) {
	case Level::Trace: return "TRC";
	case Level::Ok:    return " OK";
	case Level::Info:  return "INF";
	case Level::Warn:  return "WRN";
	case Level::Error: return "ERR";
	case Level::Seh:   return "SEH";
	default:           return "???";
	}
}

void BumpCount(Level level)
{
	switch (level) {
	case Level::Trace: InterlockedIncrement(&g_cntTrace); break;
	case Level::Ok:    InterlockedIncrement(&g_cntOk); break;
	case Level::Info:  InterlockedIncrement(&g_cntInfo); break;
	case Level::Warn:  InterlockedIncrement(&g_cntWarn); break;
	case Level::Error: InterlockedIncrement(&g_cntError); break;
	case Level::Seh:   InterlockedIncrement(&g_cntSeh); break;
	}
}

void FormatTimestamp(char* out, size_t outSz)
{
	SYSTEMTIME st{};
	GetLocalTime(&st);
	_snprintf_s(out, outSz, _TRUNCATE, "%02u:%02u:%02u.%03u",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void WriteRaw(WORD attr, const char* text, bool flushNow)
{
	if (!text)
		return;

	// Visual Studio / WinDbg Output window
	OutputDebugStringA(text);

	if (g_console) {
		SetConsoleTextAttribute(g_console, attr);
		DWORD written = 0;
		WriteConsoleA(g_console, text, (DWORD)strlen(text), &written, nullptr);
		SetConsoleTextAttribute(g_console,
			FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
	}

	if (g_log) {
		fputs(text, g_log);
		if (flushNow || g_alwaysFlush) {
			fflush(g_log);
			g_linesSinceFlush = 0;
		} else if (++g_linesSinceFlush >= 8) {
			fflush(g_log);
			g_linesSinceFlush = 0;
		}
	}

	if (!g_console) {
		fputs(text, stdout);
		if (flushNow)
			fflush(stdout);
	}
}

} // namespace

void Init(HANDLE console, FILE* logFile)
{
	g_console = console;
	g_log = logFile;
	g_minLevel = Level::Ok;
	g_alwaysFlush = false;
	g_linesSinceFlush = 0;

	// TEMPLEWARE_LOG_TRACE=1 → enable Trace by default
	char env[8]{};
	if (GetEnvironmentVariableA("TEMPLEWARE_LOG_TRACE", env, sizeof(env)) > 0
		&& (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'))
		g_minLevel = Level::Trace;

	if (GetEnvironmentVariableA("TEMPLEWARE_LOG_FLUSH", env, sizeof(env)) > 0
		&& (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'))
		g_alwaysFlush = true;

	// Session banner into log
	if (g_log) {
		SYSTEMTIME st{};
		GetLocalTime(&st);
		fprintf(g_log,
			"\n======== TempleWare DEBUG session %04u-%02u-%02u %02u:%02u:%02u "
			"pid=%lu ========\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			GetCurrentProcessId());
		fflush(g_log);
	}
}

void SetMinLevel(Level level)
{
	g_minLevel = level;
}

Level GetMinLevel()
{
	return g_minLevel;
}

void SetAlwaysFlush(bool on)
{
	g_alwaysFlush = on;
}

void VPrint(Level level, const char* fmt, va_list args)
{
	if (!fmt)
		return;
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;

	char ts[32];
	FormatTimestamp(ts, sizeof(ts));

	const DWORD tid = GetCurrentThreadId();
	const char* pf = PrefixFor(level);

	// header: 12:34:56.789 [TID] LVL |
	char line[4608];
	int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
		"%s [%04lX] %s | ", ts, tid & 0xFFFFul, pf);
	if (n < 0)
		n = 0;

	vsnprintf_s(line + n, sizeof(line) - static_cast<size_t>(n), _TRUNCATE, fmt, args);

	// ensure newline
	const size_t len = strlen(line);
	if (len + 2 < sizeof(line)) {
		line[len] = '\n';
		line[len + 1] = '\0';
	}

	const bool flushNow = (level >= Level::Error) || g_alwaysFlush;
	WriteRaw(ColorFor(level), line, flushNow);
	BumpCount(level);
}

void Print(Level level, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(level, fmt, args);
	va_end(args);
}

void Trace(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Trace, fmt, args);
	va_end(args);
}

void Ok(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Ok, fmt, args);
	va_end(args);
}

void Info(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Info, fmt, args);
	va_end(args);
}

void Warn(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Warn, fmt, args);
	va_end(args);
}

void Error(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Error, fmt, args);
	va_end(args);
}

void Seh(const char* where, DWORD code)
{
	if (!where)
		where = "?";
	// Rate-limit identical sites (3s) so a tight loop doesn't flood
	if (!RateAllow(where, 3000))
		return;
	Print(Level::Seh, "%s — %s (0x%08X)", where, SehName(code), code);
}

void OffsetMiss(const char* name, uintptr_t value)
{
	if (!name)
		return;
	if (!RateAllow(name, 3000))
		return;
	if (value)
		Print(Level::Warn, "Offset miss: %s (got 0x%llX)", name, (unsigned long long)value);
	else
		Print(Level::Warn, "Offset miss: %s", name);
}

void PatternMiss(const char* module, const char* pattern)
{
	if (!module || !pattern)
		return;
	char key[128];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "pat:%s", module);
	if (!RateAllow(key, 3000))
		return;
	// Truncate long patterns for readability
	char patShort[96];
	_snprintf_s(patShort, sizeof(patShort), _TRUNCATE, "%.80s%s",
		pattern, strlen(pattern) > 80 ? "..." : "");
	Print(Level::Warn, "Pattern miss: %s -> %s", module, patShort);
}

void Once(const char* key, const char* fmt, ...)
{
	if (!OnceAllow(key ? key : "?"))
		return;
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Info, fmt, args);
	va_end(args);
}

void Rate(const char* key, DWORD intervalMs, const char* fmt, ...)
{
	if (!RateAllow(key ? key : "?", intervalMs ? intervalMs : 1000))
		return;
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Info, fmt, args);
	va_end(args);
}

void Hex(const char* label, const void* data, size_t len)
{
	if (!data || len == 0) {
		Print(Level::Info, "%s: <null/empty>", label ? label : "hex");
		return;
	}

	constexpr size_t kMax = 256;
	const size_t n = (len > kMax) ? kMax : len;
	const auto* p = static_cast<const unsigned char*>(data);

	Print(Level::Info, "%s: %zu bytes @ %p%s",
		label ? label : "hex", len, data, len > kMax ? " (first 256)" : "");

	char row[128];
	for (size_t off = 0; off < n; off += 16) {
		int pos = _snprintf_s(row, sizeof(row), _TRUNCATE, "  %04zX: ", off);
		const size_t lineLen = (n - off > 16) ? 16 : (n - off);
		for (size_t i = 0; i < 16; ++i) {
			if (i < lineLen)
				pos += _snprintf_s(row + pos, sizeof(row) - pos, _TRUNCATE, "%02X ", p[off + i]);
			else
				pos += _snprintf_s(row + pos, sizeof(row) - pos, _TRUNCATE, "   ");
		}
		pos += _snprintf_s(row + pos, sizeof(row) - pos, _TRUNCATE, " ");
		for (size_t i = 0; i < lineLen; ++i) {
			const unsigned char c = p[off + i];
			pos += _snprintf_s(row + pos, sizeof(row) - pos, _TRUNCATE, "%c",
				(c >= 32 && c < 127) ? c : '.');
		}
		WriteRaw(ColorFor(Level::Trace), row, false);
		WriteRaw(ColorFor(Level::Trace), "\n", false);
	}
}

void Section(const char* title)
{
	// ASCII only — Windows console often isn't UTF-8 (mojibake on box-drawing)
	char bar[96];
	_snprintf_s(bar, sizeof(bar), _TRUNCATE,
		"======== %s ========", title ? title : "");
	Print(Level::Info, "%s", bar);
}

void Stats()
{
	Print(Level::Info,
		"log stats: ok=%ld info=%ld warn=%ld err=%ld seh=%ld trc=%ld suppressed=%ld",
		(long)g_cntOk, (long)g_cntInfo, (long)g_cntWarn,
		(long)g_cntError, (long)g_cntSeh, (long)g_cntTrace,
		(long)g_cntSuppressed);
}

ScopedTimer::ScopedTimer(const char* n, DWORD slowThresholdMs) noexcept
	: name(n ? n : "?")
	, t0(GetTickCount64())
	, slowMs(slowThresholdMs)
{
}

ScopedTimer::~ScopedTimer()
{
	const ULONGLONG dt = GetTickCount64() - t0;
	if (dt >= slowMs)
		Print(Level::Info, "timer %s: %llu ms", name, (unsigned long long)dt);
	else
		Print(Level::Trace, "timer %s: %llu ms", name, (unsigned long long)dt);
}

} // namespace Con

#endif // _DEBUG
