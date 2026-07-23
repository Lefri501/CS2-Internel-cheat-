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
bool   g_showTid = false;
int    g_linesSinceFlush = 0;

CRITICAL_SECTION g_cs{};
bool g_csInit = false;

volatile LONG g_cntOk = 0;
volatile LONG g_cntInfo = 0;
volatile LONG g_cntWarn = 0;
volatile LONG g_cntError = 0;
volatile LONG g_cntSeh = 0;
volatile LONG g_cntTrace = 0;
volatile LONG g_cntSuppressed = 0;

struct RateEntry {
	char  key[96];
	DWORD lastTick;
};
constexpr int kRateSlots = 96;
RateEntry g_rate[kRateSlots] = {};

struct OnceEntry {
	char key[96];
	bool used;
};
constexpr int kOnceSlots = 64;
OnceEntry g_once[kOnceSlots] = {};

void EnsureCs()
{
	if (g_csInit)
		return;
	InitializeCriticalSection(&g_cs);
	g_csInit = true;
}

struct LockGuard {
	LockGuard()
	{
		EnsureCs();
		EnterCriticalSection(&g_cs);
	}
	~LockGuard() { LeaveCriticalSection(&g_cs); }
	LockGuard(const LockGuard&) = delete;
	LockGuard& operator=(const LockGuard&) = delete;
};

bool RateAllowUnlocked(const char* key, DWORD intervalMs)
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
	return true;
}

bool OnceAllowUnlocked(const char* key)
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
		if (strncmp(g_once[i].key, key, sizeof(g_once[i].key) - 1) == 0) {
			InterlockedIncrement(&g_cntSuppressed);
			return false;
		}
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
	case Level::Trace: return FOREGROUND_INTENSITY;
	case Level::Ok:    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	case Level::Info:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	case Level::Warn:  return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	case Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
	case Level::Seh:   return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
	default:           return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	}
}

// Fixed-width level tags for column alignment
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

// Caller must hold g_cs
void WriteRawUnlocked(WORD attr, const char* text, bool flushNow)
{
	if (!text)
		return;

	OutputDebugStringA(text);

	HANDLE con = g_console;
	FILE* log = g_log;

	if (con) {
		SetConsoleTextAttribute(con, attr);
		DWORD written = 0;
		const DWORD n = static_cast<DWORD>(strlen(text));
		WriteConsoleA(con, text, n, &written, nullptr);
		SetConsoleTextAttribute(con,
			FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
	}

	if (log) {
		fputs(text, log);
		if (flushNow || g_alwaysFlush) {
			fflush(log);
			g_linesSinceFlush = 0;
		} else if (++g_linesSinceFlush >= 8) {
			fflush(log);
			g_linesSinceFlush = 0;
		}
	}

	if (!con) {
		fputs(text, stdout);
		if (flushNow)
			fflush(stdout);
	}
}

// Build header: "HH:MM:SS.mmm  LVL  "  or with tid "HH:MM:SS.mmm  LVL  tXXXX  "
int FormatHeader(char* line, size_t lineSz, Level level)
{
	char ts[32];
	FormatTimestamp(ts, sizeof(ts));
	const char* pf = PrefixFor(level);
	if (g_showTid) {
		const DWORD tid = GetCurrentThreadId() & 0xFFFFul;
		return _snprintf_s(line, lineSz, _TRUNCATE,
			"%s  %s  t%04lX  ", ts, pf, tid);
	}
	return _snprintf_s(line, lineSz, _TRUNCATE, "%s  %s  ", ts, pf);
}

void EmitLine(Level level, const char* body)
{
	if (!body)
		return;
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;

	char line[4608];
	int n = FormatHeader(line, sizeof(line) - 1, level);
	if (n < 0)
		n = 0;

	const size_t hdr = static_cast<size_t>(n);
	_snprintf_s(line + hdr, sizeof(line) - hdr - 1, _TRUNCATE, "%s", body);

	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
	line[len] = '\n';
	line[len + 1] = '\0';

	const bool flushNow = (level >= Level::Error) || g_alwaysFlush;
	{
		LockGuard lock;
		WriteRawUnlocked(ColorFor(level), line, flushNow);
	}
	BumpCount(level);
}

void EmitDetailLine(const char* body)
{
	if (!body)
		return;
	// Detail always emits if Info is allowed (same noise floor as Info)
	if (static_cast<int>(Level::Info) < static_cast<int>(g_minLevel))
		return;

	char line[4608];
	// Align under message column: time(12) + 2 + LVL(3) + 2 = 19 spaces
	// "               |  body"
	_snprintf_s(line, sizeof(line) - 1, _TRUNCATE, "               |  %s", body);

	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
	line[len] = '\n';
	line[len + 1] = '\0';

	{
		LockGuard lock;
		// Dim detail lines
		WriteRawUnlocked(FOREGROUND_INTENSITY, line, false);
	}
}

} // namespace

void Init(HANDLE console, FILE* logFile)
{
	EnsureCs();
	LockGuard lock;

	g_console = console;
	g_log = logFile;
	g_minLevel = Level::Ok;
	g_alwaysFlush = false;
	g_showTid = false;
	g_linesSinceFlush = 0;

	char env[8]{};
	env[0] = 0;
	if (GetEnvironmentVariableA("LEFRIZZEL_LOG_TRACE", env, sizeof(env)) == 0)
		GetEnvironmentVariableA("lefrizzel_LOG_TRACE", env, sizeof(env));
	if (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')
		g_minLevel = Level::Trace;

	env[0] = 0;
	if (GetEnvironmentVariableA("LEFRIZZEL_LOG_FLUSH", env, sizeof(env)) == 0)
		GetEnvironmentVariableA("lefrizzel_LOG_FLUSH", env, sizeof(env));
	if (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')
		g_alwaysFlush = true;

	env[0] = 0;
	if (GetEnvironmentVariableA("LEFRIZZEL_LOG_TID", env, sizeof(env)) == 0)
		GetEnvironmentVariableA("lefrizzel_LOG_TID", env, sizeof(env));
	if (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')
		g_showTid = true;

	if (g_log) {
		SYSTEMTIME st{};
		GetLocalTime(&st);
		fprintf(g_log,
			"\n"
			"============================================================\n"
			"  Lefrizzel AI  DEBUG SESSION\n"
			"  %04u-%02u-%02u  %02u:%02u:%02u    pid %lu\n"
			"============================================================\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			GetCurrentProcessId());
		fflush(g_log);
	}
}

void Shutdown()
{
	if (!g_csInit)
		return;
	EnterCriticalSection(&g_cs);
	g_console = nullptr;
	g_log = nullptr;
	LeaveCriticalSection(&g_cs);
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

void SetShowTid(bool on)
{
	g_showTid = on;
}

void VPrint(Level level, const char* fmt, va_list args)
{
	if (!fmt)
		return;
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;

	char body[4096];
	vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
	EmitLine(level, body);
}

void Print(Level level, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VPrint(level, fmt, args);
	va_end(args);
}

void VTag(const char* tag, Level level, const char* fmt, va_list args)
{
	if (!fmt)
		return;
	if (static_cast<int>(level) < static_cast<int>(g_minLevel))
		return;

	char msg[3800];
	vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);

	char body[4096];
	if (tag && tag[0])
		_snprintf_s(body, sizeof(body), _TRUNCATE, "[%s]  %s", tag, msg);
	else
		_snprintf_s(body, sizeof(body), _TRUNCATE, "%s", msg);
	EmitLine(level, body);
}

void Tag(const char* tag, Level level, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VTag(tag, level, fmt, args);
	va_end(args);
}

void VDetail(const char* fmt, va_list args)
{
	if (!fmt)
		return;
	char body[4096];
	vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
	EmitDetailLine(body);
}

void Detail(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	VDetail(fmt, args);
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
	{
		LockGuard lock;
		if (!RateAllowUnlocked(where, 3000))
			return;
	}
	Tag("seh", Level::Seh, "%s  —  %s (0x%08X)", where, SehName(code), code);
}

void OffsetMiss(const char* name, uintptr_t value)
{
	if (!name)
		return;
	{
		LockGuard lock;
		if (!RateAllowUnlocked(name, 3000))
			return;
	}
	if (value)
		Tag("offset", Level::Warn, "MISS  %s  (got 0x%llX)",
			name, (unsigned long long)value);
	else
		Tag("offset", Level::Warn, "MISS  %s", name);
}

void PatternMiss(const char* module, const char* pattern)
{
	if (!module || !pattern)
		return;
	char key[128];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "pat:%s:%.48s", module, pattern);
	{
		LockGuard lock;
		if (!RateAllowUnlocked(key, 3000))
			return;
	}
	// Show short pattern — full bytes are unreadable in a one-liner
	char patShort[72];
	const size_t plen = strlen(pattern);
	if (plen <= 64)
		_snprintf_s(patShort, sizeof(patShort), _TRUNCATE, "%s", pattern);
	else
		_snprintf_s(patShort, sizeof(patShort), _TRUNCATE, "%.60s...", pattern);
	Tag("pattern", Level::Warn, "MISS  %s", module);
	Detail("sig   %s", patShort);
}

void Once(const char* key, const char* fmt, ...)
{
	{
		LockGuard lock;
		if (!OnceAllowUnlocked(key ? key : "?"))
			return;
	}
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Info, fmt, args);
	va_end(args);
}

void Rate(const char* key, DWORD intervalMs, const char* fmt, ...)
{
	{
		LockGuard lock;
		if (!RateAllowUnlocked(key ? key : "?", intervalMs ? intervalMs : 1000))
			return;
	}
	va_list args;
	va_start(args, fmt);
	VPrint(Level::Info, fmt, args);
	va_end(args);
}

void RateAt(const char* key, DWORD intervalMs, Level level, const char* fmt, ...)
{
	{
		LockGuard lock;
		if (!RateAllowUnlocked(key ? key : "?", intervalMs ? intervalMs : 1000))
			return;
	}
	va_list args;
	va_start(args, fmt);
	VPrint(level, fmt, args);
	va_end(args);
}

void Hex(const char* label, const void* data, size_t len)
{
	if (!data || len == 0) {
		Tag("hex", Level::Info, "%s  <null/empty>", label ? label : "?");
		return;
	}

	constexpr size_t kMax = 256;
	const size_t n = (len > kMax) ? kMax : len;
	const auto* p = static_cast<const unsigned char*>(data);

	Tag("hex", Level::Info, "%s  %zu bytes @ %p%s",
		label ? label : "?", len, data, len > kMax ? "  (first 256)" : "");

	char row[128];
	for (size_t off = 0; off < n; off += 16) {
		int pos = _snprintf_s(row, sizeof(row), _TRUNCATE, "%04zX  ", off);
		if (pos < 0)
			pos = static_cast<int>(strlen(row));

		const size_t lineLen = (n - off > 16) ? 16 : (n - off);
		for (size_t i = 0; i < 16; ++i) {
			if (pos < 0 || static_cast<size_t>(pos) >= sizeof(row) - 1)
				break;
			const size_t remain = sizeof(row) - static_cast<size_t>(pos);
			int w;
			if (i < lineLen)
				w = _snprintf_s(row + pos, remain, _TRUNCATE, "%02X ", p[off + i]);
			else
				w = _snprintf_s(row + pos, remain, _TRUNCATE, "   ");
			if (w < 0)
				break;
			pos += w;
		}
		if (pos >= 0 && static_cast<size_t>(pos) < sizeof(row) - 1) {
			int w = _snprintf_s(row + pos, sizeof(row) - static_cast<size_t>(pos),
				_TRUNCATE, " ");
			if (w > 0)
				pos += w;
		}
		for (size_t i = 0; i < lineLen; ++i) {
			if (pos < 0 || static_cast<size_t>(pos) >= sizeof(row) - 1)
				break;
			const unsigned char c = p[off + i];
			const size_t remain = sizeof(row) - static_cast<size_t>(pos);
			int w = _snprintf_s(row + pos, remain, _TRUNCATE, "%c",
				(c >= 32 && c < 127) ? static_cast<char>(c) : '.');
			if (w < 0)
				break;
			pos += w;
		}

		EmitDetailLine(row);
	}
}

void Section(const char* title)
{
	const char* t = title ? title : "";
	char bar[120];
	_snprintf_s(bar, sizeof(bar), _TRUNCATE,
		"----------  %s  ----------", t);
	// Section banners as Info so they always show at default min-level Ok...
	// Actually use Ok so they stand out green and pass min Ok.
	Print(Level::Ok, "%s", bar);
}

void Stats()
{
	Tag("stats", Level::Info,
		"ok=%ld  info=%ld  warn=%ld  err=%ld  seh=%ld  trc=%ld  suppressed=%ld",
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
		Tag("timer", Level::Info, "%s  %llu ms", name, (unsigned long long)dt);
	else
		Tag("timer", Level::Trace, "%s  %llu ms", name, (unsigned long long)dt);
}

} // namespace Con

#endif // _DEBUG
