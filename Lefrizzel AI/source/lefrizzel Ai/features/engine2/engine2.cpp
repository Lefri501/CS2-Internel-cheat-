#define NOMINMAX
#include "engine2.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"

#include <Windows.h>
#include <cstring>

namespace Engine2 {
namespace {

void** g_ppNetworkGameClient = nullptr;
uintptr_t g_fnGetLevelName = 0;
uintptr_t g_fnGetLevelNameShort = 0;
uintptr_t g_fnIsInGame = 0;
uintptr_t g_fnIsConnected = 0;
uintptr_t g_fnRunPrediction = 0;
uintptr_t g_fnIsHearingClient = 0;
int* g_pBuildNumber = nullptr;
int* g_pWindowWidth = nullptr;
int* g_pWindowHeight = nullptr;
void** g_ppPvsOrRip = nullptr; // for pPVSManager lea
bool g_pvsIsLea = true;
bool g_inited = false;

uintptr_t Scan(const char* tag, const char* pat) {
	uintptr_t a = M::patternScan("engine2", pat);
	if (a)
		Con::Ok("Engine2 %s @ 0x%p", tag, (void*)a);
	else
		Con::OffsetMiss(tag);
	return a;
}

void* SehReadPtr(void** pp) {
	if (!pp)
		return nullptr;
	__try {
		return *pp;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

const char* SehCallNoArg(uintptr_t fn) {
	if (!fn)
		return nullptr;
	const char* r = nullptr;
	__try {
		using Fn = const char*(__fastcall*)();
		r = reinterpret_cast<Fn>(fn)();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!r || !Mem::IsReadable(r, 1) || !r[0])
		return nullptr;
	return r;
}

bool SehCallIsInGame(uintptr_t fn) {
	if (!fn)
		return false;
	__try {
		using Fn = bool(__fastcall*)();
		return reinterpret_cast<Fn>(fn)();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehCallIsConnected(uintptr_t fn) {
	if (!fn)
		return false;
	__try {
		using Fn = bool(__fastcall*)();
		return reinterpret_cast<Fn>(fn)();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehRunPrediction(void* ngc, uintptr_t fn, unsigned reason) {
	if (!ngc || !fn)
		return false;
	__try {
		using Fn = void(__fastcall*)(void*, unsigned);
		reinterpret_cast<Fn>(fn)(ngc, reason);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehIsHearing(uintptr_t fn, void* thisptr, int slot) {
	if (!fn || !thisptr)
		return false;
	__try {
		using Fn = bool(__fastcall*)(void*, int);
		return reinterpret_cast<Fn>(fn)(thisptr, slot);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

int SehReadInt(int* p) {
	if (!p || !Mem::IsReadable(p, sizeof(int)))
		return 0;
	__try {
		return *p;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

int SehSignon(void* ngc) {
	if (!ngc || !Mem::IsReadable(ngc, 0x240))
		return 0;
	__try {
		return *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(ngc) + 0x230);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

const char* SehMapAt(void* ngc, std::uintptr_t off) {
	if (!ngc || !Mem::IsReadable(ngc, off + 8))
		return nullptr;
	__try {
		const char* p = *reinterpret_cast<const char**>(reinterpret_cast<std::uint8_t*>(ngc) + off);
		if (!p || !Mem::IsReadable(p, 2) || !p[0])
			return nullptr;
		return p;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

} // namespace

bool Init() {
	if (g_inited)
		return NetworkGameClient() != nullptr || g_fnGetLevelNameShort != 0;
	g_inited = true;

	// pNetworkGameClient — mov [rip+disp], rdi
	if (uintptr_t a = Scan("pNetworkGameClient", "48 89 3D ? ? ? ? FF 87"))
		g_ppNetworkGameClient = reinterpret_cast<void**>(M::getAbsoluteAddress(a, 3));

	// Free fns (no this) — IDA verified
	g_fnGetLevelNameShort = Scan("GetLevelNameShort",
		"48 83 EC 28 E8 ? ? ? ? 84 C0 74 0C 48 8D 05 ? ? ? ? 48 83 C4 28 C3 48 8B 0D ? ? ? ? 48 85 C9 74 23 83 B9 30 02 00 00 02 7C 1A 48 8B 89 18 02 00 00");
	g_fnGetLevelName = Scan("GetLevelName",
		"48 83 EC 28 E8 ? ? ? ? 84 C0 74 0C 48 8D 05 ? ? ? ? 48 83 C4 28 C3 48 8B 0D ? ? ? ? 48 85 C9 74 23 83 B9 30 02 00 00 02 7C 1A 48 8B 89 10 02 00 00");

	g_fnIsInGame = Scan("IsInGame", "48 8B 05 ? ? ? ? 48 85 C0 74 15 80 B8 1F 14");
	g_fnIsConnected = Scan("Engine_IsConnected", "48 8B 05 ? ? ? ? 48 85 C0 74 0B 83 B8 30 02 00 00 02 0F");

	g_fnRunPrediction = Scan("RunPrediction",
		"40 55 41 56 48 83 EC 68 80 B9 00 01 00 00 00 8B");

	g_fnIsHearingClient = Scan("IsHearingClient",
		"40 53 48 83 EC 20 48 8B D9 3B 51 48 75 0D 0F B6");

	if (uintptr_t a = Scan("pBuildNumber", "89 05 ? ? ? ? 48 8D 0D ? ? ? ? FF 15 ? ? ? ? 48 8B 0D"))
		g_pBuildNumber = reinterpret_cast<int*>(M::getAbsoluteAddress(a, 2));

	if (uintptr_t a = Scan("pWindowWidth", "8B 05 ? ? ? ? 89 07"))
		g_pWindowWidth = reinterpret_cast<int*>(M::getAbsoluteAddress(a, 2));
	if (uintptr_t a = Scan("pWindowHeight", "8B 05 ? ? ? ? 89 03"))
		g_pWindowHeight = reinterpret_cast<int*>(M::getAbsoluteAddress(a, 2));

	// pPVSManager — lea rcx, [rip]; xor edx; call [rax+30]
	if (uintptr_t a = Scan("pPVSManager", "48 8D 0D ? ? ? ? 33 D2 FF 50")) {
		g_ppPvsOrRip = reinterpret_cast<void**>(M::getAbsoluteAddress(a, 3));
		g_pvsIsLea = true;
	}

	const int bn = BuildNumber();
	if (bn)
		Con::Ok("Engine2 build %d", bn);

	return true;
}

void* NetworkGameClient() {
	if (!g_inited)
		Init();
	return SehReadPtr(g_ppNetworkGameClient);
}

int SignonState() {
	return SehSignon(NetworkGameClient());
}

bool IsConnected() {
	if (!g_inited)
		Init();
	void* ngc = NetworkGameClient();
	if (!ngc)
		return false;
	// NGC+0x230 signon >= 2 (IDA GetLevelName / Engine_IsConnected)
	if (SignonState() >= 2)
		return true;
	if (g_fnIsConnected)
		return SehCallIsConnected(g_fnIsConnected);
	return false;
}

bool IsInGame() {
	if (!g_inited)
		Init();
	void* ngc = NetworkGameClient();
	if (!ngc)
		return false;
	if (SignonState() >= 2)
		return true;
	if (g_fnIsInGame)
		return SehCallIsInGame(g_fnIsInGame);
	return false;
}

bool Ready() {
	if (!IsConnected())
		return false;
	const char* m = LevelNameShort();
	return m && m[0];
}

const char* LevelNameShort() {
	if (!g_inited)
		Init();
	if (const char* s = SehCallNoArg(g_fnGetLevelNameShort))
		return s;
	return SehMapAt(NetworkGameClient(), 0x218);
}

const char* LevelName() {
	if (!g_inited)
		Init();
	if (const char* s = SehCallNoArg(g_fnGetLevelName))
		return s;
	return SehMapAt(NetworkGameClient(), 0x210);
}

int BuildNumber() {
	if (!g_inited)
		Init();
	return SehReadInt(g_pBuildNumber);
}

bool GetWindowSize(int& outW, int& outH) {
	if (!g_inited)
		Init();
	outW = SehReadInt(g_pWindowWidth);
	outH = SehReadInt(g_pWindowHeight);
	return outW > 0 && outH > 0;
}

void* PvsManager() {
	if (!g_inited)
		Init();
	if (!g_ppPvsOrRip)
		return nullptr;
	// lea rcx,[obj] → address of object (not pointer-to-pointer)
	if (g_pvsIsLea)
		return reinterpret_cast<void*>(g_ppPvsOrRip);
	return SehReadPtr(g_ppPvsOrRip);
}

bool RunPrediction(unsigned reason) {
	if (!g_inited)
		Init();
	void* ngc = NetworkGameClient();
	if (!ngc || SignonState() < 2)
		return false;
	return SehRunPrediction(ngc, g_fnRunPrediction, reason);
}

bool IsHearingClient(int slot) {
	if (!g_inited)
		Init();
	if (!g_fnIsHearingClient || slot < 0)
		return false;
	void* ngc = NetworkGameClient();
	if (!ngc)
		return false;
	return SehIsHearing(g_fnIsHearingClient, ngc, slot);
}

} // namespace Engine2
