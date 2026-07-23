#pragma once

class CUserCmd;

// Frame entry: aimbot + Autofire::Run + Triggerbot::Run (pass cmd for flick angles)
void Aimbot(CUserCmd* cmd = nullptr);

// CreateMove: humanization first-shot gate + autofire/trigger attack inject
void AimHumanize_OnCreateMove(CUserCmd* cmd);

// Map unload / death / disconnect — drop locks, AF/TR, punch baseline
void Aimbot_Reset();
