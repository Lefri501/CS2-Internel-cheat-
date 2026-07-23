#include "aim_common.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../offsets/offsets.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../gamemode/gamemode.h"
#include "../visuals/visuals.h"
#include "../prediction/prediction.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace AimCommon {

using FnGetRemovedAimPunch = void*(__fastcall*)(void* pawn, QAngle_t* out, char flag);
static FnGetRemovedAimPunch g_getRemovedAimPunch = nullptr;
static bool g_punchResolved = false;

static void ResolveGetRemovedAimPunch() {
	if (g_punchResolved)
		return;
	g_punchResolved = true;
	auto* p = M::FindPattern("client",
		"40 53 48 83 EC 20 48 8B 89 ? ? ? ? 48 8B DA E8");
	if (!p)
		p = M::FindPattern("client",
			"40 53 48 83 EC 20 48 8B 89 B8 14 00 00 48 8B DA");
	g_getRemovedAimPunch = reinterpret_cast<FnGetRemovedAimPunch>(p);
	if (g_getRemovedAimPunch)
		Con::Ok("Aim: GetRemovedAimPunch @ 0x%p", (void*)g_getRemovedAimPunch);
	else
		Con::OffsetMiss("GetRemovedAimPunch");
}

QAngle_t g_oldPunch{};
bool g_hadPunch = false;

static AimTarget g_aimTargets[Mem::kMaxPlayers]{};
static int g_aimTargetCount = 0;
static std::uint32_t g_aimListGen = 0;

void CollectAimTargets(C_CSPlayerPawn* lp) {
	g_aimTargetCount = 0;
	++g_aimListGen;
	Bones::BeginFrame(g_aimListGen);

	if (!lp || !I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return;

	int nMaxRaw = 0;
	__try {
		nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (nMaxRaw <= 0)
		return;
	// Controllers live in low slots — walking 8k entities every CreateMove kills FPS
	const int nMax = (nMaxRaw > 128) ? 128 : nMaxRaw;

	const uint8_t localTeam = lp->getTeam();
	int checked = 0;
	for (int i = 1; i <= nMax; ++i) {
		void* entRaw = nullptr;
		__try {
			entRaw = I::GameEntity->Instance->Get(i);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		auto* Entity = reinterpret_cast<C_BaseEntity*>(entRaw);
		if (!Mem::ValidEntity(Entity))
			continue;
		bool hOk = false;
		__try { hOk = Entity->handle().valid(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!hOk)
			continue;

		// Designer first — skip dump_class_info on non-controller slots (big CreateMove win)
		bool isCtrl = false;
		__try {
			CEntityIdentity* id = nullptr;
			if (!Mem::ReadField(Entity, 0x10, id) || !id || !Mem::Valid(id, 0x28))
				id = Entity->m_pEntityIdentity();
			if (id && Mem::Valid(id, 0x28)) {
				const char* designer = nullptr;
				if (!Mem::ReadField(id, 0x20, designer) || !designer)
					designer = id->m_designerName();
				if (designer && Mem::IsReadable(designer, 2) && designer[0]) {
					if (std::strcmp(designer, "cs_player_controller") == 0
						|| std::strstr(designer, "player_controller") != nullptr)
						isCtrl = true;
					else
						continue; // known non-controller — no dump
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			isCtrl = false;
		}

		if (!isCtrl) {
			// Designer missing / unreadable — fall back to class name (rare)
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
		}

		if (checked >= Mem::kMaxPlayers)
			break;
		++checked;

		auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
		bool isLocal = false;
		__try { isLocal = Controller->IsLocalPlayer(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (isLocal)
			continue;

		// Prefer m_hPlayerPawn (CS pawn); m_hPawn can be observer while dead/spec
		CBaseHandle hPawn{};
		__try {
			hPawn = Controller->m_hPlayerPawn();
			if (!hPawn.valid())
				hPawn = Controller->m_hPawn();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!hPawn.valid())
			continue;

		C_CSPlayerPawn* pawn = nullptr;
		__try {
			pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!Mem::ValidEntity(pawn))
			continue;

		// Full serial match — index-only hits recycled slots after death
		CBaseHandle actual{};
		__try { actual = pawn->handle(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!actual.valid()
			|| actual.index() != hPawn.index()
			|| actual.serial_number() != hPawn.serial_number())
			continue;

		int hp = 0;
		std::uint8_t life = 1;
		uint8_t team = 0;
		__try {
			hp = Mem::ClampHealth(pawn->m_iHealth());
			life = pawn->m_lifeState();
			team = pawn->m_iTeamNum();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (hp < 1 || life != 0)
			continue;

		// DM spawn protect / freeze invuln — invisible, no damage (default skip)
		bool immune = false;
		__try { immune = IsTargetImmune(pawn); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (immune)
			continue;

		if (!Mem::ValidTeam(static_cast<int>(team)))
			continue;
		if (GameMode::WantTeamCheck(Config::team_check) && team == localTeam)
			continue;

		if (g_aimTargetCount >= Mem::kMaxPlayers)
			break;
		// Full packed handle (index|serial) — index-only locks attach to recycled slots
		g_aimTargets[g_aimTargetCount++] = AimTarget{
			pawn, hPawn.raw(), hp
		};
	}
}

int AimTargetCount() { return g_aimTargetCount; }
const AimTarget* AimTargets() { return g_aimTargets; }

void ClearTargets() {
	g_aimTargetCount = 0;
	++g_aimListGen;
	Bones::BeginFrame(g_aimListGen);
}

std::uint64_t NowMs() {
	return GetTickCount64();
}

float LiveMultipointBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local) {
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

bool WeaponReadyForCombat(C_CSWeaponBase* wep, C_CSPlayerPawn* lp) {
	// Soft combat gate: reload / empty / plant-defuse / inspect hold.
	// Does NOT include fire-rate (next primary tick) — AF still tracks between shots.
	if (!wep || !lp)
		return false;
	if (!Mem::ValidEntity(wep) || !Mem::ValidEntity(lp))
		return false;
	__try {
		if (lp->m_bIsDefusing() || lp->m_bIsGrabbingHostage())
			return false;
		if (lp->m_bWaitForNoAttack())
			return false;
		if (wep->m_bInReload())
			return false;
		const int clip = wep->m_iClip1();
		// <0 = no clip concept (some specials); 0 = empty magazine
		if (clip == 0)
			return false;
		// Weapon inspect — no combat aim (schema: m_bInspectPending, not old look-at bools)
		if (wep->m_bInspectPending() || wep->m_bInspectShouldLoop())
			return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

bool CanWeaponFire(C_CSWeaponBase* wep, C_CSPlayerPawn* lp) {
	if (!wep || !lp || !Mem::ValidEntity(wep) || !Mem::ValidEntity(lp))
		return false;
	if (!WeaponReadyForCombat(wep, lp))
		return false;

	// Fire-rate / bolt / cycle: next primary attack tick vs controller tickbase.
	// +1 slack: online tickbase lags 1 cmd so nextTick==tickBase+1 still firable.
	// Strict >tickBase alone blocked seed while crosshair already on target.
	// Soft ms re-arm still in NoSpread::SeedCycleAllowsFire after real fire.
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
		return false;
	}

	return true;
}

namespace {

std::uint32_t s_noiseSeed = 0xA5F01234u;
QAngle_t s_humBias{};
std::uint64_t s_humBiasMs = 0;
std::uint64_t s_smoothLastMs = 0;
// Sticky speed mul — per-tick NoiseUnit made aim shake, not humanize
float s_speedMul = 1.f;
std::uint64_t s_speedMulMs = 0;
// Sticky lateral drift unit — old path re-rolled NoiseUnit every CreateMove (shake)
float s_latDrift = 0.f;
std::uint64_t s_latDriftMs = 0;

// Soft land: stop fighting when already on true bone (kills magnetic micro-jitter)
constexpr float kArriveDeg = 0.06f;
// No humanize bias inside this radius — settle on bone
constexpr float kHoldDeg = 0.12f;
// Drop sticky bias below this error (must clear even mid-interval)
constexpr float kBiasClearDeg = 0.45f;

float NoiseUnit()
{
	std::uint32_t x = s_noiseSeed;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s_noiseSeed = x ? x : 0xA5F01234u;
	return (static_cast<float>(s_noiseSeed & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu)) * 2.f - 1.f;
}

int ActiveSmoothMode()
{
	// Live Config after ApplyWeaponGroup / menu — not raw profile (desync risk)
	int mode = Config::aimbot_smooth_mode;
	if (mode < 0 || mode >= Config::SMOOTH_MODE_COUNT)
		mode = Config::SMOOTH_LINEAR;
	return mode;
}

// CreateMove dt in seconds, clamped. Normalizes smooth to ~64-tick feel.
float SmoothDtSec()
{
	const std::uint64_t now = NowMs();
	float dt = Pred::kTickInterval; // 1/64 default
	if (s_smoothLastMs != 0 && now >= s_smoothLastMs) {
		const float raw = static_cast<float>(now - s_smoothLastMs) * 0.001f;
		// Clamp: ignore hitch stalls and sub-ms spam
		if (raw >= 0.001f && raw <= 0.050f)
			dt = raw;
	}
	s_smoothLastMs = now;
	return dt;
}

// Max bias amp at current error (decays as we approach)
float HumBiasAmp(float hum, float errDeg)
{
	return 0.40f * hum * std::clamp(errDeg / 12.f, 0.15f, 1.f);
}

void RefreshHumBias(float hum, float errDeg)
{
	// Clear BEFORE interval gate — old code early-returned and left stale bias
	// in the 0.12–0.45° band (fight / micro-oscillation near bone).
	if (hum < 0.01f || errDeg < kBiasClearDeg) {
		s_humBias = {};
		return;
	}

	const std::uint64_t now = NowMs();
	// Sticky off-center: refresh every ~140–220ms
	const std::uint64_t interval = 140ull
		+ static_cast<std::uint64_t>((NoiseUnit() * 0.5f + 0.5f) * 80.f);
	if (s_humBiasMs != 0 && now < s_humBiasMs + interval)
		return;
	s_humBiasMs = now;

	const float amp = HumBiasAmp(hum, errDeg);
	s_humBias.x = NoiseUnit() * amp * 0.55f; // pitch quieter than yaw
	s_humBias.y = NoiseUnit() * amp;
	s_humBias.z = 0.f;
}

// Scale stored bias down to current amp (approach decay without full re-roll)
void ClampHumBiasToAmp(float amp)
{
	const float bmag = std::sqrt(s_humBias.x * s_humBias.x + s_humBias.y * s_humBias.y);
	if (bmag <= 1e-6f || bmag <= amp)
		return;
	const float s = amp / bmag;
	s_humBias.x *= s;
	s_humBias.y *= s;
}

float HumanSpeedMul(float hum)
{
	if (hum < 0.01f)
		return 1.f;
	const std::uint64_t now = NowMs();
	// Refresh every ~90–140ms — not every CreateMove (was pure shake)
	const std::uint64_t interval = 90ull
		+ static_cast<std::uint64_t>((NoiseUnit() * 0.5f + 0.5f) * 50.f);
	if (s_speedMulMs == 0 || now >= s_speedMulMs + interval) {
		s_speedMulMs = now;
		s_speedMul = std::clamp(1.f + NoiseUnit() * (0.12f * hum), 0.82f, 1.15f);
	}
	return s_speedMul;
}

// Sticky ±1 unit for lateral path drift — refresh ~110–180ms
float LateralDriftUnit(float hum)
{
	if (hum < 0.01f)
		return 0.f;
	const std::uint64_t now = NowMs();
	const std::uint64_t interval = 110ull
		+ static_cast<std::uint64_t>((NoiseUnit() * 0.5f + 0.5f) * 70.f);
	if (s_latDriftMs == 0 || now >= s_latDriftMs + interval) {
		s_latDriftMs = now;
		s_latDrift = NoiseUnit();
	}
	return s_latDrift;
}

} // namespace

void ResetSmoothState()
{
	s_humBias = {};
	s_humBiasMs = 0;
	s_smoothLastMs = 0;
	s_speedMul = 1.f;
	s_speedMulMs = 0;
	s_latDrift = 0.f;
	s_latDriftMs = 0;
}

QAngle_t SmoothToward(const QAngle_t& cur, const QAngle_t& target, float smooth)
{
	// trueGoal = bone. goal may get temporary humanize offset while traveling.
	QAngle_t trueGoal = target;
	trueGoal.z = 0.f;
	trueGoal.Normalize();
	trueGoal.x = std::clamp(trueGoal.x, -89.f, 89.f);
	if (!trueGoal.IsValid() || !cur.IsValid())
		return cur;

	// Snap path
	if (smooth <= 0.01f) {
		ResetSmoothState();
		return trueGoal;
	}

	// Live Config (ApplyWeaponGroup / menu) — not ActiveAimProfile
	const float hum = std::clamp(Config::aimbot_humanize, 0.f, 100.f) * 0.01f;
	const float dt = SmoothDtSec();
	// Reference: smooth slider tuned as if CreateMove ran at 64 Hz
	const float tickScale = dt / Pred::kTickInterval;

	float tdx = std::remainderf(trueGoal.x - cur.x, 360.f);
	float tdy = std::remainderf(trueGoal.y - cur.y, 360.f);
	float trueErr = std::sqrt(tdx * tdx + tdy * tdy);

	// Already on bone — hold (no humanize pull / micro-oscillation)
	if (trueErr < kArriveDeg)
		return cur;

	QAngle_t goal = trueGoal;
	float dx = tdx;
	float dy = tdy;
	float err = trueErr;

	// Humanize: soft off-center only while traveling far from bone
	if (hum > 0.01f && trueErr > kHoldDeg) {
		RefreshHumBias(hum, trueErr);
		ClampHumBiasToAmp(HumBiasAmp(hum, trueErr));
		if (s_humBias.x != 0.f || s_humBias.y != 0.f) {
			goal.x = trueGoal.x + s_humBias.x;
			goal.y = trueGoal.y + s_humBias.y;
			goal.z = 0.f;
			goal.Normalize();
			goal.x = std::clamp(goal.x, -89.f, 89.f);
			dx = std::remainderf(goal.x - cur.x, 360.f);
			dy = std::remainderf(goal.y - cur.y, 360.f);
			err = std::sqrt(dx * dx + dy * dy);
		}
	} else {
		s_humBias = {};
	}

	if (err < kArriveDeg)
		return cur;

	const float st = std::clamp(smooth, 1.f, 50.f);
	const int mode = ActiveSmoothMode();

	// Base fraction of remaining error this tick (1 = instant, 50 = slow)
	// Scale by tickScale so 128-tick isn't 2× faster than 64-tick.
	float factor = (1.f / st) * tickScale;

	switch (mode) {
	case Config::SMOOTH_CONSTANT: {
		// Fixed deg/sec (smooth 1 → ~768°/s @64t, smooth 50 → ~15°/s)
		const float degPerSec = 12.f / st * 64.f;
		const float stepDeg = degPerSec * dt;
		factor = (err > 1e-6f) ? std::clamp(stepDeg / err, 0.f, 1.f) : 1.f;
		break;
	}
	case Config::SMOOTH_SINE: {
		// Fast far, soft near — organic ease-in (use trueErr so bias doesn't stall)
		const float u = std::clamp(trueErr / 14.f, 0.f, 1.f);
		const float sineShape = std::sin(u * (kPi * 0.5f));
		factor *= (0.30f + 0.70f * sineShape);
		break;
	}
	case Config::SMOOTH_LINEAR:
	default:
		// Pure exponential ease: strong far, natural settle near
		break;
	}

	if (hum > 0.01f) {
		// Mild speed variance (±12% at 100), sticky — not per-tick shake
		factor *= HumanSpeedMul(hum);
		// Extra settle damping when close to true bone
		if (trueErr < 2.5f)
			factor *= (1.f - 0.35f * hum * (1.f - trueErr / 2.5f));
	}

	// Soft land: never take full remaining step in one tick (avoids clicky finish)
	if (err < 1.25f) {
		const float cap = 0.55f + 0.45f * (err / 1.25f);
		if (factor > cap)
			factor = cap;
	}

	// Near true bone: prefer trueGoal step so we don't orbit humanize offset
	if (trueErr < kBiasClearDeg) {
		dx = tdx;
		dy = tdy;
		err = trueErr;
		goal = trueGoal;
		s_humBias = {};
	}

	factor = std::clamp(factor, 0.f, 1.f);

	QAngle_t out = cur;
	out.x += dx * factor;
	out.y += dy * factor;

	// Lateral path drift only while far — orthogonal, low, sticky unit (not per-tick)
	if (hum > 0.01f && trueErr > 2.5f && err > 1e-4f) {
		const float inv = 1.f / err;
		const float drift = LateralDriftUnit(hum) * (0.018f * hum)
			* std::clamp((trueErr - 2.5f) * 0.10f, 0.f, 1.f);
		out.x += (-dy * inv) * drift;
		out.y += (dx * inv) * drift;
	}

	out.z = 0.f;
	out.Normalize();
	out.x = std::clamp(out.x, -89.f, 89.f);

	// Overshoot guard snaps to TRUE bone — old code snapped to biased goal (stuck off-center)
	const float ndx = std::remainderf(trueGoal.x - out.x, 360.f);
	const float ndy = std::remainderf(trueGoal.y - out.y, 360.f);
	if (ndx * tdx < 0.f && std::fabs(ndx) < 0.5f)
		out.x = trueGoal.x;
	if (ndy * tdy < 0.f && std::fabs(ndy) < 0.5f)
		out.y = trueGoal.y;
	out.Normalize();
	out.x = std::clamp(out.x, -89.f, 89.f);
	return out;
}

bool IsSniperWeapon(C_CSWeaponBase* weapon) {
	if (!weapon)
		return false;
	__try {
		// VData type is reliable for grouping; defindex can miss if econ offset lags
		if (auto* vd = weapon->Data()) {
			const int t = vd->m_WeaponType();
			if (t == 5) // CCSWeaponType::sniper
				return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	const std::uint16_t def = weapon->m_iItemDefinitionIndex();
	// AWP=9, G3SG1=11, SCAR-20=38, SSG08=40
	return def == 9 || def == 11 || def == 38 || def == 40;
}

bool IsScopeWeapon(C_CSWeaponBase* weapon) {
	if (!weapon)
		return false;
	if (IsSniperWeapon(weapon))
		return true;
	const std::uint16_t def = weapon->m_iItemDefinitionIndex();
	// AUG=8, SG553=39 (rifles with scope — not VData sniper)
	return def == 8 || def == 39;
}

bool IsLocalScoped(C_CSPlayerPawn* lp, C_CSWeaponBase* weapon) {
	if (!lp || !Mem::ValidEntity(lp))
		return false;

	// Strict: only real zoom. weaponMode alone false-positives (blocks Scope Check).
	C_CSWeaponBase* wpn = weapon;
	if (!wpn || !Mem::ValidEntity(wpn)) {
		__try { wpn = lp->GetActiveWeapon(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { wpn = nullptr; }
	}
	if (wpn && Mem::ValidEntity(wpn)) {
		__try {
			static std::uint32_t s_zoomOff = 0;
			static bool s_zoomTried = false;
			if (!s_zoomTried) {
				s_zoomTried = true;
				s_zoomOff = SchemaFinder::Get(
					hash_32_fnv1a_const("C_CSWeaponBaseGun->m_zoomLevel"));
				if (!s_zoomOff)
					s_zoomOff = 0x1CE0;
			}
			auto* wb = reinterpret_cast<std::uint8_t*>(wpn);
			if (s_zoomOff && Mem::IsReadable(wb + s_zoomOff, 4)) {
				const std::int32_t zl = *reinterpret_cast<std::int32_t*>(wb + s_zoomOff);
				// Valid zoom levels are 1..3 — reject garbage from bad offsets
				if (zl >= 1 && zl <= 3)
					return true;
			}
			const std::int32_t zlSch = wpn->m_zoomLevel();
			if (zlSch >= 1 && zlSch <= 3)
				return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// client_dll.hpp: m_bIsScoped = 0x1C70
	static std::uint32_t s_off = 0;
	static bool s_scopedTried = false;
	if (!s_scopedTried) {
		s_scopedTried = true;
		s_off = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_bIsScoped"));
		if (!s_off)
			s_off = 0x1C70;
	}
	__try {
		auto* base = reinterpret_cast<std::uint8_t*>(lp);
		if (s_off && Mem::IsReadable(base + s_off, 1) && base[s_off] != 0)
			return true;
		if (lp->m_bIsScoped())
			return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

bool IsHeavyPistol(C_CSWeaponBase* weapon) {
	if (!weapon)
		return false;
	const std::uint16_t def = weapon->m_iItemDefinitionIndex();
	// Deagle=1, R8=64
	return def == 1 || def == 64;
}

bool IsSemiWeapon(C_CSWeaponBase* weapon) {
	if (!weapon || !Mem::ValidEntity(weapon))
		return false;
	// Def fallback when VData missing: pistols + bolt snipers
	if (IsHeavyPistol(weapon))
		return true;
	__try {
		const std::uint16_t def = weapon->m_iItemDefinitionIndex();
		// SSG=40 AWP=9 G3SG1=11 SCAR20=38 + common semis
		if (def == 9 || def == 40 || def == 11 || def == 38)
			return true;
		if (def == 2 || def == 3 || def == 4 || def == 30 || def == 32
			|| def == 36 || def == 61 || def == 63)
			return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	__try {
		if (auto* vd = weapon->Data()) {
			// Full-auto flag is authoritative when readable
			if (vd->m_bIsFullAuto())
				return false;
			return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

bool IsSprayAutoWeapon(C_CSWeaponBase* weapon) {
	if (!weapon)
		return false;
	__try {
		if (auto* vd = weapon->Data()) {
			const int t = vd->m_WeaponType();
			// CCSWeaponType: smg=2 rifle=3 machinegun=6
			return t == 2 || t == 3 || t == 6;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

bool IsBehindWall(const Vector_t& eye, const Vector_t& aimPoint, void* skip, void* target,
	std::uint64_t mask) {
	if (!Trace::Ready())
		return false;
	return !Trace::IsVisible(eye, aimPoint, skip, target, mask, false);
}

bool ReadAimPunch(C_CSPlayerPawn* lp, QAngle_t& out) {
	if (!lp)
		return false;

	ResolveGetRemovedAimPunch();

	// Primary: game GetRemovedAimPunch (same path as FX_FireBullets)
	if (g_getRemovedAimPunch) {
		QAngle_t punch{};
		bool ok = false;
		__try {
			void* ret = g_getRemovedAimPunch(lp, &punch, 1);
			(void)ret;
			ok = punch.IsValid();
			if (ok)
				out = punch;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		if (ok)
			return true;
	}

	// Fallback: schema AimPunchServices::m_predictableBaseAngle (Offset::* schema+FB)
	bool ok = false;
	__try {
		const uintptr_t base = reinterpret_cast<uintptr_t>(lp);
		const uint32_t svcOff = Offset::m_pAimPunchServices();
		const uint32_t angOff = Offset::m_predictableBaseAngle();
		if (!svcOff || !angOff)
			return false;

		void* services = *reinterpret_cast<void**>(base + svcOff);
		if (!services || !Mem::IsUserPtr(services))
			return false;

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
		const uint32_t off = Offset::m_iShotsFired();
		if (!off)
			return 0;
		shots = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(lp) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return shots;
}

// Scaled punch for visible RCS (clamped). Menu: X = yaw (horiz), Y = pitch (vert).
bool GetScaledPunch(C_CSPlayerPawn* lp, QAngle_t& out) {
	QAngle_t punch{};
	if (!HitChance::ReadAimPunch(lp, punch) && !ReadAimPunch(lp, punch))
		return false;
	if (!punch.IsValid() || ReadShotsFired(lp) < 1)
		return false;
	const float sx = std::clamp(Config::rcs_scale_x, 0.f, 1.f); // yaw
	const float sy = std::clamp(Config::rcs_scale_y, 0.f, 1.f); // pitch
	out.x = std::clamp(punch.x, -12.f, 12.f) * kPunchScale * sy;
	out.y = std::clamp(punch.y, -12.f, 12.f) * kPunchScale * sx;
	out.z = 0.f;
	return out.IsValid();
}

// Full GetRemovedAimPunch for fire angles — IDA: bullet = view + punch + spread.
// Autofire must use this on the SHOT path or AK spray climbs over the target
// (menu RCS scale / smooth must not starve the fire stamp).
bool GetFirePunch(C_CSPlayerPawn* lp, QAngle_t& out) {
	out = {};
	QAngle_t punch{};
	if (!HitChance::ReadAimPunch(lp, punch) && !ReadAimPunch(lp, punch))
		return false;
	if (!punch.IsValid())
		return false;
	out.x = std::clamp(punch.x, -12.f, 12.f) * kPunchScale;
	out.y = std::clamp(punch.y, -12.f, 12.f) * kPunchScale;
	out.z = 0.f;
	return out.IsValid();
}

void ApplyPunchSubtract(QAngle_t& ang, const QAngle_t& punch) {
	ang.x -= punch.x;
	ang.y -= punch.y;
	ang.Normalize();
	ang.x = std::clamp(ang.x, -89.f, 89.f);
	ang.z = 0.f;
}

namespace {
CUserCmd* g_boundCmd = nullptr;

QAngle_t SanitizeView(const QAngle_t& ang)
{
	QAngle_t a = ang;
	if (!std::isfinite(a.x)) a.x = 0.f;
	if (!std::isfinite(a.y)) a.y = 0.f;
	a.x = std::clamp(a.x, -89.f, 89.f);
	a.z = 0.f;
	return a;
}

// Same-tick usercmd angles — server reads base + tip hist; camera alone is 1 tick late.
void WriteCmdView(CUserCmd* cmd, const QAngle_t& ang)
{
	if (!cmd)
		return;
	__try {
		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (base && base->pViewAngles
			&& reinterpret_cast<uintptr_t>(base->pViewAngles) > 0x10000ull) {
			base->pViewAngles->angValue = ang;
			base->SetBits(BASE_BITS_VIEWANGLES);
		}
		// Tip slots only (SetSubTickAngle) — full hist rewrite desyncs silent seed
		cmd->SetSubTickAngle(ang);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}
} // namespace

void BindCmd(CUserCmd* cmd) { g_boundCmd = cmd; }
void UnbindCmd() { g_boundCmd = nullptr; }

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
	const QAngle_t a = SanitizeView(ang);
	if (Input::SetViewAngle && Input::viewAngleContext) {
		Vector_t v = { a.x, a.y, 0.f };
		Input::SetViewAngle(Input::viewAngleContext, 0, &v);
	}
	if (g_boundCmd)
		WriteCmdView(g_boundCmd, a);
}

void StampCmdAngles(const QAngle_t& ang) {
	if (!g_boundCmd)
		return;
	WriteCmdView(g_boundCmd, SanitizeView(ang));
}

// Standalone delta RCS — follows spray pattern (pitch + yaw) each tick
bool ApplyDeltaRcs(C_CSPlayerPawn* lp) {
	if (!lp)
		return false;

	QAngle_t punch{};
	if (!HitChance::ReadAimPunch(lp, punch) && !ReadAimPunch(lp, punch))
		return false;
	if (!punch.IsValid())
		return false;

	const int shots = ReadShotsFired(lp);
	// Prefer cmd attack bit when available; GetAsyncKeyState can lag / miss inject
	const bool lmbHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	const bool shooting = lmbHeld && shots >= 1;

	if (!shooting || shots < 1) {
		g_oldPunch = punch;
		g_hadPunch = true;
		return false;
	}

	if (!g_hadPunch) {
		// Seed only — applying full punch as first "delta" yanks view hard
		g_oldPunch = punch;
		g_hadPunch = true;
		return false;
	}

	// Per-axis delta — keep horizontal kick (spray pattern), not pitch-only
	QAngle_t delta{};
	delta.x = punch.x - g_oldPunch.x;
	delta.y = punch.y - g_oldPunch.y;
	delta.z = 0.f;

	// Recovering punch (recoil settling) — track but don't fight
	if (delta.x < -0.01f && std::fabs(punch.x) + 0.02f < std::fabs(g_oldPunch.x)
		&& std::fabs(delta.y) < 0.02f) {
		g_oldPunch = punch;
		return false;
	}

	// Cap one-frame delta (teleport punch after lock drop / prediction hitch)
	delta.x = std::clamp(delta.x, -2.5f, 2.5f);
	delta.y = std::clamp(delta.y, -2.5f, 2.5f);
	if (std::fabs(delta.x) < 0.00005f && std::fabs(delta.y) < 0.00005f) {
		g_oldPunch = punch;
		return false;
	}

	g_oldPunch = punch;

	const float sx = std::clamp(Config::rcs_scale_x, 0.f, 1.f); // yaw
	const float sy = std::clamp(Config::rcs_scale_y, 0.f, 1.f); // pitch

	QAngle_t view{};
	if (!GetViewAngles(view))
		return false;

	// Counter punch delta: view -= dPunch (same add as FX_FireBullets)
	view.x -= delta.x * kPunchScale * sy;
	view.y -= delta.y * kPunchScale * sx;
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
bool IsBlinded(C_CSPlayerPawn* lp) {
	if (!lp)
		return false;
	__try {
		const float flash = lp->m_flFlashOverlayAlpha();
		return flash > 0.55f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// client_dll.hpp C_CSGameRules::m_bFreezePeriod = 0x40
bool IsFreezePeriod() {
	void* rules = SdkPrioA::GameRules();
	if (!rules || !Mem::IsReadable(rules, 0x48))
		return false;
	bool freeze = false;
	__try {
		freeze = *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(rules) + 0x40);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return freeze;
}

// m_bGunGameImmunity — DM spawn protect + similar invuln windows (enemy often invisible).
bool IsTargetImmune(C_CSPlayerPawn* pawn) {
	if (!pawn || !Mem::ValidEntity(pawn))
		return true;
	__try {
		// Schema first; raw dump fallback 0x3258 if resolver returns 0
		if (pawn->m_bGunGameImmunity())
			return true;
		static std::uint32_t s_off = 0;
		static bool s_tried = false;
		if (!s_tried) {
			s_tried = true;
			s_off = SchemaFinder::Get(
				hash_32_fnv1a_const("C_CSPlayerPawn->m_bGunGameImmunity"));
			if (!s_off)
				s_off = 0x3258u;
		}
		// Only raw-read when schema path may have missed (offset known)
		if (s_off) {
			auto* base = reinterpret_cast<std::uint8_t*>(pawn);
			if (Mem::IsReadable(base + s_off, 1) && base[s_off] != 0)
				return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false; // don't soft-kill all combat on one bad read
	}
	return false;
}

bool LineBlockedBySmoke(const Vector_t& eye, const Vector_t& aim) {
	if (!Bones::IsValidPos(eye) || !Bones::IsValidPos(aim))
		return false;

	const Vector_t d = aim - eye;
	const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
	if (len2 < 1.f)
		return false;

	constexpr float kSmokeR = 149.f; // ring_radius(61)+subradius(88)
	constexpr float kSmokeR2 = kSmokeR * kSmokeR;

	struct SmokeSphere { Vector_t c{}; float r2 = kSmokeR2; };
	static SmokeSphere s_smokes[16]{};
	static int s_smokeCount = 0;
	static std::uint64_t s_smokeMs = 0;

	const std::uint64_t now = NowMs();
	if (!s_smokeMs || now >= s_smokeMs + 80ull) {
		s_smokeMs = now;
		s_smokeCount = 0;

		// Prefer Esp cache (already scanned this frame) — no dump_class_info
		for (const auto& w : cached_world) {
			if (w.kind != WORLD_SMOKE || !w.effect_active)
				continue;
			if (!Bones::IsValidPos(w.position))
				continue;
			if (s_smokeCount >= 16)
				break;
			const float r = (w.radius > 1.f) ? w.radius : kSmokeR;
			s_smokes[s_smokeCount++] = SmokeSphere{ w.position, r * r };
		}

		// Fallback when world ESP/warn off — full scan at most ~12.5 Hz
		if (s_smokeCount == 0 && I::GameEntity && I::GameEntity->Instance
			&& Mem::Valid(I::GameEntity->Instance, 0x2100)) {
			static uint32_t s_didOff = 0;
			static uint32_t s_detOff = 0;
			if (!s_didOff) {
				s_didOff = SchemaFinder::Get(
					hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_bDidSmokeEffect"));
				if (!s_didOff)
					s_didOff = 0x127C;
			}
			if (!s_detOff) {
				s_detOff = SchemaFinder::Get(
					hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_vSmokeDetonationPos"));
				if (!s_detOff)
					s_detOff = 0x1290;
			}

			// Projectiles sit mid-list; hard-cap walk (full 8k was rare but expensive)
			const int nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
			const int nMax = (nMaxRaw > 4096) ? 4096 : nMaxRaw;
			int checked = 0;
			for (int i = 1; i <= nMax && checked < 48 && s_smokeCount < 16; ++i) {
				auto* ent = I::GameEntity->Instance->Get(i);
				if (!Mem::ValidEntity(ent))
					continue;

				SchemaClassInfoData_t* cls = nullptr;
				__try { ent->dump_class_info(&cls); }
				__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
				if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
					continue;
				if (HASH(cls->szName) != HASH("C_SmokeGrenadeProjectile"))
					continue;
				++checked;

				auto* base = reinterpret_cast<std::uint8_t*>(ent);
				bool did = false;
				__try {
					if (Mem::IsReadable(base + s_didOff, 1))
						did = base[s_didOff] != 0;
				} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
				if (!did)
					continue;

				Vector_t center{};
				bool haveCenter = false;
				__try {
					auto* pd = reinterpret_cast<float*>(base + s_detOff);
					if (Mem::IsReadable(pd, 12)
						&& std::isfinite(pd[0]) && std::isfinite(pd[1]) && std::isfinite(pd[2])
						&& (pd[0] != 0.f || pd[1] != 0.f || pd[2] != 0.f)) {
						center = Vector_t{ pd[0], pd[1], pd[2] };
						haveCenter = true;
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
				if (!haveCenter || !Bones::IsValidPos(center))
					continue;

				s_smokes[s_smokeCount++] = SmokeSphere{ center, kSmokeR2 };
			}
		}
	}

	for (int i = 0; i < s_smokeCount; ++i) {
		const Vector_t& center = s_smokes[i].c;
		const float r2 = s_smokes[i].r2;
		const Vector_t w = center - eye;
		float t = (w.x * d.x + w.y * d.y + w.z * d.z) / len2;
		t = std::clamp(t, 0.f, 1.f);
		const Vector_t closest{
			eye.x + d.x * t, eye.y + d.y * t, eye.z + d.z * t
		};
		const float dx = closest.x - center.x;
		const float dy = closest.y - center.y;
		const float dz = closest.z - center.z;
		if (dx * dx + dy * dy + dz * dz <= r2)
			return true;
	}
	return false;
}

int GetSeedTickFromEntry(CCSGOInputHistoryEntryPB* e)
{
	if (!e)
		return 0;
	if (e->nPlayerTickCount > 0)
		return e->nPlayerTickCount;
	return 0;
}

// Player-tick for seed, then tickbase. Render tick is never used for SPREADSEEDGEN.
int GetRenderTick(CUserCmd* cmd, int histIndex, C_CSPlayerPawn* lp)
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

	// Fallback: tickbase (no history player tick)
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


} // namespace AimCommon
