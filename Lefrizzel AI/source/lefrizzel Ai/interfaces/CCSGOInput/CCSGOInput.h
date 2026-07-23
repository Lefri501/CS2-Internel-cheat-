#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include "../../utils/math/vector/vector.h"
#include "../../hooks/includeHooks.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../CUserCmd/CUserCmd.h"

#define MULTIPLAYER_BACKUP 150
// IDA GetCUserCmdBySequenceNumber: slot stride
constexpr std::size_t kUserCmdStride = 152;

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
	// dump: SetupCmd / GetCUserCmdBySequenceNumber / GetViewAngles / SetViewAngles
	inline int (*SetupCmd)(uintptr_t controller) = nullptr;
	// GetCUserCmdBySequenceNumber(controllerOrEntity, seq) → CUserCmd*
	inline uintptr_t (*GetCUserCmdBySequenceNumber)(uintptr_t, int) = nullptr;
	// Alias used by older call sites
	inline uintptr_t (*GetControllerCmd)(uintptr_t, int) = nullptr;
	// GetCUserCmdArray(table, slot) → base of 150-cmd ring (or creates)
	inline void* (*GetCUserCmdArray)(void* table, int slot) = nullptr;
	// Entity → slot index used by cmd array path (dump misnamed GetCUserCmdTick)
	// IDA: int* GetCUserCmdTick(entity, int* outTick)
	inline int* (*GetEntityCmdSlot)(void* entity, int* outSlot) = nullptr;
	inline uintptr_t (*GetViewAngles)(uintptr_t, int) = nullptr;
	inline void (*SetViewAngle)(uintptr_t, int, Vector_t*) = nullptr;
	// dump: ForceButtonsDown(moveServices, buttonMask)
	inline void (*ForceButtonsDown)(void* moveServices, std::uint64_t buttons) = nullptr;

	// dump: pCSGOInput / CInputPtrGlobal → CCSGOInput*
	inline void** ppCSGOInput = nullptr;
	inline void* pCSGOInput = nullptr;
	// Global table for GetCUserCmdArray (Andromeda GetFirstCUserCmdArray)
	inline void** ppUserCmdArrayTable = nullptr;
	inline uintptr_t viewAngleContext = 0;
	// IDA CUserCmdArray sequence field (same as SetupCmd read)
	constexpr std::uintptr_t kCmdArraySequenceOff = 0x5910;

	// Implemented in interfaces.cpp
	CUserCmd* get_user_cmd(uintptr_t controller = 0);
	CUserCmd* get_user_cmd_by_sequence(uintptr_t controller, int seq);
	// Force buttons via movement services (ProcessMovement path). Falls back to cmd bits.
	bool ForceButtons(void* pawn, std::uint64_t buttons);
	void* GetCSGOInput();
	void init();
}
