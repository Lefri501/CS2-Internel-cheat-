#define NOMINMAX
#include "movement.h"
#include "../prediction/prediction.h"
#include "../subtick_move/subtick_move.h"
#include "../input_inject/input_inject.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/console/console.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include <Windows.h>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>


namespace {
	// Dump build 14169 (client_dll.hpp) — hard fallbacks when schema miss returns 0.
	constexpr std::uintptr_t kFlagsOff = 0x3F4;
	constexpr std::uintptr_t kAbsVelOff = 0x3F8;
	constexpr std::uintptr_t kGroundEntityOff = 0x530;
	constexpr std::uintptr_t kMoveServicesOff = 0x1248; // C_BasePlayerPawn->m_pMovementServices
	constexpr std::uintptr_t kButtonsOff = 0x50;        // CPlayer_MovementServices->m_nButtons
	constexpr std::uintptr_t kStaminaOffFallback = 0x694;
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDeg2Rad = kPi / 180.f;
	constexpr std::uint64_t kJumpMask = IN_JUMP;
	constexpr std::uint64_t kMoveMask =
		IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;

	// IDA ProcessSubTickInput: flWhen must be < 1.0; max 32 steps/tick.
	// UC FOrna subtick bhop: press @ landFrac (or 0), release @ end-of-tick.
	constexpr float kSubtickWhenRelease = 0.999f;



	void* GetMoveServices(C_CSPlayerPawn* pawn)
	{
		if (!pawn)
			return nullptr;
		void* svc = nullptr;
		__try {
			svc = pawn->m_pMovementServices();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			svc = nullptr;
		}
		if (svc)
			return svc;
		// Schema miss → hard dump offset
		__try {
			svc = *reinterpret_cast<void**>(
				reinterpret_cast<std::uintptr_t>(pawn) + kMoveServicesOff);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
		return svc;
	}

	void ZeroStamina(C_CSPlayerPawn* pawn)
	{
		void* moveSvc = GetMoveServices(pawn);
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

	std::uint64_t CmdButtons(CUserCmd* cmd)
	{
		if (!cmd)
			return 0ULL;
		std::uint64_t buttons = cmd->nButtons.nValue;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pInButtonState)
				buttons |= base->pInButtonState->nValue;
		}
		return buttons;
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

	bool BadMoveType(C_CSPlayerPawn* pawn)
	{
		__try {
			const uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
			if (!mtOff)
				return false;
			const uint8_t moveType = *reinterpret_cast<uint8_t*>(
				reinterpret_cast<uintptr_t>(pawn) + mtOff);
			return moveType == MOVETYPE_LADDER
				|| moveType == MOVETYPE_NOCLIP
				|| moveType == MOVETYPE_OBSERVER;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;
		}
	}

	bool InWater(std::uint32_t flags)
	{
		return (flags & (FL_INWATER | FL_WATERJUMP)) != 0U;
	}

	// Live pawn flags/vel. Prefer dump 0x3F4; OR schema only for FL_ONGROUND bit.
	bool ReadLiveMoveState(C_CSPlayerPawn* pawn, Vector_t& vel, std::uint32_t& flags)
	{
		vel = Vector_t{ 0.f, 0.f, 0.f };
		flags = 0;
		if (!pawn)
			return false;
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(pawn);
			const std::uintptr_t schemaOff = SchemaFinder::Get(
				hash_32_fnv1a_const("C_BaseEntity->m_fFlags"));
			const std::uint32_t dumpF = *reinterpret_cast<std::uint32_t*>(base + kFlagsOff);
			std::uint32_t schemaF = 0;
			if (schemaOff && schemaOff != kFlagsOff)
				schemaF = *reinterpret_cast<std::uint32_t*>(base + schemaOff);
			flags = dumpF;
			// Only promote real ONGROUND — never PARTIAL (false ground mid-air).
			if (schemaF & FL_ONGROUND)
				flags |= FL_ONGROUND;

			vel = pawn->m_vecAbsVelocity();
			if (vel.x == 0.f && vel.y == 0.f && vel.z == 0.f)
				vel = *reinterpret_cast<Vector_t*>(base + kAbsVelOff);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Bhop ground: FL_ONGROUND only.
	// PARTIALGROUND + m_hGroundEntity lag mid-air → false ground → sticky RemoveJump.
	// IDA CheckJumpButton / ModernJump gate on real ONGROUND + IsButtonActive(IN_JUMP).
	bool IsOnGround(C_CSPlayerPawn* /*pawn*/, std::uint32_t flags)
	{
		return (flags & FL_ONGROUND) != 0U;
	}

	// IDA PreSource1AirMove 0x1808A49D0:
	//   AngleVectors(view) → fwd, right  (right = (-sy,cy) = visual LEFT)
	//   wishvel = fwd*fmove + right*smove
	// ⇒ +flSideMove = LEFT (IN_MOVELEFT). Bits: LEFT=512 RIGHT=1024.
	void ClearMoveKeys(CUserCmd* cmd)
	{
		if (!cmd)
			return;
		cmd->nButtons.nValue &= ~kMoveMask;
		cmd->nButtons.nValueChanged &= ~kMoveMask;
		cmd->nButtons.nValueScroll &= ~kMoveMask;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pInButtonState) {
				base->pInButtonState->nValue &= ~kMoveMask;
				base->pInButtonState->nValueChanged &= ~kMoveMask;
				base->pInButtonState->nValueScroll &= ~kMoveMask;
				base->pInButtonState->SetBits(
					BUTTON_STATE_PB_BITS_BUTTONSTATE1
					| BUTTON_STATE_PB_BITS_BUTTONSTATE2
					| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			}
			base->SetBits(BASE_BITS_BUTTONPB);
		}
	}

	void SyncMoveButtons(CUserCmd* cmd, float fmove, float smove)
	{
		if (!cmd)
			return;
		ClearMoveKeys(cmd);
		std::uint64_t add = 0ULL;
		if (fmove > 1.f) add |= IN_FORWARD;
		else if (fmove < -1.f) add |= IN_BACK;
		if (smove > 1.f) add |= IN_MOVELEFT;   // +side = left
		else if (smove < -1.f) add |= IN_MOVERIGHT;
		if (!add)
			return;
		cmd->nButtons.nValue |= add;
		cmd->nButtons.nValueChanged |= add;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->pInButtonState) {
				base->pInButtonState->nValue |= add;
				base->pInButtonState->nValueChanged |= add;
				base->pInButtonState->SetBits(
					BUTTON_STATE_PB_BITS_BUTTONSTATE1
					| BUTTON_STATE_PB_BITS_BUTTONSTATE2
					| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			}
			base->SetBits(BASE_BITS_BUTTONPB);
		}
	}

	void ApplyMoves(CUserCmd* cmd, float fmove, float smove)
	{
		if (!cmd)
			return;
		auto* base = cmd->csgoUserCmd.pBaseCmd;
		if (!base)
			return;
		base->flForwardMove = fmove;
		base->flSideMove = smove;
		base->flUpMove = 0.f;
		base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);
		SyncMoveButtons(cmd, fmove, smove);
	}

	// wish = fmove*fwd + smove*right(left) → unit (cos wish, sin wish)
	void SetWishMoves(CUserCmd* cmd, float viewYawDeg, float wishYawDeg)
	{
		const float delta = NormalizeYaw(wishYawDeg - viewYawDeg) * kDeg2Rad;
		ApplyMoves(cmd,
			std::cos(delta) * Pred::kMaxMove,
			std::sin(delta) * Pred::kMaxMove);
	}

	// Subtick analog push — IDA CreateMove fill merges flAnalog* via ProcessSubTick path.
	// Uses verified add_subtick_move (CreateNewSubtickMoveStep @ IDA 0x1804E23F0),
	// NOT UC post hard RVA 0x9CA720 (interp debug — wrong).
	void AddMoveSubtick(CBaseUserCmdPB* pBaseCmd, float when, float fwdDelta, float leftDelta)
	{
		if (!pBaseCmd)
			return;
		if (pBaseCmd->subtickMovesField.nCurrentSize >= 30)
			return;
		CSubtickMoveStep* step = pBaseCmd->add_subtick_move();
		if (!step || reinterpret_cast<std::uintptr_t>(step) < 0x10000)
			return;
		step->nHasBits = 0;
		step->nCachedBits = 0;
		step->nButton = 0;
		step->bPressed = false;
		step->flWhen = std::clamp(when, 0.0f, kSubtickWhenRelease);
		step->flAnalogForwardDelta = fwdDelta;
		step->flAnalogLeftDelta = leftDelta;
		// IDA writes presence into +0x10 (nCachedBits), not nHasBits
		step->SetBits(
			MOVESTEP_BITS_WHEN
			| MOVESTEP_BITS_ANALOG_FORWARD_DELTA
			| MOVESTEP_BITS_ANALOG_LEFT_DELTA);
	}

	// CPlayer_MovementServices::m_nButtons @ +0x50
	// CInButtonState: vtable@0, nValue@+8, nValueChanged@+10, nValueScroll@+18
	// IDA IsButtonActive 0x1804AB5B0: active only if code >= 3
	//   value alone=1 dead, changed alone=2 dead, value|changed=3 ok, scroll=4 ok.
	struct MoveSvcButtons {
		std::uint64_t padVtable;     // 0x00
		std::uint64_t nValue;        // 0x08
		std::uint64_t nValueChanged; // 0x10
		std::uint64_t nValueScroll;  // 0x18
	};

	MoveSvcButtons* GetMoveSvcButtons(void* moveSvc)
	{
		if (!moveSvc)
			return nullptr;
		return reinterpret_cast<MoveSvcButtons*>(
			reinterpret_cast<std::uintptr_t>(moveSvc) + kButtonsOff);
	}

	// IDA sub_180A221C0 → IsButtonActive(moveSvc+0x50, IN_JUMP=2).
	// value|changed (3) or scroll (4+) required — value alone not enough.
	void WriteMoveSvcJump(void* moveSvc, bool pressed)
	{
		MoveSvcButtons* b = GetMoveSvcButtons(moveSvc);
		if (!b)
			return;
		__try {
			if (pressed) {
				b->nValue |= kJumpMask;
				b->nValueChanged |= kJumpMask;
				// scroll alone also active, but keep it off so edge is clean
				b->nValueScroll &= ~kJumpMask;
			} else {
				b->nValue &= ~kJumpMask;
				b->nValueChanged &= ~kJumpMask;
				b->nValueScroll &= ~kJumpMask;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// Original CreateMove already wrote jump subticks for held space.
	// Leaving them latched = no edge = no hop. Neutralize IN_JUMP steps.
	void ClearJumpSubticks(CBaseUserCmdPB* pBaseCmd)
	{
		if (!pBaseCmd || !pBaseCmd->subtickMovesField.pRep)
			return;
		const int n = pBaseCmd->subtickMovesField.nCurrentSize;
		if (n <= 0)
			return;
		const int cap = pBaseCmd->subtickMovesField.pRep->nAllocatedSize;
		const int count = (n < cap) ? n : cap;
		for (int i = 0; i < count; ++i) {
			CSubtickMoveStep* step = pBaseCmd->subtickMovesField.pRep->tElements[i];
			if (!step)
				continue;
			if (step->nButton != kJumpMask)
				continue;
			step->nButton = 0;
			step->bPressed = false;
			step->flWhen = 0.f;
			step->nHasBits = 0;
			step->nCachedBits = 0;
		}
	}

	// Strip jump: cmd + PB + moveSvc + subticks (hold-space release edge)
	void RemoveJump(CUserCmd* pCmd, CBaseUserCmdPB* pBaseCmd, void* moveSvc)
	{
		if (pCmd) {
			pCmd->nButtons.nValue &= ~kJumpMask;
			pCmd->nButtons.nValueChanged &= ~kJumpMask;
			pCmd->nButtons.nValueScroll &= ~kJumpMask;
		}
		if (pBaseCmd && pBaseCmd->pInButtonState) {
			pBaseCmd->pInButtonState->nValue &= ~kJumpMask;
			pBaseCmd->pInButtonState->nValueChanged &= ~kJumpMask;
			pBaseCmd->pInButtonState->nValueScroll &= ~kJumpMask;
			pBaseCmd->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			pBaseCmd->SetBits(BASE_BITS_BUTTONPB);
		}
		WriteMoveSvcJump(moveSvc, false);
		ClearJumpSubticks(pBaseCmd);
	}

	// Land hop: single rising-edge press. No same-tick release.
	// IDA modern (0x180882A20): hold + spam window (sv_jump_spam_penalty_time)
	// treats continuous press as not-pressed. Release happens next air tick.
	// IsButtonActive needs value|changed (code>=3).
	void AddJump(CUserCmd* pCmd, CBaseUserCmdPB* pBaseCmd, void* moveSvc,
		bool withSubticks, float pressWhen = 0.f)
	{
		if (pCmd) {
			pCmd->nButtons.nValue |= kJumpMask;
			pCmd->nButtons.nValueChanged |= kJumpMask;
			pCmd->nButtons.nValueScroll &= ~kJumpMask;
		}
		if (pBaseCmd && pBaseCmd->pInButtonState) {
			pBaseCmd->pInButtonState->nValue |= kJumpMask;
			pBaseCmd->pInButtonState->nValueChanged |= kJumpMask;
			pBaseCmd->pInButtonState->nValueScroll &= ~kJumpMask;
			pBaseCmd->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			pBaseCmd->SetBits(BASE_BITS_BUTTONPB);
		}
		WriteMoveSvcJump(moveSvc, true);
		if (!withSubticks || !pBaseCmd)
			return;

		const float when = std::clamp(pressWhen, 0.f, kSubtickWhenRelease);

		if (CSubtickMoveStep* press = pBaseCmd->add_subtick_move()) {
			press->nHasBits = 0;
			press->nCachedBits = 0;
			press->nButton = kJumpMask;
			press->bPressed = true;
			press->flWhen = when;
			press->flAnalogForwardDelta = 0.f;
			press->flAnalogLeftDelta = 0.f;
			press->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN);
		}
	}

	// Dump offsets (client_dll.hpp build 14169):
	//   CCSPlayer_MovementServices::m_LegacyJump  0x6B0  (m_bOldJumpPressed @ +0x10)
	//   CCSPlayer_MovementServices::m_ModernJump  0x6C8
	//     m_nLastActualJumpPressTick   +0x10  GameTick_t
	//     m_flLastActualJumpPressFrac  +0x14
	//     m_nLastUsableJumpPressTick   +0x18  GameTick_t  (-1024 = invalid)
	//     m_flLastUsableJumpPressFrac  +0x1C
	//     m_nLastLandedTick            +0x20
	//     m_flLastLandedFrac           +0x24
	// IDA: sv_legacy_jump ON  → sub_180882870(LegacyJump)  was-pressed BYTE @ +0x10
	//      sv_legacy_jump OFF → sub_180882A20(ModernJump)  tick/frac spam + land window
	// NEVER write ModernJump+0x10 as a single byte — that is a GameTick_t.
	constexpr std::uintptr_t kLegacyJumpOff = 0x6B0;
	constexpr std::uintptr_t kModernJumpOff = 0x6C8;
	constexpr std::uintptr_t kLastLandedFracOff = 0x24;
	constexpr std::int32_t kInvalidJumpTick = -1024; // engine sentinel after hop / fail

	void ClearJumpHistory(void* moveSvc)
	{
		if (!moveSvc)
			return;
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(moveSvc);
			// Legacy: m_bOldJumpPressed only
			*reinterpret_cast<std::uint8_t*>(base + kLegacyJumpOff + 0x10) = 0;

			// Modern: force next press to be a fresh usable edge (not spam-latched)
			*reinterpret_cast<std::int32_t*>(base + kModernJumpOff + 0x10) = kInvalidJumpTick;
			*reinterpret_cast<float*>(base + kModernJumpOff + 0x14) = 0.f;
			*reinterpret_cast<std::int32_t*>(base + kModernJumpOff + 0x18) = kInvalidJumpTick;
			*reinterpret_cast<float*>(base + kModernJumpOff + 0x1C) = 0.f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	bool ReadModernJumpLandFrac(void* moveSvc, float& outFrac)
	{
		outFrac = 0.f;
		if (!moveSvc)
			return false;
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(moveSvc);
			outFrac = *reinterpret_cast<float*>(base + kModernJumpOff + kLastLandedFracOff);
			return outFrac > 0.f && outFrac < 1.f;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool HasGroundEntity(C_CSPlayerPawn* pawn)
	{
		if (!pawn)
			return false;
		__try {
			const auto base = reinterpret_cast<std::uintptr_t>(pawn);
			const std::uint32_t h = *reinterpret_cast<std::uint32_t*>(base + kGroundEntityOff);
			// CHandle invalid = 0xFFFFFFFF
			return h != 0u && h != 0xFFFFFFFFu;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// UC CreateSubtickStep — IDA-verified path only.
	// REJECTED: client+0x9CA720 (post) → interp sample dump, not a step factory.
	// ACCEPTED: add_subtick_move → CreateNewSubtickMoveStep (IDA 0x1804E23F0) + Add.
	//   pattern @ CreateMove fill: E8 ? ? ? ? 48 8B D0 48 8B CE E8 ? ? ? ? 48 8B C8
	//   ctor 0x1804E34D0 size 56; presence bits live at +0x10 (nCachedBits).
	// NEVER zero base->nCachedBits (post bug wipes forward/side/button presence).
	CSubtickMoveStep* CreateSubtickStep(CBaseUserCmdPB* base)
	{
		if (!base)
			return nullptr;
		if (base->subtickMovesField.nCurrentSize >= 30)
			return nullptr;
		CSubtickMoveStep* step = base->add_subtick_move();
		if (!step || reinterpret_cast<std::uintptr_t>(step) < 0x10000)
			return nullptr;
		step->nHasBits = 0;
		step->nCachedBits = 0;
		step->nButton = 0;
		step->bPressed = false;
		step->flWhen = 0.f;
		step->flAnalogForwardDelta = 0.f;
		step->flAnalogLeftDelta = 0.f;
		return step;
	}

	void SetSubtickButton(CSubtickMoveStep* step, std::uint64_t button, bool pressed, float when)
	{
		if (!step)
			return;
		// IDA ProcessSubTick / CreateMove fill: flWhen must be in [0, 1). Post used 1.0f — clamp.
		step->nButton = button;
		step->bPressed = pressed;
		step->flWhen = std::clamp(when, 0.0f, kSubtickWhenRelease);
		step->flAnalogForwardDelta = 0.f;
		step->flAnalogLeftDelta = 0.f;
		step->nHasBits = 0;
		step->nCachedBits = 0;
		// Engine presence = nCachedBits @ +0x10 (not protobuf nHasBits @ +0x8)
		step->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN);
	}

	// Land-frac stand-in for post's EnginePrediction::PredictLanding
	// Dump offsets: m_fFlags 0x3F4, m_vecAbsVelocity 0x3F8 (client_dll.hpp schema)
	bool TryLandFrac(C_CSPlayerPawn* pawn, const Vector_t& vel, float groundZ, float& outFrac)
	{
		outFrac = 0.f;
		if (!pawn || vel.z >= 0.f)
			return false;
		Vector_t origin{};
		__try { origin = pawn->getPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		const Vector_t mins{ -16.f, -16.f, 0.f };
		return Pred::PredictLandingFrac(origin, mins, vel, groundZ, outFrac);
	}

	// UC-verified bhop — PasatAlexDis1's simple approach (confirmed by Bop32):
	// "use the prediction flags not ur current flag"
	//
	// 1. Strip ALL jump bits from cmd (clean slate)
	// 2. If post-prediction flags say FL_ONGROUND → re-press (value + scroll)
	// 3. If post-prediction says airborne → stay stripped (no jump in air)
	//
	// This works because:
	// - Prediction simulates the tick AHEAD (tells us if we WILL be on ground)
	// - Strip+re-press creates a fresh rising edge every ground tick
	// - In air: stripped = no jump = game doesn't see held space = no spam penalty
	// - On ground: re-press = value|scroll = IsButtonActive code 3+ = jump fires
	//
	// Subtick enhancement (Bop32 advanced): add release→press at landFrac for
	// perfect subtick timing. Only when prediction provides valid landFrac.
	//
	// Stamina zero: prevents height loss on consecutive hops.
	void ApplyBhop(C_CSPlayerPawn* pawn, CUserCmd* pCmd)
	{
		if (!pawn || !pCmd || !Config::bhop)
			return;

		int hp = 0;
		__try { hp = pawn->m_iHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
		if (hp <= 0 || hp > 200 || BadMoveType(pawn))
			return;

		CBaseUserCmdPB* pBaseCmd = pCmd->csgoUserCmd.pBaseCmd;

		// Must be holding space (cmd or physical key)
		const bool cmdJump =
			(pCmd->nButtons.nValue & kJumpMask) != 0ULL
			|| (pCmd->nButtons.nValueChanged & kJumpMask) != 0ULL
			|| (pCmd->nButtons.nValueScroll & kJumpMask) != 0ULL
			|| (pBaseCmd && pBaseCmd->pInButtonState
				&& ((pBaseCmd->pInButtonState->nValue & kJumpMask)
					|| (pBaseCmd->pInButtonState->nValueChanged & kJumpMask)
					|| (pBaseCmd->pInButtonState->nValueScroll & kJumpMask)));
		const bool keySpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
		if (!cmdJump && !keySpace)
			return;

		// STEP 1: Strip ALL jump bits (clean slate — PasatAlexDis1 approach)
		pCmd->nButtons.nValue &= ~kJumpMask;
		pCmd->nButtons.nValueChanged &= ~kJumpMask;
		pCmd->nButtons.nValueScroll &= ~kJumpMask;
		if (pBaseCmd && pBaseCmd->pInButtonState) {
			pBaseCmd->pInButtonState->nValue &= ~kJumpMask;
			pBaseCmd->pInButtonState->nValueChanged &= ~kJumpMask;
			pBaseCmd->pInButtonState->nValueScroll &= ~kJumpMask;
			pBaseCmd->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			pBaseCmd->SetBits(BASE_BITS_BUTTONPB);
		}

		// STEP 2: Read post-prediction flags (simulated this tick ahead)
		const auto& pred = Pred::Last();
		bool onGround = false;
		float landFrac = 0.f;

		if (pred.valid) {
			onGround = (pred.postFlags & FL_ONGROUND) != 0U;
			if (pred.hasLandFrac && pred.landFrac > 0.f && pred.landFrac < 1.f)
				landFrac = pred.landFrac;
		} else {
			// Prediction not available — fallback to live flags
			__try {
				const std::uint32_t liveFlags = *reinterpret_cast<std::uint32_t*>(
					reinterpret_cast<std::uintptr_t>(pawn) + kFlagsOff);
				onGround = (liveFlags & FL_ONGROUND) != 0U;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				onGround = false;
			}
		}

		// STEP 3: If NOT on ground (post-pred) → stay stripped (no jump in air)
		if (!onGround)
			return;

		// STEP 4: On ground → re-press jump (fresh rising edge)
		pCmd->nButtons.nValue |= kJumpMask;
		pCmd->nButtons.nValueScroll |= kJumpMask;
		// nValueChanged NOT set — scroll alone gives IsButtonActive code 4 (active)
		// without creating a "changed" edge that triggers spam penalty.
		if (pBaseCmd && pBaseCmd->pInButtonState) {
			pBaseCmd->pInButtonState->nValue |= kJumpMask;
			pBaseCmd->pInButtonState->nValueScroll |= kJumpMask;
			pBaseCmd->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			pBaseCmd->SetBits(BASE_BITS_BUTTONPB);
		}

		// STEP 5: Subtick precision (Bop32 advanced) — release→press at landFrac.
		// Only when prediction provides valid landFrac (flat ground, accurate pred).
		// On uneven ground landFrac may be 0 → skip subticks, button press is enough.
		if (pBaseCmd && landFrac > 0.f) {
			// Release (space was held before strip — game needs let-go)
			if (CSubtickMoveStep* rel = pBaseCmd->add_subtick_move()) {
				rel->nHasBits = 0;
				rel->nCachedBits = 0;
				rel->nButton = kJumpMask;
				rel->bPressed = false;
				rel->flWhen = std::clamp(landFrac, 0.001f, 0.999f);
				rel->flAnalogForwardDelta = 0.f;
				rel->flAnalogLeftDelta = 0.f;
				rel->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN);
			}
			// Re-press at exact land time
			if (CSubtickMoveStep* press = pBaseCmd->add_subtick_move()) {
				press->nHasBits = 0;
				press->nCachedBits = 0;
				press->nButton = kJumpMask;
				press->bPressed = true;
				press->flWhen = std::clamp(landFrac, 0.001f, 0.999f);
				press->flAnalogForwardDelta = 0.f;
				press->flAnalogLeftDelta = 0.f;
				press->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN);
			}
		}

		// STEP 6: Zero stamina (prevent height loss on consecutive hops)
		ZeroStamina(pawn);
	}

}


// Celerity set_button(DOWN/UP): hold bit only — NOT value|changed every tick
// (that is press-edge spam and breaks continuous A/D).
void SetMoveButtonHeld(CUserCmd* cmd, std::uint64_t button, bool down)
{
	if (!cmd)
		return;
	if (down) {
		cmd->nButtons.nValue |= button;
		cmd->nButtons.nValueChanged &= ~button;
		cmd->nButtons.nValueScroll &= ~button;
	} else {
		cmd->nButtons.nValue &= ~button;
		cmd->nButtons.nValueChanged &= ~button;
		cmd->nButtons.nValueScroll &= ~button;
	}
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pInButtonState) {
			if (down) {
				base->pInButtonState->nValue |= button;
				base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			} else {
				base->pInButtonState->nValue &= ~button;
				base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			}
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
		}
		base->SetBits(BASE_BITS_BUTTONPB);
	}
}

// Exact port of celerity c_autostrafe::run (New folder/autostrafe.cpp).
// CS2 leftmove is unit ±1 — NOT Source1 ±450.
void AutoStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn, const Vector_t& /*vel*/, bool onGround, std::uint32_t flags)
{
	if (!cmd || !pawn || !Config::autostrafe)
		return;
	if (onGround)
		return;
	if (BadMoveType(pawn) || InWater(flags))
		return;

	// Celerity: GetAsyncKeyState(VK_SPACE) only — not cmd jump bits
	if (!(GetAsyncKeyState(VK_SPACE) & 0x8000))
		return;

	auto* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base)
		return;

	// celerity i_csgo_input layout (pad 0x228 + thirdperson/buttons):
	// forward_move 0x260, left_move 0x264, mouse_delta_x 0x26C
	constexpr std::uintptr_t kCsgoInputForward = 0x260;
	constexpr std::uintptr_t kCsgoInputLeft = 0x264;
	constexpr std::uintptr_t kCsgoInputMouseDx = 0x26C;

	int mouseDelta = base->nMousedX;
	void* csgoInput = Input::GetCSGOInput();
	if (mouseDelta == 0 && csgoInput) {
		__try {
			mouseDelta = *reinterpret_cast<int*>(
				reinterpret_cast<std::uint8_t*>(csgoInput) + kCsgoInputMouseDx);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			mouseDelta = 0;
		}
	}

	// look right (+mdx) → left_move=-1 (RIGHT); look left → +1 (LEFT)
	// no mouse → alternate A/D each command (legit). Silent mode owns vectorial.
	float leftMove = 0.f;
	if (mouseDelta > 1)
		leftMove = -1.f;
	else if (mouseDelta < -1)
		leftMove = 1.f;
	else
		leftMove = (cmd->nCommandNumber % 2 == 0) ? 1.f : -1.f;

	// Unit scale (±1) — celerity set_forwardmove(0) / set_leftmove(left_move)
	base->flForwardMove = 0.f;
	base->flSideMove = leftMove;
	base->flUpMove = 0.f;
	base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);

	if (csgoInput) {
		__try {
			auto* p = reinterpret_cast<std::uint8_t*>(csgoInput);
			*reinterpret_cast<float*>(p + kCsgoInputForward) = 0.f;
			*reinterpret_cast<float*>(p + kCsgoInputLeft) = leftMove;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// Clear both, then hold the active side (celerity set_button DOWN)
	SetMoveButtonHeld(cmd, IN_MOVELEFT, false);
	SetMoveButtonHeld(cmd, IN_MOVERIGHT, false);
	if (leftMove > 0.f)
		SetMoveButtonHeld(cmd, IN_MOVELEFT, true);
	else if (leftMove < 0.f)
		SetMoveButtonHeld(cmd, IN_MOVERIGHT, true);
}

void Movement::PrepareBhopForPredict(CUserCmd* /*user_cmd*/)
{
	// No-op: 1.1.5 edges jump before Pred using live flags (see OnCreateMove).
}

void Movement::OnCreateMove(CUserCmd* user_cmd)
{
	if (!user_cmd || g_bMenuOpen)
		return;

	if (I::EngineClient) {
		__try {
			if (!I::EngineClient->in_game() || !I::EngineClient->connected())
				return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
	}

	C_CSPlayerPawn* pawn = H::SafeLocalPlayer();
	if (!pawn || pawn->m_iHealth() <= 0)
		return;

	// Stamina zero BEFORE prediction (this runs before Pred::Start).
	// IDA ModernJump: jump blocked when stamina > 60. Each hop adds ~16.
	// Zeroing here ensures prediction sees 0 stamina → consistent hop height.
	// ApplyBhop also zeros (after prediction) as belt-and-suspenders.
	if (Config::bhop && (GetAsyncKeyState(VK_SPACE) & 0x8000)) {
		__try {
			std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(
				reinterpret_cast<std::uintptr_t>(pawn) + kFlagsOff);
			if ((flags & FL_ONGROUND) != 0U && !BadMoveType(pawn))
				ZeroStamina(pawn);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// Jumpbug / Edgebug / Edgejump: AFTER RewriteBhop in hooks.cpp so bhop
	// air-strip cannot kill land/edge edges. Movement here only does strafe.
	// (See hooks sehCreateMovePost order.)

	// Vectorial silent strafe (mode 1) — air, independent of pred land frac
	if (Config::autostrafe && Config::autostrafe_mode == 1) {
		__try {
			SubtickMove::RewriteStrafe(user_cmd, pawn);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("SubtickStrafe", GetExceptionCode());
		}
	}

	// Mouse autostrafe (mode 0)
	Vector_t vel{};
	std::uint32_t flags = 0;
	if (!ReadLiveMoveState(pawn, vel, flags))
		return;
	const bool onGround = (flags & FL_ONGROUND) != 0U;

	if (Config::autostrafe && Config::autostrafe_mode == 0
		&& user_cmd->csgoUserCmd.pBaseCmd) {
		__try {
			AutoStrafe(user_cmd, pawn, vel, onGround, flags);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("AutoStrafe", GetExceptionCode());
		}
	}
}

void Movement::OnFrame()
{
	// No-op. Celerity bhop is CreateMove-only (cmd edges). moveSvc writes here
	// caused sticky ground / dead jump when bhop enabled.
}

std::unique_ptr<Movement> g_movement = std::make_unique<Movement>();
