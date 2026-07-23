#include "debug.h"

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#ifdef _DEBUG

namespace {
	FILE* g_adLog = nullptr;
	CRITICAL_SECTION g_adCs{};
	volatile LONG g_adCsInit = 0;

	void EnsureCs()
	{
		if (InterlockedCompareExchange(&g_adCsInit, 1, 0) == 0) {
			InitializeCriticalSection(&g_adCs);
			InterlockedExchange(&g_adCsInit, 2);
		} else {
			while (InterlockedCompareExchange(&g_adCsInit, 2, 2) != 2)
				Sleep(0);
		}
	}

	// Escape for JSON string values (hyp/loc/msg). Truncates safely.
	void JsonEscape(char* out, size_t outSz, const char* in)
	{
		if (!out || outSz == 0)
			return;
		if (!in) {
			out[0] = '\0';
			return;
		}
		size_t o = 0;
		for (size_t i = 0; in[i] && o + 2 < outSz; ++i) {
			const unsigned char c = static_cast<unsigned char>(in[i]);
			if (c == '"' || c == '\\') {
				if (o + 3 >= outSz)
					break;
				out[o++] = '\\';
				out[o++] = static_cast<char>(c);
			} else if (c < 0x20) {
				// skip control chars
				continue;
			} else {
				out[o++] = static_cast<char>(c);
			}
		}
		out[o] = '\0';
	}

	void EnsureAdLog()
	{
		EnsureCs();
		EnterCriticalSection(&g_adCs);
		if (!g_adLog) {
			char dir[MAX_PATH]{};
			char path[MAX_PATH]{};
			if (GetEnvironmentVariableA("USERPROFILE", dir, MAX_PATH) > 0) {
				strcat_s(dir, "\\Documents\\Lefrizzel AI");
				CreateDirectoryA(dir, nullptr);
				_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\ad-debug.log", dir);
				fopen_s(&g_adLog, path, "a");
			}
		}
		LeaveCriticalSection(&g_adCs);
	}
}

void initDebug()
{
	// Console is owned by init_console() in main.cpp — only open AD log here
	EnsureAdLog();
	ADLog("BOOT", "debug.cpp:initDebug", "ad_log_open", "{\"ok\":1}");
}

void ADLog(const char* hyp, const char* loc, const char* msg, const char* dataJson)
{
	EnsureAdLog();

	const long long ts = static_cast<long long>(GetTickCount64());
	char hypE[64], locE[128], msgE[128];
	JsonEscape(hypE, sizeof(hypE), hyp ? hyp : "?");
	JsonEscape(locE, sizeof(locE), loc ? loc : "?");
	JsonEscape(msgE, sizeof(msgE), msg ? msg : "?");

	const char* data = (dataJson && dataJson[0]) ? dataJson : "{}";

	char line[1280];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"{\"sessionId\":\"ad-dbg\",\"hypothesisId\":\"%s\",\"location\":\"%s\","
		"\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
		hypE, locE, msgE, data, ts);

	EnterCriticalSection(&g_adCs);
	if (g_adLog) {
		fputs(line, g_adLog);
		fflush(g_adLog);
	}
	fputs(line, stdout);
	fflush(stdout);
	LeaveCriticalSection(&g_adCs);
}

void ADLogf(const char* hyp, const char* loc, const char* msg, const char* fmt, ...)
{
	char data[768];
	if (!fmt) {
		data[0] = '{';
		data[1] = '}';
		data[2] = '\0';
	} else {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf_s(data, sizeof(data), _TRUNCATE, fmt, ap);
		va_end(ap);
	}
	ADLog(hyp, loc, msg, data);
}

#endif
