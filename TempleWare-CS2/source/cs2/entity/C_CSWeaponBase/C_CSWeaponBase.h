#pragma once
#include <cstdint>
#include "..\C_EntityInstance\C_EntityInstance.h"
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "..\..\..\..\source\templeware\utils\schema\schema.h"
#include "..\..\..\..\source\templeware\utils\memory\vfunc\vfunc.h"
#include "..\handle.h"

class CCSPlayer_WeaponServices
{
public:
	schema(bool, m_bAllowSwitchToNoWeapon, "CPlayer_WeaponServices->m_bAllowSwitchToNoWeapon");
	// m_hMyWeapons is C_NetworkUtlVectorBase<CHandle> — use raw offset + vector walk in skinchanger
	schema(CBaseHandle, m_hActiveWeapon, "CPlayer_WeaponServices->m_hActiveWeapon");
	schema(CBaseHandle, m_hLastWeapon, "CPlayer_WeaponServices->m_hLastWeapon");
	schema(int, m_iAmmo, "CPlayer_WeaponServices->m_iAmmo");
	schema(float, m_flNextAttack, "CCSPlayer_WeaponServices->m_flNextAttack");
	schema(bool, m_bIsLookingAtWeapon, "CCSPlayer_WeaponServices->m_bIsLookingAtWeapon");
	schema(bool, m_bIsHoldingLookAtWeapon, "CCSPlayer_WeaponServices->m_bIsHoldingLookAtWeapon");
};

class CPlayer_ObserverServices
{
public:
	schema(std::uint8_t, m_iObserverMode, "CPlayer_ObserverServices->m_iObserverMode");
	schema(CBaseHandle, m_hObserverTarget, "CPlayer_ObserverServices->m_hObserverTarget");
};

class CCSWeaponBaseVData
{
public:
	SCHEMA_ADD_OFFSET(const char*, m_szName, 0x720);
	// Weapon type from VData (CSWeaponType_t) - used by Andromeda's exact IsNonGunWeapon
	schema(int, m_WeaponType, "CCSWeaponBaseVData->m_WeaponType");
	// Damage / pen (autowall + mindamage)
	schema(int, m_nDamage, "CCSWeaponBaseVData->m_nDamage");
	schema(float, m_flArmorRatio, "CCSWeaponBaseVData->m_flArmorRatio");
	schema(float, m_flRange, "CCSWeaponBaseVData->m_flRange");
	schema(float, m_flRangeModifier, "CCSWeaponBaseVData->m_flRangeModifier");
	schema(float, m_flPenetration, "CCSWeaponBaseVData->m_flPenetration");
	schema(float, m_flHeadshotMultiplier, "CCSWeaponBaseVData->m_flHeadshotMultiplier");
	// Grenade base throw speed (HE/flash ~750, molly/inc ~700)
	schema(float, m_flThrowVelocity, "CCSWeaponBaseVData->m_flThrowVelocity");
};

class C_CSWeaponBase 
{
public:
	schema(bool, m_bInReload, "C_CSWeaponBase->m_bInReload");
	schema(std::int32_t, m_iClip1, "C_BasePlayerWeapon->m_iClip1");
	// Fire-rate gate: compare to controller m_nTickBase (+1 slack)
	schema(std::int32_t, m_nNextPrimaryAttackTick, "C_BasePlayerWeapon->m_nNextPrimaryAttackTick");
	// C_C4 only (safe to read if class is C4)
	schema(bool, m_bStartedArming, "C_C4->m_bStartedArming");

	// Knife / skin identity (schema dump offsets)
	schema(std::uint32_t, m_nSubclassID, "C_BaseEntity->m_nSubclassID");
	// C_EconEntity::m_AttributeManager (0x11A8) + C_AttributeContainer::m_Item (0x50) + m_iItemDefinitionIndex (0x1BA)
	schema_pfield(std::uint16_t, m_iItemDefinitionIndex, "C_EconEntity->m_AttributeManager", 0x50 + 0x1BA);

	// Accuracy (hitchance / CalcSpread)
	schema(int, m_weaponMode, "C_CSWeaponBase->m_weaponMode");
	schema(float, m_flTurningInaccuracy, "C_CSWeaponBase->m_flTurningInaccuracy");
	schema(float, m_fAccuracyPenalty, "C_CSWeaponBase->m_fAccuracyPenalty");
	schema(float, m_flRecoilIndex, "C_CSWeaponBase->m_flRecoilIndex");

	CCSWeaponBaseVData* Data();
	bool IsNonGunWeapon() const;
};

// Planted bomb (C_PlantedC4) — cast entity when class name matches
class C_PlantedC4
{
public:
	schema(bool, m_bBombTicking, "C_PlantedC4->m_bBombTicking");
	schema(std::int32_t, m_nBombSite, "C_PlantedC4->m_nBombSite");
	schema(float, m_flC4Blow, "C_PlantedC4->m_flC4Blow");
	schema(bool, m_bCannotBeDefused, "C_PlantedC4->m_bCannotBeDefused");
	schema(bool, m_bHasExploded, "C_PlantedC4->m_bHasExploded");
	schema(float, m_flTimerLength, "C_PlantedC4->m_flTimerLength");
	schema(bool, m_bBeingDefused, "C_PlantedC4->m_bBeingDefused");
	schema(float, m_flDefuseCountDown, "C_PlantedC4->m_flDefuseCountDown");
	schema(bool, m_bBombDefused, "C_PlantedC4->m_bBombDefused");
};