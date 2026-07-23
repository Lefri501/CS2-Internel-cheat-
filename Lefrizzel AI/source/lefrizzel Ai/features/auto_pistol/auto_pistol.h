#pragma once

class C_CSPlayerPawn;
class CUserCmd;

namespace AutoPistol {

bool Init();
void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd);

} // namespace AutoPistol
