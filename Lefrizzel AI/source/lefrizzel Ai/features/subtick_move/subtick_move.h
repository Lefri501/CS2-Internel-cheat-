#pragma once

// Classic bhop: hold space + ground → jump edge every tick; air → release.
// Pre: pack only +0x258 + moveSvc+0x50. Post: cmd/PB. Fail-open if pre invalid.

class CUserCmd;
class C_CSPlayerPawn;

namespace SubtickMove {

bool Init();

void PreCreateMoveBhop(void* pCSGOInput);

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn);
// AFTER original CreateMove, BEFORE Pred (cmd-only, no engine re-sim).
void RewriteBhop(CUserCmd* cmd, C_CSPlayerPawn* pawn);
void RewriteStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn);

} // namespace SubtickMove
