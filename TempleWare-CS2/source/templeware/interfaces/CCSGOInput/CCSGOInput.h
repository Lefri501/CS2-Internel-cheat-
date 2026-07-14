#pragma once
#include <Windows.h>
#include <cstdio>
#include "../../utils/math/vector/vector.h"
#include "../../hooks/includeHooks.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../CUserCmd/CUserCmd.h"

#define MULTIPLAYER_BACKUP 150

class CTinyMoveStepData
{
public:
	float flWhen;
	MEM_PAD(0x4);
	std::uint64_t nButton;
	bool bPressed;
	MEM_PAD(0x7);
};

class CMoveStepButtons
{
public:
	std::uint64_t nKeyboardPressed;
	std::uint64_t nMouseWheelheelPressed;
	std::uint64_t nUnPressed;
	std::uint64_t nKeyboardCopy;
};

class CExtendedMoveData : public CMoveStepButtons
{
public:
	float flForwardMove;
	float flSideMove;
	float flUpMove;
	std::int32_t nMouseDeltaX;
	std::int32_t nMouseDeltaY;
	std::int32_t nAdditionalStepMovesCount;
	CTinyMoveStepData tinyMoveStepData[12];
	Vector_t vecViewAngle;
	std::int32_t nTargetHandle;
};

namespace Input {
	inline uintptr_t(*GetControllerCmd)(uintptr_t, int) = nullptr;
	inline uintptr_t(*GetViewAngles)(uintptr_t, int) = nullptr;
	inline void(*SetViewAngle)(uintptr_t, int, Vector_t*) = nullptr;
	inline int(*SetupCmd)(uintptr_t) = nullptr;
	inline uintptr_t viewAngleContext = 0;

	// Implemented in interfaces.cpp (needs entity system + local player)
	CUserCmd* get_user_cmd(uintptr_t controller = 0);
	void init();
}
