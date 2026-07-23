#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../interfaces/CUserCmd/CUserCmd.h"

// CL_Bypass surface (Andromeda-compatible API).
//
// SAFE MODE: move_crc recompute + SerializePartialToArray hook are OFF.
// Writing CRC without a real libprotobuf CBaseUserCmdPB backup corrupted
// usercmds → FATAL "Failed to serialize one usercommand?".
// Active: button helpers + optional subtick queue (no CRC rewrite).

namespace CL_Bypass {

	void PreClientCreateMove(CUserCmd* cmd);
	void PostClientCreateMove(void* pCSGOInput, CUserCmd* cmd);

	// Called from SerializePartialToArray hook when msg is CBaseUserCmdPB
	void OnCBaseUserCmdPB(void* pMsg);

	// Button helpers (Andromeda API surface)
	void SetAttack(CUserCmd* cmd, bool addSubtick = false);
	void SetDontAttack(CUserCmd* cmd, bool addSubtick = false);
	void SetJump(CUserCmd* cmd, bool addSubtick = false);
	void SetDontJump(CUserCmd* cmd, bool addSubtick = false);

	void AddProcessSubTick(std::uint64_t button, bool pressed);
	void AddProcessSubTick(std::uint64_t button, bool pressed, float when);

	// SerializePartialToArray hook install + body
	bool Init();
	bool __fastcall hkSerializePartialToArray(void* msg, void* out, int size);

	// Set while original CreateMove runs (CRC capture window)
	void SetInOriginalCreateMove(bool v);
	bool InOriginalCreateMove();

} // namespace CL_Bypass
