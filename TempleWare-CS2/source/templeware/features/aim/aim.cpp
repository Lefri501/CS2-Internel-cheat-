#include "aim.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../templeware/interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../templeware/interfaces/interfaces.h"
#include "../../../templeware/interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../templeware/hooks/hooks.h"
#include "../../../templeware/interfaces/CUserCmd/CUserCmd.h"
#include "../../../templeware/config/config.h"
#include "../../../templeware/utils/schema/schema.h"
#include "../../../templeware/utils/fnv1a/fnv1a.h"
#include "../../../templeware/keybinds/keybinds.h"
#include "../../../templeware/offsets/offsets.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../autowall/autowall.h"
#include "../gamemode/gamemode.h"
#include "../../../templeware/utils/memory/memsafe/memsafe.h"
#include "../../../templeware/utils/console/console.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.0f / kPi;
// CS2 aim punch → view is *2
constexpr float kPunchScale = 2.f;
// Keep lock a few frames after brief FOV miss (smooth/desync)
constexpr int kLockGraceFrames = 8;

QAngle_t g_oldPunch{};
bool g_hadPunch = false;

std::uint32_t g_lockedTarget = 0;
std::uint32_t g_pendingTarget = 0;
std::uint64_t g_lockAcquiredMs = 0;
std::uint64_t g_switchReadyMs = 0;
std::uint64_t g_firstShotReadyMs = 0;
bool g_firstShotArmed = false;
bool g_firstShotDone = false;
bool g_blockFirstShot = false;
int g_lockGrace = 0;

// Autofire — separate lock / shoot state from aimbot
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
QAngle_t g_autofireSilentAngle{};
Vector_t g_autofireSilentEye{};
bool g_autofireSilentValid = false;
// Last good aim during FOV grace (keep ownership / visible aim)
QAngle_t g_afLastAimAngle{};
Vector_t g_afLastAimPoint{};
int g_afLastHb = Config::HB_HEAD;
bool g_afLastAimValid = false;

// Triggerbot — crosshair-on-enemy fire (no aim move)
bool g_triggerWantShoot = false;
bool g_triggerWantStop = false;
std::uint64_t g_triggerOnSinceMs = 0;
bool g_triggerWasOn = false;
QAngle_t g_triggerFireAngle{};
Vector_t g_triggerFireEye{};
bool g_triggerFireValid = false;
int g_triggerFireHist = -1;
// Cross-frame mouse speed for flick detect (deg/frame EMA)
static float g_triggerAngSpeed = 0.f;
static QAngle_t g_triggerPrevCam{};
static bool g_triggerPrevCamOk = false;

// Last predicted seed state — SPREADSEEDGEN hook reads these for comparison
std::uint64_t g_lastPredictedSeedMs = 0;
int g_lastPredictedSeedTick = 0;
QAngle_t g_lastPredictedAng{};

static constexpr int kAfMpToHb[Config::AF_MP_COUNT] = {
	Config::HB_HEAD,
	Config::HB_CHEST,
	Config::HB_STOMACH,
	Config::HB_PELVIS
};

// Align with hitchance walk gate (hitchance.cpp kWalkSpeed)
constexpr float kAfStopSpeed = 34.f;
// Hard run gate when hitchance is off (HC already models move bloom)
constexpr float kNoHcRunSpeed = 75.f;

static std::uint64_t NowMs() {
	return GetTickCount64();
}

// Live GetInaccuracy+GetSpread for dynamic multipoint (not raw penalty only).
static float LiveMultipointBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local) {
	if (!weapon)
		return -1.f;
	float inac = 0.f, spr = 0.f;
	if (HitChance::ReadCurrentBloom(weapon, local, inac, spr)
		&& std::isfinite(inac) && std::isfinite(spr) && inac >= 0.f && spr >= 0.f) {
		return std::clamp(inac + spr, 0.f, 0.35f);
	}
	float pen = weapon->m_fAccuracyPenalty();
	if (!std::isfinite(pen) || pen < 0.f)
		pen = 0.01f;
	return std::clamp(pen + 0.004f, 0.f, 0.25f);
}

static void ResetHumanize() {
	g_lockedTarget = 0;
	g_pendingTarget = 0;
	g_lockAcquiredMs = 0;
	g_switchReadyMs = 0;
	g_firstShotReadyMs = 0;
	g_firstShotArmed = false;
	g_firstShotDone = false;
	g_blockFirstShot = false;
	g_lockGrace = 0;
}

static void ResetAutofire() {
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
	g_autofireSilentValid = false;
	g_autofireSilentEye = Vector_t{ 0.f, 0.f, 0.f };
	g_afLastAimValid = false;
}

static void ResetTriggerbot() {
	g_triggerWantShoot = false;
	g_triggerWantStop = false;
	g_triggerOnSinceMs = 0;
	g_triggerWasOn = false;
	g_triggerFireValid = false;
	g_triggerFireHist = -1;
	g_triggerFireEye = Vector_t{ 0.f, 0.f, 0.f };
	g_triggerAngSpeed = 0.f;
	g_triggerPrevCamOk = false;
}

static void BeginEngagement(std::uint32_t handle, std::uint64_t now) {
	g_lockedTarget = handle;
	g_pendingTarget = 0;
	g_lockAcquiredMs = now;
	g_switchReadyMs = 0;
	g_firstShotReadyMs = 0;
	g_firstShotArmed = false;
	g_firstShotDone = false;
	g_blockFirstShot = false;
	g_lockGrace = kLockGraceFrames;
}

static void BeginAutofireEngagement(std::uint32_t handle, std::uint64_t now) {
	g_afLocked = handle;
	g_afPending = 0;
	g_afLockMs = now;
	g_afSwitchReadyMs = 0;
	g_afFirstShotReadyMs = 0;
	g_afFirstArmed = false;
	g_afFirstDone = false;
	g_afBlockFirst = false;
	g_afGrace = kLockGraceFrames;
}

bool CalcAngles(const Vector_t& viewPos, const Vector_t& aimPos, QAngle_t& out) {
	const Vector_t delta = aimPos - viewPos;
	const float len = delta.Length();
	if (len < 0.001f || !std::isfinite(len))
		return false;

	const float pitchArg = std::clamp(-delta.z / len, -1.0f, 1.0f);
	out.x = std::asin(pitchArg) * kRad2Deg;
	out.y = std::atan2(delta.y, delta.x) * kRad2Deg;
	out.z = 0.f;
	if (!out.IsValid())
		return false;
	out.Normalize();
	return true;
}

float GetFov(const QAngle_t& viewAngle, const QAngle_t& aimAngle) {
	QAngle_t delta = aimAngle - viewAngle;
	delta.Normalize();
	return delta.Length2D();
}

// Mindamage / hitchance / autowall: multipoint closest to crosshair among
// selected autofire hitboxes (same CollectHitboxMultipoints as target scan).
bool FindClosestAutofireBone(
	const Vector_t& eye,
	const QAngle_t& crosshair,
	C_CSPlayerPawn* pawn,
	C_CSWeaponBase* weapon,
	Vector_t& outPoint,
	int& outHb)
{
	if (!pawn || !Bones::IsValidPos(eye) || !crosshair.IsValid())
		return false;

	float bloom = -1.f;
	if (Config::autofire_multipoint_dynamic && weapon)
		bloom = LiveMultipointBloom(weapon, nullptr);

	const bool targetAir = (pawn->m_fFlags() & FL_ONGROUND) == 0;

	float bestFov = 1.0e9f;
	bool found = false;

	for (int mpi = 0; mpi < Config::AF_MP_COUNT; ++mpi) {
		if (!Config::autofire_hitboxes[mpi])
			continue;
		const int hb = kAfMpToHb[mpi];
		const float mpScale = Config::autofire_multipoint_scale[mpi];

		Vector_t points[9]{};
		const int nPts = Bones::CollectHitboxMultipoints(
			pawn, hb, mpScale, points, 9, &eye, bloom, targetAir);
		if (nPts <= 0 || nPts > 9)
			continue;

		for (int p = 0; p < nPts; ++p) {
			if (!Bones::IsValidPos(points[p]))
				continue;
			QAngle_t ang{};
			if (!CalcAngles(eye, points[p], ang))
				continue;
			const float fov = GetFov(crosshair, ang);
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

bool CanWeaponFire(C_CSWeaponBase* wep, C_CSPlayerPawn* lp) {
	if (!wep || !lp || !Mem::ValidEntity(wep) || !Mem::ValidEntity(lp))
		return false;
	if (lp->m_bIsDefusing() || lp->m_bIsGrabbingHostage())
		return false;
	if (lp->m_bWaitForNoAttack())
		return false;
	if (wep->m_bInReload())
		return false;
	const int clip = wep->m_iClip1();
	// <0 = weapon has no clip concept; 0 = empty
	if (clip == 0)
		return false;

	// Fire-rate / bolt / cycle: next primary attack tick vs controller tickbase
	__try {
		const std::int32_t nextTick = wep->m_nNextPrimaryAttackTick();
		const CBaseHandle hCtrl = lp->m_hController();
		if (hCtrl.valid() && I::GameEntity && I::GameEntity->Instance) {
			auto* ctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl);
			if (ctrl && Mem::ValidEntity(ctrl)) {
				const std::uint32_t tickBase = ctrl->m_nTickBase();
				if (nextTick > 0 && tickBase > 0
					&& nextTick > static_cast<std::int32_t>(tickBase) + 1)
					return false;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}

	return true;
}

// Smooth with flick catch-up: large error → near-snap so 0ms still feels responsive
static QAngle_t SmoothToward(const QAngle_t& cur, const QAngle_t& target, float smooth) {
	QAngle_t delta = target - cur;
	delta.Normalize();
	const float err = delta.Length2D();
	if (smooth <= 0.01f || err < 0.01f)
		return target;

	const float step = 1.f / std::clamp(smooth, 1.f, 50.f);
	float boost = 1.f;
	if (err > 18.f)
		boost = 12.f;                          // hard flick — almost snap
	else if (err > 10.f)
		boost = std::clamp(err / 8.f, 3.f, 8.f);
	else if (err > 4.f)
		boost = std::clamp(err / 16.f, 1.2f, 3.5f);

	float t = std::clamp(step * boost, 0.01f, 1.f);
	if (err > 22.f)
		t = 1.f;

	QAngle_t out{};
	out.x = cur.x + delta.x * t;
	out.y = cur.y + delta.y * t;
	out.z = 0.f;
	out.Normalize();
	out.x = std::clamp(out.x, -89.f, 89.f);
	return out;
}

float HitboxAimRadius(int hitbox) {
	switch (hitbox) {
	case Config::HB_HEAD:    return 4.5f;
	case Config::HB_NECK:    return 4.0f;
	case Config::HB_CHEST:   return 9.0f;
	case Config::HB_STOMACH: return 8.5f;
	case Config::HB_PELVIS:  return 8.0f;
	case Config::HB_ARMS:    return 5.5f;
	case Config::HB_LEGS:    return 6.0f;
	case Config::HB_FEET:    return 4.5f;
	default:                 return 7.0f;
	}
}

bool IsSniperWeapon(C_CSWeaponBase* weapon) {
	if (!weapon)
		return false;
	// Prefer item def — VData m_WeaponType schema miss can false-positive and block all fire
	const std::uint16_t def = weapon->m_iItemDefinitionIndex();
	// AWP=9, G3SG1=11, SCAR-20=38, SSG08=40
	return def == 9 || def == 11 || def == 38 || def == 40;
}

// True only when the angle that will fire is inside the aim hitbox cone.
bool IsCrosshairOnTarget(
	const Vector_t& eye,
	const QAngle_t& fireAngle,
	const Vector_t& aimPoint,
	int hitbox,
	bool sniper)
{
	QAngle_t toAim{};
	if (!CalcAngles(eye, aimPoint, toAim))
		return false;

	const float err = GetFov(fireAngle, toAim);
	if (!Mem::Finite(err))
		return false;

	const float dist = eye.Distance(aimPoint);
	if (dist < 1.f)
		return err <= 1.f;

	const float radius = HitboxAimRadius(hitbox);
	float maxErr = std::atan2(radius, dist) * kRad2Deg;
	maxErr *= sniper ? 0.7f : 1.0f;
	maxErr = std::clamp(maxErr, sniper ? 0.08f : 0.15f, sniper ? 0.6f : 2.0f);
	return err <= maxErr;
}

// Wall in front of aim point (hit something that is not the target).
bool IsBehindWall(const Vector_t& eye, const Vector_t& aimPoint, void* skip, void* target,
	std::uint64_t mask = Trace::kMaskVis) {
	// Inverse of IsVisible (owner-chain resolve + fraction threshold)
	if (!Trace::Ready())
		return false;
	return !Trace::IsVisible(eye, aimPoint, skip, target, mask, false);
}

bool ReadAimPunch(C_CSPlayerPawn* lp, QAngle_t& out) {
	if (!lp)
		return false;

	bool ok = false;
	__try {
		const uintptr_t base = reinterpret_cast<uintptr_t>(lp);
		uint32_t svcOff = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_pAimPunchServices"));
		if (!svcOff)
			svcOff = static_cast<uint32_t>(Offset::C_CSPlayerPawn::m_pAimPunchServices);

		void* services = *reinterpret_cast<void**>(base + svcOff);
		if (!services || !Mem::IsUserPtr(services))
			return false;

		uint32_t angOff = SchemaFinder::Get(hash_32_fnv1a_const("CCSPlayer_AimPunchServices->m_predictableBaseAngle"));
		if (!angOff)
			angOff = static_cast<uint32_t>(Offset::CCSPlayer_AimPunchServices::m_predictableBaseAngle);

		out = *reinterpret_cast<QAngle_t*>(reinterpret_cast<uintptr_t>(services) + angOff);
		ok = out.IsValid();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return ok;
}

int ReadShotsFired(C_CSPlayerPawn* lp) {
	if (!lp)
		return 0;
	int shots = 0;
	__try {
		uint32_t off = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_iShotsFired"));
		if (!off)
			off = static_cast<uint32_t>(Offset::C_CSPlayerPawn::m_iShotsFired);
		shots = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(lp) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return shots;
}

// Scaled punch for RCS (clamped)
bool GetScaledPunch(C_CSPlayerPawn* lp, QAngle_t& out) {
	QAngle_t punch{};
	if (!ReadAimPunch(lp, punch))
		return false;
	if (ReadShotsFired(lp) < 1)
		return false;
	// scale_x → pitch (x), scale_y → yaw (y) — matches menu labels historically
	out.x = std::clamp(punch.x, -12.f, 12.f) * kPunchScale * Config::rcs_scale_x;
	out.y = std::clamp(punch.y, -12.f, 12.f) * kPunchScale * Config::rcs_scale_y;
	out.z = 0.f;
	return out.IsValid();
}

bool GetViewAngles(QAngle_t& out) {
	if (!Input::GetViewAngles || !Input::viewAngleContext)
		return false;
	const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
	if (!viewPtr)
		return false;
	const Vector_t v = *reinterpret_cast<Vector_t*>(viewPtr);
	if (!std::isfinite(v.x) || !std::isfinite(v.y))
		return false;
	out = { v.x, v.y, 0.f };
	return true;
}

void SetViewAngles(const QAngle_t& ang) {
	if (!Input::SetViewAngle || !Input::viewAngleContext)
		return;
	Vector_t a = { ang.x, ang.y, 0.f };
	Input::SetViewAngle(Input::viewAngleContext, 0, &a);
}

// Standalone delta RCS only (no absolute punch on aim — avoids ground flick)
bool ApplyDeltaRcs(C_CSPlayerPawn* lp) {
	if (!lp)
		return false;

	QAngle_t punch{};
	if (!ReadAimPunch(lp, punch))
		return false;

	const int shots = ReadShotsFired(lp);
	const bool shooting = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && shots >= 1;

	if (!shooting) {
		g_oldPunch = punch;
		g_hadPunch = true;
		return false;
	}

	if (!g_hadPunch) {
		g_oldPunch = punch;
		g_hadPunch = true;
		return false;
	}

	QAngle_t delta = punch - g_oldPunch;

	const float oldMag = std::sqrt(g_oldPunch.x * g_oldPunch.x + g_oldPunch.y * g_oldPunch.y);
	const float newMag = std::sqrt(punch.x * punch.x + punch.y * punch.y);
	// Recoil recovering — don't fight recovery
	if (newMag + 0.02f < oldMag) {
		g_oldPunch = punch;
		return false;
	}

	g_oldPunch = punch;

	delta.x = std::clamp(delta.x, -3.f, 3.f);
	delta.y = std::clamp(delta.y, -3.f, 3.f);
	if (std::fabs(delta.x) < 0.0001f && std::fabs(delta.y) < 0.0001f)
		return false;

	QAngle_t view{};
	if (!GetViewAngles(view))
		return false;

	view.x -= delta.x * kPunchScale * Config::rcs_scale_x;
	view.y -= delta.y * kPunchScale * Config::rcs_scale_y;
	view.z = 0.f;
	view.Normalize();
	view.x = std::clamp(view.x, -89.f, 89.f);
	if (!view.IsValid())
		return false;

	SetViewAngles(view);
	return true;
}

bool StandaloneRcs(C_CSPlayerPawn* lp) {
	if (!Config::rcs_standalone || !lp)
		return false;
	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!pWpn || pWpn->IsNonGunWeapon()) {
		g_hadPunch = false;
		return false;
	}
	return ApplyDeltaRcs(lp);
}

// Heavily flashed → don't aim this frame
static bool IsBlinded(C_CSPlayerPawn* lp) {
	if (!lp)
		return false;
	__try {
		const float flash = lp->m_flFlashOverlayAlpha();
		return flash > 0.55f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool RunAimbot(C_CSPlayerPawn* lp) {
	if (!keybind.isActive(Config::aimbot)) {
		ResetHumanize();
		return false;
	}
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return false;
	if (!Mem::ValidEntity(lp))
		return false;
	if (IsBlinded(lp))
		return false;

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(pWpn) || pWpn->IsNonGunWeapon())
		return false;
	Config::ApplyWeaponGroup(pWpn);

	if (Config::aimbot_fov <= 0.f)
		return false;

	bool anyHb = false;
	for (int h = 0; h < Config::HB_COUNT; ++h) {
		if (Config::aim_hitboxes[h]) {
			anyHb = true;
			break;
		}
	}
	if (!anyHb)
		return false;

	const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0)
		return false;

	// Engine local shoot origin (NetClientInfo ShootPosition) — not m_vOldOrigin
	const Vector_t lep = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(lep))
		return false;

	QAngle_t qView{};
	if (!GetViewAngles(qView))
		return false;

	// FOV: compare against where bullets go (view + punch*2)
	QAngle_t qViewAim = qView;
	if (Config::rcs) {
		QAngle_t scaled{};
		if (GetScaledPunch(lp, scaled)) {
			qViewAim.x += scaled.x;
			qViewAim.y += scaled.y;
			qViewAim.Normalize();
		}
	}

	// Sticky FOV: locked target gets slightly wider window to reduce thrash
	const float baseFov = Config::aimbot_fov;
	const float stickyFov = baseFov * 1.15f;

	float bestFov = baseFov;
	QAngle_t bestAngle{};
	bool found = false;
	std::uint32_t bestHandle = 0;

	float lockedFov = stickyFov;
	QAngle_t lockedAngle{};
	bool lockedFound = false;

	const uint8_t localTeam = lp->getTeam();

	// Controllers only (nerv/ESP path) — raw C_CSPlayerPawn scan misses/ghosts slots
	int checkedPlayers = 0;
	for (int i = 1; i <= nMax; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity) || !Entity->handle().valid())
			continue;

		SchemaClassInfoData_t* cls = nullptr;
		__try {
			Entity->dump_class_info(&cls);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!cls || !Mem::Valid(cls, sizeof(void*)) || !cls->szName)
			continue;
		if (!Mem::IsReadable(cls->szName, 1))
			continue;
		if (HASH(cls->szName) != HASH("CCSPlayerController"))
			continue;

		if (checkedPlayers >= Mem::kMaxPlayers)
			continue;
		++checkedPlayers;

		auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
		if (Controller->IsLocalPlayer())
			continue;

		const CBaseHandle hPawn = Controller->m_hPawn();
		if (!hPawn.valid())
			continue;

		auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
		if (!Mem::ValidEntity(pawn))
			continue;

		// Confirm slot identity (same as ESP ResolvePawn)
		const CBaseHandle actual = pawn->handle();
		if (!actual.valid() || actual.index() != hPawn.index())
			continue;

		const int hp = Mem::ClampHealth(pawn->m_iHealth());
		if (hp < 1)
			continue;
		if (pawn->m_lifeState() != 0)
			continue;

		const uint8_t team = pawn->m_iTeamNum();
		if (!Mem::ValidTeam(static_cast<int>(team)))
			continue;
		if (GameMode::WantTeamCheck(Config::team_check) && team == localTeam)
			continue;

		const std::uint32_t pawnHandle = static_cast<std::uint32_t>(hPawn.index());
		const bool isLocked = (g_lockedTarget != 0 && pawnHandle == g_lockedTarget);
		const float maxFov = isLocked ? stickyFov : baseFov;

		for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
			if (!Config::aim_hitboxes[hb])
				continue;

			Vector_t points[8]{};
			const int nPts = Bones::CollectHitboxPoints(pawn, hb, points, 8);
			if (nPts <= 0 || nPts > 8)
				continue;

			for (int p = 0; p < nPts; ++p) {
				if (!Bones::IsValidPos(points[p]))
					continue;
				QAngle_t angle{};
				if (!CalcAngles(lep, points[p], angle))
					continue;

				const float fov = GetFov(qViewAim, angle);
				if (!Mem::Finite(fov) || fov > maxFov)
					continue;

				if (Config::aim_vis_check && Trace::Ready()) {
					if (!Trace::IsVisible(lep, points[p], lp, pawn, Trace::kMaskVis))
						continue;
				}

				if (fov < bestFov) {
					bestFov = fov;
					bestAngle = angle;
					found = true;
					bestHandle = pawnHandle;
				}

				if (isLocked && fov < lockedFov) {
					lockedFov = fov;
					lockedAngle = angle;
					lockedFound = true;
				}
			}
		}
	}

	if (!found) {
		// Brief miss while locked: keep humanize state, don't re-trigger delays
		if (g_lockedTarget != 0 && g_lockGrace > 0) {
			--g_lockGrace;
			g_blockFirstShot = g_firstShotArmed && !g_firstShotDone;
			return false;
		}
		ResetHumanize();
		return false;
	}
	g_lockGrace = kLockGraceFrames;

	const std::uint64_t now = NowMs();
	const float switchMs = std::clamp(Config::aim_target_switch_delay_ms, 0.f, 500.f);
	const float reactionMs = std::clamp(Config::aim_reaction_delay_ms, 0.f, 500.f);
	const float firstShotMs = std::clamp(Config::aim_first_shot_delay_ms, 0.f, 500.f);

	// Target switch delay
	if (g_lockedTarget == 0) {
		BeginEngagement(bestHandle, now);
	}
	else if (bestHandle != g_lockedTarget) {
		if (switchMs <= 0.01f) {
			BeginEngagement(bestHandle, now);
		}
		else {
			if (g_pendingTarget != bestHandle) {
				g_pendingTarget = bestHandle;
				g_switchReadyMs = now + static_cast<std::uint64_t>(switchMs);
			}
			if (now >= g_switchReadyMs)
				BeginEngagement(bestHandle, now);
		}
	}
	else {
		g_pendingTarget = 0;
		g_switchReadyMs = 0;
	}

	QAngle_t aimAngle{};
	if (g_lockedTarget == bestHandle) {
		aimAngle = bestAngle;
	}
	else if (lockedFound) {
		aimAngle = lockedAngle;
	}
	else {
		// Locked dead/out of FOV during switch wait — snap engagement to best
		BeginEngagement(bestHandle, now);
		aimAngle = bestAngle;
	}

	// Reaction time
	if (reactionMs > 0.01f && (now - g_lockAcquiredMs) < static_cast<std::uint64_t>(reactionMs)) {
		g_blockFirstShot = true;
		return false;
	}

	// First shot delay
	if (!g_firstShotArmed) {
		g_firstShotArmed = true;
		g_firstShotReadyMs = now + static_cast<std::uint64_t>(firstShotMs);
		g_firstShotDone = (firstShotMs <= 0.01f);
	}

	if (!g_firstShotDone) {
		if (now < g_firstShotReadyMs)
			g_blockFirstShot = true;
		else {
			g_blockFirstShot = false;
			g_firstShotDone = true;
		}
	}
	else {
		g_blockFirstShot = false;
	}

	// Absolute punch compensate so bullets land on bone (aimbot RCS)
	// view_set = aim - punch*2  ⇒  view + punch*2 = aim
	if (Config::rcs) {
		QAngle_t scaled{};
		if (GetScaledPunch(lp, scaled)) {
			aimAngle.x -= scaled.x;
			aimAngle.y -= scaled.y;
			aimAngle.Normalize();
		}
	}

	aimAngle.x = std::clamp(aimAngle.x, -89.f, 89.f);
	aimAngle.z = 0.f;
	if (!aimAngle.IsValid())
		return false;

	// Smooth toward compensated angle
	QAngle_t finalAngle = aimAngle;
	const float smooth = Config::aimbot_smooth;
	if (smooth > 0.01f) {
		QAngle_t cur{};
		if (GetViewAngles(cur))
			finalAngle = SmoothToward(cur, aimAngle, smooth);
	}

	if (!finalAngle.IsValid())
		return false;

	SetViewAngles(finalAngle);
	// Do NOT ApplyDeltaRcs while aimbot-RCS is active — absolute punch already applied
	return true;
}

// Autofire: own FOV/smooth/keybind — locks aim and injects attack when on target
bool RunAutofire(C_CSPlayerPawn* lp) {
	g_autofireWantShoot = false;
	g_autofireWantStop = false;
	g_autofireSilentValid = false;
	g_autofireSilentEye = Vector_t{ 0.f, 0.f, 0.f };
	if (!keybind.isActive(Config::autofire)) {
		ResetAutofire();
		return false;
	}
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return false;
	if (!Mem::ValidEntity(lp))
		return false;
	if (IsBlinded(lp))
		return false;

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(pWpn) || pWpn->IsNonGunWeapon())
		return false;
	Config::ApplyWeaponGroup(pWpn);

	if (Config::aimbot_fov <= 0.f)
		return false;

	bool anyHb = false;
	for (int h = 0; h < Config::AF_MP_COUNT; ++h) {
		if (Config::autofire_hitboxes[h]) {
			anyHb = true;
			break;
		}
	}
	if (!anyHb)
		return false;

	const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0)
		return false;

	// Engine local shoot origin (NetClientInfo ShootPosition) — not m_vOldOrigin
	const Vector_t lep = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(lep))
		return false;

	QAngle_t qView{};
	if (!GetViewAngles(qView))
		return false;

	QAngle_t qViewAim = qView;
	if (Config::rcs) {
		QAngle_t scaled{};
		if (GetScaledPunch(lp, scaled)) {
			qViewAim.x += scaled.x;
			qViewAim.y += scaled.y;
			qViewAim.Normalize();
		}
	}

	const float baseFov = Config::aimbot_fov;
	const float stickyFov = baseFov * 1.15f;

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
		|| (Config::autofire_autowall && minDmgAw > 0.f);

	// Bloom for dynamic multipoint: live GetInaccuracy+GetSpread vs hitbox radius
	float bloom = -1.f;
	if (Config::autofire_multipoint_dynamic && pWpn)
		bloom = LiveMultipointBloom(pWpn, lp);

	const uint8_t localTeam = lp->getTeam();
	int checkedPlayers = 0;

	for (int i = 1; i <= nMax; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity) || !Entity->handle().valid())
			continue;

		SchemaClassInfoData_t* cls = nullptr;
		__try {
			Entity->dump_class_info(&cls);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!cls || !Mem::Valid(cls, sizeof(void*)) || !cls->szName)
			continue;
		if (!Mem::IsReadable(cls->szName, 1))
			continue;
		if (HASH(cls->szName) != HASH("CCSPlayerController"))
			continue;

		if (checkedPlayers >= Mem::kMaxPlayers)
			continue;
		++checkedPlayers;

		auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
		if (Controller->IsLocalPlayer())
			continue;

		const CBaseHandle hPawn = Controller->m_hPawn();
		if (!hPawn.valid())
			continue;

		auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
		if (!Mem::ValidEntity(pawn))
			continue;

		const CBaseHandle actual = pawn->handle();
		if (!actual.valid() || actual.index() != hPawn.index())
			continue;

		const int hp = Mem::ClampHealth(pawn->m_iHealth());
		if (hp < 1)
			continue;
		if (pawn->m_lifeState() != 0)
			continue;

		const uint8_t team = pawn->m_iTeamNum();
		if (!Mem::ValidTeam(static_cast<int>(team)))
			continue;
		if (GameMode::WantTeamCheck(Config::team_check) && team == localTeam)
			continue;

		const std::uint32_t pawnHandle = static_cast<std::uint32_t>(hPawn.index());
		const bool isLocked = (g_afLocked != 0 && pawnHandle == g_afLocked);
		const float maxFov = isLocked ? stickyFov : baseFov;

		Vector_t pawnOrigin{};
		float worldDist = 1.0e12f;
		if (Bones::GetOrigin(pawn, pawnOrigin))
			worldDist = lep.Distance(pawnOrigin);

		const bool targetAir = (pawn->m_fFlags() & FL_ONGROUND) == 0;

		for (int mpi = 0; mpi < Config::AF_MP_COUNT; ++mpi) {
			if (!Config::autofire_hitboxes[mpi])
				continue;

			const int hb = kAfMpToHb[mpi];
			const float mpScale = Config::autofire_multipoint_scale[mpi];

			Vector_t points[9]{};
			const int nPts = Bones::CollectHitboxMultipoints(
				pawn, hb, mpScale, points, 9,
				&lep, bloom, targetAir);
			if (nPts <= 0 || nPts > 9)
				continue;

			for (int p = 0; p < nPts; ++p) {
				if (!Bones::IsValidPos(points[p]))
					continue;
				QAngle_t angle{};
				if (!CalcAngles(lep, points[p], angle))
					continue;

				const float fov = GetFov(qViewAim, angle);
				if (!Mem::Finite(fov) || fov > maxFov)
					continue;

				if (Config::aim_vis_check && Trace::Ready() && !Config::autofire_autowall) {
					if (!Trace::IsVisible(lep, points[p], lp, pawn, Trace::kMaskVis))
						continue;
				}

				const bool behindWall = IsBehindWall(
					lep, points[p], lp, pawn, Trace::kMaskShot);

				// No AW: never select points with a wall in front
				if (behindWall && !Config::autofire_autowall)
					continue;

				float dmg = 0.f;
				if (needDamageScan || Config::autofire_autowall || behindWall) {
					// AW on → real pen path (also covers visible); else estimate
					const AutoWall::Result aw = AutoWall::Fire(
						lep, points[p], hb, pWpn, lp, pawn, Config::autofire_autowall);
					dmg = aw.hit ? aw.damage : 0.f;
					if (dmg < 1.f)
						continue;

					// Min-damage filter at select (lethal if HP < mindmg)
					float need = (behindWall || aw.penetrated) ? minDmgAw : minDmgVis;
					if (need > 0.f) {
						if (static_cast<float>(hp) < need)
							need = static_cast<float>(hp);
						if (dmg + 0.01f < need)
							continue;
					}
				}

				const bool lethal = needDamageScan && dmg + 0.01f >= static_cast<float>(hp);
				const bool bestLethal = needDamageScan && bestDmg + 0.01f >= static_cast<float>(
					bestPawn ? Mem::ClampHealth(bestPawn->m_iHealth()) : 0);
				const bool lockedLethal = needDamageScan && lockedDmg + 0.01f >= static_cast<float>(
					lockedPawn ? Mem::ClampHealth(lockedPawn->m_iHealth()) : 0);

				// Gamesense-style SortTargets
				auto Prefer = [&](bool haveRef, float refFov, float refDist, float refDmg, bool refLethal) -> bool {
					if (!haveRef)
						return true;
					switch (selectMode) {
					case Config::AF_TARGET_DISTANCE:
						if (worldDist + 1.f < refDist)
							return true;
						if (std::fabs(worldDist - refDist) <= 1.f && fov < refFov)
							return true;
						return false;
					case Config::AF_TARGET_DAMAGE:
						if (lethal && !refLethal)
							return true;
						if (lethal == refLethal) {
							if (dmg > refDmg + 0.5f)
								return true;
							if (std::fabs(dmg - refDmg) <= 0.5f && fov < refFov)
								return true;
						}
						return false;
					case Config::AF_TARGET_CROSSHAIR:
					default:
						return fov < refFov;
					}
				};

				if (Prefer(found, bestFov, bestDist, bestDmg, bestLethal)) {
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

				if (isLocked && Prefer(lockedFound, lockedFov, lockedDist, lockedDmg, lockedLethal)) {
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

	if (!found) {
		if (g_afLocked != 0 && g_afGrace > 0) {
			--g_afGrace;
			g_afBlockFirst = true; // don't shoot on FOV miss
			// Keep frame ownership so aimbot doesn't take over; hold last aim
			if (g_afLastAimValid && !Config::autofire_silent && g_afLastAimAngle.IsValid())
				SetViewAngles(g_afLastAimAngle);
			return true;
		}
		ResetAutofire();
		return false;
	}
	g_afGrace = kLockGraceFrames;

	const std::uint64_t now = NowMs();
	const float switchMs = std::clamp(Config::aim_target_switch_delay_ms, 0.f, 500.f);
	const float reactionMs = std::clamp(Config::aim_reaction_delay_ms, 0.f, 500.f);
	const float firstShotMs = std::clamp(Config::aim_first_shot_delay_ms, 0.f, 500.f);

	if (g_afLocked == 0) {
		BeginAutofireEngagement(bestHandle, now);
	}
	else if (bestHandle != g_afLocked) {
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

	// Reaction: aim but don't shoot (keep ownership — never return false here)
	const bool inReaction = reactionMs > 0.01f
		&& (now - g_afLockMs) < static_cast<std::uint64_t>(reactionMs);
	if (inReaction) {
		g_afBlockFirst = true;
	} else {
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
		}
		else {
			g_afBlockFirst = false;
		}
	}

	if (Config::rcs) {
		QAngle_t scaled{};
		if (GetScaledPunch(lp, scaled)) {
			aimAngle.x -= scaled.x;
			aimAngle.y -= scaled.y;
			aimAngle.Normalize();
		}
	}

	aimAngle.x = std::clamp(aimAngle.x, -89.f, 89.f);
	aimAngle.z = 0.f;
	if (!aimAngle.IsValid())
		return false;

	const bool silent = Config::autofire_silent;
	QAngle_t shotAngle = aimAngle;

	if (!silent) {
		// Visible aim: smooth view toward bone
		QAngle_t finalAngle = aimAngle;
		const float smooth = Config::aimbot_smooth;
		if (smooth > 0.01f) {
			QAngle_t cur{};
			if (GetViewAngles(cur))
				finalAngle = SmoothToward(cur, aimAngle, smooth);
		}
		if (!finalAngle.IsValid())
			return false;
		SetViewAngles(finalAngle);
		shotAngle = finalAngle;
	}
	// Silent: leave crosshair alone — only subtick shot angle aims

	g_afLastAimAngle = silent ? aimAngle : shotAngle;
	g_afLastAimPoint = aimPoint;
	g_afLastHb = aimHb;
	g_afLastAimValid = true;

	if (!g_afBlockFirst && CanWeaponFire(pWpn, lp)) {
		const Vector_t vel = lp->m_vecAbsVelocity();
		const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
		const bool onGround = (lp->m_fFlags() & FL_ONGROUND) != 0;
		const bool sniper = IsSniperWeapon(pWpn);

		// Snipers: never fire unscoped
		const bool scopedOk = !sniper || lp->m_bIsScoped();

		Vector_t shotPoint = aimPoint;
		int shotHb = aimHb;
		const QAngle_t& aimRef = silent ? aimAngle : shotAngle;
		if (!silent && aimTarget) {
			Vector_t closest{};
			int closestHb = aimHb;
			if (FindClosestAutofireBone(lep, aimRef, aimTarget, pWpn, closest, closestHb)) {
				shotPoint = closest;
				shotHb = closestHb;
			}
		}

		QAngle_t fireAngle = aimRef;
		if (silent) {
			QAngle_t toShot{};
			if (CalcAngles(lep, shotPoint, toShot)) {
				if (Config::rcs) {
					QAngle_t scaled{};
					if (GetScaledPunch(lp, scaled)) {
						toShot.x -= scaled.x;
						toShot.y -= scaled.y;
						toShot.Normalize();
					}
				}
				toShot.x = std::clamp(toShot.x, -89.f, 89.f);
				toShot.z = 0.f;
				if (toShot.IsValid())
					fireAngle = toShot;
			}
		}

		const bool onTarget = IsCrosshairOnTarget(
			lep, fireAngle, shotPoint, shotHb, sniper);

		// dmgOk  = real hit path (LOS or pen) + mindamage
		// hcOk   = spread would land — only required to shoot, not to stop
		// Autostop on dmgOk so running + high HC still brakes for wall shots.
		bool dmgOk = false;
		bool hcOk = false;
		if (scopedOk && onTarget && aimTarget) {
			const bool behindWall = IsBehindWall(
				lep, shotPoint, lp, aimTarget, Trace::kMaskShot);

			if (behindWall && !Config::autofire_autowall) {
				dmgOk = false;
			} else {
				const bool allowPen = behindWall && Config::autofire_autowall;
				const float minDmg = allowPen
					? Config::autofire_mindamage_aw
					: Config::autofire_mindamage;

				if (!allowPen) {
					// Visible only: require LOS + mindmg (estimate, no pen)
					bool losOk = true;
					if (Trace::Ready())
						losOk = Trace::IsVisible(
							lep, shotPoint, lp, aimTarget, Trace::kMaskVis);
					dmgOk = losOk && AutoWall::PassesMinDamage(
						lep, shotPoint, shotHb, pWpn, lp, aimTarget, false, minDmg);
				} else {
					// Wall: AutoWall::Fire(allowPen) must actually hit target
					// (game pen or TraceLine fallback) + mindamage_aw
					const AutoWall::Result aw = AutoWall::Fire(
						lep, shotPoint, shotHb, pWpn, lp, aimTarget, true);
					if (aw.hit && aw.damage > 0.f) {
						float need = minDmg;
						if (need > 0.f) {
							const int hp = aimTarget->m_iHealth();
							if (hp > 0 && static_cast<float>(hp) < need)
								need = static_cast<float>(hp);
							dmgOk = aw.damage + 0.01f >= need;
						} else {
							dmgOk = true;
						}
					}
				}

				hcOk = dmgOk && HitChance::Passes(
					lep, fireAngle, shotPoint, shotHb, pWpn,
					Config::autofire_hitchance, lp, aimTarget);
			}
		}

		// Stop when pen+mindmg OK (even if HC fails while moving).
		// Shoot only when HC passes and speed is settled (or HC models run bloom).
		if (dmgOk) {
			const bool needStop = Config::autofire_autostop && onGround
				&& std::isfinite(speed2d) && speed2d > kAfStopSpeed;
			// Hard run gate only when hitchance is off — live GetInaccuracy already
			// folds move/air into Passes() when HC > 0.
			const bool tooFastNoStop = !Config::autofire_autostop && onGround
				&& Config::autofire_hitchance <= 0.01f
				&& std::isfinite(speed2d) && speed2d > kNoHcRunSpeed;

			if (needStop) {
				g_autofireWantStop = true;
			} else if (hcOk && !tooFastNoStop) {
				g_autofireWantShoot = true;
				g_autofireSilentAngle = fireAngle;
				g_autofireSilentEye = lep;
				g_autofireSilentValid = true;
			}
		}
	}

	return true;
}

// Detection uses full capsule so fast flicks still register.
// Quality / centerBias is applied when scoring samples (see TriggerScoreHit).
static float TriggerCapsuleScale(bool sniperScoped)
{
	return sniperScoped ? 0.98f : 1.0f;
}

// Steady aim: require ray closer to bone (kills edge early-fire).
// Flick: accept full capsule so a one-frame sweep still fires.
static float TriggerSteadyBias(bool sniperScoped)
{
	return sniperScoped ? 0.72f : 0.78f;
}

static float TriggerFlickBias()
{
	return 1.0f;
}

// How centered is the ray on allowed hitboxes? Higher = better (bone-center).
// 3 = tight multipoint, 2 = mid, 1 = full capsule edge, 0 = miss.
static int TriggerScoreHit(
	const Vector_t& eye,
	const Vector_t& dir,
	C_CSPlayerPawn* pawn,
	float radiusScale,
	int& outHb,
	Vector_t& outPoint)
{
	outHb = Config::HB_HEAD;
	outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!pawn)
		return 0;

	static constexpr int kAllHb[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS,
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};
	static constexpr float kBiasTier[] = { 0.55f, 0.75f, 1.0f };

	// Prefer tighter (more inward multipoint) hits
	for (int tier = 0; tier < 3; ++tier) {
		const float bias = kBiasTier[tier];
		float bestT = 1.0e12f;
		int bestHb = Config::HB_HEAD;
		Vector_t bestPt{};
		bool found = false;
		for (int hb : kAllHb) {
			if (hb < 0 || hb >= Config::HB_COUNT || !Config::trigger_hitboxes[hb])
				continue;
			float t = 0.f;
			Vector_t pt{};
			if (!Bones::RayHitsConfiguredHitbox(
					pawn, hb, eye, dir, radiusScale, t, pt, bias))
				continue;
			if (t >= bestT)
				continue;
			bestT = t;
			bestHb = hb;
			bestPt = pt;
			found = true;
		}
		if (found) {
			outHb = bestHb;
			outPoint = bestPt;
			return 3 - tier; // 3, 2, or 1
		}
	}
	return 0;
}

static bool CrosshairOnHitbox(
	const Vector_t& eye,
	const Vector_t& dir,
	C_CSPlayerPawn* pawn,
	bool sniperScoped,
	int& outHb,
	Vector_t& outPoint,
	float centerBias = -1.f)
{
	if (!pawn)
		return false;

	const float radiusScale = TriggerCapsuleScale(sniperScoped);
	const float bias = (centerBias > 0.f)
		? centerBias
		: TriggerSteadyBias(sniperScoped);

	static constexpr int kAllHb[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS,
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};

	float bestT = 1.0e12f;
	int bestHb = Config::HB_HEAD;
	Vector_t bestPt{};
	bool found = false;

	for (int hb : kAllHb) {
		if (hb < 0 || hb >= Config::HB_COUNT || !Config::trigger_hitboxes[hb])
			continue;

		float t = 0.f;
		Vector_t pt{};
		if (!Bones::RayHitsConfiguredHitbox(
				pawn, hb, eye, dir, radiusScale, t, pt, bias))
			continue;
		if (t >= bestT)
			continue;

		bestT = t;
		bestHb = hb;
		bestPt = pt;
		found = true;
	}

	if (!found)
		return false;
	outHb = bestHb;
	outPoint = bestPt;
	return true;
}

static QAngle_t LerpTriggerAng(const QAngle_t& a, const QAngle_t& b, float t)
{
	QAngle_t o{};
	o.x = a.x + (b.x - a.x) * t;
	float dy = b.y - a.y;
	if (dy > 180.f) dy -= 360.f;
	if (dy < -180.f) dy += 360.f;
	o.y = a.y + dy * t;
	o.z = 0.f;
	o.Normalize();
	return o;
}

static float TriggerAngDelta(const QAngle_t& a, const QAngle_t& b)
{
	float dy = std::fabs(a.y - b.y);
	if (dy > 180.f)
		dy = 360.f - dy;
	const float dx = std::fabs(a.x - b.x);
	return (std::max)(dx, dy);
}

// Fast path: hit entity is already C_CSPlayerPawn.
static C_CSPlayerPawn* ResolveEnemyPawn(
	void* hitEnt,
	C_CSPlayerPawn* lp,
	uint8_t localTeam)
{
	if (!hitEnt || !lp || !Mem::ValidEntity(hitEnt))
		return nullptr;

	auto* asBase = reinterpret_cast<C_BaseEntity*>(hitEnt);
	C_CSPlayerPawn* pawn = nullptr;

	if (asBase->IsBasePlayer()) {
		pawn = reinterpret_cast<C_CSPlayerPawn*>(hitEnt);
	} else {
		// Rare: hit something else — fall back to controller match
		if (!I::GameEntity || !I::GameEntity->Instance)
			return nullptr;
		const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
		int checked = 0;
		for (int i = 1; i <= nMax; ++i) {
			auto* Entity = I::GameEntity->Instance->Get(i);
			if (!Mem::ValidEntity(Entity) || !Entity->handle().valid())
				continue;
			SchemaClassInfoData_t* cls = nullptr;
			__try { Entity->dump_class_info(&cls); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
				continue;
			if (HASH(cls->szName) != HASH("CCSPlayerController"))
				continue;
			if (checked >= Mem::kMaxPlayers)
				break;
			++checked;
			auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
			if (Controller->IsLocalPlayer())
				continue;
			const CBaseHandle hPawn = Controller->m_hPawn();
			if (!hPawn.valid())
				continue;
			auto* p = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
			if (p && static_cast<void*>(p) == hitEnt) {
				pawn = p;
				break;
			}
		}
	}

	if (!Mem::ValidEntity(pawn))
		return nullptr;

	const int hp = Mem::ClampHealth(pawn->m_iHealth());
	if (hp < 1 || pawn->m_lifeState() != 0)
		return nullptr;

	const uint8_t team = pawn->m_iTeamNum();
	if (!Mem::ValidTeam(static_cast<int>(team)))
		return nullptr;
	if (GameMode::WantTeamCheck(Config::team_check) && team == localTeam)
		return nullptr;
	if (pawn == lp)
		return nullptr;

	return pawn;
}

static bool ReadCmdViewAngles(CUserCmd* cmd, QAngle_t& out) {
	if (!cmd)
		return false;
	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base || !base->pViewAngles)
		return false;
	out = base->pViewAngles->angValue;
	out.z = 0.f;
	return out.IsValid();
}

// IDA CreateMove (0x180C97330): attack uses attack1_start_history_index into
// CSGOInputHistoryEntryPB. During flicks base angles lag; history holds the tip.
struct TriggerAngleSample {
	QAngle_t ang{};
	int histIndex = -1; // >=0 → inputHistory slot
};

static int CollectTriggerAngles(CUserCmd* cmd, TriggerAngleSample* out, int maxOut, bool pixelPerfect)
{
	if (!out || maxOut < 1)
		return 0;
	int n = 0;

	auto push = [&](const QAngle_t& ang, int histIdx) {
		if (n >= maxOut || !ang.IsValid())
			return;
		QAngle_t a = ang;
		a.z = 0.f;
		a.Normalize();
		// Tighter dedupe so dense flick history isn't collapsed to 1–2 samples
		for (int i = 0; i < n; ++i) {
			if (std::fabs(out[i].ang.x - a.x) < 0.008f
				&& std::fabs(out[i].ang.y - a.y) < 0.008f)
				return;
		}
		out[n].ang = a;
		out[n].histIndex = histIdx;
		++n;
	};

	// Scoped / pixel-perfect: reticle only (history tip leads AWP early-fires).
	// Still take newest history as secondary so a 1-tick flick still registers.
	if (pixelPerfect) {
		QAngle_t cam{};
		if (GetViewAngles(cam))
			push(cam, -1);
		QAngle_t base{};
		if (ReadCmdViewAngles(cmd, base))
			push(base, -1);
		if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
			const int count = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			if (count > 0) {
				CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(count - 1);
				if (e && e->pViewAngles)
					push(e->pViewAngles->angValue, count - 1);
			}
		}
		return n;
	}

	// Unscoped: ALL recent history (newest first) — max flick coverage this tick
	if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
		const int count = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
		const int take = (std::min)(count, maxOut - 2);
		const int start = count - take;
		for (int i = count - 1; i >= start && i >= 0; --i) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(i);
			if (!e || !e->pViewAngles)
				continue;
			push(e->pViewAngles->angValue, i);
		}
	}

	QAngle_t cam{};
	if (GetViewAngles(cam))
		push(cam, -1);

	QAngle_t base{};
	if (ReadCmdViewAngles(cmd, base))
		push(base, -1);

	return n;
}

static bool TriggerIsFlicking(const TriggerAngleSample* samples, int n)
{
	float maxSpan = 0.f;
	if (samples && n >= 2) {
		for (int i = 0; i < n; ++i) {
			for (int j = i + 1; j < n; ++j) {
				const float d = TriggerAngDelta(samples[i].ang, samples[j].ang);
				if (d > maxSpan)
					maxSpan = d;
			}
		}
	}
	// ~1.1° within one cmd OR sustained high mouse speed
	return maxSpan >= 1.1f || g_triggerAngSpeed >= 0.85f;
}

// Engine finds which pawn; capsule multipoint decides WHERE (stops hairline early-fire).
// outScore: 3=center, 2=mid, 1=edge, 0=miss
static bool TriggerRayHitsDir(
	const Vector_t& lep,
	Vector_t fwd,
	C_CSPlayerPawn* lp,
	uint8_t localTeam,
	bool sniperScoped,
	float minBias,
	int minScore,
	C_CSPlayerPawn*& outTarget,
	Vector_t& outPoint,
	int& outHb,
	int& outScore)
{
	outTarget = nullptr;
	outHb = Config::HB_HEAD;
	outScore = 0;

	const float fl = fwd.Length();
	if (fl < 1e-4f || !Mem::Finite(fl))
		return false;
	fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;

	constexpr float kTraceDist = 8192.f;
	const Vector_t end{
		lep.x + fwd.x * kTraceDist,
		lep.y + fwd.y * kTraceDist,
		lep.z + fwd.z * kTraceDist
	};

	Trace::CGameTrace tr{};
	if (!Trace::TraceLine(lep, end, lp, tr, Trace::kMaskShot))
		return false;
	if (!Trace::DidHit(tr))
		return false;

	C_CSPlayerPawn* target = ResolveEnemyPawn(tr.hit_entity(), lp, localTeam);
	if (!target)
		return false;

	const float radiusScale = TriggerCapsuleScale(sniperScoped);
	int hitHb = Config::HB_HEAD;
	Vector_t hitPoint{};
	const int score = TriggerScoreHit(lep, fwd, target, radiusScale, hitHb, hitPoint);
	if (score < minScore)
		return false;

	// Reject edge if caller demanded a tighter multipoint floor
	if (minBias < 0.999f) {
		float t = 0.f;
		Vector_t pt{};
		if (!Bones::RayHitsConfiguredHitbox(
				target, hitHb, lep, fwd, radiusScale, t, pt, minBias))
			return false;
		hitPoint = pt;
	}

	if (sniperScoped) {
		Trace::CGameTrace tr2{};
		const Vector_t past{
			hitPoint.x + fwd.x * 2.f,
			hitPoint.y + fwd.y * 2.f,
			hitPoint.z + fwd.z * 2.f
		};
		if (Trace::TraceLine(lep, past, lp, tr2, Trace::kMaskShot) && Trace::DidHit(tr2)) {
			C_CSPlayerPawn* hitPawn = ResolveEnemyPawn(tr2.hit_entity(), lp, localTeam);
			if (hitPawn != target)
				return false;
		}
	}

	outTarget = target;
	outPoint = hitPoint;
	outHb = hitHb;
	outScore = score;
	return true;
}

static bool TriggerRayHits(
	const Vector_t& lep,
	const QAngle_t& aimView,
	C_CSPlayerPawn* lp,
	uint8_t localTeam,
	bool sniperScoped,
	float minBias,
	int minScore,
	C_CSPlayerPawn*& outTarget,
	Vector_t& outPoint,
	int& outHb,
	int& outScore)
{
	Vector_t fwd{};
	aimView.ToDirections(&fwd, nullptr, nullptr);
	return TriggerRayHitsDir(
		lep, fwd, lp, localTeam, sniperScoped, minBias, minScore,
		outTarget, outPoint, outHb, outScore);
}

// Expand discrete history samples with lerped angles along the flick path so
// a sub-frame head cross is still detected (main cause of "miss on fast flick").
static int ExpandTriggerAngles(
	const TriggerAngleSample* in, int nIn,
	TriggerAngleSample* out, int maxOut,
	bool flicking)
{
	if (!in || !out || maxOut < 1 || nIn <= 0)
		return 0;

	int n = 0;
	auto push = [&](const QAngle_t& ang, int histIdx) {
		if (n >= maxOut || !ang.IsValid())
			return;
		QAngle_t a = ang;
		a.z = 0.f;
		a.Normalize();
		for (int i = 0; i < n; ++i) {
			if (std::fabs(out[i].ang.x - a.x) < 0.004f
				&& std::fabs(out[i].ang.y - a.y) < 0.004f)
				return;
		}
		out[n].ang = a;
		out[n].histIndex = histIdx;
		++n;
	};

	// denser subdivision while flicking
	const int steps = flicking ? 6 : 2;

	for (int i = 0; i < nIn; ++i) {
		push(in[i].ang, in[i].histIndex);
		if (i + 1 >= nIn)
			continue;
		const float span = TriggerAngDelta(in[i].ang, in[i + 1].ang);
		if (span < 0.15f)
			continue;
		// More steps when span is large
		const int sub = (std::min)(steps + static_cast<int>(span * 2.f), 12);
		for (int s = 1; s < sub; ++s) {
			const float t = static_cast<float>(s) / static_cast<float>(sub);
			// Bind interpolated angle to nearest real history slot (for attack index)
			const int hist = (t < 0.5f) ? in[i].histIndex : in[i + 1].histIndex;
			push(LerpTriggerAng(in[i].ang, in[i + 1].ang, t), hist);
		}
	}
	return n;
}

// IDA CSBaseGunFire → sub_1807CFC80: seed tick is history entry +0x68
// (nPlayerTickCount), NOT +0x60 (nRenderTickCount). Seed call is
// SPREADSEEDGEN(player, &ang, v58) where v58 is built from player tick.
static int GetSeedTickFromEntry(CCSGOInputHistoryEntryPB* e)
{
	if (!e)
		return 0;
	if (e->nPlayerTickCount > 0)
		return e->nPlayerTickCount;
	if (e->nRenderTickCount > 0)
		return e->nRenderTickCount;
	return 0;
}

static int GetRenderTick(CUserCmd* cmd, int histIndex, C_CSPlayerPawn* lp)
{
	if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
		const int count = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
		int idx = histIndex;
		if (idx < 0 || idx >= count)
			idx = count > 0 ? count - 1 : -1;
		if (idx >= 0) {
			const int t = GetSeedTickFromEntry(cmd->GetInputHistoryEntry(idx));
			if (t > 0)
				return t;
		}
		for (int i = count - 1; i >= 0; --i) {
			const int t = GetSeedTickFromEntry(cmd->GetInputHistoryEntry(i));
			if (t > 0)
				return t;
		}
	}

	// Fallback: tickbase (matches public seed-trigger samples)
	__try {
		const CBaseHandle hCtrl = lp->m_hController();
		if (hCtrl.valid() && I::GameEntity && I::GameEntity->Instance) {
			auto* ctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl);
			if (ctrl && Mem::ValidEntity(ctrl)) {
				const int tb = static_cast<int>(ctrl->m_nTickBase());
				if (tb > 0)
					return tb;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return 0;
}

static bool HistoryShootPos(CCSGOInputHistoryEntryPB* e, Vector_t& out)
{
	if (!e || !e->pShootPosition)
		return false;
	const Vector4D_t& v = e->pShootPosition->vecValue;
	if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
		return false;
	if (v.x == 0.f && v.y == 0.f && v.z == 0.f)
		return false;
	out = { v.x, v.y, v.z };
	return Bones::IsValidPos(out);
}

// Triggerbot: fire when any this-tick angle sample (history tip / camera / base)
// clips a hitbox. Stamps winning angle + attack1 history index for flick sync.
bool RunTriggerbot(C_CSPlayerPawn* lp, CUserCmd* cmd) {
	g_triggerWantShoot = false;
	g_triggerWantStop = false;
	g_triggerFireValid = false;
	g_triggerFireHist = -1;

	if (!keybind.isActive(Config::triggerbot)) {
		ResetTriggerbot();
		return false;
	}
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return false;
	if (!Mem::ValidEntity(lp) || IsBlinded(lp))
		return false;
	if (!Trace::Ready())
		return false;

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(pWpn) || pWpn->IsNonGunWeapon()) {
		ResetTriggerbot();
		return false;
	}
	Config::ApplyWeaponGroup(pWpn);

	const bool sniper = IsSniperWeapon(pWpn);
	const bool scoped = lp->m_bIsScoped();
	if (Config::trigger_scoped_only && sniper && !scoped) {
		ResetTriggerbot();
		return false;
	}

	// Default eye = engine ShootPosition; per-sample history pShootPosition overrides below
	const Vector_t lepDefault = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(lepDefault))
		return false;

	const uint8_t localTeam = lp->getTeam();
	const bool sniperScoped = sniper && scoped;

	// Mouse speed EMA (deg/frame) — catches flicks even when history is thin
	{
		QAngle_t cam{};
		if (GetViewAngles(cam)) {
			cam.z = 0.f;
			if (g_triggerPrevCamOk) {
				const float d = TriggerAngDelta(g_triggerPrevCam, cam);
				g_triggerAngSpeed = g_triggerAngSpeed * 0.55f + d * 0.45f;
			}
			g_triggerPrevCam = cam;
			g_triggerPrevCamOk = true;
		}
	}

	TriggerAngleSample raw[24]{};
	const int nRaw = CollectTriggerAngles(cmd, raw, 24, sniperScoped);
	if (nRaw <= 0)
		return false;

	const bool flicking = TriggerIsFlicking(raw, nRaw);

	// Path-subdivide: catch head crossed between history samples on fast flicks
	TriggerAngleSample samples[64]{};
	const int nSamples = ExpandTriggerAngles(raw, nRaw, samples, 64, flicking);
	if (nSamples <= 0)
		return false;

	const bool seedMode = Config::trigger_mode == Config::TR_MODE_SEED_NOSPREAD
		&& HitChance::SpreadSeedReady();

	// Steady: require mid multipoint (score>=2, bias~0.78) — no edge early-fire.
	// Flick: full capsule (score>=1) so sub-frame sweeps still fire.
	const float minBias = flicking
		? TriggerFlickBias()
		: TriggerSteadyBias(sniperScoped);
	const int minScore = flicking ? 1 : 2;

	C_CSPlayerPawn* target = nullptr;
	Vector_t hitPoint{};
	int hitHb = Config::HB_HEAD;
	QAngle_t aimView{};
	Vector_t winEye = lepDefault;
	int winHist = -1;
	bool onTarget = false;
	int bestScore = 0;
	int bestIdx = -1;

	// Scan ALL samples; keep the most bone-centered hit (not first edge graze).
	// Eye priority per sample: history pShootPosition → engine ShootPos.
	for (int i = 0; i < nSamples; ++i) {
		Vector_t eye = lepDefault;
		if (samples[i].histIndex >= 0 && cmd) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(samples[i].histIndex);
			Vector_t sp{};
			if (HistoryShootPos(e, sp))
				eye = sp;
		}

		C_CSPlayerPawn* t = nullptr;
		Vector_t pt{};
		int hb = Config::HB_HEAD;
		int score = 0;
		if (!TriggerRayHits(
				eye, samples[i].ang, lp, localTeam,
				sniperScoped, minBias, minScore, t, pt, hb, score))
			continue;

		// Prefer higher score (more inward). On tie: prefer real history over
		// pure lerp, then earlier-in-list (newer raw samples first).
		const bool better =
			score > bestScore
			|| (score == bestScore && bestIdx < 0)
			|| (score == bestScore && samples[i].histIndex >= 0
				&& (bestIdx < 0 || samples[bestIdx].histIndex < 0));
		if (!better)
			continue;

		bestScore = score;
		bestIdx = i;
		target = t;
		hitPoint = pt;
		hitHb = hb;
		aimView = samples[i].ang;
		aimView.z = 0.f;
		winEye = eye;
		winHist = samples[i].histIndex;
		onTarget = true;

		// Perfect center hit — no need to keep scanning
		if (score >= 3)
			break;
	}

	// Autowall fallback only if no clear ray hit (uses best camera/base sample)
	if (!onTarget && Config::trigger_autowall) {
		aimView = raw[0].ang;
		winHist = raw[0].histIndex;

		Vector_t eye = lepDefault;
		if (winHist >= 0 && cmd) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(winHist);
			Vector_t sp{};
			if (HistoryShootPos(e, sp))
				eye = sp;
		}
		winEye = eye;

		Vector_t fwd{};
		aimView.ToDirections(&fwd, nullptr, nullptr);
		const float fl = fwd.Length();
		if (fl < 1e-4f || !Mem::Finite(fl))
			return false;
		fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;
		const float radiusScale = TriggerCapsuleScale(sniperScoped);

		const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
		float bestT = 1.0e12f;
		C_CSPlayerPawn* best = nullptr;
		Vector_t bestPt{};
		int bestHb = Config::HB_HEAD;
		int checked = 0;

		static constexpr int kAwHb[] = {
			Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
			Config::HB_STOMACH, Config::HB_PELVIS,
			Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
		};

		for (int i = 1; i <= nMax; ++i) {
			auto* Entity = I::GameEntity->Instance->Get(i);
			if (!Mem::ValidEntity(Entity) || !Entity->handle().valid())
				continue;
			SchemaClassInfoData_t* cls = nullptr;
			__try { Entity->dump_class_info(&cls); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
				continue;
			if (HASH(cls->szName) != HASH("CCSPlayerController"))
				continue;
			if (checked >= Mem::kMaxPlayers)
				break;
			++checked;

			auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
			if (Controller->IsLocalPlayer())
				continue;
			const CBaseHandle hPawn = Controller->m_hPawn();
			if (!hPawn.valid())
				continue;
			auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
			if (!Mem::ValidEntity(pawn))
				continue;
			const int hp = Mem::ClampHealth(pawn->m_iHealth());
			if (hp < 1 || pawn->m_lifeState() != 0)
				continue;
			const uint8_t team = pawn->m_iTeamNum();
			if (!Mem::ValidTeam(static_cast<int>(team)))
				continue;
			if (GameMode::WantTeamCheck(Config::team_check) && team == localTeam)
				continue;

			for (int hb : kAwHb) {
				if (hb < 0 || hb >= Config::HB_COUNT || !Config::trigger_hitboxes[hb])
					continue;
				float t = 0.f;
				Vector_t pt{};
				// AW uses steady multipoint (never full edge)
				if (!Bones::RayHitsConfiguredHitbox(
						pawn, hb, eye, fwd, radiusScale, t, pt,
						TriggerSteadyBias(sniperScoped)))
					continue;
				const float minDmg = Config::autofire_mindamage_aw > 0.f
					? Config::autofire_mindamage_aw
					: Config::autofire_mindamage;
				if (!AutoWall::PassesMinDamage(eye, pt, hb, pWpn, lp, pawn, true, minDmg))
					continue;
				if (t >= bestT)
					continue;
				bestT = t;
				best = pawn;
				bestPt = pt;
				bestHb = hb;
			}
		}
		if (best) {
			target = best;
			hitPoint = bestPt;
			hitHb = bestHb;
			onTarget = true;
		}
	}

	const std::uint64_t now = NowMs();
	if (!onTarget || !target) {
		g_triggerWasOn = false;
		g_triggerOnSinceMs = 0;
		return false;
	}

	if (!g_triggerWasOn) {
		g_triggerWasOn = true;
		g_triggerOnSinceMs = now;
	}

	// Delay holds steady aim only — flicks fire on first on-hitbox sample this tick
	const float delay = std::clamp(Config::trigger_delay_ms, 0.f, 500.f);
	if (!flicking && delay > 0.01f
		&& (now - g_triggerOnSinceMs) < static_cast<std::uint64_t>(delay + 0.5f))
		return true;

	if (!CanWeaponFire(pWpn, lp))
		return true; // on target but can't shoot (reload / fire-rate) — no autostop

	const Vector_t vel = lp->m_vecAbsVelocity();
	const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
	const bool onGround = (lp->m_fFlags() & FL_ONGROUND) != 0;

	if (sniper && Config::trigger_scoped_only && !scoped)
		return true;

	// Eye for damage/HC: prefer history shoot pos that won the ray
	Vector_t dmgEye = lepDefault;
	if (winHist >= 0 && cmd) {
		CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(winHist);
		Vector_t sp{};
		if (HistoryShootPos(e, sp))
			dmgEye = sp;
	}

	// Wall / mindmg gate — same rules as autofire fire path
	const bool behindWall = IsBehindWall(
		dmgEye, hitPoint, lp, target, Trace::kMaskShot);
	if (behindWall && !Config::trigger_autowall)
		return true; // on geometry ray but can't pen — no shoot / no stop

	const bool allowPen = behindWall && Config::trigger_autowall;
	const float minDmg = allowPen
		? (Config::autofire_mindamage_aw > 0.f
			? Config::autofire_mindamage_aw
			: Config::autofire_mindamage)
		: Config::autofire_mindamage;

	bool dmgOk = false;
	if (!allowPen) {
		dmgOk = AutoWall::PassesMinDamage(
			dmgEye, hitPoint, hitHb, pWpn, lp, target, false, minDmg);
	} else {
		const AutoWall::Result aw = AutoWall::Fire(
			dmgEye, hitPoint, hitHb, pWpn, lp, target, true);
		if (aw.hit && aw.damage > 0.f) {
			float need = minDmg;
			if (need > 0.f) {
				const int hp = target->m_iHealth();
				if (hp > 0 && static_cast<float>(hp) < need)
					need = static_cast<float>(hp);
				dmgOk = aw.damage + 0.01f >= need;
			} else {
				dmgOk = true;
			}
		}
	}
	if (!dmgOk)
		return true; // can't deal required damage — no shoot / no stop

	bool hcOk = true;
	if (seedMode) {
		// Seed nospread: no MC hitchance / run gate
	} else {
		// Hitchance: skip on flicks (HC kills flick response); scoped always 0
		float hcReq = Config::trigger_hitchance;
		if (sniperScoped || flicking)
			hcReq = 0.f;
		if (hcReq > 0.01f)
			hcOk = HitChance::Passes(
				dmgEye, aimView, hitPoint, hitHb, pWpn, hcReq, lp, target);

		// Hard run gate only when hitchance is off (same as autofire).
		if (!sniperScoped && !Config::trigger_autostop && !flicking
			&& Config::trigger_hitchance <= 0.01f
			&& onGround && std::isfinite(speed2d) && speed2d > kNoHcRunSpeed)
			return true;
	}

	// Autostop when pen+mindmg OK (even if HC fails while moving)
	if (Config::trigger_autostop && onGround
		&& std::isfinite(speed2d) && speed2d > kAfStopSpeed) {
		g_triggerWantStop = true;
		return true;
	}

	if (!hcOk)
		return true;

	// Seed mode: exact SPREADSEEDGEN(tick) + CalcSpread(seed+1) must hit.
	// Prefer the winning history sample (winHist) so seedTick/eye/angles match
	// the ray that already scored on-target — not always the tip.
	// IDA CSBaseGunFire uses nPlayerTickCount @ +0x68 — no ±1 fuzz.
	if (seedMode) {
		int atkIdx = winHist;
		int seedTick = 0;
		Vector_t seedEye = dmgEye;
		if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
			const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			if (atkIdx < 0 || atkIdx >= histCount)
				atkIdx = histCount > 0 ? histCount - 1 : -1;
			if (atkIdx >= 0) {
				CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(atkIdx);
				seedTick = GetSeedTickFromEntry(e);
				Vector_t sp{};
				if (HistoryShootPos(e, sp))
					seedEye = sp;
			}
		}
		if (seedTick <= 0)
			seedTick = GetRenderTick(cmd, atkIdx, lp);
		if (seedTick <= 0)
			return true;

		// Fire angles = aimView (winning sample). Stamp same pitch/yaw + tick.
		QAngle_t fireAng = aimView;
		fireAng.z = 0.f;
		fireAng.Normalize();
		if (!fireAng.IsValid())
			return true;

		float sx = 0.f, sy = 0.f;
		Vector_t seedDir{};
		const bool gotDir = HitChance::GetBulletDirection(
			fireAng, seedTick, pWpn, lp, seedDir, &sx, &sy);

		// Full capsule seed check. Seed determines direction within the bloom
		// cone; bloom magnitude is factored by the game at CalcSpread time.
		// Full capsule (1.0/1.0) ensures we catch the seed direction even when
		// bloom varies between our read and CalcSpread.
		Vector_t seedPt{};
		int seedHb = Config::HB_HEAD;
		bool seedHit = gotDir && HitChance::ExactShotHits(
			seedEye, fireAng, seedTick, pWpn, lp, target, hitHb, &seedPt);
		if (!seedHit) {
			seedHit = HitChance::ExactShotHitsAny(
				seedEye, fireAng, seedTick, pWpn, lp, target,
				Config::trigger_hitboxes, &seedHb, &seedPt);
		}

		if (!seedHit)
			return true;

		// Seed ray wall / mindmg on the actual seed hitpoint (not zero-spread pt)
		{
			const bool seedBehindWall = IsBehindWall(
				seedEye, seedPt, lp, target, Trace::kMaskShot);
			if (seedBehindWall && !Config::trigger_autowall)
				return true;
			const bool seedPen = seedBehindWall && Config::trigger_autowall;
			const float seedMin = seedPen
				? (Config::autofire_mindamage_aw > 0.f
					? Config::autofire_mindamage_aw
					: Config::autofire_mindamage)
				: Config::autofire_mindamage;
			if (!AutoWall::PassesMinDamage(
					seedEye, seedPt, seedHb, pWpn, lp, target, seedPen, seedMin))
				return true;
		}

		// Engine trace along seed dir: first solid hit must be target
		{
			const Vector_t seedEnd{
				seedEye.x + seedDir.x * 8192.f,
				seedEye.y + seedDir.y * 8192.f,
				seedEye.z + seedDir.z * 8192.f
			};
			Trace::CGameTrace tr{};
			if (Trace::TraceLine(seedEye, seedEnd, lp, tr, Trace::kMaskShot)) {
				void* hitEnt = tr.hit_entity();
				if (hitEnt && hitEnt != target && Trace::DidHit(tr))
					return true;
			}
		}

#ifdef _DEBUG
		// Debug-only seed diagnostic logging (non-blocking)
		{
			const std::uint32_t predictedSeed = gotDir
				? HitChance::ComputeSeed(fireAng, seedTick)
				: 0u;

			static std::uint64_t s_lastSeedLogMs = 0;
			if (NowMs() - s_lastSeedLogMs >= 80ull) {
				s_lastSeedLogMs = NowMs();
				float bloomInac = 0.f, bloomSpr = 0.f;
				HitChance::ReadCurrentBloom(pWpn, lp, bloomInac, bloomSpr);
				Con::Info(
					"seed fire tick=%d hb=%d seed=0x%08X sx=%.4f sy=%.4f inac=%.4f spr=%.4f "
					"eye=(%.0f,%.0f,%.0f) ang=(%.1f,%.1f)",
					seedTick, seedHb, predictedSeed, sx, sy,
					bloomInac, bloomSpr,
					seedEye.x, seedEye.y, seedEye.z, fireAng.x, fireAng.y);
			}
		}

		if (cmd && atkIdx >= 0) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(atkIdx);
			if (e) {
				const int verifyTick = GetSeedTickFromEntry(e);
				const QAngle_t verifyAng = (e && e->pViewAngles)
					? e->pViewAngles->angValue : QAngle_t{};
				if (verifyTick != seedTick
					|| std::fabs(verifyAng.x - fireAng.x) > 0.01f
					|| std::fabs(verifyAng.y - fireAng.y) > 0.01f) {
					Con::Info(
						"seed VERIFY MISMATCH atkIdx=%d wroteTick=%d readTick=%d "
						"wroteAng=(%.1f,%.1f) readAng=(%.1f,%.1f)",
						atkIdx, seedTick, verifyTick,
						fireAng.x, fireAng.y, verifyAng.x, verifyAng.y);
				}
			}
		}
#endif

		// Stamp the same entry/eye used for seed validation
		g_triggerFireAngle = fireAng;
		g_triggerFireEye = seedEye;
		g_triggerFireValid = true;
		g_triggerWantShoot = true;
		g_triggerFireHist = atkIdx;
		if (cmd && atkIdx >= 0)
			cmd->SetAttackHistoryFire(atkIdx, fireAng, seedEye);
	} else {
		g_triggerFireAngle = aimView;
		g_triggerFireEye = winEye;
		g_triggerFireValid = true;
		g_triggerWantShoot = true;
		g_triggerFireHist = winHist;
		if (cmd) {
			const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			const int idx = (winHist >= 0 && winHist < histCount) ? winHist
				: (histCount > 0 ? histCount - 1 : -1);
			if (idx >= 0)
				cmd->SetAttackHistoryFire(idx, aimView, winEye);
		}
	}

	return true;
}

static void StripAttack(CUserCmd* cmd) {
	if (!cmd)
		return;
	constexpr std::uint64_t kAttackMask = IN_ATTACK | IN_SECOND_ATTACK;
	cmd->nButtons.nValue &= ~kAttackMask;
	cmd->nButtons.nValueChanged &= ~kAttackMask;
	cmd->nButtons.nValueScroll &= ~kAttackMask;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (base && base->pInButtonState) {
		base->pInButtonState->nValue &= ~kAttackMask;
		base->pInButtonState->nValueChanged &= ~kAttackMask;
		base->pInButtonState->nValueScroll &= ~kAttackMask;
	}
	cmd->csgoUserCmd.nAttack1StartHistoryIndex = -1;
	cmd->csgoUserCmd.nAttack2StartHistoryIndex = -1;
	cmd->csgoUserCmd.nAttack3StartHistoryIndex = -1;
}

static void PressAttack(CUserCmd* cmd) {
	if (!cmd)
		return;
	constexpr std::uint64_t kAttack = IN_ATTACK;
	cmd->nButtons.nValue |= kAttack;
	cmd->nButtons.nValueChanged |= kAttack;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (base && base->pInButtonState) {
		base->pInButtonState->nValue |= kAttack;
		base->pInButtonState->nValueChanged |= kAttack;
	}
	// Stamp fire angle + shoot origin (UC: eye must match server weapon-fire path)
	if (g_autofireSilentValid) {
		cmd->SetSubTickAngle(g_autofireSilentAngle);
		if (Bones::IsValidPos(g_autofireSilentEye))
			cmd->SetSubTickShootPosition(g_autofireSilentEye);
	} else if (g_triggerFireValid) {
		cmd->SetSubTickAngle(g_triggerFireAngle);
		cmd->SetSubTickShootPosition(g_triggerFireEye);
		if (g_triggerFireHist >= 0)
			cmd->SetAttackHistoryFire(g_triggerFireHist, g_triggerFireAngle, g_triggerFireEye);
	}

	// Ensure attack binds to a valid history slot (IDA attack1_start_history_index)
	// Silent autofire must stamp angles into history — index-only left seed/view wrong.
	if (g_triggerWantShoot || g_autofireWantShoot) {
		const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
		int idx = cmd->csgoUserCmd.nAttack1StartHistoryIndex;
		if (g_triggerFireValid && g_triggerFireHist >= 0)
			idx = g_triggerFireHist;
		if (idx < 0 && histCount > 0)
			idx = histCount - 1;
		if (idx >= 0) {
			if (g_triggerFireValid)
				cmd->SetAttackHistoryFire(idx, g_triggerFireAngle, g_triggerFireEye);
			else if (g_autofireSilentValid)
				cmd->SetAttackHistoryFire(idx, g_autofireSilentAngle, g_autofireSilentEye);
			else
				cmd->csgoUserCmd.SetAttack1StartHistoryIndex(idx);
		} else if (cmd->csgoUserCmd.nAttack1StartHistoryIndex >= 0) {
			cmd->csgoUserCmd.CheckAndSetBits(CCSGOUserCmdPB::BITS_ATTACK1START);
		}
	}
}

// Counter-strafe: cancel horizontal velocity for standing accuracy
static void ApplyAutoStop(CUserCmd* cmd, C_CSPlayerPawn* lp) {
	if (!cmd || !lp)
		return;

	const Vector_t vel = lp->m_vecAbsVelocity();
	const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
	if (!std::isfinite(speed2d) || speed2d < 1.f)
		return;

	QAngle_t view{};
	if (!GetViewAngles(view))
		return;

	Vector_t fwd{}, right{};
	view.ToDirections(&fwd, &right, nullptr);
	fwd.z = 0.f;
	right.z = 0.f;
	const float fl = fwd.Length();
	const float rl = right.Length();
	if (fl > 1e-4f) { fwd.x /= fl; fwd.y /= fl; }
	if (rl > 1e-4f) { right.x /= rl; right.y /= rl; }

	// Opposite of velocity projected onto move axes
	float fmove = -(vel.x * fwd.x + vel.y * fwd.y);
	float smove = -(vel.x * right.x + vel.y * right.y);
	const float mlen = std::sqrt(fmove * fmove + smove * smove);
	if (mlen < 1e-4f)
		return;

	constexpr float kMaxMove = 450.f;
	fmove = (fmove / mlen) * kMaxMove;
	smove = (smove / mlen) * kMaxMove;

	constexpr std::uint64_t kMoveKeys =
		IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;
	cmd->nButtons.nValue &= ~kMoveKeys;
	cmd->nButtons.nValueChanged &= ~kMoveKeys;
	cmd->nButtons.nValueScroll &= ~kMoveKeys;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (base) {
		base->flForwardMove = fmove;
		base->flSideMove = smove;
		base->flUpMove = 0.f;
		if (base->pInButtonState) {
			base->pInButtonState->nValue &= ~kMoveKeys;
			base->pInButtonState->nValueChanged &= ~kMoveKeys;
			base->pInButtonState->nValueScroll &= ~kMoveKeys;
		}
	}
}

} // namespace

void AimHumanize_OnCreateMove(CUserCmd* cmd) {
	if (!cmd)
		return;

	const bool wantStop = g_autofireWantStop || g_triggerWantStop;
	if (wantStop && H::oGetLocalPlayer) {
		C_CSPlayerPawn* lp = nullptr;
		__try { lp = H::oGetLocalPlayer(0); }
		__except (EXCEPTION_EXECUTE_HANDLER) { lp = nullptr; }
		if (lp && lp->m_iHealth() > 0)
			ApplyAutoStop(cmd, lp);
	}

	// Autofire owns the frame: never inherit aimbot g_blockFirstShot (stale after aimbot use)
	const bool afKey = keybind.isActive(Config::autofire);
	if (afKey)
		g_blockFirstShot = false;

	if ((g_blockFirstShot && !afKey) || g_afBlockFirst)
		StripAttack(cmd);
	else if (g_autofireWantShoot || g_triggerWantShoot)
		PressAttack(cmd);
}

void Aimbot(CUserCmd* cmd) {
	if (g_bMenuOpen) {
		ResetHumanize();
		ResetAutofire();
		ResetTriggerbot();
		g_hadPunch = false;
		return;
	}
	if (!H::oGetLocalPlayer)
		return;
	if (!Input::GetViewAngles || !Input::SetViewAngle || !Input::viewAngleContext)
		return;

	C_CSPlayerPawn* lp = nullptr;
	__try { lp = H::oGetLocalPlayer(0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }

	if (!lp || lp->m_iHealth() <= 0) {
		g_hadPunch = false;
		ResetHumanize();
		ResetAutofire();
		ResetTriggerbot();
		return;
	}

	// Keep live aim settings synced to held weapon group (FOV circle, etc.)
	{
		C_CSWeaponBase* wpn = nullptr;
		__try { wpn = lp->GetActiveWeapon(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { wpn = nullptr; }
		if (Mem::ValidEntity(wpn) && !wpn->IsNonGunWeapon())
			Config::ApplyWeaponGroup(wpn);
		else
			Config::ApplyWeaponGroup(nullptr);
	}

	// Autofire owns aim while active; otherwise normal aimbot
	const bool af = RunAutofire(lp);
	const bool aimed = af ? true : RunAimbot(lp);
	if (!af) {
		g_autofireWantShoot = false;
		g_autofireWantStop = false;
	}

	// Triggerbot: never aims — only fires when crosshair already on hitbox.
	// Autofire shoot wins if both want to fire on the same tick.
	if (!af || !g_autofireWantShoot)
		RunTriggerbot(lp, cmd);
	else {
		g_triggerWantShoot = false;
		g_triggerWantStop = false;
		g_triggerFireValid = false;
	}

	// Standalone RCS only when aimbot/autofire not locking (avoids double RCS)
	if (Config::rcs_standalone && !aimed)
		StandaloneRcs(lp);
	else if (!Config::rcs && !Config::rcs_standalone)
		g_hadPunch = false;
	else if (aimed && Config::rcs)
		g_hadPunch = false; // reset delta baseline after absolute-punch aim frames
}
