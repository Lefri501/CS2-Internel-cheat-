#pragma once

// Jumpbug / Edgebug / Edgejump — CreateMove, cmd-serialized only.
//
// IDA client.dll (session f3036c5b):
//   IsButtonActive 0x1804AB5B0: code = val|changed*2|scroll*4; active iff code>=3
//   ModernJumpCheck 0x180882A20: IsButtonActive(IN_JUMP=2) → spam window
//     (sv_jump_spam_penalty_time) → else sub_1808A6A70 (actual jump)
//   CheckJumpButton 0x180B0D130, ProcessSubTickInput 0x180B03E40 (flWhen < 1.0)
//
// Jumpbug: fall + land-this-tick → unduck + jump edge @ landFrac (crouch-land cancel).
// Edgebug: fall + land-this-tick → duck hold (keep XY on lip).
// Edgejump: ground + key + about to leave ledge → jump edge before fall.
// Never write moveSvc / ModernJump history (server owns spam state).

class CUserCmd;
class C_CSPlayerPawn;

namespace JumpBug {

// True if this tick wrote a land/edge jump the bhop air-strip must not kill.
bool ClaimedJumpThisTick();

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn);

} // namespace JumpBug
