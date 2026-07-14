#pragma once
#include <cstdint>
#include <cstring>
#include "../C_EntityInstance/C_EntityInstance.h"
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "../../../../source/templeware/utils/schema/schema.h"
#include "../../../../source/templeware/utils/memory/vfunc/vfunc.h"
#include "../handle.h"

class CGameSceneNode
{
public:
	schema(Vector_t, m_vecAbsOrigin, "CGameSceneNode->m_vecAbsOrigin");
	schema(bool, m_bDormant, "CGameSceneNode->m_bDormant");
};

// Collision hull used for perspective-correct ESP boxes (mins/maxs + origin → 8 corners)
class CCollisionProperty
{
public:
	schema(Vector_t, m_vecMins, "CCollisionProperty->m_vecMins");
	schema(Vector_t, m_vecMaxs, "CCollisionProperty->m_vecMaxs");
};

class C_BaseEntity : public CEntityInstance
{
public:
	schema(CGameSceneNode*, m_pGameSceneNode, "C_BaseEntity->m_pGameSceneNode");
	schema(CCollisionProperty*, m_pCollision, "C_BaseEntity->m_pCollision");
	schema(int, m_iMaxHealth, "C_BaseEntity->m_iMaxHealth");
	schema(std::int32_t, m_iHealth, "C_BaseEntity->m_iHealth");
	schema(std::uint8_t, m_lifeState, "C_BaseEntity->m_lifeState");
	schema(std::uint8_t, m_iTeamNum, "C_BaseEntity->m_iTeamNum");
	schema(std::uint32_t, m_fFlags, "C_BaseEntity->m_fFlags");
	schema(CBaseHandle, m_hOwnerEntity, "C_BaseEntity->m_hOwnerEntity");
	schema(CBaseHandle, m_hController, "C_BasePlayerPawn->m_hController");
	bool IsBasePlayer()
	{
		if (!Mem::ValidEntity(this))
			return false;
		SchemaClassInfoData_t* pClassInfo = nullptr;
		dump_class_info(&pClassInfo);
		if (!pClassInfo || !Mem::Valid(pClassInfo, sizeof(void*)) || !pClassInfo->szName)
			return false;
		if (!Mem::IsReadable(pClassInfo->szName, 1))
			return false;
		return hash_32_fnv1a_const(pClassInfo->szName) == hash_32_fnv1a_const("C_CSPlayerPawn");
	}

	bool IsViewmodelAttachment()
	{
		if (!Mem::ValidEntity(this))
			return false;
		SchemaClassInfoData_t* pClassInfo = nullptr;
		dump_class_info(&pClassInfo);
		if (!pClassInfo || !Mem::Valid(pClassInfo, sizeof(void*)) || !pClassInfo->szName)
			return false;
		if (!Mem::IsReadable(pClassInfo->szName, 1))
			return false;
		// Arms + any arms-like HUD model
		const auto h = hash_32_fnv1a_const(pClassInfo->szName);
		if (h == hash_32_fnv1a_const("C_CS2HudModelArms"))
			return true;
		// Substring fallback if Valve renames slightly
		return (std::strstr(pClassInfo->szName, "HudModelArms") != nullptr);
	}

	bool IsViewmodel()
	{
		if (!Mem::ValidEntity(this))
			return false;
		SchemaClassInfoData_t* pClassInfo = nullptr;
		dump_class_info(&pClassInfo);
		if (!pClassInfo || !Mem::Valid(pClassInfo, sizeof(void*)) || !pClassInfo->szName)
			return false;
		if (!Mem::IsReadable(pClassInfo->szName, 1))
			return false;
		const auto h = hash_32_fnv1a_const(pClassInfo->szName);
		if (h == hash_32_fnv1a_const("C_CS2HudModelWeapon")
			|| h == hash_32_fnv1a_const("C_CS2HudModelAddon")
			|| h == hash_32_fnv1a_const("C_CS2HudModelBase"))
			return true;
		// Weapon HUD mesh variants
		if (std::strstr(pClassInfo->szName, "HudModelWeapon")
			|| std::strstr(pClassInfo->szName, "HudModelAddon"))
			return true;
		return false;
	}

	bool IsPlayerController()
	{
		if (!Mem::ValidEntity(this))
			return false;
		SchemaClassInfoData_t* _class = nullptr;
		dump_class_info(&_class);
		if (!_class || !Mem::Valid(_class, sizeof(void*)) || !_class->szName)
			return false;
		if (!Mem::IsReadable(_class->szName, 1))
			return false;
		return hash_32_fnv1a_const(_class->szName) == hash_32_fnv1a_const("CCSPlayerController");
	}
};
