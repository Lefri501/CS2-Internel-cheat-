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

// IDA client.dll (verified):
//   InitTraceData        @ 0x18083E4D0
//   CreateTrace          @ 0x180842660  (used by FireBullet pipeline)
//   DamageToPoint        @ 0x180844C50  → loops surfaces, calls HandleBulletPen
//   HandleBulletPen      @ 0x180860350
//   GetTraceInfo         @ 0x180844D70  (seg = segments + 56 * idx)
//   FreeTraceData        @ 0x18083F210
//   InitFilter           @ 0x1803432D0
//   FireBullet caller    @ 0x1808472C0 uses mask 0x1C300B
// IDA-verified signatures (client.dll)
constexpr const char* kPatInitTraceData =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8D 79 ? 33 F6 C7 47";
constexpr const char* kPatCreateTrace =
	"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? F2 0F 10 02";
constexpr const char* kPatDamageToPoint =
	"40 53 57 41 56 48 83 EC ? 8B 84 24";
constexpr const char* kPatHandleBulletPen =
	"48 8B C4 44 89 48 ? 48 89 50 ? 48 89 48 ? 55 57";
constexpr const char* kPatGetTraceInfo =
	"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 48 8B E9 0F 29 74 24";
constexpr const char* kPatInitFilter =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F B6 41 ? 33 FF 24";
// FreeTraceData prologue is long; short unique head
constexpr const char* kPatFreeTraceData =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 33 F6 48 8D B9";

// IDA DamageToPoint / InitTraceData layout
constexpr std::size_t kOffSurfacesCount = 0x1C20; // 7200
constexpr std::size_t kOffSurfacesPtr   = 0x1C28; // 7208
constexpr std::size_t kOffSegmentsPtr   = 0x08;
constexpr std::size_t kSegStride        = 56;
constexpr std::size_t kTraceDataSize    = 0x2200; // covers embedded segs + surfaces
constexpr std::uint64_t kMaskShotPen    = 0x1C300Bull; // FireBullet filter mask

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

struct GameTraceAw {
	void* surface;      // +0
	void* hit_entity;   // +8
	void* hitbox_data;  // +16
	std::uint8_t pad[0xF0];
};

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
	for (float step = 2.f; step <= 90.f; step += 2.f) {
		const Vector_t p = start + dir * step;
		const Vector_t p2 = p + dir * 4.f;
		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(p, p2, skip, tr, Trace::kMaskShot))
			continue;
		if (!tr.startsolid()) {
			exitOut = p;
			return true;
		}
	}
	return false;
}

// IDA HandleBulletPen loss (generic wall, surf_pen≈1):
//   base = max(0, 3/wpn_pen * 1.25) * (inv_surf * 3) + 0.16 * dmg
//   loss = thickness^2 * inv_surf / 24 + base
float PenLoss(float damage, float weaponPen, float thickness, float surfPen = 1.f) {
	const float invSurf = 1.f / (std::max)(0.1f, surfPen);
	const float wpen = (std::max)(0.05f, weaponPen);
	const float base = (std::max)(0.f, (3.f / wpen) * 1.25f) * (invSurf * 3.f)
		+ 0.16f * damage;
	return (thickness * thickness * invSurf) / 24.f + base;
}

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
	int pensLeft = 4;
	int walls = 0;

	for (int iter = 0; iter < 6 && damage >= 1.f; ++iter) {
		const float remain = (std::min)(ws.range - traveled, fullDist - traveled + 8.f);
		if (remain <= 1.f)
			break;
		const Vector_t next = cur + dir * remain;

		Trace::CGameTrace tr{};
		if (!Trace::TraceLine(cur, next, local, tr, Trace::kMaskShot))
			break;

		void* hit = tr.hit_entity();
		const float frac = (std::clamp)(tr.fraction(), 0.f, 1.f);
		traveled += remain * frac;

		auto FinalizeHit = [&]() -> Result {
			Result out{};
			float dmg = ApplyRangeFalloff(damage, traveled, ws.rangeMod);
			ScaleDamage(dmg, HitboxToHitgroup(hitbox), target, ws);
			out.damage = (std::max)(dmg, 1.f);
			out.penetrated = walls > 0;
			out.hit = true;
			return out;
		};

		if (hit == target)
			return FinalizeHit();

		if (!Trace::DidHit(tr) && traveled + 8.f >= fullDist)
			return FinalizeHit();

		if (!Trace::DidHit(tr) || pensLeft <= 0)
			break;

		const Vector_t enter = tr.endpos();
		Vector_t exitPos{};
		if (!TraceToExitSimple(enter, dir, exitPos, local))
			break;

		const float thickness = (exitPos - enter).Length();
		damage -= PenLoss(damage, ws.pen, thickness, 1.f);
		if (damage < 1.f)
			break;
		traveled += thickness;
		cur = exitPos;
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
	if (!g_initTraceData || !g_createTrace || !g_damageToPoint || !g_initFilter)
		return out;

	alignas(16) std::uint8_t traceBuf[kTraceDataSize];
	std::memset(traceBuf, 0, sizeof(traceBuf));

	float priorMax = 0.f;
	int targetIdx = -1;
	int surfaces = 0;
	int seh = 0;

	__try {
		g_initTraceData(traceBuf);
		g_initFilter(in.filter, in.local, kMaskShotPen, 3, 15);
		g_createTrace(traceBuf, in.eye, in.delta, in.filter, in.pens, true);
		g_damageToPoint(traceBuf, in.damage, in.pen, in.rangeMod, in.pens, in.team, nullptr);

		surfaces = *reinterpret_cast<int*>(traceBuf + kOffSurfacesCount);
		auto* infos = *reinterpret_cast<TraceInfo**>(traceBuf + kOffSurfacesPtr);
		void* segs = *reinterpret_cast<void**>(traceBuf + kOffSegmentsPtr);

		if (infos && surfaces > 0 && surfaces <= 64) {
			for (int i = 0; i < surfaces; ++i) {
				const TraceInfo& info = infos[i];
				if (info.damage >= 1.f && info.damage > priorMax)
					priorMax = info.damage;

				if (!g_getTraceInfo || !segs || !in.target)
					continue;

				const std::uint16_t idx = info.exit_idx ? info.exit_idx : info.enter_idx;
				auto* seg = reinterpret_cast<std::uint8_t*>(segs)
					+ kSegStride * static_cast<std::size_t>(idx & 0x7FFF);
				GameTraceAw gt{};
				g_getTraceInfo(traceBuf, &gt, info.distance, seg);
				if (gt.hit_entity == in.target)
					targetIdx = i;
			}

			if (targetIdx >= 0) {
				out.rawOk = true;
				if (priorMax >= 1.f && targetIdx > 0) {
					out.rawDmg = priorMax;
					out.rawPen = true;
				} else if (infos[targetIdx].damage >= 1.f) {
					out.rawDmg = infos[targetIdx].damage;
					out.rawPen = targetIdx > 0;
				} else {
					out.rawDmg = in.damage * std::pow(in.rangeMod, in.dist / 500.f);
					out.rawPen = targetIdx > 0;
				}
			} else if (!g_getTraceInfo) {
				out.rawOk = true;
				if (priorMax >= 1.f) {
					out.rawDmg = priorMax;
					out.rawPen = surfaces > 1;
				} else {
					out.rawDmg = in.damage * std::pow(in.rangeMod, in.dist / 500.f);
					out.rawPen = false;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		seh = 1;
	}

	if (g_freeTraceData) {
		__try {
			g_freeTraceData(traceBuf);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
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

Result FireGame(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen)
{
	Result r{};
	WeaponStats ws{};
	if (!ReadWeaponStats(weapon, ws) || !g_gamePath)
		return r;

	Vector_t delta = aimPoint - eye;
	const float dist = delta.Length();
	if (dist < 1.f)
		return r;
	// CreateTrace expects direction * range (FireBullet scales angles then * weapon range)
	delta = delta * (ws.range / dist);

	alignas(16) std::uint8_t filter[0x50]{};

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
	if (o.seh)
		return r;
	r.damage = o.damage;
	r.hit = o.hit;
	r.penetrated = o.penetrated;
	return r;
}

} // namespace

bool Init() {
	g_initTraceData = reinterpret_cast<FnInitTraceData>(FindClient(kPatInitTraceData));
	g_createTrace = reinterpret_cast<FnCreateTrace>(FindClient(kPatCreateTrace));
	g_damageToPoint = reinterpret_cast<FnDamageToPoint>(FindClient(kPatDamageToPoint));
	g_getTraceInfo = reinterpret_cast<FnGetTraceInfo>(FindClient(kPatGetTraceInfo));
	g_initFilter = reinterpret_cast<FnInitFilter>(FindClient(kPatInitFilter));
	g_freeTraceData = reinterpret_cast<FnFreeTraceData>(FindClient(kPatFreeTraceData));
	(void)FindClient(kPatHandleBulletPen);

	// FreeTraceData pattern may miss — optional; InitTraceData + Create + Damage required
	g_gamePath = g_initTraceData && g_createTrace && g_damageToPoint && g_initFilter;
	g_ready = true;

	if (g_gamePath)
		Con::Ok("AutoWall: game pen ready (InitTrace+Create+Damage)%s",
			g_freeTraceData ? "" : " [no free]");
	else {
		Con::Ok("AutoWall: estimate + TraceLine pen (game patterns partial)");
		if (!g_initTraceData) Con::OffsetMiss("AutoWall::InitTraceData");
		if (!g_createTrace)   Con::OffsetMiss("AutoWall::CreateTrace");
		if (!g_damageToPoint) Con::OffsetMiss("AutoWall::DamageToPoint");
		if (!g_initFilter)    Con::OffsetMiss("AutoWall::InitFilter");
	}
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

	// Visible: fast estimate (caller owns LOS)
	if (!allowPen)
		return EstimateVisible(eye, aimPoint, hitbox, weapon, target);

	// Game CreateTrace path can AV / corrupt when signatures drift — only use when
	// Trace system is healthy (same phys world). On any Trace fault, stay on estimate.
	if (g_gamePath && Trace::Ready()) {
		Result game = FireGame(eye, aimPoint, hitbox, weapon, local, target, true);
		if (game.hit)
			return game;
	}

	// TraceLine pen fallback only when TraceShape is known good (no AV circuit-break).
	// Otherwise estimate (range falloff only) — never crash the process for mindmg.
	if (Trace::Ready()) {
		Result pen = FirePenTrace(eye, aimPoint, hitbox, weapon, local, target);
		if (pen.hit)
			return pen;
		return pen;
	}

	return EstimateVisible(eye, aimPoint, hitbox, weapon, target);
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

	// Standard: if target HP < mindmg, require lethal (still shoot low-HP)
	float need = minDamage;
	if (target) {
		const int hp = target->m_iHealth();
		if (hp > 0 && static_cast<float>(hp) < need)
			need = static_cast<float>(hp);
	}
	return r.damage + 0.01f >= need;
}

} // namespace AutoWall
