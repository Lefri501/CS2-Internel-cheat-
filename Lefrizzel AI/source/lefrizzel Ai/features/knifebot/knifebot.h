#pragma once

class C_CSPlayerPawn;
class CUserCmd;

namespace KnifeBot {

bool Init();
void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd);

} // namespace KnifeBot
