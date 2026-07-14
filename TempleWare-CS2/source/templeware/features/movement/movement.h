#pragma once
#include <memory>
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../config/config.h"

class Movement {
public:
	void OnCreateMove(CUserCmd* user_cmd);
};

extern std::unique_ptr<Movement> g_movement;
