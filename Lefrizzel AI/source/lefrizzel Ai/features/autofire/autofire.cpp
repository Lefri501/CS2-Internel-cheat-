#include "autofire.h"
#include "../aim/aim_common.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../nospread/nospread.h"
#include "../autowall/autowall.h"
#include "../prediction/prediction.h"

#include "../../utils/memory/memsafe/memsafe.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace Autofire {

std::uint32_t g_afLocked = 0;
std::uint32_t g_afPending = 0;
std::uint64_t g_afLockMs = 0;
std::uint64_t g_afSwitchReadyMs = 0;
std::uint64_t g_afFirstShotReadyMs = 0;
bool g_afFirstArmed = false;
bool g_afFirstDone = false;
bool g_afBlockFirst = false;
int g_afGrace = 0;
bool g_autofireWantShoot = false;
bool g_autofireWantStop = false;
bool g_autofireWantScope = false;
QAngle_t g_autofireSilentAngle{};
Vector_t g_autofireSilentEye{};
bool g_autofireSilentValid = false;
int g_autofireFireHist = -1;
QAngle_t g_afLastAimAngle{};
Vector_t g_afLastAimPoint{};
int g_afLastHb = Config::HB_HEAD;
bool g_afLastAimValid = false;

// Multipoint only for head / chest / stomach / pelvis. Else center.
static bool IsAfMultipointHitbox(int hb) {
	return hb == Config::HB_HEAD || hb == Config::HB_CHEST
		|| hb == Config::HB_STOMACH || hb == Config::HB_PELVIS;
}

// 0 = center only (not in MP list, or unsupported box)
float AutofireMultipointScale(int hb) {
	if (!IsAfMultipointHitbox(hb))
		return 0.f;
	if (!Config::autofire_multipoint[hb])
		return 0.f;
	return std::clamp(Config::autofire_multipoint_scale[hb], 0.f, 1.f);
}

// Soft LOS gate only. Wallbang + mindmg enforced in scan / shoot paths
// via AutoWall::Fire (must hit target entity + pen dmg).
// AF Autowall checkbox AND global AW keybind must both be active.
static bool AfAutowallActive() {
	return Config::autofire_autowall && keybind.isActive(Config::autowall);
}

static bool AfAimAllowed(const Vector_t& eye, const Vector_t& point,
	C_CSPlayerPawn* lp, C_CSPlayerPawn* pawn) {
	if (!Trace::Ready())
		return false;

	const bool behindWall = AimCommon::IsBehindWall(
		eye, point, lp, pawn, Trace::kMaskShot);
	const bool awOn = AfAutowallActive();

	if (!awOn) {
		// No pen: wall = reject
		if (behindWall)
			return false;
		if (Config::autofire_vis_check
			&& !Trace::IsVisible(eye, point, lp, pawn, Trace::kMaskVis))
			return false;
	} else if (behindWall) {
		// AW on + wall: allow scan — AutoWall mindmg gate kills non-pen
		// (do NOT treat as visible)
	} else if (Config::autofire_vis_check) {
		if (!Trace::IsVisible(eye, point, lp, pawn, Trace::kMaskVis))
			return false;
	}

	if (Config::autofire_smoke_check && AimCommon::LineBlockedBySmoke(eye, point))
		return false;
	return true;
}

int AimHbPriority(int hb) {
	if (hb < 0 || hb >= Config::HB_COUNT)
		return 99;
	return hb;
}

// Torso only (not head/neck/limbs) — body-if-lethal + prefer-body.
bool IsBodyHitbox(int hb) {
	return hb == Config::HB_CHEST
		|| hb == Config::HB_STOMACH
		|| hb == Config::HB_PELVIS;
}

bool IsHeadHitbox(int hb) {
	return hb == Config::HB_HEAD || hb == Config::HB_NECK;
}

void ResetAutofire() {
	g_afLocked = 0;
	g_afPending = 0;
	g_afLockMs = 0;
	g_afSwitchReadyMs = 0;
	g_afFirstShotReadyMs = 0;
	g_afFirstArmed = false;
	g_afFirstDone = false;
	g_afBlockFirst = false;
	g_afGrace = 0;
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireWantScope = false;
	g_autofireSilentValid = false;
	g_autofireSilentEye = Vector_t{ 0.f, 0.f, 0.f };
	g_autofireFireHist = -1;
	g_afLastAimValid = false;
	AimCommon::ResetSmoothState();
}

void BeginAutofireEngagement(std::uint32_t handle, std::uint64_t now) {
	g_afLocked = handle;
	g_afPending = 0;
	g_afLockMs = now;
	g_afSwitchReadyMs = 0;
	g_afFirstShotReadyMs = 0;
	g_afFirstArmed = false;
	g_afFirstDone = false;
	g_afBlockFirst = false;
	g_afGrace = AimCommon::kLockGraceFrames;
	// New target: don't hold previous enemy view through reaction; clean smooth clock
	g_afLastAimValid = false;
	AimCommon::ResetSmoothState();
}

bool FindClosestAutofireBone(
	const Vector_t& eye,
	const QAngle_t& crosshair,
	C_CSPlayerPawn* pawn,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outPoint,
	int& outHb,
	int preferHb = -1)
{
	if (!pawn || !Bones::IsValidPos(eye) || !crosshair.IsValid())
		return false;

	float bloom = -1.f;
	if (Config::autofire_multipoint_dynamic && weapon)
		bloom = AimCommon::LiveMultipointBloom(weapon, local);

	const bool targetAir = (pawn->m_fFlags() & FL_ONGROUND) == 0;

	// Estimate range once — skip dyn bloom crush point-blank (same as scan path)
	float estDist = -1.f;
	{
		Vector_t o{};
		if (Bones::GetOrigin(pawn, o) && Bones::IsValidPos(o))
			estDist = eye.Distance(o);
	}
	if (estDist > 0.f && estDist < 160.f)
		bloom = -1.f;

	float bestFov = 1.0e9f;
	bool found = false;

	for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
		if (!Config::autofire_hitboxes[hb])
			continue;
		if (preferHb >= 0 && hb != preferHb)
			continue;
		const float mpScale = AutofireMultipointScale(hb);

		Vector_t points[9]{};
		const int nPts = Bones::CollectHitboxMultipoints(
			pawn, hb, mpScale, points, 9, &eye, bloom, targetAir);
		if (nPts <= 0 || nPts > 9)
			continue;

		for (int p = 0; p < nPts; ++p) {
			if (!Bones::IsValidPos(points[p]))
				continue;
			QAngle_t ang{};
			if (!AimCommon::CalcAngles(eye, points[p], ang))
				continue;
			const float fov = AimCommon::GetFov(crosshair, ang);
			if (!Mem::Finite(fov) || fov >= bestFov)
				continue;
			bestFov = fov;
			outPoint = points[p];
			outHb = hb;
			found = true;
		}
	}
	return found;
}

bool FireRayHitsTarget(
	const Vector_t& eye,
	const QAngle_t& ang,
	C_CSPlayerPawn* target,
	int preferHb,
	bool sniper,
	float eyeDist = -1.f,
	bool seedLoose = false)
{
	if (!target || !Mem::ValidEntity(target) || !ang.IsValid() || !Bones::IsValidPos(eye))
		return false;

	Vector_t fwd{};
	ang.ToDirections(&fwd, nullptr, nullptr);
	const float fl = fwd.Length();
	if (fl < 1e-4f || !Mem::Finite(fl))
		return false;
	fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;

	// Mid+ bias — rim hits = first-shot miss while smooth still catching up.
	// Point-blank: open capsule (angular aim error is large for same world offset).
	// Seed mode: looser — ExactShotHits is real gate; this is only "aim near enemy".
	float rScale = sniper ? 0.88f : 0.90f;
	float bias = sniper ? 0.55f : 0.60f;
	if (seedLoose) {
		rScale = sniper ? 0.98f : 1.05f;
		bias = sniper ? 0.85f : 0.92f;
	}
	if (eyeDist > 0.f && eyeDist < 220.f) {
		// 0–220u: scale toward 0.98 / 0.85 so close enemies still register onTarget
		const float tClose = 1.f - std::clamp(eyeDist / 220.f, 0.f, 1.f);
		rScale = (std::min)(1.05f, rScale + tClose * 0.12f);
		bias = (std::min)(0.95f, bias + tClose * 0.32f);
	}

	float t = 0.f;
	Vector_t pt{};
	if (preferHb >= 0 && preferHb < Config::HB_COUNT) {
		if (Bones::RayHitsConfiguredHitbox(
				target, preferHb, eye, fwd, rScale, t, pt, bias))
			return true;
	}
	for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
		if (!Config::autofire_hitboxes[hb])
			continue;
		if (hb == preferHb)
			continue;
		if (Bones::RayHitsConfiguredHitbox(
				target, hb, eye, fwd, rScale, t, pt, bias))
			return true;
	}
	return false;
}

bool RunAutofireImpl(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireWantScope = false;
	g_autofireSilentValid = false;
	g_autofireSilentEye = Vector_t{ 0.f, 0.f, 0.f };
	g_autofireFireHist = -1;
	if (!keybind.isActive(Config::autofire)) {
		ResetAutofire();
		return false;
	}
	// Early-outs must ResetAutofire — else sticky g_afBlockFirst strips M1
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100)) {
		ResetAutofire();
		return false;
	}
	if (!Mem::ValidEntity(lp)) {
		ResetAutofire();
		return false;
	}
	// Freeze time / buy time — no combat (default on)
	if (AimCommon::IsFreezePeriod()) {
		ResetAutofire();
		return false;
	}
	// Local still spawn-immune (DM) — don't fire yet
	if (AimCommon::IsTargetImmune(lp)) {
		ResetAutofire();
		return false;
	}
	if (Config::autofire_flash_check && AimCommon::IsBlinded(lp)) {
		ResetAutofire();
		return false;
	}

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(pWpn) || pWpn->IsNonGunWeapon()) {
		ResetAutofire();
		return false;
	}
	// Reload / empty clip / defuse / inspect — full bail: no scan, no aim, no lock.
	// Fire-rate alone does NOT bail (still track between shots).
	if (!AimCommon::WeaponReadyForCombat(pWpn, lp)) {
		ResetAutofire();
		return false;
	}
	Config::ApplyWeaponGroup(pWpn);

	if (Config::autofire_fov <= 0.f) {
		ResetAutofire();
		return false;
	}

	bool anyHb = false;
	for (int h = 0; h < Config::HB_COUNT; ++h) {
		if (Config::autofire_hitboxes[h]) {
			anyHb = true;
			break;
		}
	}
	if (!anyHb) {
		ResetAutofire();
		return false;
	}

	if (AimCommon::AimTargetCount() <= 0) {
		ResetAutofire();
		return false;
	}

	// Engine local shoot origin (NetClientInfo ShootPosition) — not m_vOldOrigin
	const Vector_t lep = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(lep)) {
		ResetAutofire();
		return false;
	}

	QAngle_t qView{};
	if (!AimCommon::GetViewAngles(qView)) {
		ResetAutofire();
		return false;
	}

	// FOV = bullet direction (view + FULL punch). Menu rcs_scale is view-only.
	QAngle_t qViewAim = qView;
	{
		QAngle_t fullPunch{};
		if (AimCommon::GetFirePunch(lp, fullPunch)) {
			qViewAim.x += fullPunch.x;
			qViewAim.y += fullPunch.y;
			qViewAim.Normalize();
		}
	}

	const float baseFov = Config::autofire_fov;
	const float stickyFov = baseFov * 1.15f;
	// Point-blank: angular FOV to hitbox center can exceed menu FOV while enemy
	// fills the screen (head 8u @ 40u ≈ 11°). Floor effective FOV by range.
	// Low menu FOV (2–6°): also lift mid-range slightly so multipoint rim isn't
	// FOV-rejected while center is still in cone.
	auto RangeFov = [&](float worldDist, float maxFov) -> float {
		if (!(worldDist > 1.f) || !(maxFov > 0.f))
			return maxFov;
		// Approx half-height torso angular size (units → deg)
		const float bodyHalf = 28.f; // ~ chest/head cluster
		const float ang = std::atan2(bodyHalf, worldDist) * AimCommon::kRad2Deg;
		// Allow at least body angular size * 1.15, never below menu FOV for far
		const float floorFov = std::clamp(ang * 1.15f, maxFov, 45.f);
		// Only raise FOV when close (dist < ~500u / body dominates)
		if (worldDist < 500.f) {
			// Cap lift: low FOV gets more headroom (maxFov*3+6), high FOV less bloat
			const float liftCap = maxFov * 3.f + 6.f;
			return (std::max)(maxFov, (std::min)(floorFov, liftCap));
		}
		// Very low FOV far: slight multipoint pad (center gate uses *1.25 already)
		if (maxFov < 6.f && worldDist < 1200.f)
			return maxFov * 1.15f;
		return maxFov;
	};

	float bestFov = baseFov;
	float bestDmg = -1.f;
	float bestDist = 1.0e12f;
	QAngle_t bestAngle{};
	Vector_t bestPoint{};
	int bestHb = Config::HB_HEAD;
	bool found = false;
	std::uint32_t bestHandle = 0;
	C_CSPlayerPawn* bestPawn = nullptr;

	float lockedFov = stickyFov;
	float lockedDmg = -1.f;
	float lockedDist = 1.0e12f;
	QAngle_t lockedAngle{};
	Vector_t lockedPoint{};
	int lockedHb = Config::HB_HEAD;
	bool lockedFound = false;
	C_CSPlayerPawn* lockedPawn = nullptr;

	const int selectMode = Config::autofire_target_select;
	const float minDmgVis = Config::autofire_mindamage;
	const float minDmgAw = Config::autofire_mindamage_aw;
	// Damage scan for: damage-sort mode, or mindmg filter (don't lock targets we won't shoot)
	const bool needDamageScan =
		(selectMode == Config::AF_TARGET_DAMAGE)
		|| (minDmgVis > 0.f)
		|| (AfAutowallActive() && minDmgAw > 0.f);

	// Bloom for dynamic multipoint: live GetInaccuracy+GetSpread vs hitbox radius
	float bloom = -1.f;
	if (Config::autofire_multipoint_dynamic && pWpn)
		bloom = AimCommon::LiveMultipointBloom(pWpn, lp);

	// Visible multipoint: up to 3 pens/HB. Wall: 1 center only (game pen is expensive).
	constexpr int kMaxFirePerHbVis = 3;
	constexpr int kMaxFirePerHbWall = 1;
	// BUG FIX: old fixed budget (12/24) died with 3+ targets + mindmg/AW —
	// later pawns never got AutoWall::Fire → no lock → no shoot.
	// Scale with target count; reserve per-pawn so wall stacks don't starve.
	const int nAimTargets = AimCommon::AimTargetCount();
	const int scanPenBudget = AfAutowallActive()
		? (std::min)(48, 10 + nAimTargets * 6)
		: (std::min)(64, 16 + nAimTargets * 8);
	int scanPenLeft = scanPenBudget;
	// Per-pawn wall-pen allowance (AW). Visible cheap estimates don't burn this.
	const int kPenPerPawnWall = 4;

	// Focus Target: while already shooting, only rescan the locked pawn
	const bool focusHold = Config::autofire_focus_target
		&& g_afLocked != 0
		&& AimCommon::ReadShotsFired(lp) >= 1;

	// Scan locked target first so pen budget hits the sticky lock before FOV spam
	int scanOrder[64];
	int nScan = 0;
	{
		const int nT = (std::min)(nAimTargets, 64);
		int lockedSlot = -1;
		if (g_afLocked != 0) {
			for (int i = 0; i < nT; ++i) {
				if (AimCommon::AimTargets()[i].handle == g_afLocked) {
					lockedSlot = i;
					break;
				}
			}
		}
		if (lockedSlot >= 0)
			scanOrder[nScan++] = lockedSlot;
		for (int i = 0; i < nT; ++i) {
			if (i == lockedSlot)
				continue;
			scanOrder[nScan++] = i;
		}
	}

	for (int si = 0; si < nScan; ++si) {
		const int ti = scanOrder[si];
		C_CSPlayerPawn* pawn = AimCommon::AimTargets()[ti].pawn;
		if (!Mem::ValidEntity(pawn))
			continue;

		const int hp = AimCommon::AimTargets()[ti].hp;
		const std::uint32_t pawnHandle = AimCommon::AimTargets()[ti].handle;
		if (focusHold && pawnHandle != g_afLocked)
			continue;

		const bool isLocked = (g_afLocked != 0 && pawnHandle == g_afLocked);
		const float maxFovBase = isLocked ? stickyFov : baseFov;

		Vector_t pawnOrigin{};
		float worldDist = 1.0e12f;
		if (Bones::GetOrigin(pawn, pawnOrigin))
			worldDist = lep.Distance(pawnOrigin);
		// Effective FOV: raised at point-blank so body-filling enemies aren't FOV-rejected
		const float maxFov = RangeFov(worldDist, maxFovBase);
		const float centerFovGate = maxFov * (worldDist < 180.f ? 1.55f : 1.25f);

		const bool targetAir = (pawn->m_fFlags() & FL_ONGROUND) == 0;
		// Close range: don't starve multipoint with bloom clamp (see CollectHitboxMultipoints)
		const float bloomForMp = (worldDist < 160.f) ? -1.f : bloom;

		// Cheap center wall check once per pawn — skip multipoint flood when all wall
		bool pawnCenterWall = false;
		if (AfAutowallActive() && Trace::Ready()) {
			Vector_t chest{};
			if (Bones::GetHitboxPoint(pawn, Config::HB_CHEST, chest)
				&& Bones::IsValidPos(chest)) {
				pawnCenterWall = AimCommon::IsBehindWall(
					lep, chest, lp, pawn, Trace::kMaskShot);
			}
		}

		// Fresh per-pawn wall pen budget (don't let pawn A burn all global pens)
		int pawnPenLeft = kPenPerPawnWall;
		// If global nearly empty but this is locked target, give a last-chance pens
		if (isLocked && scanPenLeft < 3)
			scanPenLeft = (std::max)(scanPenLeft, 3);

		for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
			if (!Config::autofire_hitboxes[hb])
				continue;

			const float mpScale = AutofireMultipointScale(hb);

			// Cheap gate: center FOV + selection filters before full multipoint
			Vector_t center{};
			if (!Bones::GetHitboxPoint(pawn, hb, center) || !Bones::IsValidPos(center))
				continue;
			QAngle_t centerAng{};
			if (!AimCommon::CalcAngles(lep, center, centerAng))
				continue;
			const float centerFov = AimCommon::GetFov(qViewAim, centerAng);
			if (!Mem::Finite(centerFov) || centerFov > centerFovGate)
				continue;
			if (!AfAimAllowed(lep, center, lp, pawn))
				continue;

			// Wall + AW: center-only first. Multipoint only if center pens (or no wall).
			const bool centerWall = pawnCenterWall || (Trace::Ready()
				&& AimCommon::IsBehindWall(lep, center, lp, pawn, Trace::kMaskShot));

			Vector_t points[9]{};
			int nPts = 0;
			if (centerWall && AfAutowallActive()) {
				// 1 point only — full multipoint wall pen is the FPS killer
				points[0] = center;
				nPts = 1;
			} else {
				// bloomForMp=-1 close range → no dyn crush (only center when bloom high)
				nPts = Bones::CollectHitboxMultipoints(
					pawn, hb, mpScale, points, 9,
					&lep, bloomForMp, targetAir);
			}
			if (nPts <= 0 || nPts > 9)
				continue;

			float cachedVisDmg = -1.f;
			float cachedWallDmg = -1.f; // reuse across nearby multipoints same HB
			int fireCount = 0;
			// Close + visible: more pen samples (mindmg often needs center vs multipoint)
			const int maxFireHb = centerWall ? kMaxFirePerHbWall
				: (worldDist < 200.f ? 5 : kMaxFirePerHbVis);

			for (int p = 0; p < nPts; ++p) {
				if (!Bones::IsValidPos(points[p]))
					continue;
				QAngle_t angle{};
				if (!AimCommon::CalcAngles(lep, points[p], angle))
					continue;

				const float fov = AimCommon::GetFov(qViewAim, angle);
				if (!Mem::Finite(fov) || fov > maxFov)
					continue;

				if (!AfAimAllowed(lep, points[p], lp, pawn))
					continue;

				const bool behindWall = centerWall || (Trace::Ready()
					&& AimCommon::IsBehindWall(lep, points[p], lp, pawn, Trace::kMaskShot));

				// Wall + AW off already blocked by AfAimAllowed.
				// Wall + AW on: MUST run pen Fire — no free pass.
				// BUG: old reuseVis required !AW, so AW-on + 3 visible enemies
				// ran full AutoWall per multipoint and exhausted scanPenLeft mid-list.
				float dmg = 0.f;
				bool penetrated = false;
				const bool forceAw = behindWall && AfAutowallActive();
				// Visible damage only needed for sort/mindmg; wall always needs pen.
				const bool wantDmg = needDamageScan || forceAw || behindWall
					|| (AfAutowallActive() && behindWall);
				if (wantDmg || forceAw || behindWall) {
					// Visible: reuse first sample on this HB (AW on OR off)
					const bool reuseVis = !behindWall && cachedVisDmg >= 0.f;
					const bool reuseWall = behindWall && AfAutowallActive()
						&& cachedWallDmg >= 0.f;
					if (reuseVis) {
						dmg = cachedVisDmg;
					} else if (reuseWall) {
						dmg = cachedWallDmg;
						penetrated = (cachedWallDmg >= 1.f);
						// Failed wall cache (0) — skip multipoint spam
						if (cachedWallDmg < 1.f)
							continue;
					} else {
						// Budget: wall pens burn global + per-pawn; visible only global
						if (fireCount >= maxFireHb)
							continue;
						if (scanPenLeft <= 0)
							continue;
						if (behindWall && pawnPenLeft <= 0)
							continue;

						// Visible: no pen needed. Wall: pen only if AW on.
						const bool allowPen = behindWall && AfAutowallActive();
						const AutoWall::Result aw = AutoWall::Fire(
							lep, points[p], hb, pWpn, lp, pawn, allowPen);
						++fireCount;
						--scanPenLeft;
						if (behindWall)
							--pawnPenLeft;

						dmg = aw.hit ? aw.damage : 0.f;
						penetrated = aw.penetrated;
						// Wall: must hit target with real pen dmg
						if (behindWall && (!aw.hit || !aw.penetrated || dmg < 1.f)) {
							cachedWallDmg = 0.f;
							continue;
						}
						if (!behindWall)
							cachedVisDmg = dmg; // always cache visible (AW on/off)
						if (behindWall && AfAutowallActive() && aw.hit && aw.penetrated)
							cachedWallDmg = dmg;
					}
					if (dmg < 1.f)
						continue;

					// Behind wall / real pen → AW mindmg; visible → vis mindmg
					float need = (behindWall || penetrated) ? minDmgAw : minDmgVis;
					if (need > 0.f) {
						if (static_cast<float>(hp) < need)
							need = static_cast<float>(hp);
						if (dmg + 0.01f < need)
							continue;
					}
				} else if (behindWall) {
					// Safety: never pick wall target without damage path
					continue;
				}

				const bool lethal = dmg + 0.01f >= static_cast<float>(hp);
				const bool bestLethal = bestDmg + 0.01f >= static_cast<float>(
					bestPawn ? Mem::ClampHealth(bestPawn->m_iHealth()) : 0);
				const bool lockedLethal = lockedDmg + 0.01f >= static_cast<float>(
					lockedPawn ? Mem::ClampHealth(lockedPawn->m_iHealth()) : 0);

				// Gamesense-style SortTargets + body policy overlays
				auto Prefer = [&](bool haveRef, float refFov, float refDist, float refDmg,
					bool refLethal, int refHb) -> bool {
					if (!haveRef)
						return true;

					const bool candBody = IsBodyHitbox(hb);
					const bool refBody = IsBodyHitbox(refHb);
					const bool candHead = IsHeadHitbox(hb);
					const bool refHead = IsHeadHitbox(refHb);

					// Body if lethal: oneshot torso beats head (same pawn / any).
					// Only when dmg already lethal on a body box.
					if (Config::autofire_body_if_lethal) {
						if (lethal && candBody && !(refLethal && refBody))
							return true;
						if (refLethal && refBody && !(lethal && candBody))
							return false;
					}

					// Prefer body: soft bias — body beats head/limbs when both valid,
					// never hard-blocks head (if no body point, head still wins).
					if (Config::autofire_prefer_body) {
						if (candBody && !refBody)
							return true;
						if (!candBody && refBody)
							return false;
					}

					switch (selectMode) {
					case Config::AF_TARGET_DISTANCE:
						if (worldDist + 1.f < refDist)
							return true;
						if (std::fabs(worldDist - refDist) <= 1.f && fov < refFov)
							return true;
						return false;
					case Config::AF_TARGET_DAMAGE: {
						// Lethal > non-lethal. Default: head > body when both lethal.
						if (lethal && !refLethal)
							return true;
						if (!lethal && refLethal)
							return false;

						const int pri = AimHbPriority(hb);
						const int refPri = AimHbPriority(refHb);

						if (lethal) {
							// Both lethal: head before body (unless body-if-lethal
							// already forced body above), then FOV
							if (pri < refPri)
								return true;
							if (pri > refPri)
								return false;
							return fov < refFov;
						}

						// Both non-lethal: default head > body (prefer-body may
						// already have flipped); then dmg / pri / FOV
						if (!Config::autofire_prefer_body) {
							if (candHead && !refHead)
								return true;
							if (!candHead && refHead)
								return false;
						}
						if (dmg > refDmg + 0.5f)
							return true;
						if (dmg + 0.5f < refDmg)
							return false;
						if (pri < refPri)
							return true;
						if (pri > refPri)
							return false;
						return fov < refFov;
					}
					case Config::AF_TARGET_CROSSHAIR:
					default:
						return fov < refFov;
					}
				};

				if (Prefer(found, bestFov, bestDist, bestDmg, bestLethal, bestHb)) {
					bestFov = fov;
					bestDmg = dmg;
					bestDist = worldDist;
					bestAngle = angle;
					bestPoint = points[p];
					bestHb = hb;
					found = true;
					bestHandle = pawnHandle;
					bestPawn = pawn;
				}

				if (isLocked && Prefer(lockedFound, lockedFov, lockedDist, lockedDmg, lockedLethal, lockedHb)) {
					lockedFov = fov;
					lockedDmg = dmg;
					lockedDist = worldDist;
					lockedAngle = angle;
					lockedPoint = points[p];
					lockedHb = hb;
					lockedFound = true;
					lockedPawn = pawn;
				}
			}
		}
	}

	// Silent Aim checkbox only — seed mode does NOT force silent.
	// Seed still rewrites fire hist angle when shooting; camera follows Silent Aim.
	const bool afSilentNow = Config::autofire_silent;

	if (!found) {
		if (g_afLocked != 0 && g_afGrace > 0) {
			--g_afGrace;
			g_afBlockFirst = true; // don't shoot on FOV miss
			// Keep frame ownership so aimbot doesn't take over; hold last aim
			if (!afSilentNow && g_afLastAimValid && g_afLastAimAngle.IsValid())
				AimCommon::SetViewAngles(g_afLastAimAngle);
			return true;
		}
		ResetAutofire();
		return false;
	}
	g_afGrace = AimCommon::kLockGraceFrames;

	const std::uint64_t now = AimCommon::NowMs();
	const float switchMs = std::clamp(Config::aim_target_switch_delay_ms, 0.f, 500.f);
	const float reactionMs = std::clamp(Config::aim_reaction_delay_ms, 0.f, 500.f);
	const float firstShotMs = std::clamp(Config::aim_first_shot_delay_ms, 0.f, 500.f);

	if (g_afLocked == 0) {
		BeginAutofireEngagement(bestHandle, now);
	}
	else if (bestHandle != g_afLocked) {
		// Focus Target: never switch mid-spray even if another pawn scores better
		const bool blockSwitch = Config::autofire_focus_target
			&& AimCommon::ReadShotsFired(lp) >= 1;
		if (!blockSwitch) {
			if (switchMs <= 0.01f) {
				BeginAutofireEngagement(bestHandle, now);
			}
			else {
				if (g_afPending != bestHandle) {
					g_afPending = bestHandle;
					g_afSwitchReadyMs = now + static_cast<std::uint64_t>(switchMs);
				}
				if (now >= g_afSwitchReadyMs)
					BeginAutofireEngagement(bestHandle, now);
			}
		}
	}
	else {
		g_afPending = 0;
		g_afSwitchReadyMs = 0;
	}

	QAngle_t aimAngle{};
	Vector_t aimPoint{};
	int aimHb = Config::HB_HEAD;
	C_CSPlayerPawn* aimTarget = nullptr;
	if (g_afLocked == bestHandle) {
		aimAngle = bestAngle;
		aimPoint = bestPoint;
		aimHb = bestHb;
		aimTarget = bestPawn;
	}
	else if (lockedFound) {
		aimAngle = lockedAngle;
		aimPoint = lockedPoint;
		aimHb = lockedHb;
		aimTarget = lockedPawn;
	}
	else {
		BeginAutofireEngagement(bestHandle, now);
		aimAngle = bestAngle;
		aimPoint = bestPoint;
		aimHb = bestHb;
		aimTarget = bestPawn;
	}

	// Delays (menu ms, 0 = off). Stack order after lock:
	//   1) reaction  — no FIRE. Still aim/smooth (low FOV needs settle). Do NOT arm
	//      first-shot yet — old path started first-shot during reaction → double wait
	//      and g_afFirstDone cleared reaction block.
	//   2) first shot — arm when reaction ends; aim OK, fire blocked until ready
	//   3) switch delay — only when bestHandle changes (BeginAutofireEngagement)
	// Seed + on target: skip reaction/first-shot (parked = same-frame fire).
	// Deagle air: jump bloom makes tight FOV/ray fail → felt late / no air shot.
	// Silent seed: geometric fire ray is source of truth.
	const bool seedModeEarly =
		Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD;
	const bool heavySeed = seedModeEarly && AimCommon::IsHeavyPistol(pWpn);
	bool alreadyOnBone = false;
	if (seedModeEarly && aimTarget && Bones::IsValidPos(aimPoint)) {
		// Heavy pistol seed: any valid aim lock = instant (Solve is real gate)
		if (heavySeed) {
			alreadyOnBone = true;
		} else {
			// Geometric first (silent + online punch lag)
			{
				QAngle_t ga{};
				if (AimCommon::CalcAngles(lep, aimPoint, ga)) {
					QAngle_t fp{};
					if (AimCommon::GetFirePunch(lp, fp)) {
						ga.x += fp.x; ga.y += fp.y;
					}
					ga.z = 0.f;
					ga.Normalize();
					const float d = lep.Distance(aimPoint);
					if (FireRayHitsTarget(lep, ga, aimTarget, aimHb, false, d, true))
						alreadyOnBone = true;
				}
			}
			// Cam FOV backup (non-silent smooth nearly settled)
			if (!alreadyOnBone) {
				QAngle_t cam{};
				if (AimCommon::GetViewAngles(cam) && cam.IsValid()) {
					QAngle_t toBone{};
					if (AimCommon::CalcAngles(lep, aimPoint, toBone)) {
						const float fov = AimCommon::GetFov(cam, toBone);
						// 6° — air/smooth lag; seed Exact still gates fire
						if (std::isfinite(fov) && fov <= 6.0f)
							alreadyOnBone = true;
					}
				}
			}
		}
	}

	// Seed deagle: always skip reaction/first-shot delays (Solve is only gate).
	if (heavySeed && aimTarget)
		alreadyOnBone = true;

	const bool inReaction = !alreadyOnBone
		&& reactionMs > 0.01f
		&& (now - g_afLockMs) < static_cast<std::uint64_t>(reactionMs);
	if (inReaction) {
		g_afBlockFirst = true;
	} else if (alreadyOnBone) {
		// Instant seed path — mark delays satisfied
		g_afFirstArmed = true;
		g_afFirstDone = true;
		g_afBlockFirst = false;
	} else {
		// Arm first-shot only after reaction so timers stack, not overlap.
		if (!g_afFirstArmed) {
			g_afFirstArmed = true;
			g_afFirstShotReadyMs = now + static_cast<std::uint64_t>(firstShotMs);
			g_afFirstDone = (firstShotMs <= 0.01f);
		}
		if (!g_afFirstDone) {
			if (now < g_afFirstShotReadyMs)
				g_afBlockFirst = true;
			else {
				g_afBlockFirst = false;
				g_afFirstDone = true;
			}
		} else {
			g_afBlockFirst = false;
		}
	}

	// View RCS (optional, scaled) — cosmetic crosshair track.
	// Fire path always uses FULL punch below (AK spray was climbing when
	// smooth lagged or RCS scale < 1 / RCS off).
	QAngle_t viewAim = aimAngle;
	if (Config::rcs) {
		QAngle_t scaled{};
		if (AimCommon::GetScaledPunch(lp, scaled))
			AimCommon::ApplyPunchSubtract(viewAim, scaled);
	}
	viewAim.x = std::clamp(viewAim.x, -89.f, 89.f);
	viewAim.z = 0.f;
	if (!viewAim.IsValid()) {
		ResetAutofire();
		return false;
	}

	// Seed = fire-hist rewrite only. Camera follows bone/viewAim never seed ang.
	// Silent checkbox: no camera move; non-silent: smooth to bone.
	// Do NOT StampCmdAngles(seed) later — that snaps model (AWP air miss flick).
	const bool silent = afSilentNow;
	QAngle_t shotAngle = viewAim;
	const bool seedModeNow =
		Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD;

	if (!silent) {
		QAngle_t finalAngle = viewAim;
		const float smooth = std::clamp(Config::aimbot_smooth, 0.f, 50.f);
		if (smooth > 0.01f) {
			QAngle_t cur{};
			if (AimCommon::GetViewAngles(cur) && cur.IsValid())
				finalAngle = AimCommon::SmoothToward(cur, viewAim, smooth);
		}
		if (!finalAngle.IsValid()) {
			ResetAutofire();
			return false;
		}
		AimCommon::SetViewAngles(finalAngle);
		shotAngle = finalAngle;
	} else if (!seedModeNow) {
		// Silent non-seed: stamp bone aim to base+tip. Seed uses hist only.
		AimCommon::StampCmdAngles(viewAim);
	}

	g_afLastAimAngle = silent ? viewAim : shotAngle;
	g_afLastAimPoint = aimPoint;
	g_afLastHb = aimHb;
	g_afLastAimValid = true;

	// First-shot / FOV-grace humanize: no FIRE yet, but still settle move + scope
	// so seed/AW don't open while running the moment delay ends.
	if (g_afBlockFirst) {
		if (aimTarget && Config::autofire_autostop) {
			const Vector_t vel = Pred::Velocity(lp);
			const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
			const bool onGround = (Pred::Flags(lp) & FL_ONGROUND) != 0;
			if (onGround && std::isfinite(speed2d)
				&& speed2d > AimCommon::kAfStopSpeed)
				g_autofireWantStop = true;
		}
		if (aimTarget && Config::autofire_autoscope
			&& AimCommon::IsScopeWeapon(pWpn)
			&& !AimCommon::IsLocalScoped(lp, pWpn))
			g_autofireWantScope = true;
		return true;
	}

	if (AimCommon::CanWeaponFire(pWpn, lp)) {
		const Vector_t vel = Pred::Velocity(lp);
		const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
		const bool onGround = (Pred::Flags(lp) & FL_ONGROUND) != 0;
		const bool sniper = AimCommon::IsSniperWeapon(pWpn);
		const bool scopeWpn = AimCommon::IsScopeWeapon(pWpn);
		const bool sniperScoped = sniper && AimCommon::IsLocalScoped(lp, pWpn);

		// Scope Check: shared flag (aim_scoped_only) — must be zoomed
		const bool scopedOk = !scopeWpn || !Config::aim_scoped_only
			|| AimCommon::IsLocalScoped(lp, pWpn);

		Vector_t shotPoint = aimPoint;
		int shotHb = aimHb;

		const bool seedModeGate =
			Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD;

		// Seed mode: center + short vel lead (multipoint rim / no lead = miss movers).
		// HC mode: refine multipoint vs punch-aware view for closest-to-crosshair.
		if (aimTarget) {
			if (seedModeGate) {
				const Vector_t seedPt = NoSpread::SeedAimPoint(aimTarget, shotHb, shotPoint);
				if (Bones::IsValidPos(seedPt) && AfAimAllowed(lep, seedPt, lp, aimTarget))
					shotPoint = seedPt;
			} else {
				// Refine vs punch-aware view (FOV), not lagged smooth alone.
				QAngle_t refineRef = viewAim;
				Vector_t closest{};
				int closestHb = aimHb;
				const int preferHb =
					(Config::autofire_target_select == Config::AF_TARGET_CROSSHAIR)
					? -1 : aimHb;
				if (FindClosestAutofireBone(
						lep, refineRef, aimTarget, pWpn, lp, closest, closestHb, preferHb)) {
					shotPoint = closest;
					shotHb = closestHb;
				}
			}
		}

		// FIRE: always bone → full punch. Never use smoothed view as fire stamp
		// (smooth lag + half RCS = AK overspray above head).
		QAngle_t fireAngle{};
		if (!AimCommon::CalcAngles(lep, shotPoint, fireAngle)) {
			fireAngle = viewAim;
		} else {
			QAngle_t fullPunch{};
			if (AimCommon::GetFirePunch(lp, fullPunch))
				AimCommon::ApplyPunchSubtract(fireAngle, fullPunch);
			fireAngle.x = std::clamp(fireAngle.x, -89.f, 89.f);
			fireAngle.z = 0.f;
			if (!fireAngle.IsValid())
				fireAngle = viewAim;
		}

		// onTarget: geometric wish dir (fireAngle + punch). Seed uses looser capsule —
		// ExactShotHits is real hit gate. HC keeps tight so low FOV doesn't spam miss.
		QAngle_t gateAng = fireAngle;
		{
			QAngle_t gatePunch{};
			if (AimCommon::GetFirePunch(lp, gatePunch)) {
				gateAng.x += gatePunch.x;
				gateAng.y += gatePunch.y;
				gateAng.z = 0.f;
				gateAng.Normalize();
				gateAng.x = std::clamp(gateAng.x, -89.f, 89.f);
			}
		}
		const float shotDist = Bones::IsValidPos(shotPoint)
			? lep.Distance(shotPoint) : -1.f;
		bool onTarget = FireRayHitsTarget(
			lep, gateAng, aimTarget, shotHb, sniper, shotDist, seedModeGate);

		// Seed deagle air: geometric wish often misses capsule (jump punch/bloom)
		// before Solve rewrites hist — never reach seed path. Locked aim = onTarget.
		if (!onTarget && seedModeGate && aimTarget
			&& AimCommon::IsHeavyPistol(pWpn) && Bones::IsValidPos(shotPoint))
			onTarget = true;

		// Seed general: looser FOV backup so air/smooth lag still enters Solve
		if (!onTarget && seedModeGate && aimTarget && Bones::IsValidPos(shotPoint)) {
			QAngle_t cam{};
			QAngle_t toPt{};
			if (AimCommon::GetViewAngles(cam) && AimCommon::CalcAngles(lep, shotPoint, toPt)) {
				const float fov = AimCommon::GetFov(cam, toPt);
				if (std::isfinite(fov) && fov <= 12.f)
					onTarget = true;
			}
		}

		// Low FOV + smooth lag: multipoint fail → retry hitbox center once
		if (!onTarget && aimTarget && shotHb >= 0 && shotHb < Config::HB_COUNT) {
			Vector_t cpt{};
			if (Bones::GetHitboxPoint(aimTarget, shotHb, cpt) && Bones::IsValidPos(cpt)
				&& AfAimAllowed(lep, cpt, lp, aimTarget)) {
				QAngle_t ca{};
				if (AimCommon::CalcAngles(lep, cpt, ca)) {
					QAngle_t fp{};
					if (AimCommon::GetFirePunch(lp, fp))
						AimCommon::ApplyPunchSubtract(ca, fp);
					ca.x = std::clamp(ca.x, -89.f, 89.f);
					ca.z = 0.f;
					if (ca.IsValid()) {
						QAngle_t ga = ca;
						QAngle_t gp{};
						if (AimCommon::GetFirePunch(lp, gp)) {
							ga.x += gp.x; ga.y += gp.y; ga.z = 0.f;
							ga.Normalize();
							ga.x = std::clamp(ga.x, -89.f, 89.f);
						}
						const float cd = lep.Distance(cpt);
						if (FireRayHitsTarget(
								lep, ga, aimTarget, shotHb, sniper, cd, seedModeGate)) {
							shotPoint = cpt;
							fireAngle = ca;
							gateAng = ga;
							onTarget = true;
						}
					}
				}
			}
		}

		// dmgOk  = selection filters (smoke/vis) + real hit path + mindamage
		// hcOk   = spread would land — only required to shoot, not to stop
		// BUG: HC/seed lived inside `else` of Trace::Ready — when Trace down + mindmg
		// off, dmgOk=true but afMode never ran → hcOk stuck false → never WantShoot.
		bool dmgOk = false;
		bool hcOk = false;
		if (scopedOk && onTarget && aimTarget) {
			if (!AfAimAllowed(lep, shotPoint, lp, aimTarget)) {
				dmgOk = false;
			} else if (!Trace::Ready()) {
				// Trace not ready: still allow if mindmg off (close fight fail-open)
				dmgOk = (Config::autofire_mindamage <= 0.f);
			} else {
				const bool behindWall = AimCommon::IsBehindWall(
					lep, shotPoint, lp, aimTarget, Trace::kMaskShot);

				if (behindWall && !AfAutowallActive()) {
					dmgOk = false;
				} else if (behindWall && AfAutowallActive()) {
					// Wallbang: must pen wall + hit target + dmg >= mindamage_aw
					const AutoWall::Result aw = AutoWall::Fire(
						lep, shotPoint, shotHb, pWpn, lp, aimTarget, true);
					if (aw.hit && aw.penetrated && aw.damage >= 1.f) {
						float need = Config::autofire_mindamage_aw;
						if (need > 0.f) {
							const int hp = aimTarget->m_iHealth();
							if (hp > 0 && static_cast<float>(hp) < need)
								need = static_cast<float>(hp);
							dmgOk = aw.damage + 0.01f >= need;
						} else {
							dmgOk = true;
						}
					} else {
						dmgOk = false;
					}
				} else {
					// Visible: LOS + Min Damage (no pen estimate).
					const bool losOk = Trace::IsVisible(
						lep, shotPoint, lp, aimTarget, Trace::kMaskVis);
					const bool closeOpen = shotDist > 0.f && shotDist < 96.f;
					if (!losOk && !closeOpen) {
						dmgOk = false;
					} else if (Config::autofire_mindamage <= 0.f) {
						dmgOk = true;
					} else {
						dmgOk = AutoWall::PassesMinDamage(
							lep, shotPoint, shotHb, pWpn, lp, aimTarget, false,
							Config::autofire_mindamage);
						if (!dmgOk && shotDist > 0.f && shotDist < 250.f
							&& shotHb >= 0 && shotHb < Config::HB_COUNT) {
							Vector_t cpt{};
							if (Bones::GetHitboxPoint(aimTarget, shotHb, cpt)
								&& Bones::IsValidPos(cpt)
								&& AfAimAllowed(lep, cpt, lp, aimTarget)) {
								dmgOk = AutoWall::PassesMinDamage(
									lep, cpt, shotHb, pWpn, lp, aimTarget, false,
									Config::autofire_mindamage);
								if (dmgOk) {
									shotPoint = cpt;
									QAngle_t ca{};
									if (AimCommon::CalcAngles(lep, cpt, ca)) {
										QAngle_t fp{};
										if (AimCommon::GetFirePunch(lp, fp))
											AimCommon::ApplyPunchSubtract(ca, fp);
										ca.x = std::clamp(ca.x, -89.f, 89.f);
										ca.z = 0.f;
										if (ca.IsValid())
											fireAngle = ca;
									}
								}
							}
						}
					}
				}
			}

			// HC / Seed always when dmgOk — independent of Trace::Ready branch.
			if (dmgOk) {
				int afMode = Config::autofire_mode;
				if (afMode < 0 || afMode >= Config::AF_MODE_COUNT)
					afMode = Config::AF_MODE_HITCHANCE;
				if (afMode == Config::AF_MODE_SEED_NOSPREAD) {
					if (!NoSpread::Ready())
						NoSpread::Init();
					if (!NoSpread::Ready())
						afMode = Config::AF_MODE_HITCHANCE;
				}

				if (afMode == Config::AF_MODE_SEED_NOSPREAD) {
					// Parity with trigger seed (IDA SPREADSEEDGEN + CalcSpread seed+1):
					// live eye, hist with player tick, no fill-frac steal, Exact fast path.
					hcOk = false;
					if (NoSpread::Ready() && NoSpread::SeedCycleAllowsFire(pWpn, lp)) {
						int atkIdx = -1;
						float seedFrac = 0.f;
						// Live eye default — hist/fill lag online desyncs wantDir.
						Vector_t seedEye = lep;
						QAngle_t camForHist{};
						const bool haveCamHist = AimCommon::GetViewAngles(camForHist);
						if (haveCamHist)
							camForHist.z = 0.f;

						if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
							const int histCount =
								cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
							// Prefer newest hist with nPlayerTickCount near cam
							// (old: always last slot — often no player tick / wrong bin).
							for (int hi = histCount - 1; hi >= 0; --hi) {
								CCSGOInputHistoryEntryPB* he =
									cmd->GetInputHistoryEntry(hi);
								if (!he || he->nPlayerTickCount <= 0)
									continue;
								if (he->pViewAngles && haveCamHist) {
									QAngle_t ha = he->pViewAngles->angValue;
									ha.z = 0.f;
									float dy = ha.y - camForHist.y;
									while (dy > 180.f) dy -= 360.f;
									while (dy < -180.f) dy += 360.f;
									const float dp = ha.x - camForHist.x;
									const float d = std::sqrt(dp * dp + dy * dy);
									if (d > 0.75f)
										continue;
								}
								atkIdx = hi;
								break;
							}
							if (atkIdx < 0)
								atkIdx = histCount > 0 ? histCount - 1 : -1;
							if (atkIdx >= 0) {
								CCSGOInputHistoryEntryPB* e =
									cmd->GetInputHistoryEntry(atkIdx);
								if (e) {
									seedFrac = e->flPlayerTickFraction;
									if (!std::isfinite(seedFrac))
										seedFrac = 0.f;
									// Hist eye only if near live (<3u)
									if (e->pShootPosition) {
										const Vector4D_t& v = e->pShootPosition->vecValue;
										const Vector_t sp{ v.x, v.y, v.z };
										if (Bones::IsValidPos(sp)) {
											const float dx = sp.x - lep.x;
											const float dy = sp.y - lep.y;
											const float dz = sp.z - lep.z;
											if (dx * dx + dy * dy + dz * dz < 9.f)
												seedEye = sp;
										}
									}
								}
							}
						}
						// Prefer hist nPlayerTickCount — never steal fill tick when set.
						int seedTick = AimCommon::GetRenderTick(cmd, atkIdx, lp);
						{
							QAngle_t viewHint = fireAngle;
							viewHint.z = 0.f;
							viewHint.Normalize();
							HitChance::SeedFireContext sfc{};
							if (HitChance::BuildSeedFireContext(
									lp, pWpn, viewHint, seedTick, seedFrac,
									seedEye, sfc) && sfc.ok) {
								if (seedTick <= 0 && sfc.tick > 0)
									seedTick = sfc.tick;
								// Keep hist frac; fill only if missing
								if (seedFrac <= 0.f && std::isfinite(sfc.frac))
									seedFrac = sfc.frac;
								if (Bones::IsValidPos(sfc.eye)) {
									const float dx = sfc.eye.x - lep.x;
									const float dy = sfc.eye.y - lep.y;
									const float dz = sfc.eye.z - lep.z;
									if (dx * dx + dy * dy + dz * dz < 9.f)
										seedEye = sfc.eye;
								}
							}
						}
						if (seedTick > 0) {
							// Wish: unscoped sniper must stay near camera — bone-wish
							// + big rewrite stamps hist far from cam → model flick +
							// FillGunFireData interp miss (IDA 7CFDD0 mode 1).
							const bool unscopedSniper = sniper && !sniperScoped;
							QAngle_t wish{};
							bool haveWish = false;
							if (unscopedSniper && haveCamHist && camForHist.IsValid()) {
								wish = camForHist;
								wish.z = 0.f;
								wish.Normalize();
								haveWish = wish.IsValid();
							}
							if (!haveWish) {
								wish = fireAngle;
								wish.z = 0.f;
								wish.Normalize();
								haveWish = wish.IsValid();
							}
							// Bone-wish only if near cam (unscoped) or always (scoped/other)
							{
								QAngle_t w{};
								if (AimCommon::CalcAngles(seedEye, shotPoint, w)) {
									QAngle_t punch{};
									if (HitChance::ReadSeedFirePunch(
											lp, pWpn, seedTick, seedFrac, punch)
										&& punch.IsValid())
										AimCommon::ApplyPunchSubtract(w, punch);
									w.z = 0.f;
									w.x = std::clamp(w.x, -89.f, 89.f);
									w.Normalize();
									if (w.IsValid()) {
										if (!unscopedSniper) {
											wish = w;
										} else if (haveWish) {
											float dy = w.y - wish.y;
											while (dy > 180.f) dy -= 360.f;
											while (dy < -180.f) dy += 360.f;
											const float d = std::sqrt(
												(w.x - wish.x) * (w.x - wish.x) + dy * dy);
											// Only if bone already near cam (parked)
											if (d <= 8.f)
												wish = w;
										} else {
											wish = w;
										}
									}
								}
							}

							// Fast path: natural pellet already on enabled HB
							// (parked / low bloom) — skip multi-HB Solve spiral.
							NoSpread::Shot shot{};
							bool solved = false;
							{
								int hbFast = -1;
								Vector_t ptFast{};
								bool exact = false;
								__try {
									exact = HitChance::ExactShotHitsAny(
										seedEye, wish, seedTick, pWpn, lp, aimTarget,
										Config::autofire_hitboxes, &hbFast, &ptFast,
										seedFrac);
								} __except (EXCEPTION_EXECUTE_HANDLER) {
									exact = false;
								}
								if (exact && hbFast >= 0 && Bones::IsValidPos(ptFast)
									&& wish.IsValid()) {
									shot.fireAngles = wish;
									shot.fireAngles.z = 0.f;
									shot.hitPoint = ptFast;
									shot.hitbox = hbFast;
									shot.seedTick = seedTick;
									shot.seedFrac = seedFrac;
									shot.ok = true;
									solved = true;
								}
							}
							if (!solved) {
								__try {
									solved = NoSpread::Solve(
										seedEye, wish, seedTick, seedFrac, pWpn, lp,
										aimTarget, shotHb, Config::autofire_hitboxes,
										shot);
								} __except (EXCEPTION_EXECUTE_HANDLER) {
									solved = false;
								}
							}
							// Reject wild rewrite vs cam — online desync / flick
							if (solved && shot.ok && unscopedSniper && haveCamHist) {
								QAngle_t sa = shot.fireAngles;
								sa.z = 0.f;
								float dy = sa.y - camForHist.y;
								while (dy > 180.f) dy -= 360.f;
								while (dy < -180.f) dy += 360.f;
								const float d = std::sqrt(
									(sa.x - camForHist.x) * (sa.x - camForHist.x) + dy * dy);
								if (d > 16.f)
									solved = false;
							}
							if (solved && shot.ok && shot.fireAngles.IsValid()) {
								Vector_t hitPt = shot.hitPoint;
								int hitHb = shot.hitbox;
								// Always hard-gate pellet dmg — never inherit pre-Solve dmgOk
								// (pre used aim multipoint; pellet may be feet @ 20 under mindmg 100).
								bool seedOk = false;
								if (Bones::IsValidPos(hitPt) && hitHb >= 0
									&& hitHb < Config::HB_COUNT
									&& Config::autofire_hitboxes[hitHb]
									&& Trace::Ready()) {
									// Wall on PELLET (live eye) — not pre-Solve multipoint.
									// !Trace::Ready used to force vis path (wall bypass) — fail closed.
									const bool behind = AimCommon::IsBehindWall(
										lep, hitPt, lp, aimTarget,
										Trace::kMaskShot);
									if (behind && !AfAutowallActive()) {
										seedOk = false;
									} else {
										const bool awOn = behind && AfAutowallActive();
										seedOk = NoSpread::SeedPassesDamage(
											seedEye, hitPt, hitHb, pWpn, lp,
											aimTarget, awOn,
											Config::autofire_mindamage,
											Config::autofire_mindamage_aw);
									}
								}
								if (seedOk) {
									if (shot.seedTick > 0)
										seedTick = shot.seedTick;
									if (std::isfinite(shot.seedFrac))
										seedFrac = shot.seedFrac;
									fireAngle = shot.fireAngles;
									// Keep roll — IDA seed ignores z; FireBullet uses it
									if (!std::isfinite(fireAngle.z))
										fireAngle.z = 0.f;
									fireAngle.x = std::clamp(fireAngle.x, -89.f, 89.f);
									fireAngle.Normalize();
									shotPoint = hitPt;
									shotHb = hitHb;
									g_autofireFireHist = atkIdx;
									if (cmd && atkIdx >= 0) {
										CCSGOInputHistoryEntryPB* e =
											cmd->GetInputHistoryEntry(atkIdx);
										if (e && seedTick > 0) {
											e->nPlayerTickCount = seedTick;
											e->flPlayerTickFraction = seedFrac;
											if (e->nRenderTickCount <= 0
												|| (e->nRenderTickCount > 0
													&& (e->nRenderTickCount > seedTick
														? e->nRenderTickCount - seedTick
														: seedTick - e->nRenderTickCount) > 1)) {
												e->nRenderTickCount = seedTick;
												e->flRenderTickFraction = seedFrac;
											}
											e->SetBits(
												EInputHistoryBits::INPUT_HISTORY_BITS_PLAYERTICKCOUNT
												| EInputHistoryBits::INPUT_HISTORY_BITS_PLAYERTICKFRACTION
												| EInputHistoryBits::INPUT_HISTORY_BITS_RENDERTICKCOUNT
												| EInputHistoryBits::INPUT_HISTORY_BITS_RENDERTICKFRACTION);
										}
										cmd->SetAttackHistoryFire(
											atkIdx, fireAngle, seedEye);
									}
									g_autofireSilentEye = seedEye;
									hcOk = true;
									NoSpread::NoteSeedFired(pWpn, lp);
								}
							}
						}
					}
				} else {
					// Hitchance — PassesSafe uses live GetInaccuracy+GetSpread
					hcOk = false;
					float needHc = std::clamp(
						Config::autofire_hitchance, 0.f, 100.f);
					if (sniper && lp->m_bIsScoped() && needHc <= 0.01f)
						needHc = 0.f;
					if (needHc <= 0.01f) {
						hcOk = true;
					} else {
						if (!HitChance::Ready())
							HitChance::Init();
						hcOk = HitChance::PassesSafe(
							lep, fireAngle, shotPoint, shotHb, pWpn,
							needHc, lp, aimTarget);
					}
				}
			}
		}

		// Fire when HC/seed says ok. Movement is NEVER touched unless Autostop on.
		// No hard run/air speed kill — those felt like forced stops with Autostop off.
		// Scoped Only only gates dmgOk above; it never brakes movement.
		if (dmgOk) {
			if (Config::autofire_autostop && onGround
				&& std::isfinite(speed2d) && speed2d > AimCommon::kAfStopSpeed)
				g_autofireWantStop = true;

			// Autoscope: hold attack2 while hittable target in FOV and not zoomed.
			// Respects same dmg/vis/autowall gate as fire — no scope on dead walls.
			if (Config::autofire_autoscope && scopeWpn
				&& !AimCommon::IsLocalScoped(lp, pWpn))
				g_autofireWantScope = true;

			if (hcOk) {
				g_autofireWantShoot = true;
				g_autofireSilentAngle = fireAngle;
				// Seed FIRE already wrote FillGunFireData eye into g_autofireSilentEye.
				// Hitchance path: always live shootpos.
				const bool seedModeNow =
					Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD
					&& NoSpread::Ready();
				if (!seedModeNow || !Bones::IsValidPos(g_autofireSilentEye))
					g_autofireSilentEye = lep;
				g_autofireSilentValid = true;
			}
		}
	}

	return true;
}

bool RunSafe(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	bool ok = false;
	__try {
		ok = RunAutofireImpl(lp, cmd);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ResetAutofire();
		ok = false;
	}
	return ok;
}

bool Run(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	return RunSafe(lp, cmd);
}

void Reset() {
	ResetAutofire();
}

bool WantShoot() { return g_autofireWantShoot; }
bool WantStop() { return g_autofireWantStop; }
bool WantScope() { return g_autofireWantScope; }
bool BlockFirstShot() { return g_afBlockFirst; }
bool SilentValid() { return g_autofireSilentValid; }
const QAngle_t& SilentAngle() { return g_autofireSilentAngle; }
const Vector_t& SilentEye() { return g_autofireSilentEye; }
int FireHistIndex() { return g_autofireFireHist; }

void ClearShootFlags() {
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireWantScope = false;
	g_afBlockFirst = false;
	g_autofireSilentValid = false;
}

} // namespace Autofire
