#pragma once

class C_CSPlayerPawn;
class CUserCmd;
struct QAngle_t;
struct Vector_t;

namespace Autofire {

// SEH-guarded frame run. True while autofire owns aim this tick.
bool Run(C_CSPlayerPawn* lp, CUserCmd* cmd);
void Reset();

bool WantShoot();
bool WantStop();
bool WantScope(); // autoscope: hold attack2 this tick
bool BlockFirstShot();
bool SilentValid();
const QAngle_t& SilentAngle();
const Vector_t& SilentEye();
int FireHistIndex();

void ClearShootFlags();

} // namespace Autofire
