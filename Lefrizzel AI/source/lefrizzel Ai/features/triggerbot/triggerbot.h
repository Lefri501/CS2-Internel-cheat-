#pragma once

class C_CSPlayerPawn;
class CUserCmd;
struct QAngle_t;
struct Vector_t;

namespace Triggerbot {

// SEH-guarded. True while trigger is active this tick (crosshair on target path).
bool Run(C_CSPlayerPawn* lp, CUserCmd* cmd);
void Reset();

bool WantShoot();
bool WantStop();
bool FireValid();
const QAngle_t& FireAngle();
const Vector_t& FireEye();
int FireHistIndex();

void ClearShootFlags();

} // namespace Triggerbot
