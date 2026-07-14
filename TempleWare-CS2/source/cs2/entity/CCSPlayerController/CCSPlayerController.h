#pragma once
#include <cstdint>
#include <cstddef>
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "../../../templeware/utils/schema/schema.h"
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

private:
	uintptr_t address;
};
