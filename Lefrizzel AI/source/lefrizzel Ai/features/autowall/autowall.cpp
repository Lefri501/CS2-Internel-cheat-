#include "autowall.h"

#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../config/config.h"
#include "../trace/trace.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace AutoWall {
namespace {

// IDA client.dll FireBullet path (imagebase 0x180000000) — 2026-07-17:
//   InitTraceData   0x18083E6F0
//   CreateTrace     0x180842880  → calls dump "AutowallTraceData" 0x1809CD0E0 (phys TraceShape wrap)
//   DamageToPoint   0x180844E70  (dump: TestSurfaces) → HandleBulletPen per surface
//   HandleBulletPen 0x180860570  — stop if surf_pen<0.1 or dmg after loss < 1
//   GetTraceInfo    0x180844F90
//   FreeTraceData   0x18083F430
// DUMP NAME MISMATCH (do NOT use as pen entry):
//   AutowallInit     0x180927B90 — entity teardown, NOT pen
//   AutowallTracePos 0x180AC34C0 — skybox/VR slot, NOT pen
//   AutowallTraceData 0x1809CD0E0 — already inside CreateTrace; not InitTraceData
constexpr const char* kPatInitTraceData =
	"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 79 08 33 F6 C7 47 08 80 00 00 00";
constexpr const char* kPatInitTraceDataLoose =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8D 79 ? 33 F6 C7 47";
constexpr const char* kPatCreateTrace =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 F2 0F 10 02";
constexpr const char* kPatCreateTraceLoose =
	"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? F2 0F 10 02";
// dump TestSurfaces
constexpr const char* kPatDamageToPoint =
	"40 53 57 41 56 48 83 EC 50 8B 84 24 90 00 00 00";
constexpr const char* kPatDamageToPointLoose =
	"40 53 57 41 56 48 83 EC ? 8B 84 24";
// dump TraceHandleBulletPen
constexpr const char* kPatHandleBulletPen =
	"48 8B C4 44 89 48 20 48 89 50 10 48 89 48 08 55";
constexpr const char* kPatGetTraceInfo =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 80 00 00 00 48 8B E9 0F 29 74 24";
constexpr const char* kPatGetTraceInfoLoose =
	"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 48 8B E9 0F 29 74 24";
constexpr const char* kPatInitFilter =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F B6 41 ? 33 FF 24";
// Unique: lea rdi, [rcx+0x1C38] (embedded surfaces @ +7224)
constexpr const char* kPatFreeTraceData =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 33 F6 48 8D B9 38 1C 00 00";

// IDA InitTraceData: surfaces count@7200, ptr@7208→embedded@7224; segs ptr@+8
constexpr std::size_t kOffSurfacesCount = 0x1C20; // 7200
constexpr std::size_t kOffSurfacesPtr   = 0x1C28; // 7208
constexpr std::size_t kOffSegmentsPtr   = 0x08;
constexpr std::size_t kSegStride        = 56;
constexpr std::size_t kTraceDataSize    = 0x2200; // >= 7444
constexpr std::size_t kFilterSize       = 0x70;
// FireBullet InitFilter mask = 0x1C300B
constexpr std::uint64_t kMaskShotPen    = 0x1C300Bull;

struct TraceInfo {
	float  unk;        // +0
	float  distance;   // +4
	float  damage;     // +8  written by DamageToPoint
	std::int32_t pen_count; // +12
	std::uint16_t enter_idx; // +16
	std::uint16_t exit_idx;  // +18
	std::uint32_t flags;     // +20  bit0 = continue / solid
};
static_assert(sizeof(TraceInfo) == 24);

// InitTraceInfo writes through +187; FireBullet copies 208-byte slots
struct GameTraceAw {
	void* surface;      // +0
	void* hit_entity;   // +8
	void* hitbox_data;  // +16
	std::uint8_t pad[0xC0];
};
static_assert(sizeof(GameTraceAw) >= 0xC0);

using FnInitTraceData = void*(__fastcall*)(void* trace);
using FnCreateTrace = void(__fastcall*)(void* trace, const Vector_t* start, const Vector_t* delta,
	void* filter, int penCount, bool unk);
using FnDamageToPoint = void(__fastcall*)(void* trace, float damage, float pen, float rangeMod,
	int penCount, int team, void* unused);
using FnGetTraceInfo = void(__fastcall*)(void* trace, GameTraceAw* out, float distance, void* seg);
using FnInitFilter = void(__fastcall*)(void* filter, void* skip, std::uint64_t mask, int layer, std::uint16_t a5);
using FnFreeTraceData = void(__fastcall*)(void* trace);

FnInitTraceData g_initTraceData = nullptr;
FnCreateTrace g_createTrace = nullptr;
FnDamageToPoint g_damageToPoint = nullptr;
FnGetTraceInfo g_getTraceInfo = nullptr;
FnInitFilter g_initFilter = nullptr;
FnFreeTraceData g_freeTraceData = nullptr;
bool g_ready = false;
bool g_gamePath = false;

// --- perf: pen is ~0.1–0.5ms each; AF multipoint×enemies tanks FPS ---
// Cache quantized eye/aim results; budget game-pen calls per ms tick.
// NEVER cache budget-miss as fail — that blocked real wallbangs for TTL.
constexpr int kCacheSlots = 64;
constexpr int kPosQuant = 4;          // tighter quant → less multipoint reuse error
constexpr std::uint64_t kCacheTtlMs = 32;
constexpr int kMaxGamePenPerMs = 28;  // CreateTrace path / tick (was 10 — starved scan+shoot)
constexpr int kMaxSoftPenPerMs = 40;  // TraceLine fallback if SEH

struct PenCacheKey {
	void* target = nullptr;
	void* weapon = nullptr;
	int hitbox = -1;
	int allowPen = 0;
	int eyeQ[3]{};
	int aimQ[3]{};
};

struct PenCacheSlot {
	PenCacheKey key{};
	Result r{};
	std::uint64_t ms = 0;
	bool valid = false;
};

PenCacheSlot g_cache[kCacheSlots]{};
std::uint64_t g_budgetMs = 0;
int g_gamePenUsed = 0;
int g_softPenUsed = 0;

int Quant(float v) {
	return static_cast<int>(v) / kPosQuant;
}

void MakeKey(const Vector_t& eye, const Vector_t& aim, int hitbox,
	void* weapon, void* target, bool allowPen, PenCacheKey& k)
{
	k.target = target;
	k.weapon = weapon;
	k.hitbox = hitbox;
	k.allowPen = allowPen ? 1 : 0;
	k.eyeQ[0] = Quant(eye.x); k.eyeQ[1] = Quant(eye.y); k.eyeQ[2] = Quant(eye.z);
	k.aimQ[0] = Quant(aim.x); k.aimQ[1] = Quant(aim.y); k.aimQ[2] = Quant(aim.z);
}

bool KeyEq(const PenCacheKey& a, const PenCacheKey& b) {
	return a.target == b.target && a.weapon == b.weapon
		&& a.hitbox == b.hitbox && a.allowPen == b.allowPen
		&& a.eyeQ[0] == b.eyeQ[0] && a.eyeQ[1] == b.eyeQ[1] && a.eyeQ[2] == b.eyeQ[2]
		&& a.aimQ[0] == b.aimQ[0] && a.aimQ[1] == b.aimQ[1] && a.aimQ[2] == b.aimQ[2];
}

std::uint32_t KeyHash(const PenCacheKey& k) {
	// FNV-1a-ish mix of pointers + quants
	std::uint64_t h = 14695981039346656037ull;
	auto mix = [&](std::uint64_t v) {
		h ^= v;
		h *= 1099511628211ull;
	};
	mix(reinterpret_cast<std::uint64_t>(k.target));
	mix(reinterpret_cast<std::uint64_t>(k.weapon));
	mix(static_cast<std::uint64_t>(k.hitbox) | (static_cast<std::uint64_t>(k.allowPen) << 16));
	mix(static_cast<std::uint64_t>(k.eyeQ[0]) | (static_cast<std::uint64_t>(k.eyeQ[1]) << 21)
		| (static_cast<std::uint64_t>(k.eyeQ[2]) << 42));
	mix(static_cast<std::uint64_t>(k.aimQ[0]) | (static_cast<std::uint64_t>(k.aimQ[1]) << 21)
		| (static_cast<std::uint64_t>(k.aimQ[2]) << 42));
	return static_cast<std::uint32_t>(h ^ (h >> 32));
}

void TickBudget() {
	const std::uint64_t now = GetTickCount64();
	if (now != g_budgetMs) {
		g_budgetMs = now;
		g_gamePenUsed = 0;
		g_softPenUsed = 0;
	}
}

bool CacheLookup(const PenCacheKey& key, Result& out) {
	const std::uint64_t now = GetTickCount64();
	const int idx = static_cast<int>(KeyHash(key) % kCacheSlots);
	// primary + 2 linear probes
	for (int p = 0; p < 3; ++p) {
		const int i = (idx + p) % kCacheSlots;
		const PenCacheSlot& s = g_cache[i];
		if (!s.valid || now - s.ms > kCacheTtlMs)
			continue;
		if (KeyEq(s.key, key)) {
			out = s.r;
			return true;
		}
	}
	return false;
}

void CacheStore(const PenCacheKey& key, const Result& r) {
	const int idx = static_cast<int>(KeyHash(key) % kCacheSlots);
	// prefer empty / expired, else overwrite primary
	int best = idx;
	const std::uint64_t now = GetTickCount64();
	for (int p = 0; p < 3; ++p) {
		const int i = (idx + p) % kCacheSlots;
		if (!g_cache[i].valid || now - g_cache[i].ms > kCacheTtlMs) {
			best = i;
			break;
		}
	}
	g_cache[best].key = key;
	g_cache[best].r = r;
	g_cache[best].ms = now;
	g_cache[best].valid = true;
}

std::uint8_t* FindClient(const char* pat) {
	auto* p = M::FindPattern("client.dll", pat);
	if (!p)
		p = M::FindPattern("client", pat);
	return p;
}

int HitboxToHitgroup(int hb) {
	switch (hb) {
	case Config::HB_HEAD:    return 1;
	case Config::HB_NECK:    return 8;
	case Config::HB_CHEST:   return 2;
	case Config::HB_STOMACH: return 3;
	case Config::HB_PELVIS:  return 3;
	case Config::HB_ARMS:    return 4;
	case Config::HB_LEGS:    return 6;
	case Config::HB_FEET:    return 7;
	default:                 return 2;
	}
}

struct WeaponStats {
	float damage = 30.f;
	float pen = 1.f;
	float range = 8192.f;
	float rangeMod = 0.98f;
	float armorRatio = 0.5f;
	float hsMult = 4.f;
};

bool ReadWeaponStats(C_CSWeaponBase* weapon, WeaponStats& s) {
	if (!weapon)
		return false;
	auto* vdata = weapon->Data();
	if (!vdata)
		return false;

	s.damage = static_cast<float>(vdata->m_nDamage());
	s.pen = vdata->m_flPenetration();
	s.range = vdata->m_flRange();
	s.rangeMod = vdata->m_flRangeModifier();
	s.armorRatio = vdata->m_flArmorRatio();
	s.hsMult = vdata->m_flHeadshotMultiplier();

	if (!std::isfinite(s.damage) || s.damage < 1.f || s.damage > 500.f)
		s.damage = 30.f;
	if (!std::isfinite(s.pen) || s.pen < 0.f || s.pen > 10.f)
		s.pen = 1.f;
	if (!std::isfinite(s.range) || s.range < 1.f)
		s.range = 8192.f;
	if (!std::isfinite(s.rangeMod) || s.rangeMod <= 0.f || s.rangeMod > 1.f)
		s.rangeMod = 0.98f;
	if (!std::isfinite(s.armorRatio) || s.armorRatio < 0.05f || s.armorRatio > 5.f)
		s.armorRatio = 0.5f;
	if (!std::isfinite(s.hsMult) || s.hsMult < 0.5f || s.hsMult > 20.f)
		s.hsMult = 4.f;
	return true;
}

// CCSPlayer_ItemServices::m_bHasHelmet @ +0x49 (verified dump)
bool HasHelmet(C_CSPlayerPawn* target) {
	if (!target)
		return false;
	void* svc = nullptr;
	__try {
		svc = target->m_pItemServices();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!svc || !Mem::Valid(svc, 0x50))
		return false;
	__try {
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(svc) + 0x49);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

float ApplyRangeFalloff(float damage, float dist, float rangeMod) {
	if (damage <= 0.f)
		return 0.f;
	return damage * std::pow(rangeMod, dist / 500.f);
}

void ApplyArmor(float& damage, int armor, float armorRatio) {
	if (armor <= 0 || damage <= 0.f)
		return;
	// VData stores 2x ratio; ScaleDamage uses *0.5 (CS2)
	const float ratio = armorRatio * 0.5f;
	float dmgHealth = damage * ratio;
	const float dmgArmor = (damage - dmgHealth) * 0.5f;
	if (dmgArmor > static_cast<float>(armor))
		dmgHealth = damage - static_cast<float>(armor) * 0.5f;
	if (dmgHealth < 1.f)
		dmgHealth = 1.f;
	damage = dmgHealth;
}

void ScaleDamage(float& damage, int hitgroup, C_CSPlayerPawn* target, const WeaponStats& ws) {
	if (!target || damage <= 0.f)
		return;

	switch (hitgroup) {
	case 1: damage *= ws.hsMult; break;           // head
	case 3: damage *= 1.25f; break;               // stomach / pelvis
	case 6: case 7: damage *= 0.75f; break;       // legs / feet
	default: break;                               // chest / arms / neck
	}

	const int armor = target->m_ArmorValue();
	if (armor <= 0)
		return;

	const bool head = (hitgroup == 1);
	if (head && !HasHelmet(target))
		return; // no helmet → full head damage

	ApplyArmor(damage, armor, ws.armorRatio);
}

Result EstimateVisible(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* target)
{
	Result r{};
	WeaponStats ws{};
	if (!target)
		return r;
	if (!ReadWeaponStats(weapon, ws)) {
		ws.damage = 40.f;
		ws.rangeMod = 0.98f;
		ws.armorRatio = 0.5f;
		ws.hsMult = 4.f;
	}

	const float dist = eye.Distance(aimPoint);
	float dmg = ApplyRangeFalloff(ws.damage, dist, ws.rangeMod);
	ScaleDamage(dmg, HitboxToHitgroup(hitbox), target, ws);
	r.damage = (std::max)(dmg, 1.f);
	r.hit = true;
	r.penetrated = false;
	return r;
}

bool TraceToExitSimple(const Vector_t& start, const Vector_t& dir, Vector_t& exitOut, void* skip) {
	// IDA 0x180860570 HandleBulletPen — NO separate TraceToExit callee.
	// Exit geometry lives in TraceInfo enter_idx/exit_idx after CreateTrace.
	// Soft path: step out of solid, confirm free air. Search far enough for
	// thick props (IDA has no hard thickness kill — only dmg < 1).
	bool wasSolid = true;
	for (float step = 0.25f; step <= 90.f; step += 0.5f) {
		const Vector_t p = start + dir * step;
		const Vector_t p2 = p + dir * 4.f;
		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(p, p2, skip, tr, Trace::kMaskShotPen))
			continue;
		const bool solid = tr.startsolid();
		if (wasSolid && !solid && (!Trace::DidHit(tr) || tr.fraction() > 0.02f)) {
			exitOut = p + dir * 0.25f;
			return true;
		}
		if (!solid && !Trace::DidHit(tr)) {
			exitOut = p + dir * 0.25f;
			return true;
		}
		wasSolid = solid || (Trace::DidHit(tr) && tr.fraction() < 0.02f);
	}
	return false;
}

// IDA HandleBulletPen 0x180860570 (re-verified this session):
//   surf_pen @ surfaceData+8; if < 0.1 → pens wiped / bullet dies
//   inv = 1/surf_pen  (via sub_1801BBCD0 clamp)
//   base = max(0, 3/wpn_pen*1.25)*(inv*3) + 0.16*dmg   (thin/grate mods below)
//   thickness = |exit-enter|; loss = thickness^2 * inv / 24 + base
//   dmg -= loss; if dmg < 1 → stop (return 1 = bullet dead)
//   else --pens, continue (return 0)
// Special (IDA): type 76 → pen=2; 85/87 → pen=3; thin 71/89 → 0.05 scale + pen=3;
//   dual grate 0x2000 thin → pen=32 scale~1e-5; thick grate → pen=3
float PenLoss(float damage, float weaponPen, float thickness, float surfPen,
	float scaleMod = 0.16f)
{
	const float invSurf = 1.f / (std::max)(0.1f, surfPen);
	const float wpen = (std::max)(0.05f, weaponPen);
	const float base = (std::max)(0.f, (3.f / wpen) * 1.25f) * (invSurf * 3.f)
		+ scaleMod * damage;
	return (thickness * thickness * invSurf) / 24.f + base;
}

// Soft TraceLine pen — ONLY when game CreateTrace path unavailable.
// Fail closed: real surface data, real exit, IDA loss, target entity hit after ≥1 wall.
Result FirePenTrace(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target)
{
	Result r{};
	WeaponStats ws{};
	if (!target)
		return r;
	if (!ReadWeaponStats(weapon, ws)) {
		ws.damage = 40.f;
		ws.pen = 1.f;
		ws.range = 8192.f;
		ws.rangeMod = 0.98f;
		ws.armorRatio = 0.5f;
		ws.hsMult = 4.f;
	}
	// Weapon with no pen power cannot wallbang
	if (ws.pen < 0.05f)
		return r;
	if (!Trace::Ready())
		return r;

	Vector_t dir = aimPoint - eye;
	const float fullDist = dir.Length();
	if (fullDist < 1.f)
		return r;
	dir = dir / fullDist;

	float damage = ws.damage;
	Vector_t cur = eye;
	float traveled = 0.f;
	// IDA FireBullet pen count typically 4
	int pensLeft = 4;
	int walls = 0;

	constexpr int kMaxWalls = 4;
	for (int iter = 0; iter < 16 && damage >= 1.f && walls <= kMaxWalls; ++iter) {
		const float remain = (std::min)(ws.range - traveled, fullDist - traveled + 4.f);
		if (remain <= 1.f)
			break;
		const Vector_t next = cur + dir * remain;

		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(cur, next, local, tr, Trace::kMaskShotPen))
			break;

		void* hit = tr.hit_entity();
		const float frac = (std::clamp)(tr.fraction(), 0.f, 1.f);
		traveled += remain * frac;

		// Hit target after ≥1 wall only
		if (Trace::HitsTarget(hit, target)) {
			if (walls == 0)
				return r; // visible — not pen path
			if (damage < 1.f)
				return r;
			float dmg = ApplyRangeFalloff(damage, traveled, ws.rangeMod);
			ScaleDamage(dmg, HitboxToHitgroup(hitbox), target, ws);
			if (dmg < 1.f)
				return r;
			r.damage = dmg;
			r.penetrated = true;
			r.hit = true;
			return r;
		}

		// Clear / past aim without target → miss
		if (!Trace::DidHit(tr) || frac >= 0.999f)
			break;

		if (pensLeft <= 0 || walls >= kMaxWalls)
			break;

		// Wall/prop — real surface pen (IDA: surf_pen < 0.1 → stop)
		float surfPen = 0.f;
		float surfHard = 1.f;
		if (!Trace::GetHitSurfaceData(tr, surfPen, surfHard)
			|| !std::isfinite(surfPen) || surfPen < 0.1f) {
			break;
		}
		// IDA mods: keep raw pen, clamp floor only (type 76/85 force higher later)
		surfPen = std::clamp(surfPen, 0.1f, 32.f);

		const Vector_t enter = tr.endpos();
		Vector_t exitPos{};
		if (!TraceToExitSimple(enter, dir, exitPos, local))
			break;

		const float thickness = (exitPos - enter).Length();
		if (thickness < 0.05f)
			break;
		// IDA: no hard thickness kill — only dmg < 1 after loss.
		// Soft path: allow up to ~90u (matches exit search); absurd slabs die via PenLoss.
		if (thickness > 90.f)
			break;

		// Thin surfaces (IDA 71/89 < 6u): scale 0.05 instead of 0.16
		const float scaleMod = (thickness < 6.f) ? 0.05f : 0.16f;
		const float loss = PenLoss(damage, ws.pen, thickness, surfPen, scaleMod);
		damage -= loss;
		if (damage < 1.f)
			break;
		traveled += thickness;
		cur = exitPos + dir * 1.0f;
		--pensLeft;
		++walls;
	}
	return r;
}

// SEH-only core — no C++ objects with destructors (C2712)
struct GameFireIn {
	const Vector_t* eye;
	const Vector_t* delta;
	void* filter;
	void* local;
	void* target;
	float damage;
	float pen;
	float rangeMod;
	float dist;       // eye → aim (for falloff when final surface dmg wiped)
	int pens;
	int team;
	int hitgroup;
	float armorRatio;
	float hsMult;
	int armor;
	bool helmet;
};

struct GameFireOut {
	float damage;
	bool hit;
	bool penetrated;
	int seh;
	// raw scan results from SEH core (scaled outside)
	float rawDmg;
	bool rawPen;
	bool rawOk;
};

void ScaleGameDamage(const GameFireIn& in, float dmg, bool penetrated, GameFireOut& out) {
	if (dmg < 1.f)
		return;
	switch (in.hitgroup) {
	case 1: dmg *= in.hsMult; break;
	case 3: dmg *= 1.25f; break;
	case 6: case 7: dmg *= 0.75f; break;
	default: break;
	}
	if (in.armor > 0) {
		const bool head = (in.hitgroup == 1);
		if (!head || in.helmet) {
			const float ratio = in.armorRatio * 0.5f;
			float dmgHealth = dmg * ratio;
			const float dmgArmor = (dmg - dmgHealth) * 0.5f;
			if (dmgArmor > static_cast<float>(in.armor))
				dmgHealth = dmg - static_cast<float>(in.armor) * 0.5f;
			if (dmgHealth < 1.f)
				dmgHealth = 1.f;
			dmg = dmgHealth;
		}
	}
	out.damage = dmg;
	out.penetrated = penetrated;
	out.hit = true;
}

// SEH core only — POD locals, no C++ dtors (C2712)
GameFireOut FireGameSeh(const GameFireIn& in) {
	GameFireOut out{};
	if (!g_initTraceData || !g_createTrace || !g_damageToPoint || !g_initFilter || !g_freeTraceData)
		return out;

	alignas(16) std::uint8_t traceBuf[kTraceDataSize];
	std::memset(traceBuf, 0, sizeof(traceBuf));

	float priorMax = 0.f;
	int targetIdx = -1;
	int surfaces = 0;
	int seh = 0;
	bool inited = false;

	__try {
		g_initTraceData(traceBuf);
		inited = true;

		// FireBullet: InitFilter(filter, skip, 0x1C300B, 3, 15) then set extras
		g_initFilter(in.filter, in.local, kMaskShotPen, 3, 15);
		{
			auto* f = static_cast<std::uint8_t*>(in.filter);
			// filter+16 |= 0x4000000000 (FireBullet after InitFilter)
			*reinterpret_cast<std::uint64_t*>(f + 16) |= 0x4000000000ull;
			f[57] |= 2; // FireBullet: v230 |= 2
		}

		g_createTrace(traceBuf, in.eye, in.delta, in.filter, in.pens, true);
		g_damageToPoint(traceBuf, in.damage, in.pen, in.rangeMod, in.pens, in.team, nullptr);

		surfaces = *reinterpret_cast<int*>(traceBuf + kOffSurfacesCount);
		auto* infos = *reinterpret_cast<TraceInfo**>(traceBuf + kOffSurfacesPtr);
		void* segs = *reinterpret_cast<void**>(traceBuf + kOffSegmentsPtr);

		// ONLY accept hit when GetTraceInfo entity matches target.
		// Never invent dmg from priorMax / falloff when target not hit
		// (that caused autofire to wallbang every wall).
		if (infos && surfaces > 0 && surfaces <= 64 && g_getTraceInfo && segs && in.target) {
			for (int i = 0; i < surfaces; ++i) {
				const TraceInfo& info = infos[i];
				if (info.damage >= 1.f && info.damage > priorMax)
					priorMax = info.damage;

				// FireBullet: seg = segments + 56 * exit_idx (fallback enter)
				const std::uint16_t idx = info.exit_idx ? info.exit_idx : info.enter_idx;
				if (idx >= 128)
					continue;
				auto* seg = reinterpret_cast<std::uint8_t*>(segs)
					+ kSegStride * static_cast<std::size_t>(idx);
				alignas(16) GameTraceAw gt{};
				g_getTraceInfo(traceBuf, &gt, info.distance, seg);
				if (Trace::HitsTarget(gt.hit_entity, in.target)) {
					targetIdx = i;
					// Prefer surface that actually hit target
					if (info.damage >= 1.f)
						priorMax = info.damage;
				}
			}

			if (targetIdx >= 0) {
				// ONLY the surface that hit the target — never priorMax from walls
				const TraceInfo& ti = infos[targetIdx];
				float dmg = ti.damage;
				if (dmg < 1.f) {
					// Target segment with wiped dmg = bullet died in wall
					out.rawOk = false;
				} else {
					// When pens==0 (visible-only call), first surface only
					// When pens>0 and wallbang expected: require real pen
					// (targetIdx>0 or pen_count dropped). First surface with
					// pens still full + blocked world = false positive.
					const bool didPen =
						(targetIdx > 0) || (ti.pen_count < in.pens);
					if (in.pens > 0 && !didPen && targetIdx == 0) {
						// Hit first surface as target — visible, not wallbang.
						// Caller may still accept as visible estimate path.
						out.rawOk = true;
						out.rawDmg = dmg;
						out.rawPen = false;
					} else if (in.pens > 0 && !didPen) {
						out.rawOk = false; // wall hit without pen proof
					} else {
						out.rawOk = true;
						out.rawDmg = dmg;
						out.rawPen = didPen || (in.pens == 0);
					}
				}
			}
			// targetIdx < 0 → wall stopped bullet / no target → rawOk false
			(void)priorMax;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		seh = 1;
	}

	// Always free if InitTraceData ran — DamageToPoint may heap-grow surfaces
	if (inited && g_freeTraceData) {
		__try {
			g_freeTraceData(traceBuf);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			seh = 1;
		}
	}

	out.seh = seh;
	if (seh) {
		out.hit = false;
		out.damage = 0.f;
		return out;
	}
	if (out.rawOk)
		ScaleGameDamage(in, out.rawDmg, out.rawPen, out);
	return out;
}

// outSeh: true if game path crashed (caller may soft-fallback)
Result FireGame(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	bool* outSeh = nullptr)
{
	Result r{};
	if (outSeh) *outSeh = false;
	WeaponStats ws{};
	if (!ReadWeaponStats(weapon, ws) || !g_gamePath)
		return r;

	Vector_t delta = aimPoint - eye;
	const float dist = delta.Length();
	if (dist < 1.f)
		return r;
	// CreateTrace expects direction * range (FireBullet: dir * weapon range)
	delta = delta * (ws.range / dist);

	alignas(16) std::uint8_t filter[kFilterSize]{};

	GameFireIn in{};
	in.eye = &eye;
	in.delta = &delta;
	in.filter = filter;
	in.local = local;
	in.target = target;
	in.damage = ws.damage;
	in.pen = ws.pen;
	in.rangeMod = ws.rangeMod;
	in.dist = dist;
	in.pens = allowPen ? 4 : 0;
	in.team = static_cast<int>(local->m_iTeamNum());
	in.hitgroup = HitboxToHitgroup(hitbox);
	in.armorRatio = ws.armorRatio;
	in.hsMult = ws.hsMult;
	in.armor = target ? target->m_ArmorValue() : 0;
	in.helmet = HasHelmet(target);

	const GameFireOut o = FireGameSeh(in);
	if (o.seh) {
		if (outSeh) *outSeh = true;
		return r;
	}
	r.damage = o.damage;
	r.hit = o.hit;
	r.penetrated = o.penetrated;
	return r;
}

} // namespace

bool Init() {
	g_initTraceData = reinterpret_cast<FnInitTraceData>(FindClient(kPatInitTraceData));
	if (!g_initTraceData)
		g_initTraceData = reinterpret_cast<FnInitTraceData>(FindClient(kPatInitTraceDataLoose));
	g_createTrace = reinterpret_cast<FnCreateTrace>(FindClient(kPatCreateTrace));
	if (!g_createTrace)
		g_createTrace = reinterpret_cast<FnCreateTrace>(FindClient(kPatCreateTraceLoose));
	g_damageToPoint = reinterpret_cast<FnDamageToPoint>(FindClient(kPatDamageToPoint));
	if (!g_damageToPoint)
		g_damageToPoint = reinterpret_cast<FnDamageToPoint>(FindClient(kPatDamageToPointLoose));
	g_getTraceInfo = reinterpret_cast<FnGetTraceInfo>(FindClient(kPatGetTraceInfo));
	if (!g_getTraceInfo)
		g_getTraceInfo = reinterpret_cast<FnGetTraceInfo>(FindClient(kPatGetTraceInfoLoose));
	g_initFilter = reinterpret_cast<FnInitFilter>(FindClient(kPatInitFilter));
	g_freeTraceData = reinterpret_cast<FnFreeTraceData>(FindClient(kPatFreeTraceData));
	void* pen = FindClient(kPatHandleBulletPen);

	// Game path needs GetTraceInfo to verify target surface — else no invent dmg.
	g_gamePath = g_initTraceData && g_createTrace && g_damageToPoint
		&& g_initFilter && g_freeTraceData && g_getTraceInfo;
	g_ready = true;

	if (g_gamePath)
		Con::Ok("AutoWall: game pen ready (Init+Create+Damage+GetInfo+Free)%s",
			pen ? "" : " [HandleBulletPen miss — DamageToPoint still calls it]");
	else {
		Con::Ok("AutoWall: TraceLine pen fallback ONLY (strict fail-closed)");
		if (!g_initTraceData) Con::OffsetMiss("AutoWall::InitTraceData");
		if (!g_createTrace)   Con::OffsetMiss("AutoWall::CreateTrace");
		if (!g_damageToPoint) Con::OffsetMiss("AutoWall::DamageToPoint");
		if (!g_getTraceInfo)  Con::OffsetMiss("AutoWall::GetTraceInfo");
		if (!g_initFilter)    Con::OffsetMiss("AutoWall::InitFilter");
		if (!g_freeTraceData) Con::OffsetMiss("AutoWall::FreeTraceData");
	}
	if (!pen)
		Con::OffsetMiss("AutoWall::HandleBulletPen");
	return true;
}

bool Ready() {
	return g_ready;
}

Result Fire(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen)
{
	Result r{};
	if (!weapon || !local || !target)
		return r;
	if (!g_ready)
		Init();

	TickBudget();

	// Quantized cache — multipoints within ~8u reuse last pen result
	PenCacheKey key{};
	MakeKey(eye, aimPoint, hitbox, weapon, target, allowPen, key);
	if (CacheLookup(key, r))
		return r;

	// Hard LOS: EstimateVisible ONLY when first solid hit is the target.
	// Never treat "no hit" as clear (filter miss ≠ free wallbang).
	// Never trust IsVisible alone.
	bool clearLos = false;
	bool blockedByWorld = false;
	if (!Trace::Ready()) {
		// No TraceShape — never invent pen; visible estimate only if caller forbids pen
		if (!allowPen) {
			r = EstimateVisible(eye, aimPoint, hitbox, weapon, target);
			CacheStore(key, r);
			return r;
		}
		CacheStore(key, r);
		return r;
	}

	{
		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(eye, aimPoint, local, tr, Trace::kMaskShotPen)) {
			CacheStore(key, r);
			return r;
		}
		void* hit = tr.hit_entity();
		if (Trace::HitsTarget(hit, target)) {
			clearLos = true;
		} else if (Trace::DidHit(tr) && tr.fraction() < 0.999f) {
			blockedByWorld = true;
		} else {
			// No solid between eye and aimPoint — still not EstimateVisible
			// unless hull also clear of non-target (world brush catch).
			Trace::CGameTrace th{};
			const Vector_t mins{ -0.5f, -0.5f, -0.5f };
			const Vector_t maxs{  0.5f,  0.5f,  0.5f };
			if (Trace::TraceHull(eye, aimPoint, mins, maxs, local, th, Trace::kMaskShotPen)) {
				void* hh = th.hit_entity();
				if (Trace::HitsTarget(hh, target))
					clearLos = true;
				else if (Trace::DidHit(th) && th.fraction() < 0.97f)
					blockedByWorld = true;
				else
					clearLos = true; // air path, target capsule may miss line
			} else {
				CacheStore(key, r);
				return r;
			}
		}
	}

	// Visible: armor/range estimate only — never pen invent (cheap, always cache)
	if (clearLos && !blockedByWorld) {
		r = EstimateVisible(eye, aimPoint, hitbox, weapon, target);
		CacheStore(key, r);
		return r;
	}

	// Wall / prop between eye and aim
	if (!allowPen) {
		CacheStore(key, r);
		return r; // wallbang disabled — fail closed
	}

	// Weapon must be able to pen (VData m_flPenetration)
	{
		WeaponStats ws{};
		if (!ReadWeaponStats(weapon, ws) || ws.pen < 0.05f) {
			CacheStore(key, r);
			return r;
		}
	}

	// Prefer game CreateTrace + DamageToPoint + HandleBulletPen (IDA verified).
	// Budget: skip when flooded — do NOT cache miss (TTL would block real pens).
	if (g_gamePath) {
		if (g_gamePenUsed < kMaxGamePenPerMs) {
			++g_gamePenUsed;
			bool seh = false;
			Result game = FireGame(eye, aimPoint, hitbox, weapon, local, target, true, &seh);
			if (!seh) {
				if (game.hit && game.damage >= 1.f) {
					// Wall path requires real pen proof from DamageToPoint surfaces
					if (blockedByWorld && !game.penetrated) {
						CacheStore(key, r);
						return r;
					}
					if (blockedByWorld)
						game.penetrated = true;
					CacheStore(key, game);
					return game;
				}
				// Game clean + no target hit = cannot pen this wall/prop
				CacheStore(key, r);
				return r;
			}
			// SEH — fall through to soft
		}
		// over budget: try soft, else uncached miss
	}

	if (g_softPenUsed < kMaxSoftPenPerMs) {
		++g_softPenUsed;
		Result pen = FirePenTrace(eye, aimPoint, hitbox, weapon, local, target);
		if (pen.hit && pen.damage >= 1.f && pen.penetrated) {
			CacheStore(key, pen);
			return pen;
		}
		// Real miss from soft path — cache
		CacheStore(key, r);
		return r;
	}

	// Over both budgets: do not cache — next tick re-evaluates
	return r;
}

bool PassesMinDamage(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamage)
{
	const Result r = Fire(eye, aimPoint, hitbox, weapon, local, target, allowPen);
	if (!r.hit || r.damage <= 0.f)
		return false;

	if (minDamage <= 0.f)
		return true;

	// If target HP < mindmg → require lethal only (hp-aware)
	float need = minDamage;
	if (target) {
		const int hp = target->m_iHealth();
		if (hp > 0 && static_cast<float>(hp) < need)
			need = static_cast<float>(hp);
	}
	return r.damage + 0.01f >= need;
}

} // namespace AutoWall
