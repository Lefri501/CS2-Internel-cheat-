#pragma once
#include <memory>
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../config/config.h"

class Movement {
public:
	// Legacy no-op (kept so hooks/callers compile).
	void PrepareBhopForPredict(CUserCmd* user_cmd);
	// 1.1.5 bhop + autostrafe — call BEFORE Pred::Start (live FL_ONGROUND).
	void OnCreateMove(CUserCmd* user_cmd);
	void OnFrame();
};

extern std::unique_ptr<Movement> g_movement;
