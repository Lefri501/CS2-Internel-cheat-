#include "sdk_prio_a.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"

#include "../gamemode/gamemode.h"
#include "../world/world.h"
#include "../world/weather.h"
#include "../hitmarker/hitmarker.h"
#include "../hitsound/hitsound.h"
#include "../nade_pred/nade_pred.h"
#include "../nade_lineup/nade_lineup.h"
#include "../bomb/bomb.h"
#include "../skinchanger/skinchanger.h"
#include "../aim/aim.h"
#include "../backtrack/backtrack.h"
#include "../prediction/prediction.h"
#include "../hitlog/hitlog.h"
#include "../tracers/tracers.h"
#include "../notify/notify.h"
#include "../sound_esp/sound_esp.h"
#include "../visuals/visuals.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

namespace SdkPrioA {
namespace {

// Dump patterns (patterns.hpp client::*)
constexpr const char* kPatOnAdd =
	"48 89 74 24 10 57 48 83 EC 20 41 B9 FF 7F 00 00 41 8B C0 41 23 C1 48 8B F2 41 83 F8 FF 48 8B F9 44 0F 45 C8 41 81 F9 00 40 00 00 73 0D FF 81 90";
constexpr const char* kPatOnRemove =
	"48 89 74 24 10 57 48 83 EC 20 41 B9 FF 7F 00 00 41 8B C0 41 23 C1 48 8B F2 41 83 F8 FF 48 8B F9 44 0F 45 C8 41 81 F9 00 40 00 00 73 08 FF 89 90";
constexpr const char* kPatLevelShutdown =
	"48 83 EC 28 48 8B 0D ? ? ? ? 48 8D 15";
// IDA 0x1807CDDB0 C_CSWeaponBase::GetEconWpnData — unique through second call + null check
constexpr const char* kPatGetEconWpnData =
	"40 53 48 83 EC 40 48 8B D9 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 85 C0 75";
constexpr const char* kPatGetEconWpnDataLoose =
	"40 53 48 83 EC 40 48 8B D9 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 85 C0";
constexpr const char* kPatGetStaticData =
	"40 56 48 83 EC 20 48 89 5C 24 30 48 8B F1 48 8B 1D";
constexpr const char* kPatGetBasePlayerWeaponVData =
	"48 81 EC 38 01 00 00 48 85 C9 75 0A 33 C0 48 81 C4 38 01 00 00 C3 48 89";
constexpr const char* kPatGloveApply =
	"40 55 56 57 48 8D AC 24 30 FD FF FF 48 81 EC D0";
constexpr const char* kPatPGameRules =
	"48 8B 1D ? ? ? ? 48 8D 54 24 ? 0F 28 D0 48 8D 4C 24";
constexpr const char* kPatIsOverwatch =
	"48 83 EC 28 E8 ? ? ? ? 0F B6 40 72 48 83 C4";
constexpr const char* kPatIsDemoOrHltv =
	"48 83 EC 28 48 8B 0D ? ? ? ? 48 8B 01 FF 90 50 01 00 00 84 C0 75 0D";
constexpr const char* kPatComputeHitboxBox =
	"48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 20 80 FF FF";
constexpr const char* kPatGetHitGroup =
	"40 53 48 83 EC 20 48 83 79 10 00 48 8B D9 74 16";
constexpr const char* kPatInitMoveFilter =
	"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 41 39 33 FF C7 41 34";
constexpr const char* kPatTracePlayerBBox =
	"48 89 5C 24 18 55 57 41 54 41 55 41 56 48 83 EC";
constexpr const char* kPatGetAbsOrigin =
	"40 53 48 83 EC 20 48 8B 99 30 03 00 00 90 80 BB 10 01 00 00 00 74 08 48 8B CB E8";

Entry g_entries[] = {
	{ "TraceToExit", Status::NotStarted, nullptr, "" },
	{ "OnAddEntity", Status::NotStarted, nullptr, "" },
	{ "OnRemoveEntity", Status::NotStarted, nullptr, "" },
	{ "LevelShutdown", Status::NotStarted, nullptr, "" },
	{ "GloveApply_PerTick", Status::NotStarted, nullptr, "" },
	{ "C_EconItemView_GetStaticData", Status::NotStarted, nullptr, "" },
	{ "C_EconItemView_GetBasePlayerWeaponVData", Status::NotStarted, nullptr, "" },
	{ "C_CSWeaponBase_GetEconWpnData", Status::NotStarted, nullptr, "" },
	{ "GetMapName", Status::NotStarted, nullptr, "" },
	{ "GetMapBspName", Status::NotStarted, nullptr, "" },
	{ "pGameRules", Status::NotStarted, nullptr, "" },
	{ "IsOverwatch", Status::NotStarted, nullptr, "" },
	{ "IsDemoOrHltv", Status::NotStarted, nullptr, "" },
	{ "C_BaseEntity_ComputeHitboxSurroundingBox", Status::NotStarted, nullptr, "" },
	{ "GetHitGroup", Status::NotStarted, nullptr, "" },
	{ "InitPlayerMovementTraceFilter", Status::NotStarted, nullptr, "" },
	{ "TracePlayerBBox", Status::NotStarted, nullptr, "" },
	{ "GetAbsOrigin", Status::NotStarted, nullptr, "" },
};

constexpr int kEntryCount = static_cast<int>(sizeof(g_entries) / sizeof(g_entries[0]));

Entry* FindEntry(const char* name) {
	for (int i = 0; i < kEntryCount; ++i) {
		if (std::strcmp(g_entries[i].name, name) == 0)
			return &g_entries[i];
	}
	return nullptr;
}

void SetEntry(const char* name, Status st, void* addr, const char* note) {
	if (Entry* e = FindEntry(name)) {
		e->status = st;
		e->addr = addr;
		e->note = note ? note : "";
	}
}

std::uint8_t* ScanClient(const char* pat) {
	auto* p = M::FindPattern("client.dll", pat);
	if (!p)
		p = M::FindPattern("client", pat);
	return p;
}

std::uint8_t* ScanClientEither(const char* strict, const char* loose) {
	auto* p = ScanClient(strict);
	if (!p && loose)
		p = ScanClient(loose);
	return p;
}

using FnGetEconWpn = void*(__fastcall*)(void* weapon);
using FnGetAbs = Vector_t*(__fastcall*)(void* entity);
using FnComputeBox = char(__fastcall*)(void* entity, Vector_t* mins, Vector_t* maxs);
using FnHitGroup = int(__fastcall*)(void* hitbox);
using FnEconView = void*(__fastcall*)(void* view);
using FnBool = bool(__fastcall*)();

FnGetEconWpn g_getEconWpn = nullptr;
FnGetAbs g_getAbsOrigin = nullptr;
FnComputeBox g_computeBox = nullptr;
FnHitGroup g_getHitGroup = nullptr;
FnEconView g_getStaticData = nullptr;
FnEconView g_getBaseVData = nullptr;
FnBool g_isOverwatch = nullptr;
FnBool g_isDemoOrHltv = nullptr;
GloveApplyFn g_gloveApply = nullptr;
void** g_ppGameRules = nullptr;
void* g_initMoveFilter = nullptr;
void* g_tracePlayerBBox = nullptr;
void* g_onAddAddr = nullptr;
void* g_onRemoveAddr = nullptr;
void* g_levelShutdownAddr = nullptr;

std::uint32_t g_entityGen = 1;
std::uint32_t g_mapGen = 1;
bool g_ready = false;

void LevelShutdownCleanup() {
	// Order: combat state first (drop free'd pawn*), then visual caches, then skin
	__try { Aimbot_Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Backtrack::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Pred::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { NadePred::Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { World::InvalidateEnvCache(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { World::Weather::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	// Shutdown clears marks; re-Install so g_ready stays true next map
	__try {
		Hitmarker::Shutdown();
		Hitmarker::Install();
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Hitsound::Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { HitLog::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Tracers::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Notify::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { SoundEsp::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Esp::InvalidateCaches(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { SkinChanger::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

const char* StatusName(Status s) {
	switch (s) {
	case Status::NotStarted: return "NOT_STARTED";
	case Status::Resolved: return "RESOLVED";
	case Status::Hooked: return "HOOKED";
	case Status::AlreadyHad: return "ALREADY_HAD";
	case Status::Blocked: return "BLOCKED";
	case Status::Failed: return "FAILED";
	}
	return "?";
}

const Entry* Entries(int& countOut) {
	countOut = kEntryCount;
	return g_entries;
}

std::uint32_t EntityGen() { return g_entityGen; }
std::uint32_t MapGen() { return g_mapGen; }

bool IsOverwatch() {
	if (!g_isOverwatch)
		return false;
	bool r = false;
	__try { r = g_isOverwatch(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return r;
}

bool IsDemoOrHltv() {
	if (!g_isDemoOrHltv)
		return false;
	bool r = false;
	__try { r = g_isDemoOrHltv(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return r;
}

bool ShouldSoftDisableCombat() {
	return IsOverwatch() || IsDemoOrHltv();
}

void* GameRules() {
	if (!g_ppGameRules || !Mem::IsReadable(g_ppGameRules, sizeof(void*)))
		return nullptr;
	void* p = nullptr;
	__try { p = *g_ppGameRules; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return p;
}

void* GetEconWpnData(C_CSWeaponBase* weapon) {
	if (!weapon || !g_getEconWpn || !Mem::ValidEntity(weapon))
		return nullptr;
	void* r = nullptr;
	__try { r = g_getEconWpn(weapon); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return r;
}

void* EconItemView_GetStaticData(void* econItemView) {
	if (!econItemView || !g_getStaticData)
		return nullptr;
	void* r = nullptr;
	__try { r = g_getStaticData(econItemView); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return r;
}

void* EconItemView_GetBasePlayerWeaponVData(void* econItemView) {
	if (!econItemView || !g_getBaseVData)
		return nullptr;
	void* r = nullptr;
	__try { r = g_getBaseVData(econItemView); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return r;
}

bool GetAbsOrigin(void* entity, Vector_t& out) {
	if (!entity || !g_getAbsOrigin)
		return false;
	Vector_t* p = nullptr;
	__try { p = g_getAbsOrigin(entity); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!p || !Mem::IsReadable(p, sizeof(Vector_t)))
		return false;
	__try { out = *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool ComputeHitboxSurroundingBox(void* entity, Vector_t& minsOut, Vector_t& maxsOut) {
	if (!entity || !g_computeBox)
		return false;
	Vector_t mins{}, maxs{};
	char ok = 0;
	__try { ok = g_computeBox(entity, &mins, &maxs); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!ok)
		return false;
	minsOut = mins;
	maxsOut = maxs;
	return true;
}

int GetHitGroupFromHitbox(void* hitboxObj) {
	if (!hitboxObj || !g_getHitGroup)
		return -1;
	int g = -1;
	__try { g = g_getHitGroup(hitboxObj); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	return g;
}

void* InitPlayerMovementTraceFilterFn() { return g_initMoveFilter; }
void* TracePlayerBBoxFn() { return g_tracePlayerBBox; }
GloveApplyFn GloveApplyPerTick() { return g_gloveApply; }

void OnLevelShutdown() {
	++g_mapGen;
	++g_entityGen;
	LevelShutdownCleanup();
}

void OnEntityAdded(void*, void*, int) {
	++g_entityGen;
}

void OnEntityRemoved(void*, void*, int) {
	++g_entityGen;
}

void* OnAddAddr() { return g_onAddAddr; }
void* OnRemoveAddr() { return g_onRemoveAddr; }
void* LevelShutdownAddr() { return g_levelShutdownAddr; }

void MarkHooked(const char* name, const char* note) {
	if (Entry* e = FindEntry(name)) {
		e->status = Status::Hooked;
		if (note)
			e->note = note;
	}
}

bool Init() {
	Con::Section("SdkPrioA");

	// Already implemented in GameMode (EnsureMap)
	SetEntry("GetMapName", Status::AlreadyHad, nullptr,
		"GameMode::TryClientMapName / engine2 LevelName");
	SetEntry("GetMapBspName", Status::AlreadyHad, nullptr,
		"GameMode::TryClientMapBspName");

	// Dump TraceToExit prologue == CreateTrace — not an exit helper.
	// IDA HandleBulletPen 0x180860570: exit via segment enter/exit idx + GetTraceInfo;
	// no separate TraceToExit call. Soft path uses TraceToExitSimple (0.5u step).
	SetEntry("TraceToExit", Status::Blocked, nullptr,
		"==CreateTrace; exit is HandleBulletPen segment idx; TraceToExitSimple");

	auto resolve = [](const char* name, const char* pat, const char* loose = nullptr) -> void* {
		void* p = ScanClientEither(pat, loose);
		if (p) {
			SetEntry(name, Status::Resolved, p, "ok");
			Con::Ok("PrioA %s @ 0x%p", name, p);
		} else {
			SetEntry(name, Status::Failed, nullptr, "scan miss");
			Con::OffsetMiss(name);
		}
		return p;
	};

	g_getEconWpn = reinterpret_cast<FnGetEconWpn>(
		resolve("C_CSWeaponBase_GetEconWpnData", kPatGetEconWpnData, kPatGetEconWpnDataLoose));
	g_getStaticData = reinterpret_cast<FnEconView>(
		resolve("C_EconItemView_GetStaticData", kPatGetStaticData));
	g_getBaseVData = reinterpret_cast<FnEconView>(
		resolve("C_EconItemView_GetBasePlayerWeaponVData", kPatGetBasePlayerWeaponVData));
	g_gloveApply = reinterpret_cast<GloveApplyFn>(
		resolve("GloveApply_PerTick", kPatGloveApply));
	g_isOverwatch = reinterpret_cast<FnBool>(
		resolve("IsOverwatch", kPatIsOverwatch));
	g_isDemoOrHltv = reinterpret_cast<FnBool>(
		resolve("IsDemoOrHltv", kPatIsDemoOrHltv));
	g_computeBox = reinterpret_cast<FnComputeBox>(
		resolve("C_BaseEntity_ComputeHitboxSurroundingBox", kPatComputeHitboxBox));
	g_getHitGroup = reinterpret_cast<FnHitGroup>(
		resolve("GetHitGroup", kPatGetHitGroup));
	g_initMoveFilter = resolve("InitPlayerMovementTraceFilter", kPatInitMoveFilter);
	g_tracePlayerBBox = resolve("TracePlayerBBox", kPatTracePlayerBBox);
	g_getAbsOrigin = reinterpret_cast<FnGetAbs>(
		resolve("GetAbsOrigin", kPatGetAbsOrigin));

	if (auto* site = ScanClient(kPatPGameRules)) {
		const uintptr_t abs = M::getAbsoluteAddress(reinterpret_cast<uintptr_t>(site), 3, 0);
		if (abs) {
			g_ppGameRules = reinterpret_cast<void**>(abs);
			SetEntry("pGameRules", Status::Resolved, g_ppGameRules, "global ptr");
			Con::Ok("PrioA pGameRules @ 0x%p", (void*)abs);
		} else {
			SetEntry("pGameRules", Status::Failed, nullptr, "rip resolve fail");
		}
	} else {
		SetEntry("pGameRules", Status::Failed, nullptr, "scan miss");
		Con::OffsetMiss("pGameRules");
	}

	g_onAddAddr = resolve("OnAddEntity", kPatOnAdd);
	if (g_onAddAddr)
		SetEntry("OnAddEntity", Status::Resolved, g_onAddAddr, "awaiting hook");
	g_onRemoveAddr = resolve("OnRemoveEntity", kPatOnRemove);
	if (g_onRemoveAddr)
		SetEntry("OnRemoveEntity", Status::Resolved, g_onRemoveAddr, "awaiting hook");
	g_levelShutdownAddr = resolve("LevelShutdown", kPatLevelShutdown);
	if (g_levelShutdownAddr)
		SetEntry("LevelShutdown", Status::Resolved, g_levelShutdownAddr, "awaiting hook");

	g_ready = true;

	int n = 0;
	const Entry* e = Entries(n);
	int ok = 0, fail = 0, had = 0, blocked = 0;
	for (int i = 0; i < n; ++i) {
		switch (e[i].status) {
		case Status::Resolved:
		case Status::Hooked: ++ok; break;
		case Status::AlreadyHad: ++had; break;
		case Status::Blocked: ++blocked; break;
		case Status::Failed: ++fail; break;
		default: break;
		}
	}
	Con::Info("PrioA summary: resolved=%d already=%d blocked=%d failed=%d (hooks pending)",
		ok, had, blocked, fail);
	return true;
}

bool Ready() { return g_ready; }

// InstallHooks is a no-op here — Hooks::init owns SafetyHook install
// and calls MarkHooked after success. Kept for API completeness.
void InstallHooks() {}

} // namespace SdkPrioA
