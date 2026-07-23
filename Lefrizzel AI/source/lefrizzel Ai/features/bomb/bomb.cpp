#define NOMINMAX
#include "bomb.h"

#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../config/config.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Bomb {
namespace {

// pPlantedC4s: count dword + entity slot (IDA dword_18236E674 / qword_18236E678)
int* g_pPlantedCount = nullptr;
void** g_pPlantedSlot = nullptr; // &entity* (NOT pointer-to-array)

// GetBombsiteA/B — need C_CSPlayerResource* as this (IDA qword_1823749A0)
void** g_ppPlayerResource = nullptr;
uintptr_t g_fnSiteA = 0;
uintptr_t g_fnSiteB = 0;
uintptr_t g_fnStartDefuse = 0;

// Schema / IDA: C_CSPlayerResource->m_bombsiteCenterA/B @ 0x648 / 0x654
constexpr std::uint32_t kOffSiteA = 0x648;
constexpr std::uint32_t kOffSiteB = 0x654;

Vector_t g_siteA{};
Vector_t g_siteB{};
bool g_haveA = false;
bool g_haveB = false;
bool g_inited = false;

uintptr_t ScanClient(const char* tag, const char* pat) {
	uintptr_t a = M::patternScan("client", pat);
	if (!a)
		a = M::patternScan("client.dll", pat);
	if (a)
		Con::Ok("Bomb %s @ 0x%p", tag, (void*)a);
	else
		Con::OffsetMiss(tag);
	return a;
}

bool VecLooksLikeSite(const Vector_t& v) {
	if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
		return false;
	if (std::fabs(v.x) < 1.f && std::fabs(v.y) < 1.f && std::fabs(v.z) < 1.f)
		return false;
	// reject NaN-ish / absurd
	if (std::fabs(v.x) > 20000.f || std::fabs(v.y) > 20000.f || std::fabs(v.z) > 5000.f)
		return false;
	return true;
}

void* PlayerResourcePtr() {
	if (g_ppPlayerResource && Mem::IsReadable(g_ppPlayerResource, sizeof(void*))) {
		void* p = nullptr;
		__try { p = *g_ppPlayerResource; }
		__except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
		if (p && Mem::Valid(p, 0x660))
			return p;
	}
	return nullptr;
}

// Fallback: find C_CSPlayerResource in the entity list (survives round recreate)
void* FindPlayerResourceEntity() {
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return nullptr;
	int nMax = 0;
	__try {
		nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	const int lim = (nMax > 0 && nMax < 4096) ? nMax : 2048;
	for (int i = 1; i <= lim; ++i) {
		void* entRaw = nullptr;
		__try {
			entRaw = I::GameEntity->Instance->Get(i);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		auto* Entity = reinterpret_cast<C_BaseEntity*>(entRaw);
		if (!Mem::ValidEntity(Entity))
			continue;
		SchemaClassInfoData_t* cls = nullptr;
		__try { Entity->dump_class_info(&cls); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
			continue;
		const auto h = HASH(cls->szName);
		if (h != HASH("C_CSPlayerResource") && h != HASH("CCSPlayerResource"))
			continue;
		return Entity;
	}
	return nullptr;
}

bool ReadSiteFromResource(void* res, std::uint32_t off, Vector_t& out) {
	if (!res || !Mem::Valid(res, off + sizeof(Vector_t)))
		return false;
	Vector_t tmp{};
	__try {
		tmp = *reinterpret_cast<Vector_t*>(reinterpret_cast<std::uint8_t*>(res) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!VecLooksLikeSite(tmp))
		return false;
	out = tmp;
	return true;
}

bool SehGetCenter(uintptr_t fn, void* resource, Vector_t& out) {
	if (!fn || !resource)
		return false;
	__try {
		using Fn = Vector_t*(__fastcall*)(void*, Vector_t*);
		Vector_t tmp{};
		// IDA: this = C_CSPlayerResource*, rdx = out Vector*
		reinterpret_cast<Fn>(fn)(resource, &tmp);
		if (!VecLooksLikeSite(tmp))
			return false;
		out = tmp;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

std::uint32_t SiteOffA() {
	const std::uint32_t s = SchemaFinder::Get(
		hash_32_fnv1a_const("C_CSPlayerResource->m_bombsiteCenterA"));
	return s ? s : kOffSiteA;
}

std::uint32_t SiteOffB() {
	const std::uint32_t s = SchemaFinder::Get(
		hash_32_fnv1a_const("C_CSPlayerResource->m_bombsiteCenterB"));
	return s ? s : kOffSiteB;
}

} // namespace

bool Init() {
	if (g_inited)
		return g_pPlantedCount != nullptr || g_fnSiteA != 0 || g_ppPlayerResource != nullptr;
	g_inited = true;

	// cmp count; mov entity-slot — IDA @ 0x18081244d
	if (uintptr_t site = ScanClient("pPlantedC4s",
		"0F 85 ? ? ? ? 39 3D ? ? ? ? 7E ? 49 8B 0E 48 8B 1D ? ? ? ? 48 8B D3 4C 8B 81 48 01 00 00")) {
		g_pPlantedCount = reinterpret_cast<int*>(M::getAbsoluteAddress(site + 6, 2, 0));
		g_pPlantedSlot = reinterpret_cast<void**>(M::getAbsoluteAddress(site + 17, 3, 0));
	}

	// Distinct A/B via unique [rdi+0x648] / [rdi+0x654] loads (IDA 0x18088BDB0 / 0x18088BE10)
	g_fnSiteA = ScanClient("GetBombsiteACenter",
		"48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 0D ? ? ? ? 48 85 C9 74 19 E8 ? ? ? ? 84 C0 75 ? F2 0F 10 05 ? ? ? ? 8B 05 ? ? ? ? EB ? F2 0F 10 87 48 06 00 00");
	if (!g_fnSiteA)
		g_fnSiteA = ScanClient("GetBombsiteACenter",
			"48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 0D ? ? ? ? 48 85 C9 74 19 E8");

	g_fnSiteB = ScanClient("GetBombsiteBCenter",
		"48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 0D ? ? ? ? 48 85 C9 74 19 E8 ? ? ? ? 84 C0 75 ? F2 0F 10 05 ? ? ? ? 8B 05 ? ? ? ? EB ? F2 0F 10 87 54 06 00 00");
	// Adjacent in client (IDA: A @ …BDB0, B @ …BE10 = +0x60)
	if (!g_fnSiteB && g_fnSiteA)
		g_fnSiteB = g_fnSiteA + 0x60;

	// C_CSPlayerResource** — ctor stores this (IDA 0x18087EA70 → qword_1823749A0)
	if (uintptr_t pr = ScanClient("pCSPlayerResource",
		"40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 89 1D ? ? ? ? 48 83 C4 20 5B C3")) {
		// Prefer the match whose global yields readable site centers
		auto tryAbs = [](uintptr_t fn) -> void** {
			return reinterpret_cast<void**>(M::getAbsoluteAddress(fn + 0xE, 3, 0));
		};
		void** cand = tryAbs(pr);
		Vector_t probe{};
		void* obj = nullptr;
		if (cand && Mem::IsReadable(cand, 8)) {
			__try { obj = *cand; } __except (EXCEPTION_EXECUTE_HANDLER) { obj = nullptr; }
		}
		if (obj && ReadSiteFromResource(obj, SiteOffA(), probe)) {
			g_ppPlayerResource = cand;
		} else {
			// Second identical prologue writes a different global — scan next manually via +0x1000 window is hard;
			// entity-list fallback in RefreshSites covers it. Still keep first candidate.
			g_ppPlayerResource = cand;
		}
	}

	g_fnStartDefuse = ScanClient("StartDefuse",
		"48 89 4C 24 08 55 56 41 54 41 55 41 56 48 8D 6C");

	RefreshSites();
	return true;
}

void RefreshSites() {
	Vector_t a{}, b{};
	bool gotA = false, gotB = false;

	void* res = PlayerResourcePtr();
	if (!res)
		res = FindPlayerResourceEntity();

	if (res) {
		// Prefer direct schema/IDA field read — stable across rounds
		gotA = ReadSiteFromResource(res, SiteOffA(), a);
		gotB = ReadSiteFromResource(res, SiteOffB(), b);

		// Game helpers as backup (must pass PlayerResource, NOT the check global)
		if (!gotA && g_fnSiteA)
			gotA = SehGetCenter(g_fnSiteA, res, a);
		if (!gotB && g_fnSiteB)
			gotB = SehGetCenter(g_fnSiteB, res, b);
	}

	// Never wipe a good cache on a failed refresh (round teardown / null resource)
	if (gotA) {
		g_siteA = a;
		g_haveA = true;
	}
	if (gotB) {
		g_siteB = b;
		g_haveB = true;
	}

	if (gotA || gotB)
		Con::Ok("Bomb sites A=%d B=%d", g_haveA ? 1 : 0, g_haveB ? 1 : 0);
}

bool GetSiteCenter(int site, Vector_t& out) {
	if (!g_haveA && !g_haveB)
		RefreshSites();
	if (site == 0 && g_haveA) { out = g_siteA; return true; }
	if (site == 1 && g_haveB) { out = g_siteB; return true; }
	return false;
}

int ClassifySite(const Vector_t& pos) {
	if (!g_haveA && !g_haveB)
		RefreshSites();
	if (!g_haveA && !g_haveB)
		return -1;
	auto dist2 = [](const Vector_t& a, const Vector_t& b) {
		const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
		return dx * dx + dy * dy + dz * dz;
	};
	if (g_haveA && !g_haveB)
		return 0;
	if (!g_haveA && g_haveB)
		return 1;
	return (dist2(pos, g_siteA) <= dist2(pos, g_siteB)) ? 0 : 1;
}

int PlantedC4Count() {
	if (!g_pPlantedCount || !Mem::IsReadable(g_pPlantedCount, sizeof(int)))
		return 0;
	__try {
		const int n = *g_pPlantedCount;
		return (n >= 0 && n < 16) ? n : 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

void* PlantedC4Entity() {
	const int n = PlantedC4Count();
	if (n <= 0 || !g_pPlantedSlot || !Mem::IsReadable(g_pPlantedSlot, sizeof(void*)))
		return nullptr;
	__try {
		// IDA: qword_18236E678 IS the C_PlantedC4* (not a pointer-to-array)
		void* e = *g_pPlantedSlot;
		if (e && Mem::ValidEntity(e))
			return e;
		// Rare: slot holds T* array base (CUtlVector memory)
		if (e && Mem::IsReadable(e, sizeof(void*))) {
			void* e0 = *reinterpret_cast<void**>(e);
			if (e0 && Mem::ValidEntity(e0))
				return e0;
		}
		return nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool TryStartDefuse(void* plantedC4, void* localPawn) {
	if (!Config::auto_defuse || !g_fnStartDefuse || !plantedC4 || !localPawn)
		return false;
	// Free'd planted / dead pawn mid-round — never call game defuse
	if (!Mem::ValidEntity(plantedC4) || !Mem::ValidEntity(localPawn))
		return false;
	__try {
		using Fn = void(__fastcall*)(void*, void*);
		reinterpret_cast<Fn>(g_fnStartDefuse)(plantedC4, localPawn);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

} // namespace Bomb
