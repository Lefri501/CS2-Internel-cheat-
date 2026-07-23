#include "nospread.h"
#include "../hitchance/hitchance.h"
#include "../bones/bones.h"
#include "../aim/aim_common.h"
#include "../autowall/autowall.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════
// Seed nospread — clean rewrite (IDA 2026-07-23)
//
// CSBaseGunFire:
//   punched = view + ComputeAimPunchFire(tick,frac)
//   seed    = SPREADSEEDGEN(punched, nPlayerTickCount)   @ 0x180CB6E80
//   (sx,sy) = CalcSpread(def, nBullets, mode, seed+1, inac, spr, recoil)
//             @ 0x180CB77A0  (game path only — local ran1 ULP ≠ server)
//   dir     = fwd - right*sx + up*sy                      @ FireBullet
//
// Solve: freeze sx/sy from wish, invert for view so dir hits capsule,
//        ExactShotHits verify. No per-weapon WAIT gates.
// ═══════════════════════════════════════════════════════════════════════

namespace NoSpread {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.f / kPi;
// Accept scale — limbs need looser rim (feet/legs thin capsules).
constexpr float kSearchScale = 0.96f;
constexpr float kSearchBias  = 0.92f;
constexpr float kAcceptScale = 0.96f;
constexpr float kAcceptBias  = 0.92f;
// IDA SPREADSEEDGEN 0x180CB6E80: SHA1(quant pitch, quant yaw, tick) — roll not hashed.
// FireBullet 0x1808474E0: dir = fwd - right*sx + up*sy (AngleVectors uses roll).

bool DirToAngles(const Vector_t& dir, QAngle_t& out)
{
	const float hyp = std::sqrt(dir.x * dir.x + dir.y * dir.y);
	if (!std::isfinite(hyp) || hyp < 1e-8f)
		return false;
	out.x = -std::atan2(dir.z, hyp) * kRad2Deg;
	out.y = std::atan2(dir.y, dir.x) * kRad2Deg;
	out.z = 0.f;
	out.Normalize();
	return out.IsValid();
}

bool CalcAngles(const Vector_t& from, const Vector_t& to, QAngle_t& out)
{
	Vector_t d{ to.x - from.x, to.y - from.y, to.z - from.z };
	const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (!std::isfinite(len) || len < 1e-4f)
		return false;
	d.x /= len; d.y /= len; d.z /= len;
	return DirToAngles(d, out);
}

float AngDelta(const QAngle_t& a, const QAngle_t& b)
{
	float dp = a.x - b.x;
	float dy = a.y - b.y;
	while (dy > 180.f) dy -= 360.f;
	while (dy < -180.f) dy += 360.f;
	return std::sqrt(dp * dp + dy * dy);
}

bool NormalizeDir(Vector_t& d)
{
	const float lenSqr = d.x * d.x + d.y * d.y + d.z * d.z;
	if (!std::isfinite(lenSqr) || lenSqr < 1e-12f)
		return false;
	const float inv = 1.f / std::sqrt(lenSqr);
	d.x *= inv; d.y *= inv; d.z *= inv;
	return std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z);
}

// IDA FireBullet inverse: wantDir = fwd - right*sx + up*sy
// → fwd ≈ want + right*sx - up*sy, iterate orthonormal basis.
bool SolveViewForDir(
	const Vector_t& wantDir,
	float sx,
	float sy,
	const QAngle_t& punchedStart,
	QAngle_t& outPunched)
{
	QAngle_t punched = punchedStart;
	punched.z = 0.f;
	punched.Normalize();
	if (!punched.IsValid())
		return false;

	for (int it = 0; it < 12; ++it) {
		Vector_t fwd{}, right{}, up{};
		punched.ToDirections(&fwd, &right, &up);
		Vector_t ideal{
			wantDir.x + right.x * sx - up.x * sy,
			wantDir.y + right.y * sx - up.y * sy,
			wantDir.z + right.z * sx - up.z * sy
		};
		if (!NormalizeDir(ideal))
			return false;
		QAngle_t next{};
		if (!DirToAngles(ideal, next))
			return false;
		if (AngDelta(punched, next) < 0.008f) {
			outPunched = next;
			return true;
		}
		punched = next;
	}
	outPunched = punched;
	return outPunched.IsValid();
}

// IDA CB1340: floor(AngleNormalize(a)*2)*0.5 — keep punch away from bin edges.
void NudgeBinSafe(QAngle_t& view, const QAngle_t* punch)
{
	QAngle_t punched = view;
	if (punch) {
		punched.x += punch->x;
		punched.y += punch->y;
	}
	punched.Normalize();

	auto edge = [](float a) {
		// fractional part of a*2 after normalize into half-deg bins
		float n = a;
		while (n > 180.f) n -= 360.f;
		while (n < -180.f) n += 360.f;
		const float q = std::floor(n * 2.f) * 0.5f;
		return n - q; // [0, 0.5)
	};
	const float fx = edge(punched.x);
	const float fy = edge(punched.y);
	if (fx < 0.10f)
		view.x += (0.10f - fx);
	else if (fx > 0.40f)
		view.x -= (fx - 0.40f);
	if (fy < 0.10f)
		view.y += (0.10f - fy);
	else if (fy > 0.40f)
		view.y -= (fy - 0.40f);
	view.z = 0.f;
	view.x = std::clamp(view.x, -89.f, 89.f);
	view.Normalize();
}

bool HbEnabled(int hb, const bool* enabled)
{
	if (hb < 0 || hb >= Config::HB_COUNT)
		return false;
	if (!enabled)
		return true;
	return enabled[hb];
}

bool IsCore(int hb)
{
	return hb == Config::HB_HEAD || hb == Config::HB_NECK
		|| hb == Config::HB_CHEST || hb == Config::HB_STOMACH
		|| hb == Config::HB_PELVIS;
}

// Rewrite budget from live bloom. Air deagle jump-inac needs wide cone
// (old 22° cap → Solve never finds pellet mid-jump).
float MaxDeltaDeg(float inac, float spr, C_CSWeaponBase* weapon, C_CSPlayerPawn* local)
{
	const float bloom = (std::isfinite(inac) && std::isfinite(spr))
		? (inac + spr) : 0.f;
	const float cone = bloom * kRad2Deg;

	bool air = false;
	if (local) {
		__try {
			const std::uint32_t f = local->m_fFlags();
			air = (f & FL_ONGROUND) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			air = false;
		}
	}

	const bool heavy = AimCommon::IsHeavyPistol(weapon);
	const bool sniper = AimCommon::IsSniperWeapon(weapon);

	float d = 0.f;
	if (heavy) {
		// Jump inac can exceed 0.5 — need full rewrite room or Solve never accepts.
		d = (std::max)(air ? 16.0f : 8.0f, cone * (air ? 2.6f : 1.5f) + (air ? 6.0f : 2.0f));
		return std::clamp(d, air ? 16.0f : 8.0f, air ? 72.f : 32.f);
	}
	if (sniper) {
		// IDA FillGunFireData interpolates hist angles with smoothstep f²(3-2f).
		// Big rewrite → model flick + server seed miss online (unscoped air/run).
		// Unscoped air: cap VERY tight — prefer natural Exact / roll-trick only.
		// Roll-trick keeps pitch/yaw near camera (no model rotation), only changes roll.
		bool scoped = false;
		if (local) {
			__try { scoped = AimCommon::IsLocalScoped(local, weapon); }
			__except (EXCEPTION_EXECUTE_HANDLER) { scoped = false; }
		}
		if (!scoped && air) {
			// Unscoped air: 8° max. Roll-trick + natural Exact only.
			// Larger rewrites cause visible model flick via FillGunFireData interp.
			d = (std::max)(5.0f, cone * 0.9f + 1.0f);
			return std::clamp(d, 5.0f, 8.f);
		}
		if (!scoped) {
			// Unscoped ground/run: 12° max.
			d = (std::max)(5.0f, cone * 1.0f + 1.5f);
			return std::clamp(d, 5.0f, 12.f);
		}
		// Scoped: can afford more (model not visible to player, zoom hides flick)
		d = (std::max)(air ? 6.0f : 5.0f, cone * 1.15f + 1.5f);
		const float cap = air ? 14.f : 18.f;
		return std::clamp(d, air ? 6.0f : 5.0f, cap);
	}
	d = (std::max)(10.0f, cone * 2.0f + 3.0f);
	const float cap = air ? 48.f : 56.f;
	return std::clamp(d, 10.0f, cap);
}

// Half-deg quant (IDA CB1340) — roll-trick bin skip without SHA1.
float QuantHalf(float a)
{
	while (a > 180.f) a -= 360.f;
	while (a < -180.f) a += 360.f;
	return std::floor(a * 2.f) * 0.5f;
}

// UC / IDA roll-trick: same seed (pitch/yaw quant), roll cancels pellet.
// pitch += deg(atan(|s|)), roll = -deg(atan2(sx,sy)); CRS(corrected)==seed0.
// k budget: 0 + ±2.5° half-deg steps (11) — old 33 SHA1s dominated Solve cost.
bool RollTrickView(
	const QAngle_t& wishUnpunched,
	const QAngle_t* punch,
	int seedTick,
	float sx,
	float sy,
	QAngle_t& outView)
{
	if (seedTick <= 0)
		return false;
	if (!std::isfinite(sx) || !std::isfinite(sy))
		return false;

	QAngle_t punchedWish = wishUnpunched;
	if (punch && punch->IsValid()) {
		punchedWish.x += punch->x;
		punchedWish.y += punch->y;
	}
	punchedWish.z = 0.f;
	punchedWish.Normalize();

	const float len = std::sqrt(sx * sx + sy * sy);
	if (!std::isfinite(len) || len < 1e-8f)
		return false;

	const std::uint32_t seed0 = HitChance::ComputeSeed(punchedWish, seedTick);
	const float pitchAdj = std::atan(len) * kRad2Deg;
	const float roll = -std::atan2(sx, sy) * kRad2Deg;
	const float basePitch = punchedWish.x + pitchAdj;

	// Skip SHA1 when quant pitch already tested (same SPREADSEEDGEN bin).
	float lastQx = 1e9f;
	for (int k = 0; k < 11; ++k) {
		float tryPitch = basePitch;
		if (k > 0) {
			const int step = (k + 1) / 2;
			const float sign = (k % 2 == 1) ? 1.f : -1.f;
			tryPitch += sign * static_cast<float>(step) * 0.5f;
		}
		tryPitch = std::clamp(tryPitch, -89.f, 89.f);
		const float qx = QuantHalf(tryPitch);
		if (k > 0 && std::fabs(qx - lastQx) < 1e-4f)
			continue;
		lastQx = qx;

		QAngle_t punchedCand{ tryPitch, punchedWish.y, roll };
		punchedCand.Normalize();
		if (HitChance::ComputeSeed(punchedCand, seedTick) != seed0)
			continue;

		QAngle_t view = punchedCand;
		if (punch && punch->IsValid()) {
			view.x -= punch->x;
			view.y -= punch->y;
		}
		view.x = std::clamp(view.x, -89.f, 89.f);
		view.Normalize();
		if (!view.IsValid())
			continue;
		outView = view;
		return true;
	}
	return false;
}

// Local ran1 only — never game CalcSpread on search (stomps RNG → crash/FPS).
bool BulletDir(
	const QAngle_t& view,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	const QAngle_t* punch,
	Vector_t& outDir,
	float* outSx,
	float* outSy)
{
	return HitChance::GetBulletDirectionCached(
		view, seedTick, weapon, inac, spr, outDir, outSx, outSy, 1u, punch,
		/*useLocalSpread=*/true);
}

// One game CalcSpread verify (matches server). Call only on accept candidates.
bool BulletDirGame(
	const QAngle_t& view,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	const QAngle_t* punch,
	Vector_t& outDir,
	float* outSx,
	float* outSy)
{
	return HitChance::GetBulletDirectionCached(
		view, seedTick, weapon, inac, spr, outDir, outSx, outSy, 1u, punch,
		/*useLocalSpread=*/false);
}

// All menu-enabled HBs. Prefer first, then core, then limbs.
bool RayHitsAny(
	C_CSPlayerPawn* target,
	const Vector_t& eye,
	const Vector_t& dir,
	int preferHb,
	const bool* enabled,
	int& outHb,
	Vector_t& outPt,
	float scale,
	float bias,
	bool /*allowLimbs*/)
{
	static constexpr int kCore[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS
	};
	static constexpr int kLimb[] = {
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};
	int tryList[12]{};
	int n = 0;
	auto push = [&](int hb) {
		if (hb < 0 || hb >= Config::HB_COUNT || !HbEnabled(hb, enabled))
			return;
		for (int i = 0; i < n; ++i)
			if (tryList[i] == hb) return;
		tryList[n++] = hb;
	};
	// Prefer whatever ray/crosshair hit (limb or core)
	if (preferHb >= 0)
		push(preferHb);
	for (int hb : kCore)
		push(hb);
	for (int hb : kLimb)
		push(hb);
	for (int i = 0; i < n; ++i) {
		float t = 0.f;
		Vector_t pt{};
		if (!Bones::RayHitsConfiguredHitbox(
				target, tryList[i], eye, dir, scale, t, pt, bias))
			continue;
		outHb = tryList[i];
		outPt = pt;
		return true;
	}
	return false;
}

// localAir/heavy/sniper precomputed once per SolveForAim — avoid SEH spam.
struct TryCtx {
	bool localAir = false;
	bool heavy = false;
	bool sniper = false;
	float accScale = kAcceptScale;
	float accBias = kAcceptBias;
	float gScale = 0.98f;
	float gBias = 0.94f;
	bool corePreferGate = false; // grounded non-heavy high bloom
};

void BuildTryCtx(C_CSWeaponBase* weapon, C_CSPlayerPawn* local,
	float inac, float spr, TryCtx& c)
{
	c = {};
	c.heavy = AimCommon::IsHeavyPistol(weapon);
	c.sniper = AimCommon::IsSniperWeapon(weapon);
	if (local) {
		__try {
			c.localAir = (local->m_fFlags() & FL_ONGROUND) == 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			c.localAir = false;
		}
	}
	if (c.heavy && c.localAir) {
		c.accScale = 1.05f;
		c.accBias = 0.98f;
		c.gScale = 1.05f;
		c.gBias = 0.98f;
	} else {
		const float bloom = (std::isfinite(inac) && std::isfinite(spr)) ? (inac + spr) : 0.f;
		if (bloom > 0.25f || c.sniper) {
			c.gScale = 0.96f;
			c.gBias = 0.92f;
		}
		c.corePreferGate = !c.localAir && !c.heavy && bloom > 0.28f;
	}
}

// verifyGame: false on search candidates; true once on accept (tier0 stomp cost).
bool TryView(
	const Vector_t& eye,
	const QAngle_t& view,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punch,
	int preferHb,
	const bool* enabled,
	float maxDelta,
	const QAngle_t& wish,
	const TryCtx& ctx,
	Shot& best,
	bool verifyGame = true)
{
	// Pitch/yaw delta only — roll free (seed ignores roll).
	QAngle_t wishPy = wish; wishPy.z = 0.f;
	QAngle_t viewPy = view; viewPy.z = 0.f;
	if (!view.IsValid() || AngDelta(wishPy, viewPy) > maxDelta + 0.05f)
		return false;

	// Keep roll — IDA FireBullet basis uses it (seed hash does not).
	QAngle_t cand = view;
	if (!std::isfinite(cand.z))
		cand.z = 0.f;
	cand.Normalize();
	if (!cand.IsValid())
		return false;

	// Search local ran1; final accept re-verifies game CalcSpread when available.
	Vector_t dir{};
	float sx = 0.f, sy = 0.f;
	if (!BulletDir(cand, seedTick, weapon, inac, spr, punch, dir, &sx, &sy))
		return false;

	int hb = -1;
	Vector_t pt{};
	if (!RayHitsAny(target, eye, dir, preferHb, enabled, hb, pt,
			ctx.accScale, ctx.accBias, true))
		return false;

	if (verifyGame) {
		// Game-path re-verify when available. NEVER hard-fail if game path missing.
		Vector_t gDir{};
		float gsx = 0.f, gsy = 0.f;
		if (BulletDirGame(cand, seedTick, weapon, inac, spr, punch, gDir, &gsx, &gsy)) {
			int ghb = -1;
			Vector_t gpt{};
			if (!RayHitsAny(target, eye, gDir, preferHb, enabled, ghb, gpt,
					ctx.gScale, ctx.gBias, true))
				return false;
			hb = ghb;
			pt = gpt;
			sx = gsx;
			sy = gsy;
			dir = gDir;
		}
	}

	// Core prefer only when grounded + high bloom.
	if (ctx.corePreferGate && !IsCore(hb) && preferHb >= 0 && IsCore(preferHb)) {
		bool anyCore = false;
		if (enabled) {
			for (int i = 0; i < Config::HB_COUNT; ++i)
				if (enabled[i] && IsCore(i)) { anyCore = true; break; }
		} else anyCore = true;
		if (anyCore)
			return false;
	}

	best.fireAngles = cand;
	best.hitPoint = pt;
	best.hitbox = hb;
	best.seedTick = seedTick;
	best.seedFrac = tickFrac;
	best.sx = sx;
	best.sy = sy;
	best.ok = true;
	return true;
}

// Local probe + single game CalcSpread.
bool AcceptView(
	const Vector_t& eye,
	const QAngle_t& view,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punch,
	int preferHb,
	const bool* enabled,
	float maxDelta,
	const QAngle_t& wish,
	const TryCtx& ctx,
	Shot& best)
{
	Shot tmp{};
	if (!TryView(eye, view, seedTick, tickFrac, weapon, local, target, inac, spr,
			punch, preferHb, enabled, maxDelta, wish, ctx, tmp, /*verifyGame=*/false))
		return false;

	// Game-path re-verify once. Missing game path → accept local (same as old).
	Vector_t gDir{};
	float gsx = 0.f, gsy = 0.f;
	if (BulletDirGame(tmp.fireAngles, seedTick, weapon, inac, spr, punch, gDir, &gsx, &gsy)) {
		int ghb = -1;
		Vector_t gpt{};
		if (!RayHitsAny(target, eye, gDir, preferHb, enabled, ghb, gpt,
				ctx.gScale, ctx.gBias, true))
			return false;
		tmp.hitbox = ghb;
		tmp.hitPoint = gpt;
		tmp.sx = gsx;
		tmp.sy = gsy;
	}
	// Re-check core prefer on final HB
	if (ctx.corePreferGate && !IsCore(tmp.hitbox) && preferHb >= 0 && IsCore(preferHb)) {
		bool anyCore = false;
		if (enabled) {
			for (int i = 0; i < Config::HB_COUNT; ++i)
				if (enabled[i] && IsCore(i)) { anyCore = true; break; }
		} else anyCore = true;
		if (anyCore)
			return false;
	}
	best = tmp;
	return best.ok;
}

// Finish a local probe without re-running BulletDir local.
bool AcceptProbe(
	const Vector_t& eye,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punch,
	int preferHb,
	const bool* enabled,
	const TryCtx& ctx,
	Shot& probe,
	Shot& best)
{
	if (!probe.ok)
		return false;
	Vector_t gDir{};
	float gsx = 0.f, gsy = 0.f;
	if (BulletDirGame(probe.fireAngles, seedTick, weapon, inac, spr, punch, gDir, &gsx, &gsy)) {
		int ghb = -1;
		Vector_t gpt{};
		if (!RayHitsAny(target, eye, gDir, preferHb, enabled, ghb, gpt,
				ctx.gScale, ctx.gBias, true))
			return false;
		probe.hitbox = ghb;
		probe.hitPoint = gpt;
		probe.sx = gsx;
		probe.sy = gsy;
	}
	if (ctx.corePreferGate && !IsCore(probe.hitbox) && preferHb >= 0 && IsCore(preferHb)) {
		bool anyCore = false;
		if (enabled) {
			for (int i = 0; i < Config::HB_COUNT; ++i)
				if (enabled[i] && IsCore(i)) { anyCore = true; break; }
		} else anyCore = true;
		if (anyCore)
			return false;
	}
	best = probe;
	return best.ok;
}

// Per-weapon fire gap. CanWeaponFire alone fails-open when nextTick=0 / tickbase lag
// → deagle re-fires every ~160ms (log 003530) and overspray-misses.
struct Latch {
	std::uint16_t def = 0;
	std::uint64_t fireMs = 0;
};
Latch g_latch{};

std::uint16_t WeaponDef(C_CSWeaponBase* w)
{
	if (!w) return 0;
	std::uint16_t d = 0;
	__try { d = w->m_iItemDefinitionIndex(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { d = 0; }
	return d;
}

std::uint64_t NowMs()
{
	return static_cast<std::uint64_t>(GetTickCount64());
}

// ── Live VData accuracy (dump CCSWeaponBaseVData / IDA GetInaccuracy path) ──
// No per-def hardcodes. Cycle + stand/fire/jump/spread from weapon VData.

struct VDataAcc {
	float cycle = 0.1f;
	float spread = 0.f;
	float stand = 0.f;
	float fire = 0.f;
	float jump = 0.f;
	float move = 0.f;
	float recoverStand = 0.f;
	bool ok = false;
};

bool ReadVDataAcc(C_CSWeaponBase* weapon, VDataAcc& out)
{
	out = {};
	if (!weapon || !Mem::ValidEntity(weapon))
		return false;
	CCSWeaponBaseVData* vd = nullptr;
	int mode = 0;
	__try {
		vd = weapon->Data();
		mode = weapon->m_weaponMode();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!vd)
		return false;
	if (mode != 0 && mode != 1)
		mode = 0;

	__try {
		out.cycle = (mode == 1) ? vd->m_flCycleTimeSecondary() : vd->m_flCycleTimePrimary();
		out.spread = (mode == 1) ? vd->m_flSpread1() : vd->m_flSpread0();
		out.stand = (mode == 1) ? vd->m_flInaccuracyStand1() : vd->m_flInaccuracyStand0();
		out.fire = (mode == 1) ? vd->m_flInaccuracyFire1() : vd->m_flInaccuracyFire0();
		out.jump = (mode == 1) ? vd->m_flInaccuracyJump1() : vd->m_flInaccuracyJump0();
		out.move = (mode == 1) ? vd->m_flInaccuracyMove1() : vd->m_flInaccuracyMove0();
		out.recoverStand = vd->m_flRecoveryTimeStand();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}

	auto fin = [](float& v, float fb) {
		if (!std::isfinite(v) || v < 0.f) v = fb;
	};
	fin(out.cycle, 0.1f);
	fin(out.spread, 0.f);
	fin(out.stand, 0.f);
	fin(out.fire, 0.f);
	fin(out.jump, 0.f);
	fin(out.move, 0.f);
	fin(out.recoverStand, 0.f);
	// Clamp cycle to sane range (AWP ~1.45, SMG ~0.07)
	out.cycle = std::clamp(out.cycle, 0.05f, 2.5f);
	out.ok = true;
	return true;
}

// Soft re-arm after NoteSeedFired. Deagle: engine cycle is hard gate — soft gap
// only ~1 tick so AF/TR feel instant (old 50–120ms + bloom = late / no air).
std::uint64_t SeedMinGapMs(C_CSWeaponBase* weapon)
{
	if (AimCommon::IsHeavyPistol(weapon))
		return 8ull; // ~half tick anti double-frame only
	VDataAcc a{};
	if (!ReadVDataAcc(weapon, a) || !a.ok)
		return 4ull;
	float frac = 0.05f;
	if (AimCommon::IsSniperWeapon(weapon))
		frac = 0.10f;
	else if (AimCommon::IsSemiWeapon(weapon))
		frac = 0.08f;
	else if (AimCommon::IsSprayAutoWeapon(weapon))
		frac = 0.03f;
	float sec = a.cycle * frac;
	const auto ms = static_cast<std::uint64_t>(sec * 1000.f + 0.5f);
	return std::clamp(ms, 2ull, 48ull);
}

// Deagle: never bloom-block. Seed Solve already uses live inac+spr (incl jump).
// Old ground bloom gate still delayed re-tab after fire.
bool BloomSettledForSeed(C_CSWeaponBase* /*weapon*/, C_CSPlayerPawn* /*local*/)
{
	return true;
}

} // namespace

// ── Public ───────────────────────────────────────────────────────────

bool Init()
{
	if (!HitChance::Init())
		return false;
	return HitChance::SpreadSeedReady();
}

bool Ready()
{
	return HitChance::SpreadSeedReady();
}

// Solve pellet for one aim point / prefer HB. Shared bloom+punch from outer.
// Cost budget: natural → roll → closed-form first (cheap hits most frames).
// Wide bin/spiral only if needed; local ran1 only until AcceptView.
bool SolveForAim(
	const Vector_t& eye,
	const QAngle_t& wishIn,
	const Vector_t& aimPt,
	int preferHitbox,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float inac,
	float spr,
	const QAngle_t* punchPtr,
	const bool* enabledHitboxes,
	const TryCtx& ctx,
	Shot& out)
{
	if (!Bones::IsValidPos(aimPt))
		return false;

	QAngle_t wish = wishIn;
	wish.z = 0.f;
	wish.Normalize();
	if (!wish.IsValid()) {
		if (!CalcAngles(eye, aimPt, wish))
			return false;
	}

	// Geometric wish toward this aim, punch-sub
	{
		QAngle_t toAim{};
		if (CalcAngles(eye, aimPt, toAim)) {
			if (punchPtr && punchPtr->IsValid()) {
				toAim.x -= punchPtr->x;
				toAim.y -= punchPtr->y;
			}
			toAim.z = 0.f;
			toAim.x = std::clamp(toAim.x, -89.f, 89.f);
			toAim.Normalize();
			if (toAim.IsValid())
				wish = toAim;
		}
	}

	const float maxDelta = MaxDeltaDeg(inac, spr, weapon, local);

	Vector_t wantDir{};
	{
		QAngle_t toAim{};
		if (!CalcAngles(eye, aimPt, toAim))
			return false;
		toAim.ToDirections(&wantDir, nullptr, nullptr);
		if (!NormalizeDir(wantDir))
			return false;
	}

	out.seedFrac = tickFrac;

	// Shared wish pellet (one BulletDir) — reused by roll + closed-form.
	Vector_t wishDir{};
	float wishSx = 0.f, wishSy = 0.f;
	const bool haveWishPellet = BulletDir(
		wish, seedTick, weapon, inac, spr, punchPtr, wishDir, &wishSx, &wishSy)
		&& std::isfinite(wishSx) && std::isfinite(wishSy);

	// 0) Natural seed FIRST — zero rewrite when crosshair already good.
	if (AcceptView(eye, wish, seedTick, tickFrac, weapon, local, target, inac, spr,
			punchPtr, preferHitbox, enabledHitboxes, maxDelta, wish, ctx, out))
		return true;

	// 1) Roll-trick — reuses wishSx/Sy (no second BulletDir)
	if (haveWishPellet) {
		QAngle_t rollView{};
		if (RollTrickView(wish, punchPtr, seedTick, wishSx, wishSy, rollView)) {
			if (AcceptView(eye, rollView, seedTick, tickFrac, weapon, local, target,
					inac, spr, punchPtr, preferHitbox, enabledHitboxes,
					maxDelta, wish, ctx, out))
				return true;
		}
	}

	// 2) Closed-form invert + roll (reuse wish pellet; 1 refine iter max)
	if (haveWishPellet) {
		QAngle_t punchedWish = wish;
		if (punchPtr) {
			punchedWish.x += punchPtr->x;
			punchedWish.y += punchPtr->y;
		}
		punchedWish.z = 0.f;
		punchedWish.Normalize();

		QAngle_t punchedSolve{};
		if (SolveViewForDir(wantDir, wishSx, wishSy, punchedWish, punchedSolve)) {
			QAngle_t viewSolve = punchedSolve;
			if (punchPtr) {
				viewSolve.x -= punchPtr->x;
				viewSolve.y -= punchPtr->y;
			}
			viewSolve.z = 0.f;
			viewSolve.x = std::clamp(viewSolve.x, -89.f, 89.f);
			viewSolve.Normalize();

			{
				QAngle_t rv{};
				if (RollTrickView(viewSolve, punchPtr, seedTick, wishSx, wishSy, rv)) {
					if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
							inac, spr, punchPtr, preferHitbox, enabledHitboxes,
							maxDelta, wish, ctx, out))
						return true;
				}
			}

			if (AcceptView(eye, viewSolve, seedTick, tickFrac, weapon, local, target,
					inac, spr, punchPtr, preferHitbox, enabledHitboxes,
					maxDelta, wish, ctx, out))
				return true;

			// One refine pass (old: 3) — second rarely gains after seed-bin lock.
			{
				Vector_t d{};
				float nsx = 0.f, nsy = 0.f;
				if (BulletDir(viewSolve, seedTick, weapon, inac, spr, punchPtr,
						d, &nsx, &nsy)) {
					QAngle_t punched = viewSolve;
					if (punchPtr) {
						punched.x += punchPtr->x;
						punched.y += punchPtr->y;
					}
					punched.z = 0.f;
					punched.Normalize();
					QAngle_t nextP{};
					if (SolveViewForDir(wantDir, nsx, nsy, punched, nextP)) {
						QAngle_t next = nextP;
						if (punchPtr) {
							next.x -= punchPtr->x;
							next.y -= punchPtr->y;
						}
						next.z = 0.f;
						next.x = std::clamp(next.x, -89.f, 89.f);
						next.Normalize();
						QAngle_t wishPy = wish; wishPy.z = 0.f;
						QAngle_t nextPy = next; nextPy.z = 0.f;
						if (next.IsValid() && AngDelta(wishPy, nextPy) <= maxDelta) {
							QAngle_t rv{};
							if (RollTrickView(next, punchPtr, seedTick, nsx, nsy, rv)) {
								if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local,
										target, inac, spr, punchPtr, preferHitbox,
										enabledHitboxes, maxDelta, wish, ctx, out))
									return true;
							}
							if (AcceptView(eye, next, seedTick, tickFrac, weapon, local,
									target, inac, spr, punchPtr, preferHitbox,
									enabledHitboxes, maxDelta, wish, ctx, out))
								return true;
						}
					}
				}
			}
		}
	}
	if (out.ok)
		return true;

	// Unscoped sniper air: skip bins+spiral (model flick via FillGunFireData).
	bool skipWideSearch = false;
	if (ctx.sniper && local) {
		bool scoped = false;
		__try {
			scoped = AimCommon::IsLocalScoped(local, weapon);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		if (!scoped && ctx.localAir)
			skipWideSearch = true;
	}

	// 3) Half-deg bins — 4×4 (was 8×8). Roll only after local hit probe fails.
	if (!skipWideSearch)
	{
		static constexpr float kStep[] = {
			-1.5f, -0.5f, 0.5f, 1.5f
		};
		for (float dx : kStep) {
			for (float dy : kStep) {
				QAngle_t c = wish;
				c.x += dx;
				c.y += dy;
				c.z = 0.f;
				c.x = std::clamp(c.x, -89.f, 89.f);
				c.Normalize();
				QAngle_t wishPy = wish; wishPy.z = 0.f;
				if (AngDelta(wishPy, c) > maxDelta)
					continue;
				// Local-only probe first → game verify without re-ran1
				Shot probe{};
				if (TryView(eye, c, seedTick, tickFrac, weapon, local, target, inac, spr,
						punchPtr, preferHitbox, enabledHitboxes, maxDelta, wish, ctx,
						probe, /*verifyGame=*/false)) {
					if (AcceptProbe(eye, seedTick, weapon, target, inac, spr, punchPtr,
							preferHitbox, enabledHitboxes, ctx, probe, out))
						return true;
					continue;
				}
				// Roll only when plain bin missed
				Vector_t d{};
				float sx = 0.f, sy = 0.f;
				if (BulletDir(c, seedTick, weapon, inac, spr, punchPtr, d, &sx, &sy)) {
					QAngle_t rv{};
					if (RollTrickView(c, punchPtr, seedTick, sx, sy, rv)) {
						if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
								inac, spr, punchPtr, preferHitbox, enabledHitboxes,
								maxDelta, wish, ctx, out))
							return true;
					}
				}
			}
		}
	}
	if (out.ok)
		return true;

	// 4) Cone spiral — 20/28 (was 32/56). Roll only on local miss.
	if (!skipWideSearch)
	{
		const float bloom = (std::isfinite(inac) && std::isfinite(spr)) ? (inac + spr) : 0.f;
		// Air/heavy need denser cone; still far under old 56.
		int budget = 20;
		if (bloom > 0.15f || ctx.localAir)
			budget = 28;
		if (ctx.heavy && ctx.localAir)
			budget = 32;
		constexpr float kGolden = 2.399963229728653f;
		for (int i = 0; i < budget; ++i) {
			const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(budget);
			const float r = maxDelta * std::sqrt(t);
			const float th = static_cast<float>(i) * kGolden;
			QAngle_t c = wish;
			c.x += r * std::cos(th);
			c.y += r * std::sin(th);
			c.z = 0.f;
			c.x = std::clamp(c.x, -89.f, 89.f);
			c.Normalize();
			QAngle_t wishPy = wish; wishPy.z = 0.f;
			if (AngDelta(wishPy, c) > maxDelta + 0.02f)
				continue;
			Shot probe{};
			if (TryView(eye, c, seedTick, tickFrac, weapon, local, target, inac, spr,
					punchPtr, preferHitbox, enabledHitboxes, maxDelta, wish, ctx,
					probe, /*verifyGame=*/false)) {
				if (AcceptProbe(eye, seedTick, weapon, target, inac, spr, punchPtr,
						preferHitbox, enabledHitboxes, ctx, probe, out))
					return true;
				continue;
			}
			Vector_t d{};
			float sx = 0.f, sy = 0.f;
			if (BulletDir(c, seedTick, weapon, inac, spr, punchPtr, d, &sx, &sy)) {
				QAngle_t rv{};
				if (RollTrickView(c, punchPtr, seedTick, sx, sy, rv)) {
					if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
							inac, spr, punchPtr, preferHitbox, enabledHitboxes,
							maxDelta, wish, ctx, out))
						return true;
				}
			}
		}
	}

	// 5) Pure geometric + roll (fallback if wish rewrite missed aim)
	{
		QAngle_t pure{};
		if (CalcAngles(eye, aimPt, pure)) {
			if (punchPtr) {
				pure.x -= punchPtr->x;
				pure.y -= punchPtr->y;
			}
			pure.z = 0.f;
			pure.x = std::clamp(pure.x, -89.f, 89.f);
			pure.Normalize();
			if (pure.IsValid()) {
				if (AcceptView(eye, pure, seedTick, tickFrac, weapon, local, target,
						inac, spr, punchPtr, preferHitbox, enabledHitboxes,
						maxDelta, pure, ctx, out))
					return true;
				Vector_t d{};
				float sx = 0.f, sy = 0.f;
				if (BulletDir(pure, seedTick, weapon, inac, spr, punchPtr, d, &sx, &sy)) {
					QAngle_t rv{};
					if (RollTrickView(pure, punchPtr, seedTick, sx, sy, rv)) {
						if (AcceptView(eye, rv, seedTick, tickFrac, weapon, local, target,
								inac, spr, punchPtr, preferHitbox, enabledHitboxes,
								maxDelta, pure, ctx, out))
							return true;
					}
				}
			}
		}
	}

	return out.ok;
}

bool Solve(
	const Vector_t& eye,
	const QAngle_t& wishView,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int preferHitbox,
	const bool* enabledHitboxes,
	Shot& out)
{
	out = Shot{};
	if (!Ready() || seedTick <= 0)
		return false;
	if (!Mem::ValidEntity(weapon) || !Mem::ValidEntity(local) || !Mem::ValidEntity(target))
		return false;
	if (!Bones::IsValidPos(eye))
		return false;
	if (preferHitbox < 0 || preferHitbox >= Config::HB_COUNT)
		preferHitbox = Config::HB_HEAD;

	float inac = 0.f, spr = 0.f;
	if (!HitChance::ReadCurrentBloom(weapon, local, inac, spr))
		return false;

	// One punch for whole Solve (all HBs share seed stamp)
	QAngle_t punch{};
	const QAngle_t* punchPtr = nullptr;
	bool punchOk = HitChance::ReadSeedFirePunch(local, weapon, seedTick, tickFrac, punch)
		&& punch.IsValid();
	if (!punchOk)
		punchOk = HitChance::ReadAimPunch(local, punch) && punch.IsValid();
	if (punchOk) {
		punch.z = 0.f;
		punchPtr = &punch;
	}

	// Air/heavy/sniper flags once — reused by every TryView.
	TryCtx ctx{};
	BuildTryCtx(weapon, local, inac, spr, ctx);

	// HB order: prefer first, then core, then limbs (menu filter).
	static constexpr int kOrder[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS,
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};
	int tryHb[Config::HB_COUNT]{};
	int nTry = 0;
	auto pushHb = [&](int hb) {
		if (hb < 0 || hb >= Config::HB_COUNT) return;
		if (enabledHitboxes && !enabledHitboxes[hb]) return;
		for (int i = 0; i < nTry; ++i)
			if (tryHb[i] == hb) return;
		tryHb[nTry++] = hb;
	};
	pushHb(preferHitbox);
	for (int hb : kOrder)
		pushHb(hb);
	if (nTry == 0) {
		for (int hb : kOrder)
			tryHb[nTry++] = hb;
	}

	QAngle_t wishBase = wishView;
	wishBase.z = 0.f;
	wishBase.Normalize();

	// Prefer HB first (often hits) — full spiral only on that HB.
	// Other HBs: natural+roll+closed-form only (no bin/spiral) unless prefer failed.
	// Still covers multi-HB; avoids N×56 spiral FPS death.
	for (int i = 0; i < nTry; ++i) {
		const int hb = tryHb[i];
		Vector_t aimPt{};
		if (!Bones::GetHitboxPoint(target, hb, aimPt) || !Bones::IsValidPos(aimPt))
			aimPt = SeedAimPoint(target, hb, eye);
		else
			aimPt = SeedAimPoint(target, hb, aimPt);
		if (!Bones::IsValidPos(aimPt))
			continue;

		Shot cand{};
		if (SolveForAim(eye, wishBase, aimPt, hb, seedTick, tickFrac, weapon, local,
				target, inac, spr, punchPtr, enabledHitboxes, ctx, cand)
			&& cand.ok && cand.fireAngles.IsValid()) {
			out = cand;
			return true;
		}
		// Prefer missed with full search — secondary HBs still get full path
		// (RayHitsAny inside Accept already multi-HB; aim point is the difference).
	}

	return out.ok;
}

bool SeedCycleAllowsFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local)
{
	if (!weapon)
		return true;
	const std::uint16_t def = WeaponDef(weapon);
	const std::uint64_t now = NowMs();
	const std::uint64_t gap = SeedMinGapMs(weapon);

	// First shot: latch empty → always ok.
	// Soft gap only while engine also not ready. Engine ready → drop latch
	// immediately (deagle air + parked react). No bloom gate.
	if (def != 0 && g_latch.def == def && g_latch.fireMs != 0
		&& (now - g_latch.fireMs) < gap) {
		if (local && AimCommon::CanWeaponFire(weapon, local)) {
			g_latch.fireMs = 0;
			return true;
		}
		return false;
	}

	// Heavy pistol (deagle/R8) bloom gate: after fire, don't re-fire until bloom
	// has recovered below threshold. CanWeaponFire only checks fire-rate tick,
	// not accuracy. Air/run deagle bloom is massive (0.4-0.8) → Solve "finds"
	// solution but spread cone is huge → pellet misses → spam re-fire → overspray.
	// IDA GetInaccuracy: jump/move/stand/fire components stack. After fire,
	// fire component decays over recoveryTimeStand. Gate until bloom < threshold.
	if (def != 0 && g_latch.def == def && g_latch.fireMs != 0
		&& AimCommon::IsHeavyPistol(weapon) && local) {
		// Time since last fire
		const float sinceFireMs = static_cast<float>(now - g_latch.fireMs);
		// VData recovery time (deagle ~0.4s stand, ~0.6s crouch)
		VDataAcc acc{};
		float recoveryMs = 400.f; // default deagle-ish
		if (ReadVDataAcc(weapon, acc) && acc.ok && acc.recoverStand > 0.01f)
			recoveryMs = acc.recoverStand * 1000.f;
		// Bloom recovery fraction (0=just fired, 1=fully recovered)
		const float recoverFrac = std::clamp(sinceFireMs / recoveryMs, 0.f, 1.f);
		// Read live bloom
		float inac = 0.f, spr = 0.f;
		if (HitChance::ReadCurrentBloom(weapon, local, inac, spr)) {
			const float bloom = (std::isfinite(inac) ? inac : 0.f)
				+ (std::isfinite(spr) ? spr : 0.f);
			// Air/running: bloom threshold higher (jump inac stacks)
			bool air = false;
			__try { air = (local->m_fFlags() & FL_ONGROUND) == 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { air = false; }
			// Gate: bloom must be below threshold OR enough time passed
			// Ground: bloom < 0.12 (stand inac ~0.04 + small fire residual)
			// Air: bloom < 0.30 (jump inac ~0.25 + fire residual)
			// Hard timeout: always allow after 85% recovery (never soft-lock)
			const float bloomGate = air ? 0.30f : 0.12f;
			const float hardTimeoutMs = recoveryMs * 0.85f;
			if (bloom > bloomGate && sinceFireMs < hardTimeoutMs) {
				// Still too inaccurate — block re-fire
				return false;
			}
		}
		// Bloom recovered or hard timeout — clear latch, allow fire
		g_latch.fireMs = 0;
	}

	return true;
}

bool SeedCycleAllowsFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local, const SeedGate&)
{
	return SeedCycleAllowsFire(weapon, local);
}

void NoteSeedFired(C_CSWeaponBase* weapon, C_CSPlayerPawn* /*local*/)
{
	if (!weapon)
		return;
	g_latch.def = WeaponDef(weapon);
	g_latch.fireMs = NowMs();
}

Vector_t SeedAimPoint(C_CSPlayerPawn* target, int hitbox, const Vector_t& fallback)
{
	Vector_t pt = fallback;
	if (target && hitbox >= 0 && hitbox < Config::HB_COUNT) {
		Bones::Capsule cap{};
		if (Bones::GetHitboxCapsule(target, hitbox, cap) && cap.ok
			&& Bones::IsValidPos(cap.center))
			pt = cap.center;
		else {
			Vector_t c{};
			if (Bones::GetHitboxPoint(target, hitbox, c) && Bones::IsValidPos(c))
				pt = c;
		}
	}
	if (!target || !Bones::IsValidPos(pt))
		return pt;

	Vector_t vel{};
	__try { vel = target->m_vecAbsVelocity(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return pt; }

	const float sp = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
	if (!std::isfinite(sp) || sp < 30.f)
		return pt;

	// Short lead only — long lead + seed rewrite = overshoot miss on strafe.
	const bool head = (hitbox == Config::HB_HEAD || hitbox == Config::HB_NECK);
	const float leadT = head ? 0.004f : 0.006f;
	Vector_t lead{
		pt.x + vel.x * leadT,
		pt.y + vel.y * leadT,
		pt.z + vel.z * leadT
	};
	const float dx = lead.x - pt.x;
	const float dy = lead.y - pt.y;
	const float h = std::sqrt(dx * dx + dy * dy);
	const float maxH = head ? 3.f : 5.f;
	if (h > maxH && h > 1e-4f) {
		const float s = maxH / h;
		lead.x = pt.x + dx * s;
		lead.y = pt.y + dy * s;
	}
	const float dz = lead.z - pt.z;
	const float maxZ = head ? 2.f : 3.f;
	if (std::fabs(dz) > maxZ)
		lead.z = pt.z + (dz > 0.f ? maxZ : -maxZ);
	return Bones::IsValidPos(lead) ? lead : pt;
}

// Hard mindmg on the REAL pellet hitbox only.
// allowPen must match PELLET wall state (caller re-checks after Solve).
// Never rewrite HB (old feet→head under mindmg 100).
bool SeedPassesDamage(
	const Vector_t& eye,
	Vector_t& inOutPoint,
	int& inOutHb,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamageVis,
	float minDamageAw)
{
	if (!target || !Mem::ValidEntity(target) || !Bones::IsValidPos(eye)
		|| !Bones::IsValidPos(inOutPoint) || inOutHb < 0)
		return false;

	auto needFor = [&](float cfgNeed) -> float {
		if (cfgNeed <= 0.f)
			return 0.f;
		float need = cfgNeed;
		const int hp = target->m_iHealth();
		if (hp > 0 && static_cast<float>(hp) < need)
			need = static_cast<float>(hp);
		return need;
	};

	auto dmgOk = [&](const Vector_t& pt, int hb) -> bool {
		if (!Bones::IsValidPos(pt) || hb < 0)
			return false;
		if (allowPen) {
			const AutoWall::Result aw = AutoWall::Fire(
				eye, pt, hb, weapon, local, target, true);
			// Wall path: must real-pen + hit. mindmg_aw=0 → any pen dmg >= 1.
			// Free-LOS with allowPen=true (stale flag) → !penetrated → reject.
			if (!aw.hit || !aw.penetrated || aw.damage < 1.f)
				return false;
			const float need = needFor(minDamageAw);
			if (need > 0.f && aw.damage + 0.01f < need)
				return false;
			return true;
		}
		// Visible: allowPen=false → wall returns !hit (fail closed).
		const AutoWall::Result vis = AutoWall::Fire(
			eye, pt, hb, weapon, local, target, false);
		if (!vis.hit || vis.damage < 1.f)
			return false;
		const float need = needFor(minDamageVis);
		if (need > 0.f && vis.damage + 0.01f < need)
			return false;
		return true;
	};

	// 1) Exact Solve pellet point + HB
	if (dmgOk(inOutPoint, inOutHb))
		return true;

	// 2) Same HB capsule center only (rim under-estimate) — NOT other HBs
	const Vector_t center = SeedAimPoint(target, inOutHb, inOutPoint);
	if (Bones::IsValidPos(center) && center.Distance(inOutPoint) > 0.5f
		&& dmgOk(center, inOutHb)) {
		inOutPoint = center;
		return true;
	}

	return false;
}

bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX,
	float* outSpreadY,
	unsigned seedAdd,
	float tickFrac)
{
	return HitChance::GetBulletDirection(
		fireAngles, seedTick, weapon, local, outDir, outSpreadX, outSpreadY,
		seedAdd, tickFrac);
}

bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint,
	float tickFrac)
{
	return HitChance::ExactShotHits(
		eye, fireAngles, seedTick, weapon, local, target, hitbox, outPoint, tickFrac);
}

bool ExactShotHitsAny(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	const bool* enabledHitboxes,
	int* outHitbox,
	Vector_t* outPoint,
	float tickFrac)
{
	return HitChance::ExactShotHitsAny(
		eye, fireAngles, seedTick, weapon, local, target, enabledHitboxes,
		outHitbox, outPoint, tickFrac);
}

std::uint32_t ComputeSeed(const QAngle_t& angles, int attackTick)
{
	return HitChance::ComputeSeed(angles, attackTick);
}

// ── Legacy shims (no more WAIT gates) ────────────────────────────────

SeedGate GetSeedGate(C_CSWeaponBase*, bool)
{
	SeedGate g{};
	g.maxCompDelta = 6.f;
	g.maxDeltaCap = 80.f;
	g.maxRad = 0.55f;
	g.maxLat = 900.f;
	g.maxAimDist = 120.f;
	g.maxBloom = 0.99f;
	g.minScore = 1.05f;
	g.allowAnyHb = true;
	return g;
}

float SeedAimDistLimit(const SeedGate&, float, bool sniperHot)
{
	return sniperHot ? 48.f : 120.f;
}

bool BloomAllowsSeedFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local, const SeedGate&)
{
	return BloomSettledForSeed(weapon, local);
}

float ComputeSeedMaxDelta(
	C_CSWeaponBase* weapon,
	const SeedGate& gate,
	float inac,
	float spr,
	bool)
{
	const float d = MaxDeltaDeg(inac, spr, weapon, nullptr);
	return std::clamp(d, 0.5f, gate.maxDeltaCap > 1.f ? gate.maxDeltaCap : 80.f);
}

void GetWildGateLimits(
	const SeedGate& gate,
	bool,
	float bloom,
	float& outRad,
	float& outLat,
	float& outAim,
	float& outAng)
{
	const float b = (std::isfinite(bloom) && bloom > 0.f) ? bloom : 0.f;
	outRad = (std::max)(gate.maxRad, b * 1.55f + 0.04f);
	outLat = 900.f;
	outAim = 120.f;
	outAng = gate.maxDeltaCap > 1.f ? gate.maxDeltaCap : 80.f;
}

bool FindBestShot(
	const Vector_t& eye,
	const QAngle_t& wishAngles,
	const Vector_t& /*aimPoint*/,
	int preferHitbox,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float /*maxDeltaDeg*/,
	BestShot& out,
	float tickFrac)
{
	out = BestShot{};
	// All hitboxes on for legacy path
	bool all[Config::HB_COUNT];
	for (int i = 0; i < Config::HB_COUNT; ++i)
		all[i] = true;

	Shot s{};
	if (!Solve(eye, wishAngles, seedTick, tickFrac, weapon, local, target,
			preferHitbox, all, s) || !s.ok)
		return false;

	out.angles = s.fireAngles;
	out.point = s.hitPoint;
	out.hitbox = s.hitbox;
	out.seedTick = s.seedTick;
	out.score = 2.0f;
	out.preferHit = (s.hitbox == preferHitbox);
	out.sameSeedBin = true;
	return true;
}

bool FindNoSpreadAngles(
	const QAngle_t& wishAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	QAngle_t& outAngles,
	int* outSeedTick)
{
	return HitChance::FindNoSpreadAngles(
		wishAngles, seedTick, weapon, local, outAngles, outSeedTick);
}

namespace SeedDbg {

const char* HbName(int hb)
{
	switch (hb) {
	case Config::HB_HEAD: return "head";
	case Config::HB_NECK: return "neck";
	case Config::HB_CHEST: return "chest";
	case Config::HB_STOMACH: return "stomach";
	case Config::HB_PELVIS: return "pelvis";
	case Config::HB_ARMS: return "arms";
	case Config::HB_LEGS: return "legs";
	case Config::HB_FEET: return "feet";
	default: return "?";
	}
}

const char* WpnTag(int def)
{
	switch (def) {
	case 1: return "deagle";
	case 7: return "ak47";
	case 9: return "awp";
	case 16: return "m4a4";
	case 40: return "ssg08";
	case 60: return "m4a1";
	case 61: return "usp";
	case 64: return "r8";
	default: return "gun";
	}
}

void Log(const Snap& s, unsigned intervalMs)
{
#ifdef _DEBUG
	char key[72]{};
	std::snprintf(key, sizeof(key), "seed.%s.%s.%s",
		s.who ? s.who : "?",
		s.event ? s.event : "?",
		s.reason && s.reason[0] ? s.reason : "-");

	static struct {
		char key[72];
		DWORD last;
	} s_rate[48]{};
	const DWORD now = GetTickCount();
	const DWORD iv = intervalMs ? intervalMs : 150u;
	bool allow = true;
	int freeSlot = -1;
	for (int i = 0; i < 48; ++i) {
		if (s_rate[i].key[0] == '\0') {
			if (freeSlot < 0) freeSlot = i;
			continue;
		}
		if (std::strncmp(s_rate[i].key, key, 71) == 0) {
			if (now - s_rate[i].last < iv)
				allow = false;
			else
				s_rate[i].last = now;
			freeSlot = -2;
			break;
		}
	}
	if (freeSlot >= 0 && allow) {
		std::snprintf(s_rate[freeSlot].key, 72, "%s", key);
		s_rate[freeSlot].last = now;
	}
	if (!allow)
		return;

	int def = s.def;
	if (!def && s.weapon) {
		__try { def = s.weapon->m_iItemDefinitionIndex(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { def = 0; }
	}

	Con::Info("[seed] %s %s %s  %s  path=%s",
		s.who ? s.who : "?",
		s.event ? s.event : "?",
		s.reason && s.reason[0] ? s.reason : "-",
		WpnTag(def),
		s.path && s.path[0] ? s.path : "-");
	if (s.event && (s.event[0] == 'F' || s.event[0] == 'T')) {
		Con::Detail("angles", "wish=(%.2f, %.2f) fire=(%.2f, %.2f) dAng=%.2f",
			s.wish.x, s.wish.y, s.fire.x, s.fire.y, s.dAng);
		Con::Detail("hit", "prefer=%s hit=%s dAim=%.1f sx=%.4f sy=%.4f",
			HbName(s.preferHb), HbName(s.hitHb), s.dAim, s.sx, s.sy);
	}
#else
	(void)s;
	(void)intervalMs;
#endif
}

} // namespace SeedDbg

} // namespace NoSpread
