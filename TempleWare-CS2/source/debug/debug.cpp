#include "debug.h"

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#ifdef _DEBUG

namespace {
	FILE* g_adLog = nullptr;
	CRITICAL_SECTION g_adCs;
	bool g_adCsInit = false;

	void EnsureAdLog() {
		if (g_adLog)
			return;
		if (!g_adCsInit) {
			InitializeCriticalSection(&g_adCs);
			g_adCsInit = true;
		}
		char dir[MAX_PATH]{};
		char path[MAX_PATH]{};
		if (!GetEnvironmentVariableA("USERPROFILE", dir, MAX_PATH))
			return;
		strcat_s(dir, "\\Documents\\TempleWare");
		CreateDirectoryA(dir, nullptr);
		snprintf(path, sizeof(path), "%s\\ad-debug.log", dir);
		fopen_s(&g_adLog, path, "a");
	}
}

void initDebug() {
	if (AllocConsole()) {
		FILE* pFile = nullptr;
		freopen_s(&pFile, "CONOUT$", "w", stdout);
		freopen_s(&pFile, "CONOUT$", "w", stderr);
		freopen_s(&pFile, "CONIN$", "r", stdin);
		printf("Console allocated.\n\n");
	}
	EnsureAdLog();
	ADLog("BOOT", "debug.cpp:initDebug", "ad_log_open", "{\"ok\":1}");
}

void ADLog(const char* hyp, const char* loc, const char* msg, const char* dataJson) {
	EnsureAdLog();
	if (!g_adCsInit)
		return;

	const long long ts = static_cast<long long>(GetTickCount64());
	char line[1024];
	const char* data = (dataJson && dataJson[0]) ? dataJson : "{}";
	_snprintf_s(line, _TRUNCATE,
		"{\"sessionId\":\"ad-dbg\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
		hyp ? hyp : "?", loc ? loc : "?", msg ? msg : "?", data, ts);

	EnterCriticalSection(&g_adCs);
	if (g_adLog) {
		fputs(line, g_adLog);
		fflush(g_adLog);
	}
	// Also mirror to console for live watch
	fputs(line, stdout);
	fflush(stdout);
	LeaveCriticalSection(&g_adCs);
}

void ADLogf(const char* hyp, const char* loc, const char* msg, const char* fmt, ...) {
	char data[768];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(data, sizeof(data), fmt, ap);
	va_end(ap);
	ADLog(hyp, loc, msg, data);
}

#endif
