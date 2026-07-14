#include "vacdetect.h"
#include "../console/console.h"
#include "../crypto/xorstr.h"
#include <atomic>
#include <cwchar>
#include <cstring>

typedef struct _UNICODE_STRING_NT {
	USHORT Length;
	USHORT MaximumLength;
	PWSTR  Buffer;
} UNICODE_STRING_NT;

typedef struct _LDR_DLL_NOTIFICATION_DATA {
	ULONG              Flags;
	UNICODE_STRING_NT* FullDllName;
	UNICODE_STRING_NT* BaseDllName;
	PVOID              DllBase;
	ULONG              SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA;

#define LDR_DLL_NOTIFICATION_REASON_LOADED   1

typedef void (CALLBACK* PLDR_DLL_NOTIFICATION_FUNCTION)(
	ULONG NotificationReason,
	LDR_DLL_NOTIFICATION_DATA* NotificationData,
	PVOID Context);

typedef NTSTATUS(NTAPI* LdrRegisterDllNotification_t)(
	ULONG Flags,
	PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
	PVOID Context,
	PVOID* Cookie);

typedef NTSTATUS(NTAPI* LdrUnregisterDllNotification_t)(PVOID Cookie);

static PVOID g_Cookie = nullptr;
static std::atomic<int> g_DetectionCount{ 0 };
static VacDetect::VacLoadCallback g_UserCallback = nullptr;
static wchar_t g_LastPath[520]{};
static CRITICAL_SECTION g_PathCs;
static bool g_PathCsInit = false;

// Soft-pause: absolute tick deadline (GetTickCount64)
static std::atomic<unsigned long long> g_SoftPauseUntil{ 0 };

static void EnsurePathCs() {
	if (!g_PathCsInit) {
		InitializeCriticalSection(&g_PathCs);
		g_PathCsInit = true;
	}
}

static void StoreLastPath(const wchar_t* path) {
	if (!path)
		return;
	EnsurePathCs();
	EnterCriticalSection(&g_PathCs);
	wcsncpy_s(g_LastPath, path, _TRUNCATE);
	LeaveCriticalSection(&g_PathCs);
}

static bool IsAllowlistedBase(const wchar_t* baseName) {
	if (!baseName)
		return false;

	static const wchar_t* kOk[] = {
		L"steamclient.dll", L"steamclient64.dll", L"steamservice.dll",
		L"steam_api64.dll", L"steam_api.dll", L"steamui.dll",
		L"friendsui.dll", L"gameoverlayrenderer64.dll", L"gameoverlayrenderer.dll",
		L"video.dll", L"openvr_api.dll", L"vstdlib_s64.dll", L"tier0_s64.dll",
		L"filesystem_stdio.dll", L"crashhandler64.dll", L"sdl3.dll", L"sdl2.dll",
		L"chrome_elf.dll", L"libcef.dll",
	};

	for (const wchar_t* ok : kOk) {
		if (_wcsicmp(baseName, ok) == 0)
			return true;
	}
	if (_wcsnicmp(baseName, L"icudt", 5) == 0)
		return true;
	return false;
}

static bool LooksRandomHexName(const wchar_t* baseName) {
	if (!baseName)
		return false;

	wchar_t name[64]{};
	size_t n = 0;
	for (; baseName[n] && baseName[n] != L'.' && n + 1 < 64; ++n)
		name[n] = baseName[n];
	name[n] = 0;
	if (n < 8 || n > 16)
		return false;

	int hex = 0;
	for (size_t i = 0; i < n; ++i) {
		const wchar_t c = name[i];
		const bool isHex = (c >= L'0' && c <= L'9')
			|| (c >= L'a' && c <= L'f')
			|| (c >= L'A' && c <= L'F');
		if (isHex)
			++hex;
		else if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')))
			return false;
	}
	return hex >= static_cast<int>(n * 3 / 4);
}

static bool PathHasSteamRoot(const wchar_t* fullPath) {
	if (!fullPath)
		return false;
	return wcsstr(fullPath, L"\\Steam\\") != nullptr
		|| wcsstr(fullPath, L"\\steam\\") != nullptr
		|| wcsstr(fullPath, L"/Steam/") != nullptr
		|| wcsstr(fullPath, L"steamapps") != nullptr;
}

static bool IsVacLikeDll(const wchar_t* fullPath, const wchar_t* baseName) {
	if (!fullPath)
		return false;
	if (IsAllowlistedBase(baseName))
		return false;
	if (baseName && (wcsstr(baseName, L"api-ms-") || wcsstr(baseName, L"ext-ms-")))
		return false;

	const bool fromSteam = PathHasSteamRoot(fullPath);

	bool tmpExt = false;
	if (baseName) {
		const size_t len = wcslen(baseName);
		if (len > 4) {
			const wchar_t* ext = baseName + len - 4;
			if (_wcsicmp(ext, L".tmp") == 0)
				tmpExt = true;
		}
	}

	const bool randomName = LooksRandomHexName(baseName);

	if (fromSteam && (tmpExt || randomName))
		return true;

	if ((wcsstr(fullPath, L"\\Temp\\") || wcsstr(fullPath, L"\\tmp\\"))
		&& (tmpExt || randomName))
		return true;

	return false;
}

static void CALLBACK DllNotificationCallback(
	ULONG NotificationReason,
	LDR_DLL_NOTIFICATION_DATA* Data,
	PVOID /*Context*/)
{
	if (NotificationReason != LDR_DLL_NOTIFICATION_REASON_LOADED)
		return;
	if (!Data || !Data->FullDllName || !Data->FullDllName->Buffer)
		return;

	const wchar_t* fullPath = Data->FullDllName->Buffer;
	const wchar_t* baseName = Data->BaseDllName ? Data->BaseDllName->Buffer : nullptr;

	if (!IsVacLikeDll(fullPath, baseName))
		return;

	g_DetectionCount.fetch_add(1, std::memory_order_relaxed);
	StoreLastPath(fullPath);

	// Auto soft-pause on every hit (refreshes window)
	VacDetect::SoftPause(60000);

	Con::Warn("[VAC] soft-pause 60s — %ws (count=%d)",
		baseName ? baseName : fullPath,
		g_DetectionCount.load(std::memory_order_relaxed));

	if (g_UserCallback)
		g_UserCallback(fullPath);
}

namespace VacDetect {

bool Install(VacLoadCallback onVacLoad) {
	if (g_Cookie)
		return true;

	g_UserCallback = onVacLoad;
	EnsurePathCs();

	HMODULE ntdll = GetModuleHandleA(XS("ntdll.dll"));
	if (!ntdll)
		return false;

	auto pRegister = reinterpret_cast<LdrRegisterDllNotification_t>(
		GetProcAddress(ntdll, XS("LdrRegisterDllNotification")));
	if (!pRegister)
		return false;

	NTSTATUS status = pRegister(0, DllNotificationCallback, nullptr, &g_Cookie);
	if (status != 0) {
		g_Cookie = nullptr;
		return false;
	}

	Con::Ok("VAC monitor installed (soft-pause on detect)");
	return true;
}

void Uninstall() {
	if (!g_Cookie)
		return;

	HMODULE ntdll = GetModuleHandleA(XS("ntdll.dll"));
	if (ntdll) {
		auto pUnregister = reinterpret_cast<LdrUnregisterDllNotification_t>(
			GetProcAddress(ntdll, XS("LdrUnregisterDllNotification")));
		if (pUnregister)
			pUnregister(g_Cookie);
	}

	g_Cookie = nullptr;
	g_UserCallback = nullptr;
	g_SoftPauseUntil.store(0, std::memory_order_relaxed);
}

bool WasVacDetected() {
	return g_DetectionCount.load(std::memory_order_relaxed) > 0;
}

int GetDetectionCount() {
	return g_DetectionCount.load(std::memory_order_relaxed);
}

bool GetLastDetectedPath(wchar_t* out, size_t cch) {
	if (!out || cch == 0)
		return false;
	out[0] = 0;
	if (!g_PathCsInit)
		return false;
	EnterCriticalSection(&g_PathCs);
	const bool ok = g_LastPath[0] != 0;
	if (ok)
		wcsncpy_s(out, cch, g_LastPath, _TRUNCATE);
	LeaveCriticalSection(&g_PathCs);
	return ok;
}

void SoftPause(unsigned ms) {
	if (ms == 0)
		ms = 60000;
	const unsigned long long until = GetTickCount64() + static_cast<unsigned long long>(ms);
	// Extend if already paused further out
	unsigned long long cur = g_SoftPauseUntil.load(std::memory_order_relaxed);
	while (until > cur) {
		if (g_SoftPauseUntil.compare_exchange_weak(cur, until, std::memory_order_relaxed))
			break;
	}
}

void SoftResume() {
	g_SoftPauseUntil.store(0, std::memory_order_relaxed);
}

bool IsSoftPaused() {
	const unsigned long long until = g_SoftPauseUntil.load(std::memory_order_relaxed);
	if (until == 0)
		return false;
	return GetTickCount64() < until;
}

unsigned SoftPauseRemainingMs() {
	const unsigned long long until = g_SoftPauseUntil.load(std::memory_order_relaxed);
	if (until == 0)
		return 0;
	const unsigned long long now = GetTickCount64();
	if (now >= until)
		return 0;
	return static_cast<unsigned>(until - now);
}

} // namespace VacDetect
