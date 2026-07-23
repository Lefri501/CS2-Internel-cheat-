#include "aim.h"
#include "aim_common.h"
#include "../autofire/autofire.h"
#include "../triggerbot/triggerbot.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../lefrizzel Ai/interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../lefrizzel Ai/interfaces/interfaces.h"
#include "../../../lefrizzel Ai/interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../lefrizzel Ai/hooks/hooks.h"
#include "../../../lefrizzel Ai/interfaces/CUserCmd/CUserCmd.h"
#include "../../../lefrizzel Ai/config/config.h"
#include "../../../lefrizzel Ai/utils/schema/schema.h"
#include "../../../lefrizzel Ai/utils/fnv1a/fnv1a.h"
#include "../../../lefrizzel Ai/keybinds/keybinds.h"
#include "../../../lefrizzel Ai/offsets/offsets.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../hitchance/hitchance.h"
#include "../nospread/nospread.h"
#include "../autowall/autowall.h"
#include "../gamemode/gamemode.h"
#include "../input_inject/input_inject.h"
#include "../sdk_prio_a/sdk_prio_a.h"
#include "../prediction/prediction.h"
#include "../backtrack/backtrack.h"
#include "../hitmarker/hitmarker.h"
#include "../../../lefrizzel Ai/utils/memory/memsafe/memsafe.h"
#include "../../../lefrizzel Ai/utils/memory/patternscan/patternscan.h"
#include "../../../lefrizzel Ai/utils/console/console.h"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

using AimCommon::CalcAngles;
using AimCommon::GetFov;
using AimCommon::CanWeaponFire;
using AimCommon::IsSniperWeapon;
using AimCommon::IsScopeWeapon;
using AimCommon::IsLocalScoped;
using AimCommon::IsBlinded;
using AimCommon::SmoothToward;
using AimCommon::ResetSmoothState;
using AimCommon::IsBehindWall;
using AimCommon::GetScaledPunch;
using AimCommon::GetFirePunch;
using AimCommon::ApplyPunchSubtract;
using AimCommon::GetViewAngles;
using AimCommon::SetViewAngles;
using AimCommon::NowMs;
using AimCommon::LiveMultipointBloom;
using AimCommon::GetSeedTickFromEntry;
using AimCommon::GetRenderTick;
using AimCommon::kLockGraceFrames;
using AimCommon::g_oldPunch;
using AimCommon::g_hadPunch;
using AimCommon::StandaloneRcs;
using AimCommon::ApplyDeltaRcs;

std::uint32_t g_lockedTarget = 0;
std::uint32_t g_pendingTarget = 0;
std::uint64_t g_lockAcquiredMs = 0;
std::uint64_t g_switchReadyMs = 0;
std::uint64_t g_firstShotReadyMs = 0;
bool g_firstShotArmed = false;
bool g_firstShotDone = false;
bool g_blockFirstShot = false;
int g_lockGrace = 0;
// Sticky hitbox while locked — stops head/chest thrash (magnetic shake)
int g_lockedHitbox = -1;
// Last written aim — hold through FOV grace so camera does not freeze
QAngle_t g_lastAimAngle{};
bool g_lastAimValid = false;

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
	g_lockedHitbox = -1;
	g_lastAimValid = false;
	ResetSmoothState();
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
	g_lockedHitbox = -1;
	// Clean path noise + smooth clock on every new engagement
	ResetSmoothState();
}

// Soft fail: clear lock/delays so flash/scope/FOV blips don't skip reaction later
static bool AimbotSoftFail() {
	ResetHumanize();
	return false;
}

bool RunAimbot(C_CSPlayerPawn* lp) {
	if (!keybind.isActive(Config::aimbot)) {
		ResetHumanize();
		return false;
	}
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return AimbotSoftFail();
	if (!Mem::ValidEntity(lp))
		return AimbotSoftFail();

	C_CSWeaponBase* pWpn = lp->GetActiveWeapon();
	if (!Mem::ValidEntity(pWpn) || pWpn->IsNonGunWeapon())
		return AimbotSoftFail();
	Config::ApplyWeaponGroup(pWpn);

	if (Config::aim_flash_check && IsBlinded(lp))
		return AimbotSoftFail();
	// Shared Scope Check (aim / AF / trigger all use aim_scoped_only)
	if (Config::aim_scoped_only && IsScopeWeapon(pWpn) && !IsLocalScoped(lp, pWpn))
		return AimbotSoftFail();

	if (Config::aimbot_fov <= 0.f)
		return AimbotSoftFail();

	bool anyHb = false;
	for (int h = 0; h < Config::HB_COUNT; ++h) {
		if (Config::aim_hitboxes[h]) {
			anyHb = true;
			break;
		}
	}
	if (!anyHb)
		return AimbotSoftFail();

	if (AimCommon::AimTargetCount() <= 0)
		return AimbotSoftFail();

	// Engine local shoot origin (NetClientInfo ShootPosition) — not m_vOldOrigin
	const Vector_t lep = Bones::GetShootPos(lp);
	if (!Bones::IsValidPos(lep))
		return AimbotSoftFail();

	QAngle_t qView{};
	if (!GetViewAngles(qView))
		return AimbotSoftFail();

	// FOV = bullet direction (view + FULL punch). Menu rcs_scale is view-only;
	// half-scale FOV ranked wrong targets during spray (crosshair ≠ impact).
	QAngle_t qViewAim = qView;
	{
		QAngle_t fullPunch{};
		if (GetFirePunch(lp, fullPunch)) {
			qViewAim.x += fullPunch.x;
			qViewAim.y += fullPunch.y;
			qViewAim.Normalize();
		}
	}

	// Sticky FOV: locked target gets slightly wider window to reduce thrash
	const float baseFov = Config::aimbot_fov;
	const float stickyFov = baseFov * 1.15f;

	// Aimbot: hitbox centers only — closest to crosshair.
	// Sticky hitbox on locked target: no head↔chest thrash while tracking.
	// Backtrack lag stamp is manual-fire only (AimHumanize OnFire).
	float bestFov = baseFov;
	QAngle_t bestAngle{};
	bool found = false;
	std::uint32_t bestHandle = 0;
	int bestHb = -1;

	float lockedFov = stickyFov;
	QAngle_t lockedAngle{};
	bool lockedFound = false;
	int lockedHb = -1;

	// Prefer sticky hitbox if still valid; only switch if clearly better (0.65°)
	constexpr float kHbSwitchSlack = 0.65f;

	for (int ti = 0; ti < AimCommon::AimTargetCount(); ++ti) {
		C_CSPlayerPawn* pawn = AimCommon::AimTargets()[ti].pawn;
		if (!Mem::ValidEntity(pawn))
			continue;

		const std::uint32_t pawnHandle = AimCommon::AimTargets()[ti].handle;
		const bool isLocked = (g_lockedTarget != 0 && pawnHandle == g_lockedTarget);
		const float maxFov = isLocked ? stickyFov : baseFov;

		// Per-target best (for sticky hb preference)
		float tBestFov = maxFov + 1.f;
		QAngle_t tBestAng{};
		int tBestHb = -1;
		float tStickyFov = maxFov + 1.f;
		QAngle_t tStickyAng{};
		bool tStickyOk = false;

		for (int hb = 0; hb < Config::HB_COUNT; ++hb) {
			if (!Config::aim_hitboxes[hb])
				continue;

			Vector_t point{};
			if (!Bones::GetHitboxPoint(pawn, hb, point) || !Bones::IsValidPos(point))
				continue;
			QAngle_t angle{};
			if (!CalcAngles(lep, point, angle))
				continue;

			const float fov = GetFov(qViewAim, angle);
			if (!Mem::Finite(fov) || fov > maxFov)
				continue;

			if (Config::aim_vis_check) {
				if (!Trace::Ready())
					continue;
				if (!Trace::IsVisible(lep, point, lp, pawn, Trace::kMaskVis))
					continue;
			}
			if (Config::aim_smoke_check && AimCommon::LineBlockedBySmoke(lep, point))
				continue;

			if (fov < tBestFov) {
				tBestFov = fov;
				tBestAng = angle;
				tBestHb = hb;
			}

			// Sticky: keep last hitbox while locked if still in FOV
			if (isLocked && g_lockedHitbox >= 0 && hb == g_lockedHitbox) {
				tStickyFov = fov;
				tStickyAng = angle;
				tStickyOk = true;
			}
		}

		if (tBestHb < 0)
			continue;

		// Locked: sticky hitbox unless another is clearly closer.
		QAngle_t pickAng = tBestAng;
		float pickFov = tBestFov;
		int pickHb = tBestHb;
		if (isLocked && tStickyOk && tBestFov + kHbSwitchSlack >= tStickyFov) {
			pickAng = tStickyAng;
			pickFov = tStickyFov;
			pickHb = g_lockedHitbox;
		}

		if (!found || pickFov < bestFov) {
			bestFov = pickFov;
			bestAngle = pickAng;
			found = true;
			bestHandle = pawnHandle;
			bestHb = pickHb;
		}

		if (isLocked) {
			lockedFov = pickFov;
			lockedAngle = pickAng;
			lockedFound = true;
			lockedHb = pickHb;
		}
	}

	if (!found) {
		// Brief miss while locked: keep delay state, don't re-trigger reaction/first-shot
		if (g_lockedTarget != 0 && g_lockGrace > 0) {
			--g_lockGrace;
			// firstShotDone stays false through reaction + first-shot window
			g_blockFirstShot = !g_firstShotDone;
			// Hold last aim so camera does not freeze mid-track
			if (g_lastAimValid && g_lastAimAngle.IsValid())
				SetViewAngles(g_lastAimAngle);
			return true;
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
	int aimHb = bestHb;
	if (g_lockedTarget == bestHandle) {
		aimAngle = bestAngle;
		aimHb = bestHb;
	}
	else if (lockedFound) {
		aimAngle = lockedAngle;
		aimHb = lockedHb;
	}
	else {
		// Locked dead/out of FOV during switch wait — snap engagement to best
		BeginEngagement(bestHandle, now);
		aimAngle = bestAngle;
		aimHb = bestHb;
	}

	// Remember hitbox for sticky tracking
	if (aimHb >= 0)
		g_lockedHitbox = aimHb;

	// AF/aimbot never arm lag-comp. Manual fire only (AimHumanize OnFire).
	Backtrack::ClearPending();

	// Reaction: no view move yet (legit acquire). Fire blocked the whole window.
	// Stacks with First Shot: total wait ≈ reaction + first_shot before LMB allowed.
	// Return true = keep frame ownership (block standalone RCS + AF steal).
	if (reactionMs > 0.01f && (now - g_lockAcquiredMs) < static_cast<std::uint64_t>(reactionMs)) {
		g_blockFirstShot = true;
		return true;
	}

	// First shot delay (strip LMB only; smooth may run after reaction)
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
	// view_set = aim - punch  ⇒  view + punch = aim (IDA FX_FireBullets)
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
		return AimbotSoftFail();

	// Prefer live Config after ApplyWeaponGroup (same as profile)
	const float smooth = std::clamp(Config::aimbot_smooth, 0.f, 50.f);
	QAngle_t finalAngle = aimAngle;
	if (smooth > 0.01f) {
		QAngle_t cur{};
		if (GetViewAngles(cur) && cur.IsValid())
			finalAngle = SmoothToward(cur, aimAngle, smooth);
	}

	if (!finalAngle.IsValid())
		return AimbotSoftFail();

	SetViewAngles(finalAngle);
	g_lastAimAngle = finalAngle;
	g_lastAimValid = true;
	// Do NOT ApplyDeltaRcs while aimbot-RCS is active — absolute punch already applied
	return true;
}



// IDA CCSGOInput button words — auto_pistol: only +0x258 = IsButtonActive code 3
constexpr std::uintptr_t kCsgoBtn0 = 0x250;
constexpr std::uintptr_t kCsgoBtn1 = 0x258;
constexpr std::uintptr_t kCsgoBtn2 = 0x260;
constexpr std::uintptr_t kCsgoBtn3 = 0x268;

static void ClearCsgoAttackInput()
{
	void* pInput = Input::GetCSGOInput();
	if (!pInput)
		return;
	constexpr std::uint64_t kAttack = IN_ATTACK;
	__try {
		auto* b = reinterpret_cast<std::uint8_t*>(pInput);
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn0) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn1) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn2) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn3) &= ~kAttack;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Rising edge for semi: clear other slots, set active word (+0x258)
static void EdgeCsgoAttackInput()
{
	void* pInput = Input::GetCSGOInput();
	if (!pInput)
		return;
	constexpr std::uint64_t kAttack = IN_ATTACK;
	__try {
		auto* b = reinterpret_cast<std::uint8_t*>(pInput);
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn0) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn2) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn3) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn1) |= kAttack;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Semi/bolt (AWP, deagle, pistols…): rising edge required. Sticky WantShoot holds
// IN_ATTACK → FIRE log, zero bullets. Full-auto holds bit only.
// After semi press, release next CreateMove so next arm can edge again.
static bool s_semiNeedRelease = false;
// Full-auto: track OUR hold state across frames. cmd is fresh each CreateMove so
// cmd->nButtons.nValue only reflects player input, not our previous AF press.
// Without this, nValueChanged is set every frame → server sees repeated "press"
// events → spray desync (bullets climb / random spread reset).
static bool s_autoHeld = false;

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
		s_autoHeld = false;
	}

static void PressScope(CUserCmd* cmd) {
	if (!cmd)
		return;
	__try {
		constexpr std::uint64_t kScope = IN_SECOND_ATTACK;
		cmd->nButtons.nValue |= kScope;
		cmd->nButtons.nValueChanged |= kScope;
		// Alive only — dead/respawn moveSvc free'd (TDM crash)
		if (C_CSPlayerPawn* lp = H::SafeLocalAlive())
			InputInject::ForceDown(lp, kScope);
		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (base && base->pInButtonState
			&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
			base->pInButtonState->nValue |= kScope;
			base->pInButtonState->nValueChanged |= kScope;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static bool ActiveWeaponIsSemi()
{
	C_CSPlayerPawn* lp = H::SafeLocalAlive();
	if (!lp)
		return false;
	C_CSWeaponBase* w = nullptr;
	__try { w = lp->GetActiveWeapon(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return Mem::ValidEntity(w) && AimCommon::IsSemiWeapon(w);
}

static void PressAttack(CUserCmd* cmd) {
	if (!cmd)
		return;

	__try {
		constexpr std::uint64_t kAttack = IN_ATTACK;
		// Full-auto: hold bit, edge only on first down (re-edge every CM = spray desync).
		// Semi/bolt: CS2 needs rising edge on cmd AND CCSGOInput+0x258.
		// Holding M1 / sticky WantShoot keeps IN_ATTACK → no edge → seed FIRE, no bullet.
		// Match auto_pistol: clear → value|changed → inject +0x258 → release next CM.
		const bool semi = ActiveWeaponIsSemi();
		if (semi) {
			// Force release state first so this frame is a true press edge
			cmd->nButtons.nValue &= ~kAttack;
			cmd->nButtons.nValueChanged &= ~kAttack;
			cmd->nButtons.nValueScroll &= ~kAttack;
			if (CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd) {
				if (base->pInButtonState
					&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
					base->pInButtonState->nValue &= ~kAttack;
					base->pInButtonState->nValueChanged &= ~kAttack;
					base->pInButtonState->nValueScroll &= ~kAttack;
				}
			}
			ClearCsgoAttackInput();
		}

		const bool alreadyDown = s_autoHeld || (cmd->nButtons.nValue & kAttack) != 0;
		cmd->nButtons.nValue |= kAttack;
		if (semi || !alreadyDown)
			cmd->nButtons.nValueChanged |= kAttack;
		cmd->nButtons.nValueScroll &= ~kAttack;
		if (semi)
			s_semiNeedRelease = true;
		else
			s_autoHeld = true;

		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (base && base->pInButtonState
			&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
			const bool pbDown = s_autoHeld || (base->pInButtonState->nValue & kAttack) != 0;
			base->pInButtonState->nValue |= kAttack;
			if (semi || !pbDown)
				base->pInButtonState->nValueChanged |= kAttack;
			base->pInButtonState->nValueScroll &= ~kAttack;
			// Only signal protobuf state change on first press (semi always edges)
			if (semi || !pbDown) {
				base->pInButtonState->SetBits(
					BUTTON_STATE_PB_BITS_BUTTONSTATE1
					| BUTTON_STATE_PB_BITS_BUTTONSTATE2
					| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
				base->SetBits(BASE_BITS_BUTTONPB);
			}
		}

		if (semi)
			EdgeCsgoAttackInput();

		// Bind attack-hist. Seed path already wrote tick/angles/eye in AF/trigger
		// FIRE — do NOT re-StampInputHistory (WriteSubtickFromEntry can skew
		// nPlayerTickCount / frac → different SPREADSEEDGEN hash → miss).
		if (Triggerbot::WantShoot() || Autofire::WantShoot()) {
			const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
			int idx = cmd->csgoUserCmd.nAttack1StartHistoryIndex;
			if (Triggerbot::FireValid() && Triggerbot::FireHistIndex() >= 0)
				idx = Triggerbot::FireHistIndex();
			else if (Autofire::SilentValid() && Autofire::FireHistIndex() >= 0)
				idx = Autofire::FireHistIndex();
			if (idx < 0 && histCount > 0)
				idx = histCount - 1;
			if (idx >= 0 && histCount > 0) {
				cmd->csgoUserCmd.SetAttack1StartHistoryIndex(idx);

				CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(idx);
				const bool seedPath = Triggerbot::FireValid() || Autofire::SilentValid();
				const QAngle_t& ang = Triggerbot::FireValid() ? Triggerbot::FireAngle()
					: (Autofire::SilentValid() ? Autofire::SilentAngle() : QAngle_t{});
				const Vector_t& eye = Triggerbot::FireValid() ? Triggerbot::FireEye()
					: (Autofire::SilentValid() ? Autofire::SilentEye() : Vector_t{});

				// Silent AF: stamp bone/view aim only — NEVER compensated seed angles
				// (seed rewrite can be 20–50° for air AWP → model flick + desync).
				// Seed fire uses attack-hist only (SetAttackHistoryFire below).
				if (Config::autofire_silent && Autofire::SilentValid() && ang.IsValid()) {
					const bool seedMode =
						Config::autofire_mode == Config::AF_MODE_SEED_NOSPREAD;
					if (!seedMode)
						AimCommon::StampCmdAngles(ang);
				}

			if (seedPath && ang.IsValid()) {
					// Seed path: AF/Trigger already called SetAttackHistoryFire with solved
					// angles + eye + tick/frac. Do NOT re-stamp — second call can reset fields
					// the seed solve carefully set (desync source). Only bind attack1 index
					// and note hitmarker.
					// World hitmarker pin: fire ray = punched view (matches FireBullet).
					if (Bones::IsValidPos(eye)) {
						QAngle_t rayAng = ang;
						QAngle_t punch{};
						if (AimCommon::GetFirePunch(H::SafeLocalPlayer(), punch)
							&& punch.IsValid()) {
							rayAng.x += punch.x;
							rayAng.y += punch.y;
						}
						rayAng.z = 0.f;
						rayAng.Normalize();
						Hitmarker::NoteLastFire(eye, rayAng);
					}
				} else if (!seedPath && ang.IsValid()) {
					// Hitchance / non-seed: fill angles + subtick stamp.
					cmd->SetAttackHistoryFire(idx, ang, eye);
					if (Bones::IsValidPos(eye)) {
						QAngle_t rayAng = ang;
						QAngle_t punch{};
						if (AimCommon::GetFirePunch(H::SafeLocalPlayer(), punch)
							&& punch.IsValid()) {
							rayAng.x += punch.x;
							rayAng.y += punch.y;
						}
						rayAng.z = 0.f;
						rayAng.Normalize();
						Hitmarker::NoteLastFire(eye, rayAng);
					}
					e = cmd->GetInputHistoryEntry(idx);
					if (e) {
						int ptick = e->nPlayerTickCount > 0 ? e->nPlayerTickCount : 0;
						int rtick = e->nRenderTickCount > 0 ? e->nRenderTickCount : ptick;
						if (ptick > 0 && rtick > 0) {
							const int skew = rtick > ptick ? rtick - ptick : ptick - rtick;
							if (skew > 1)
								rtick = ptick;
						}
						float pfrac = e->flPlayerTickFraction;
						float rfrac = e->flRenderTickFraction;
						if (!std::isfinite(pfrac)) pfrac = 0.f;
						if (!std::isfinite(rfrac)) rfrac = pfrac;
						if (pfrac < 0.f || pfrac >= 1.f) pfrac = 0.f;
						if (rfrac < 0.f || rfrac >= 1.f) rfrac = pfrac;
						if (ptick > 0)
							Pred::StampInputHistory(e, rtick, rfrac, ptick, pfrac, ang, eye);
					}
				}
			} else if (cmd->csgoUserCmd.nAttack1StartHistoryIndex >= 0) {
				cmd->csgoUserCmd.CheckAndSetBits(CCSGOUserCmdPB::BITS_ATTACK1START);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("PressAttack", GetExceptionCode());
	}
}

// Counter-strafe: cancel horizontal velocity for standing accuracy
static void ApplyAutoStop(CUserCmd* cmd, C_CSPlayerPawn* lp) {
	if (!cmd || !lp)
		return;

	const Vector_t vel = Pred::Velocity(lp);
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

void Aimbot_Reset() {
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		AimCommon::g_hadPunch = false;
		AimCommon::ClearTargets();
		AimCommon::UnbindCmd();
		s_semiNeedRelease = false;
		s_autoHeld = false;
	}

void AimHumanize_OnCreateMove(CUserCmd* cmd) {
	if (!cmd)
		return;

	// Belt-and-suspenders: never counter-strafe unless the matching Autostop toggle is on.
	const bool wantStop =
		(Config::autofire_autostop && Autofire::WantStop())
		|| (Config::trigger_autostop && Triggerbot::WantStop());
	if (wantStop) {
		if (C_CSPlayerPawn* lp = H::SafeLocalPlayer()) {
			if (lp->m_iHealth() > 0) {
				// Autostop is ground-only — never rewrite move while airborne
				if ((Pred::Flags(lp) & FL_ONGROUND) != 0)
					ApplyAutoStop(cmd, lp);
			}
		}
	}

	// Fire only when AF/trigger armed WantShoot.
	// Strip manual M1 during aimbot OR autofire reaction/first-shot windows —
	// old path only used g_blockFirstShot (aimbot). AF BlockFirstShot was ignored
	// so holding M1 during AF delays bypassed humanize + desynced seed hist.
	//
	// Semi/bolt (AWP/deagle/pistols): press (cmd edge + CCSGOInput+0x258) one CM,
	// full release next. Without release, held M1 / sticky +0x258 never rises
	// → seed FIRE log, no bullet (log 00:48 flood).
	const bool afFire = Autofire::WantShoot() || Triggerbot::WantShoot();
		if (s_semiNeedRelease) {
			// Release unconditionally — don't gate on ActiveWeaponIsSemi().
			// Weapon may have switched (quickswitch) between press and release frame;
			// gating on current weapon leaves old edge stuck → next semi has no rising edge.
			StripAttack(cmd);
			ClearCsgoAttackInput();
			s_semiNeedRelease = false;
			s_autoHeld = false;
		} else if (afFire) {
		PressAttack(cmd);
	} else if (g_blockFirstShot || Autofire::BlockFirstShot()) {
		StripAttack(cmd);
		if (ActiveWeaponIsSemi())
			ClearCsgoAttackInput();
		s_semiNeedRelease = false;
	} else {
		if (s_semiNeedRelease) {
			ClearCsgoAttackInput();
			s_semiNeedRelease = false;
		}
	}

	// Lag-comp: manual M1 only (never AF / trigger).
	if (Config::backtrack && !afFire
		&& (cmd->nButtons.nValue & IN_ATTACK)
		&& !g_blockFirstShot
		&& !Autofire::BlockFirstShot()) {
		__try {
			Backtrack::OnFire(cmd, H::SafeLocalPlayer(), Config::backtrack_ms);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("Backtrack::OnFire", GetExceptionCode());
		}
	}

	// Manual fire ray for tracers — guns only (knife M1 must not spawn beams).
	// AF/TR already call NoteLastFire in PressAttack.
	if (Config::tracers && !afFire
		&& (cmd->nButtons.nValue & IN_ATTACK)
		&& !g_blockFirstShot
		&& !Autofire::BlockFirstShot()) {
		__try {
			C_CSPlayerPawn* lp = H::SafeLocalPlayer();
			if (lp) {
				C_CSWeaponBase* w = nullptr;
				__try { w = lp->GetActiveWeapon(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
				if (w && Mem::ValidEntity(w) && !w->IsNonGunWeapon()) {
					Vector_t eye = Bones::GetShootPos(lp);
					QAngle_t view{};
					if (Bones::IsValidPos(eye) && AimCommon::GetViewAngles(view) && view.IsValid()) {
						QAngle_t punch{};
						if (AimCommon::GetFirePunch(lp, punch) && punch.IsValid()) {
							view.x += punch.x;
							view.y += punch.y;
						}
						view.z = 0.f;
						view.Normalize();
						Hitmarker::NoteLastFire(eye, view);
					}
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// Autoscope after strip/fire so attack2 is not wiped by StripAttack.
	if (Autofire::WantScope())
		PressScope(cmd);
}

void Aimbot(CUserCmd* cmd) {
	// Cleared every CreateMove; RunAimbot re-arms only while its delays are active.
	// Prevents sticky strip when AF owns the frame (RunAimbot not called).
	// BindCmd: sehCreateMovePost (spans AimHumanize PressAttack stamp).
	g_blockFirstShot = false;
	(void)cmd;

	if (g_bMenuOpen) {
		// Still track held weapon group for menu "Live:" / Jump Active / PushLive.
		// Do not run combat; do not ApplyProfile (menu PushLive owns live when editing active).
		if (C_CSPlayerPawn* mlp = H::SafeLocalPlayer()) {
			if (mlp->m_iHealth() > 0) {
				C_CSWeaponBase* wpn = mlp->GetActiveWeapon();
				if (Mem::ValidEntity(wpn) && !wpn->IsNonGunWeapon())
					Config::weapon_group_active = Config::ClassifyWeaponGroup(wpn);
				else
					Config::weapon_group_active = Config::WG_GENERAL;
			}
		}
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		g_hadPunch = false;
		return;
	}
	// Priority A: soft-disable combat on Overwatch / demo / HLTV
	if (SdkPrioA::ShouldSoftDisableCombat()) {
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
		g_hadPunch = false;
		return;
	}
	if (!Input::GetViewAngles || !Input::SetViewAngle || !Input::viewAngleContext)
		return;

	// Alive only — dead pawn weapon services free mid-TDM
	C_CSPlayerPawn* lp = H::SafeLocalAlive();
	if (!lp) {
		g_hadPunch = false;
		ResetHumanize();
		Autofire::Reset();
		Triggerbot::Reset();
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

	// Target list built once in sehCreateMovePost (shared with Backtrack).
	// Rebuild only if empty (menu path / early return skipped collect).
	if (AimCommon::AimTargetCount() <= 0)
		AimCommon::CollectAimTargets(lp);

	// Autofire owns aim only while Run() returns true (tracking / grace / delays).
	// Silent AF key held with no target must NOT block aimbot — old silentAfHold
	// killed visible aim whenever the AF key was down without a lock.
	const bool af = Autofire::Run(lp, cmd);
	const bool aimed = af ? true : RunAimbot(lp);
	if (!af)
		Autofire::ClearShootFlags();

	// Triggerbot: skip entirely while autofire owns aim (avoid dual HC/seed)
	if (!af)
		Triggerbot::Run(lp, cmd);
	else
		Triggerbot::ClearShootFlags();

	// Standalone RCS only when aimbot/autofire not locking (avoids double RCS).
	// While absolute aimbot RCS owns the frame: seed delta baseline from live punch
	// so standalone doesn't spike view when lock drops mid-spray.
	// On aimbot→standalone transition: reset baseline so first standalone frame
	// seeds fresh instead of applying stale delta (punch recovery spike).
	static bool s_wasAimed = false;
	if (Config::rcs_standalone && !aimed) {
		if (s_wasAimed) {
			// Transition frame: seed baseline, don't apply delta
			g_hadPunch = false;
		}
		StandaloneRcs(lp);
	} else if (aimed && Config::rcs) {
		QAngle_t p{};
		if (HitChance::ReadAimPunch(lp, p) || AimCommon::ReadAimPunch(lp, p)) {
			g_oldPunch = p;
			g_hadPunch = true;
		}
	} else if (!Config::rcs && !Config::rcs_standalone) {
		g_hadPunch = false;
	}
	s_wasAimed = aimed;
}
