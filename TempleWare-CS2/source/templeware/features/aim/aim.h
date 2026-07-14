#pragma once

struct CUserCmd;

// Frame entry: aimbot / autofire / triggerbot (pass cmd for flick-accurate angles)
void Aimbot(CUserCmd* cmd = nullptr);

// CreateMove: humanization first-shot gate + autofire/trigger attack inject
void AimHumanize_OnCreateMove(CUserCmd* cmd);
