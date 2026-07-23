#pragma once
#include <cstdint>
#include <cstddef>
#include "../../../lefrizzel Ai/utils/memory/memorycommon.h"
#include "../../../lefrizzel Ai/utils/math/vector/vector.h"
#include "../../../lefrizzel Ai/utils/schema/schema.h"
#include "../C_CSWeaponBase/C_CSWeaponBase.h"

class CCSPlayerController {
public:
	CCSPlayerController(uintptr_t address);
	uintptr_t getAddress() const;

	// CUtlString in game — use ReadSanitizedName(), never treat field as const char*.
	bool ReadSanitizedName(char* buf, size_t bufSize) const;

	schema(bool, IsLocalPlayer, "CBasePlayerController->m_bIsLocalPlayerController");
	schema(std::uint32_t, m_nTickBase, "CBasePlayerController->m_nTickBase");
	schema(CBaseHandle, m_hPawn, "CBasePlayerController->m_hPawn");
	schema(CBaseHandle, m_hPlayerPawn, "CCSPlayerController->m_hPlayerPawn");
	schema(CBaseHandle, m_hObserverPawn, "CCSPlayerController->m_hObserverPawn");
	schema(bool, m_bPawnIsAlive, "CCSPlayerController->m_bPawnIsAlive");
	schema(std::uint64_t, m_steamID, "CBasePlayerController->m_steamID");
	// Dump client_dll: money @ 0x810, rank @ 0x888
	schema(void*, m_pInGameMoneyServices, "CCSPlayerController->m_pInGameMoneyServices");
	schema(std::int32_t, m_iCompetitiveRanking, "CCSPlayerController->m_iCompetitiveRanking");
	schema(std::int32_t, m_iCompetitiveWins, "CCSPlayerController->m_iCompetitiveWins");

private:
	uintptr_t address;
};

// CCSPlayerController_InGameMoneyServices — m_iAccount @ +0x40 (schema dump)
class CCSPlayerController_InGameMoneyServices {
public:
	schema(std::int32_t, m_iAccount, "CCSPlayerController_InGameMoneyServices->m_iAccount");
};
