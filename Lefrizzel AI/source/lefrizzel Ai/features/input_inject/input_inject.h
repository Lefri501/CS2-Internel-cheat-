#pragma once

#include <cstdint>

// Clean multi-path button / subtick inject.
// IDA (client.dll):
//   ForceButtonsDown        0x180A11230  (moveSvc, mask) — ProcessMovement path
//   ProcessSubTickInput     0x180B03E40  — flWhen in [0,1), max ~32 steps
//   CreateNewSubtickMoveStep 0x1804E23F0 — via CBaseUserCmdPB::add_subtick_move
//   IsButtonActive          value|changed (code>=3) or scroll
//
// Always write: cmd.nButtons + base.pInButtonState + (optional) moveSvc + subtick step.
// Never leave value alone without changed for a press edge.

class CUserCmd;
class C_CSPlayerPawn;
class CBaseUserCmdPB;

namespace InputInject {

constexpr float kWhenStart = 0.f;
constexpr float kWhenMid = 0.5f;
constexpr float kWhenEnd = 0.999f;

// Resolve once (uses Pred / Input patterns already scanned).
bool Init();

// ---- Button state (all layers) ----

// Hold: value set, changed cleared (continuous A/D).
// Edge press: value|changed. Edge release: all clear + optional release subtick.
void SetHeld(CUserCmd* cmd, std::uint64_t button, bool down);
void SetEdge(CUserCmd* cmd, std::uint64_t button, bool pressed);

// Full clean path: cmd + PB + moveSvc buttons (+ ForceButtonsDown when press).
void Apply(
	CUserCmd* cmd,
	C_CSPlayerPawn* pawn,
	std::uint64_t button,
	bool down,
	bool edge = false);

// Press @ when, optional release @ releaseWhen (<0 = no release step).
bool SubtickButton(
	CBaseUserCmdPB* base,
	std::uint64_t button,
	bool pressed,
	float when);

// Convenience: attack / attack2 / jump / duck rising edge + force down.
bool InjectAttack(CUserCmd* cmd, C_CSPlayerPawn* pawn, bool secondary = false);
bool InjectJump(CUserCmd* cmd, C_CSPlayerPawn* pawn, float pressWhen = kWhenStart);
bool InjectDuck(CUserCmd* cmd, C_CSPlayerPawn* pawn, bool down);

// Clear one button everywhere (cmd/PB/moveSvc/jump-history for IN_JUMP).
void Clear(CUserCmd* cmd, C_CSPlayerPawn* pawn, std::uint64_t button);

// Move-services ForceButtonsDown (ProcessMovement samples this).
bool ForceDown(C_CSPlayerPawn* pawn, std::uint64_t buttons);

// Wipe stale IN_JUMP subtick steps (held-space latch).
void ClearJumpSubticks(CBaseUserCmdPB* base);

// Zero flAnalog*Delta on every existing step (keep buttons/when).
// Use before injecting own analog step — engine key-event deltas
// otherwise chain on top and desync the server-side analog state.
void ClearAnalogSubticks(CBaseUserCmdPB* base);

// ---- Subtick-perfect movement helpers ----

// Sort subtick steps by flWhen ascending (engine processes in order).
void SortSubticks(CBaseUserCmdPB* base);

// Clamp all flWhen into [0, 0.999] and drop empty/null slots.
void SanitizeSubticks(CBaseUserCmdPB* base);

// Analog move delta step (vectorial strafe).
bool SubtickAnalog(CBaseUserCmdPB* base, float when, float fwdDelta, float leftDelta);

// Land-frac jump: press at landFrac, clear latch, force moveSvc edge.
// Returns true if jump edge written.
bool SubtickJumpAtLand(
	CUserCmd* cmd,
	C_CSPlayerPawn* pawn,
	float landFrac,
	bool onGround);

} // namespace InputInject
