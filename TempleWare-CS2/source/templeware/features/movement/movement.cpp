#define NOMINMAX
#include "movement.h"
#include "../prediction/prediction.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/console/console.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include <Windows.h>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace {
	constexpr std::uintptr_t kStaminaOffFallback = 0x694;
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDeg2Rad = kPi / 180.f;

	void ZeroStamina(C_CSPlayerPawn* pawn)
	{
		void* moveSvc = nullptr;
		__try {
			moveSvc = pawn->m_pMovementServices();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
		if (!moveSvc)
			return;

		std::uintptr_t off = SchemaFinder::Get(
			hash_32_fnv1a_const("CCSPlayer_MovementServices->m_flStamina"));
		if (!off)
			off = kStaminaOffFallback;

		__try {
			*reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(moveSvc) + off) = 0.f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	float NormalizeYaw(float yaw)
	{
		while (yaw > 180.f) yaw -= 360.f;
		while (yaw < -180.f) yaw += 360.f;
		return yaw;
	}

	bool ReadCmdYaw(CUserCmd* cmd, float& yaw)
	{
		yaw = 0.f;
		if (!cmd)
			return false;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pViewAngles) {
				yaw = base->pViewAngles->angValue.y;
				return true;
			}
		}
		if (Input::GetViewAngles && Input::viewAngleContext) {
			const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
			if (viewPtr) {
				yaw = reinterpret_cast<Vector_t*>(viewPtr)->y;
				return true;
			}
		}
		return false;
	}

	// Silent / vectorial: keep camera, rotate wishdir into forward/side.
	void SetWishMoves(CUserCmd* cmd, float viewYawDeg, float wishYawDeg)
	{
		if (!cmd)
			return;
		auto* base = cmd->csgoUserCmd.pBaseCmd;
		if (!base)
			return;

		const float delta = NormalizeYaw(wishYawDeg - viewYawDeg) * kDeg2Rad;
		base->flForwardMove = std::cos(delta) * Pred::kMaxMove;
		base->flSideMove = -std::sin(delta) * Pred::kMaxMove;
		base->flUpMove = 0.f;
		base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);
	}

	void SetSideOnly(CUserCmd* cmd, float side)
	{
		if (!cmd)
			return;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			base->flSideMove = side;
			base->flForwardMove = 0.f;
			base->flUpMove = 0.f;
			base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);
		}
	}

	// Marusense-style: strip jump from cmd + protobuf + every subtick jump step.
	void StripJump(CUserCmd* cmd, CBaseUserCmdPB* base)
	{
		constexpr std::uint64_t kJumpMask = IN_JUMP;

		cmd->nButtons.nValue &= ~kJumpMask;
		cmd->nButtons.nValueChanged &= ~kJumpMask;
		cmd->nButtons.nValueScroll &= ~kJumpMask;

		if (base && base->pInButtonState) {
			base->pInButtonState->nValue &= ~kJumpMask;
			base->pInButtonState->nValueChanged &= ~kJumpMask;
			base->pInButtonState->nValueScroll &= ~kJumpMask;
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
		}

		if (base && base->subtickMovesField.pRep) {
			const int n = base->subtickMovesField.nCurrentSize;
			for (int i = 0; i < n; ++i) {
				CSubtickMoveStep* step = nullptr;
				__try {
					step = base->subtickMovesField.pRep->tElements[i];
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					continue;
				}
				if (!step || (step->nButton & kJumpMask) == 0ULL)
					continue;
				__try {
					step->nButton &= ~kJumpMask;
					step->bPressed = false;
					step->SetBits(
						ESubtickMoveStepBits::MOVESTEP_BITS_BUTTON
						| ESubtickMoveStepBits::MOVESTEP_BITS_PRESSED);
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
		}

		if (base)
			base->SetBits(BASE_BITS_BUTTONPB);
	}
}

// Never writes viewangles / SetViewAngle — that caused camera flicks.
void AutoStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn, const Vector_t& vel, bool onGround)
{
	if (!cmd || !pawn || onGround || !Config::autostrafe)
		return;

	__try {
		const uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
		if (mtOff) {
			const uint8_t moveType = *reinterpret_cast<uint8_t*>(
				reinterpret_cast<uintptr_t>(pawn) + mtOff);
			if (moveType == MOVETYPE_LADDER || moveType == MOVETYPE_NOCLIP)
				return;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}

	const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);

	const bool holdingMove =
		(cmd->nButtons.nValue & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT)) != 0;

	int mouseDx = 0;
	if (auto* base = cmd->csgoUserCmd.pBaseCmd)
		mouseDx = base->nMousedX;

	// Mode 0 — Mouse (legit): only sidemove from mouse delta. No angle touch.
	if (Config::autostrafe_mode == 0) {
		if (holdingMove)
			return;
		if (mouseDx > -2 && mouseDx < 2)
			return;
		SetSideOnly(cmd, mouseDx < 0 ? -Pred::kMaxMove : Pred::kMaxMove);
		return;
	}

	// Mode 1 — Vectorial (silent): optimal wishdir vs current view, remap moves.
	if (speed2d < 5.f)
		return;

	float viewYaw = 0.f;
	if (!ReadCmdYaw(cmd, viewYaw))
		return;

	const float velYaw = Pred::VelocityYawDeg(vel);
	const float ideal = Pred::IdealStrafeDeltaDeg(speed2d);

	float sideSign = 0.f;
	if ((cmd->nButtons.nValue & IN_MOVELEFT) && !(cmd->nButtons.nValue & IN_MOVERIGHT))
		sideSign = -1.f;
	else if ((cmd->nButtons.nValue & IN_MOVERIGHT) && !(cmd->nButtons.nValue & IN_MOVELEFT))
		sideSign = 1.f;
	else if (mouseDx <= -2)
		sideSign = -1.f;
	else if (mouseDx >= 2)
		sideSign = 1.f;
	else {
		const float leftWish = NormalizeYaw(velYaw - ideal);
		const float rightWish = NormalizeYaw(velYaw + ideal);
		const float leftSpd = Pred::PredictSpeedAfterStrafe(vel, leftWish, -1.f);
		const float rightSpd = Pred::PredictSpeedAfterStrafe(vel, rightWish, 1.f);
		sideSign = (leftSpd >= rightSpd) ? -1.f : 1.f;
	}

	float bestWish = NormalizeYaw(velYaw + sideSign * ideal);
	float bestSpd = Pred::PredictSpeedAfterStrafe(vel, bestWish, sideSign);
	for (int i = -2; i <= 2; ++i) {
		if (i == 0)
			continue;
		const float y = NormalizeYaw(velYaw + sideSign * (ideal + static_cast<float>(i) * 1.5f));
		const float spd = Pred::PredictSpeedAfterStrafe(vel, y, sideSign);
		if (spd > bestSpd) {
			bestSpd = spd;
			bestWish = y;
		}
	}

	SetWishMoves(cmd, viewYaw, bestWish);
}

// Marusense BunnyHop: while holding jump in air, strip jump so next ground
// contact can fire a fresh jump (scroll-style). Leave jump alone on ground.
void Bhop(CUserCmd* pCmd)
{
	if (!pCmd || !H::oGetLocalPlayer)
		return;

	C_CSPlayerPawn* pLocalPawn = H::oGetLocalPlayer(0);
	if (!pLocalPawn || pLocalPawn->m_iHealth() <= 0)
		return;

	CBaseUserCmdPB* pBaseCmd = pCmd->csgoUserCmd.pBaseCmd;

	constexpr std::uint64_t kJumpMask = IN_JUMP;
	const std::uint64_t cmdButtons = pCmd->nButtons.nValue;
	const std::uint64_t baseButtons =
		(pBaseCmd && pBaseCmd->pInButtonState) ? pBaseCmd->pInButtonState->nValue : 0ULL;
	if (((cmdButtons | baseButtons) & kJumpMask) == 0ULL)
		return;

	__try {
		const uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
		if (mtOff) {
			const uint8_t moveType = *reinterpret_cast<uint8_t*>(
				reinterpret_cast<uintptr_t>(pLocalPawn) + mtOff);
			if (moveType == MOVETYPE_LADDER || moveType == MOVETYPE_NOCLIP)
				return;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}

	ZeroStamina(pLocalPawn);

	const bool onGround = (pLocalPawn->m_fFlags() & FL_ONGROUND) != 0;
	if (onGround)
		return; // leave held jump — engine jumps on contact

	StripJump(pCmd, pBaseCmd);
}

void Movement::OnCreateMove(CUserCmd* user_cmd)
{
	if (!user_cmd || !H::oGetLocalPlayer)
		return;

	if (g_bMenuOpen)
		return;

	if (I::EngineClient) {
		__try {
			if (!I::EngineClient->in_game() || !I::EngineClient->connected())
				return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
	}

	static bool s_predInit = false;
	if (!s_predInit) {
		Pred::Init();
		s_predInit = true;
	}

	C_CSPlayerPawn* pawn = nullptr;
	__try { pawn = H::oGetLocalPlayer(0); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (!pawn || pawn->m_iHealth() <= 0)
		return;

	Vector_t vel{};
	__try { vel = pawn->m_vecAbsVelocity(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	const bool onGround = (pawn->m_fFlags() & FL_ONGROUND) != 0;

	if (Config::autostrafe) {
		__try {
			AutoStrafe(user_cmd, pawn, vel, onGround);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("AutoStrafe", GetExceptionCode());
		}
	}

	if (Config::bhop) {
		__try {
			Bhop(user_cmd);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("Bhop", GetExceptionCode());
		}
	}
}

std::unique_ptr<Movement> g_movement = std::make_unique<Movement>();
