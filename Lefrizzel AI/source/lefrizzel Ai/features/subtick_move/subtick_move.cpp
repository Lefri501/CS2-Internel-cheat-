#define NOMINMAX
#include "subtick_move.h"

#include "../input_inject/input_inject.h"
#include "../prediction/prediction.h"
#include "../movement/jumpbug.h"
#include "../../config/config.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../hooks/hooks.h"

namespace SubtickMove {
namespace {

constexpr std::uintptr_t kFlagsOff = 0x3F4;
constexpr std::uintptr_t kAbsVelOff = 0x3F8;
constexpr std::uint64_t kJumpMask = IN_JUMP;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.f;

bool g_inited = false;

float NormalizeYaw(float yaw)
{
	while (yaw > 180.f) yaw -= 360.f;
	while (yaw < -180.f) yaw += 360.f;
	return yaw;
}

bool BadMoveType(C_CSPlayerPawn* pawn)
{
	// SEH / missing schema → NOT bad (false killed all bhop / jump before)
	if (!pawn)
		return false;
	__try {
		const uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
		if (!mtOff)
			return false;
		const uint8_t moveType = *reinterpret_cast<uint8_t*>(
			reinterpret_cast<uintptr_t>(pawn) + mtOff);
		// Only block obvious non-walk. Unknown values = allow jump.
		if (moveType == MOVETYPE_LADDER
			|| moveType == MOVETYPE_NOCLIP
			|| moveType == MOVETYPE_OBSERVER)
			return true;
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ReadLive(C_CSPlayerPawn* pawn, Vector_t& vel, std::uint32_t& flags)
{
	vel = Vector_t{ 0.f, 0.f, 0.f };
	flags = 0;
	if (!pawn)
		return false;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(pawn);
		flags = *reinterpret_cast<std::uint32_t*>(base + kFlagsOff);
		vel = pawn->m_vecAbsVelocity();
		if (vel.x == 0.f && vel.y == 0.f && vel.z == 0.f)
			vel = *reinterpret_cast<Vector_t*>(base + kAbsVelOff);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ReadYaw(CUserCmd* cmd, float& yaw)
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
	return false;
}

// (RemoveJump / AddJump / SetJumpSubtick removed — RewriteBhop uses WriteCmdJumpEdge)

// ---- Silent directional WASD + autostrafe ----
constexpr float kMoveUnit = 1.f;
constexpr std::uint64_t kWasdMask =
	IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;
constexpr std::uintptr_t kCsgoInputForward = 0x260;
constexpr std::uintptr_t kCsgoInputLeft = 0x264;
constexpr std::uintptr_t kCsgoInputMouseDx = 0x26C;

bool KeyDown(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void ReadUserWasd(CUserCmd* cmd, float& outFwd, float& outSide, std::uint64_t& outButtons)
{
	outFwd = 0.f;
	outSide = 0.f;
	outButtons = 0;

	bool w = KeyDown('W') || KeyDown(VK_UP);
	bool s = KeyDown('S') || KeyDown(VK_DOWN);
	bool a = KeyDown('A') || KeyDown(VK_LEFT);
	bool d = KeyDown('D') || KeyDown(VK_RIGHT);

	if (cmd) {
		const std::uint64_t b =
			cmd->nButtons.nValue | cmd->nButtons.nValueChanged | cmd->nButtons.nValueScroll;
		if (b & IN_FORWARD) w = true;
		if (b & IN_BACK) s = true;
		if (b & IN_MOVELEFT) a = true;
		if (b & IN_MOVERIGHT) d = true;
		if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
			if (base->flForwardMove > 0.1f) w = true;
			if (base->flForwardMove < -0.1f) s = true;
			if (base->flSideMove > 0.1f) a = true;
			if (base->flSideMove < -0.1f) d = true;
			if (base->pInButtonState) {
				const auto* ib = base->pInButtonState;
				const std::uint64_t pb =
					ib->nValue | ib->nValueChanged | ib->nValueScroll;
				if (pb & IN_FORWARD) w = true;
				if (pb & IN_BACK) s = true;
				if (pb & IN_MOVELEFT) a = true;
				if (pb & IN_MOVERIGHT) d = true;
			}
		}
	}

	if (w && !s) {
		outFwd = +kMoveUnit;
		outButtons |= IN_FORWARD;
	} else if (s && !w) {
		outFwd = -kMoveUnit;
		outButtons |= IN_BACK;
	}
	if (a && !d) {
		outSide = +kMoveUnit;
		outButtons |= IN_MOVELEFT;
	} else if (d && !a) {
		outSide = -kMoveUnit;
		outButtons |= IN_MOVERIGHT;
	}
}

void SetMoveHeld(CUserCmd* cmd, std::uint64_t button, bool down)
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

void SetWasdHeld(CUserCmd* cmd, std::uint64_t buttons)
{
	SetMoveHeld(cmd, IN_FORWARD, (buttons & IN_FORWARD) != 0);
	SetMoveHeld(cmd, IN_BACK, (buttons & IN_BACK) != 0);
	SetMoveHeld(cmd, IN_MOVELEFT, (buttons & IN_MOVELEFT) != 0);
	SetMoveHeld(cmd, IN_MOVERIGHT, (buttons & IN_MOVERIGHT) != 0);
}

void WriteCsgoInputMove(float fmove, float smove)
{
	void* csgoInput = Input::GetCSGOInput();
	if (!csgoInput)
		return;
	__try {
		auto* p = reinterpret_cast<std::uint8_t*>(csgoInput);
		*reinterpret_cast<float*>(p + kCsgoInputForward) = fmove;
		*reinterpret_cast<float*>(p + kCsgoInputLeft) = smove;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

int ReadMouseDeltaX(CBaseUserCmdPB* base)
{
	int mouseDelta = base ? base->nMousedX : 0;
	if (mouseDelta != 0)
		return mouseDelta;
	void* csgoInput = Input::GetCSGOInput();
	if (!csgoInput)
		return 0;
	__try {
		mouseDelta = *reinterpret_cast<int*>(
			reinterpret_cast<std::uint8_t*>(csgoInput) + kCsgoInputMouseDx);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		mouseDelta = 0;
	}
	return mouseDelta;
}

bool WantJumpHeld(CUserCmd* cmd)
{
	// Pre-Pred strip clears cmd IN_JUMP — physical space is source of truth.
	// Also accept residual cmd bits if strip path skipped.
	if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0)
		return true;
	if (cmd && (cmd->nButtons.nValue & kJumpMask))
		return true;
	if (cmd && cmd->csgoUserCmd.pBaseCmd && cmd->csgoUserCmd.pBaseCmd->pInButtonState) {
		if (cmd->csgoUserCmd.pBaseCmd->pInButtonState->nValue & kJumpMask)
			return true;
	}
	return false;
}

} // namespace

bool Init()
{
	if (g_inited)
		return true;
	g_inited = true;
	InputInject::Init();
	Pred::Init();
	Con::Ok("SubtickMove ready (bhop: space+ground every tick edge, pre pack+moveSvc)");
	return true;
}

// Bhop — IDA CreateMove pack LUT + IsButtonActive (code>=3 = value|changed).
// Classic: hold space + ground → edge every tick. Air → full release.
// BUG was: wasAir/spaceEdge gate + always strip when hop=false → no first jump
// if ground miss or space already held. Fail-open: if no ground sample, leave jump alone.

constexpr std::uintptr_t kCsgoBtn0 = 0x250;
constexpr std::uintptr_t kCsgoBtn1 = 0x258; // alone → LUT code 3
constexpr std::uintptr_t kCsgoBtn2 = 0x260;
constexpr std::uintptr_t kCsgoBtn3 = 0x268;
constexpr std::uintptr_t kMoveSvcOff = 0x1248;
constexpr std::uintptr_t kMoveSvcBtn = 0x50;
constexpr std::uintptr_t kGroundEntityOff = 0x530;

// wasAir: only for soft ground-entity land (flag lag online). NOT for hop gate.
bool g_wasAir = true;

struct PreHopState {
	bool spaceHeld = false;
	bool wantHop = false;   // space && on ground this tick
	bool onGround = false;
	bool valid = false;     // false → Rewrite must NOT strip jump
};
PreHopState g_pre;

struct SvcBtn {
	std::uint64_t vt;
	std::uint64_t nValue;
	std::uint64_t nValueChanged;
	std::uint64_t nValueScroll;
};

bool LiveOnGround(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return false;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(pawn);
		const std::uint32_t dumpF = *reinterpret_cast<std::uint32_t*>(base + kFlagsOff);
		if (dumpF & FL_ONGROUND)
			return true;
		const std::uint32_t schemaF = pawn->m_fFlags();
		if (schemaF & FL_ONGROUND)
			return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

// Soft land only after air — m_hGroundEntity alone mid-air was sticky jump.
bool SoftLandContact(C_CSPlayerPawn* pawn)
{
	if (!pawn || !g_wasAir)
		return false;
	__try {
		const std::uint32_t h = *reinterpret_cast<std::uint32_t*>(
			reinterpret_cast<std::uintptr_t>(pawn) + kGroundEntityOff);
		return h != 0u && h != 0xFFFFFFFFu;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Classic bhop: space + (ground | soft land after air) → hop. No edge-only gate.
bool WantHopThisTick(C_CSPlayerPawn* pawn, bool spaceHeld, bool& outGround)
{
	outGround = LiveOnGround(pawn);
	const bool landing = outGround || SoftLandContact(pawn);

	if (!spaceHeld) {
		g_wasAir = !landing;
		return false;
	}
	if (!landing) {
		g_wasAir = true;
		return false;
	}
	// Ground + space → edge every land tick (standing first jump + chain).
	g_wasAir = false;
	return true;
}

void* GetMoveSvc(C_CSPlayerPawn* pawn)
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
	__try {
		svc = *reinterpret_cast<void**>(
			reinterpret_cast<std::uintptr_t>(pawn) + kMoveSvcOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return svc;
}

// Clear all 4 jump words; edge → ONLY +0x258 (LUT code 3). Held +0x250|+0x258 = dead.
void WriteCsgoJump(void* pInput, bool edgeDown)
{
	if (!pInput)
		return;
	__try {
		auto* b = reinterpret_cast<std::uint8_t*>(pInput);
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn0) &= ~kJumpMask;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn1) &= ~kJumpMask;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn2) &= ~kJumpMask;
		*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn3) &= ~kJumpMask;
		if (edgeDown)
			*reinterpret_cast<std::uint64_t*>(b + kCsgoBtn1) |= kJumpMask;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void WriteCmdJumpEdge(CUserCmd* cmd, bool down)
{
	if (!cmd)
		return;
	if (down) {
		cmd->nButtons.nValue |= kJumpMask;
		cmd->nButtons.nValueChanged |= kJumpMask;
		cmd->nButtons.nValueScroll &= ~kJumpMask;
	} else {
		cmd->nButtons.nValue &= ~kJumpMask;
		cmd->nButtons.nValueChanged &= ~kJumpMask;
		cmd->nButtons.nValueScroll &= ~kJumpMask;
	}
	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base || !base->pInButtonState)
		return;
	if (down) {
		base->pInButtonState->nValue |= kJumpMask;
		base->pInButtonState->nValueChanged |= kJumpMask;
		base->pInButtonState->nValueScroll &= ~kJumpMask;
	} else {
		base->pInButtonState->nValue &= ~kJumpMask;
		base->pInButtonState->nValueChanged &= ~kJumpMask;
		base->pInButtonState->nValueScroll &= ~kJumpMask;
	}
	base->pInButtonState->SetBits(
		BUTTON_STATE_PB_BITS_BUTTONSTATE1
		| BUTTON_STATE_PB_BITS_BUTTONSTATE2
		| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
	base->SetBits(BASE_BITS_BUTTONPB);
}

void WriteMoveSvcJump(void* svc, bool edgeDown)
{
	if (!svc)
		return;
	__try {
		auto* bs = reinterpret_cast<SvcBtn*>(
			reinterpret_cast<std::uintptr_t>(svc) + kMoveSvcBtn);
		if (edgeDown) {
			bs->nValue |= kJumpMask;
			bs->nValueChanged |= kJumpMask;
			bs->nValueScroll &= ~kJumpMask;
		} else {
			bs->nValue &= ~kJumpMask;
			bs->nValueChanged &= ~kJumpMask;
			bs->nValueScroll &= ~kJumpMask;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void PreCreateMoveBhop(void* pCSGOInput)
{
	g_pre = PreHopState{};
	if (!Config::bhop)
		return;

	C_CSPlayerPawn* pawn = H::SafeLocalAlive();
	if (!pawn)
		pawn = H::SafeLocalPlayer();
	// No pawn / bad move: leave keyboard jump alone (do not clear pack).
	if (!pawn || BadMoveType(pawn))
		return;

	int hp = 0;
	__try { hp = pawn->m_iHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (hp <= 0 || hp > 200)
		return;

	const bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	bool ground = false;
	const bool hop = WantHopThisTick(pawn, space, ground);

	g_pre.spaceHeld = space;
	g_pre.onGround = ground;
	g_pre.wantHop = hop;
	g_pre.valid = true;

	void* svc = GetMoveSvc(pawn);

	// Only rewrite pack when we own the decision (space held or hop).
	// !space: clear so next press is clean edge. space+air: release. space+ground: edge.
	if (space || hop) {
		WriteCsgoJump(pCSGOInput, hop);
		WriteMoveSvcJump(svc, hop);
	} else {
		// Space up: clear held latch so next press packs edge, not code-1 hold.
		WriteCsgoJump(pCSGOInput, false);
		WriteMoveSvcJump(svc, false);
	}
}

void RewriteBhop(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	if (!cmd || !Config::bhop || !pawn)
		return;
	if (JumpBug::ClaimedJumpThisTick())
		return;

	int hp = 0;
	__try { hp = pawn->m_iHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (hp <= 0 || hp > 200 || BadMoveType(pawn))
		return;

	// Pre failed / never ran → do NOT strip cmd jump (was: hop=false always clear).
	if (!g_pre.valid)
		return;

	const bool hop = g_pre.wantHop;
	const bool space = g_pre.spaceHeld
		|| (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	void* pInput = Input::GetCSGOInput();

	if (base)
		InputInject::ClearJumpSubticks(base);

	// Post: ProcessMovement already ran. Clear pack for next pre (avoid code-1 hold).
	WriteCsgoJump(pInput, false);
	WriteMoveSvcJump(GetMoveSvc(pawn), false);

	// Server packet: hop edge on ground; release in air / no space.
	// Only rewrite when space is involved — never wipe unrelated jump state.
	if (hop) {
		WriteCmdJumpEdge(cmd, true);
	} else if (space) {
		// Air + hold space: release so next land is rising edge
		WriteCmdJumpEdge(cmd, false);
	} else {
		WriteCmdJumpEdge(cmd, false);
	}
}

// IDA ProcessSubTickInput 0x180B03E40 (patterns.hpp ProcessSubTickInput):
//   - copies CCSGOInput subtick ring → cmd steps, stops at flWhen >= 1.0
//   - hard cap 32 steps/tick (Warning string at max)
// IDA CreateNewSubtickMoveStep 0x1804E23F0 → 56-byte CSubtickMoveStep
//   +0x18 button, +0x20 pressed, +0x24 when, +0x28 analogFwd, +0x2C analogLeft
// AirAccelerate samples wishdir each subtick — multi-step analog > single base move.
//
// Silent WASD subtick exploit:
//   1) wipe engine key-event analog garbage (else deltas stack / desync)
//   2) ideal wish from vel yaw + Pred air math (or user WASD cone)
//   3) base flForward/Side unit (±1) + held WASD buttons
//   4) N subtick analog steps across [0, 1) reinforcing same wish
//   5) SanitizeSubticks (sort + clamp; keep flWhen==1.0 alone)

// Online-safe: single base unit move + WASD hold only.
// Multi-step analog subticks (3× AirAccelerate sample) felt great offline
// but desynced vs server air accel on MM — client gains speed server rejects.
// Keep one optional analog step @ 0 for ProcessSubTick sample parity.
constexpr float kStrafeWhen0 = 0.f;
constexpr int kStrafeSteps = 1;

void NormalizeUnitMove(float& fmove, float& smove)
{
	if (std::fabs(fmove) < 0.08f) fmove = 0.f;
	if (std::fabs(smove) < 0.08f) smove = 0.f;
	const float ml = std::sqrt(fmove * fmove + smove * smove);
	if (ml > 1e-4f) {
		fmove = (fmove / ml) * kMoveUnit;
		smove = (smove / ml) * kMoveUnit;
	}
}

std::uint64_t ButtonsFromMove(float fmove, float smove)
{
	std::uint64_t buttons = 0;
	if (fmove > 0.15f) buttons |= IN_FORWARD;
	else if (fmove < -0.15f) buttons |= IN_BACK;
	// IDA PreSource1AirMove: +side = LEFT (IN_MOVELEFT)
	if (smove > 0.15f) buttons |= IN_MOVELEFT;
	else if (smove < -0.15f) buttons |= IN_MOVERIGHT;
	return buttons;
}

// Pack wish into base moves + WASD hold. Minimal subtick — online desync safe.
void ApplySilentSubtickStrafe(
	CUserCmd* cmd,
	CBaseUserCmdPB* base,
	float fmove,
	float smove)
{
	NormalizeUnitMove(fmove, smove);
	const std::uint64_t buttons = ButtonsFromMove(fmove, smove);

	// Engine key-event analog garbage this tick — wipe so base move wins.
	InputInject::ClearAnalogSubticks(base);

	base->flForwardMove = fmove;
	base->flSideMove = smove;
	base->flUpMove = 0.f;
	base->SetBits(BASE_BITS_FORWARDMOVE | BASE_BITS_LEFTMOVE | BASE_BITS_UPMOVE);

	// Do NOT WriteCsgoInputMove post-original — CreateMove already consumed
	// a1 floats; writing poisons next tick / desyncs with server cmd copy.

	// Hold only (no nValueChanged spam) — continuous A/D for air accel.
	SetWasdHeld(cmd, buttons);

	// Single reinforcing analog step @ flWhen=0 (optional; not multi-step stack)
	if (kStrafeSteps > 0)
		InputInject::SubtickAnalog(base, kStrafeWhen0, fmove, smove);

	InputInject::SanitizeSubticks(base);
}

void RewriteStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	if (!cmd || !pawn || !Config::autostrafe)
		return;
	if (Config::autostrafe_mode != 1)
		return;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base)
		return;

	Vector_t vel{};
	std::uint32_t flags = 0;
	if (!ReadLive(pawn, vel, flags))
		return;
	if ((flags & FL_ONGROUND) != 0U)
		return;
	if (BadMoveType(pawn) || (flags & (FL_INWATER | FL_WATERJUMP)))
		return;
	// Space held = bhop chain; also allow pure air if jump already latched
	if (!(GetAsyncKeyState(VK_SPACE) & 0x8000) && !WantJumpHeld(cmd))
		return;

	float userFwd = 0.f;
	float userSide = 0.f;
	std::uint64_t userButtons = 0;
	ReadUserWasd(cmd, userFwd, userSide, userButtons);

	float fmove = 0.f;
	float smove = 0.f;

	float viewYaw = 0.f;
	if (!ReadYaw(cmd, viewYaw))
		viewYaw = 0.f;

	const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
	const float velYaw = (speed2d > 1.f)
		? Pred::VelocityYawDeg(vel)
		: viewYaw;
	const float ideal = Pred::IdealStrafeDeltaDeg(
		(std::max)(speed2d, 1.f), Pred::kAirWishSpeed);

	if ((userButtons & kWasdMask) != 0) {
		// User WASD: keep their direction, but snap to nearest ideal air-accel angle
		// in that half-plane so hold-W still gains speed (subtick-packed).
		const float userDelta = NormalizeYaw(
			std::atan2(userSide, userFwd) * (180.f / kPi));
		const float userWish = NormalizeYaw(viewYaw + userDelta);

		// Pick L/R ideal closest to user wish
		const float wishL = NormalizeYaw(velYaw + ideal);
		const float wishR = NormalizeYaw(velYaw - ideal);
		const float dL = std::fabs(NormalizeYaw(wishL - userWish));
		const float dR = std::fabs(NormalizeYaw(wishR - userWish));
		const float wishYaw = (dL <= dR) ? wishL : wishR;

		// Blend: if user is already near ideal, use pure ideal; else bias toward user
		const float toIdeal = std::fabs(NormalizeYaw(wishYaw - userWish));
		const float useYaw = (toIdeal < 35.f) ? wishYaw : userWish;

		const float delta = NormalizeYaw(useYaw - viewYaw) * kDeg2Rad;
		fmove = std::cos(delta) * kMoveUnit;
		smove = std::sin(delta) * kMoveUnit;
	} else {
		// No keys: full silent circle strafe (pred pick L/R by speed gain)
		static int s_side = 1;
		const int mouseDelta = ReadMouseDeltaX(base);
		if (mouseDelta > 1)
			s_side = -1;
		else if (mouseDelta < -1)
			s_side = +1;
		else if (speed2d < 30.f)
			s_side = (cmd->nCommandNumber % 2 == 0) ? 1 : -1;

		const float wishL = NormalizeYaw(velYaw + ideal);
		const float wishR = NormalizeYaw(velYaw - ideal);
		const float spdL = Pred::PredictSpeedAfterWishMoves(
			vel, viewYaw, wishL, Pred::kTickInterval);
		const float spdR = Pred::PredictSpeedAfterWishMoves(
			vel, viewYaw, wishR, Pred::kTickInterval);

		float wishYaw = wishL;
		if (mouseDelta > 1)
			wishYaw = wishR;
		else if (mouseDelta < -1)
			wishYaw = wishL;
		else if (spdR > spdL + 0.05f)
			wishYaw = wishR;
		else if (spdL > spdR + 0.05f)
			wishYaw = wishL;
		else
			wishYaw = (s_side > 0) ? wishL : wishR;

		s_side = (std::fabs(NormalizeYaw(wishYaw - wishL))
			<= std::fabs(NormalizeYaw(wishYaw - wishR))) ? 1 : -1;

		const float delta = NormalizeYaw(wishYaw - viewYaw) * kDeg2Rad;
		fmove = std::cos(delta) * kMoveUnit;
		smove = std::sin(delta) * kMoveUnit;
	}

	ApplySilentSubtickStrafe(cmd, base, fmove, smove);
}

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	if (!cmd || !pawn)
		return;
	if (Config::autostrafe)
		RewriteStrafe(cmd, pawn);
}

} // namespace SubtickMove
