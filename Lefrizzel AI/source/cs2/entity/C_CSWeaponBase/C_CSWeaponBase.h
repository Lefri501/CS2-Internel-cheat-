#pragma once
#include <cstdint>
#include "..\C_EntityInstance\C_EntityInstance.h"
#include "../../../lefrizzel Ai/utils/memory/memorycommon.h"
#include "../../../lefrizzel Ai/utils/math/vector/vector.h"
#include "..\..\..\..\source\lefrizzel Ai\utils\schema\schema.h"
#include "..\..\..\..\source\lefrizzel Ai\utils\memory\vfunc\vfunc.h"
#include "..\handle.h"

class CCSPlayer_WeaponServices
{
public:
	// m_hMyWeapons is C_NetworkUtlVectorBase<CHandle> — use raw offset + vector walk in skinchanger
	schema(CBaseHandle, m_hActiveWeapon, "CPlayer_WeaponServices->m_hActiveWeapon");
	schema(CBaseHandle, m_hLastWeapon, "CPlayer_WeaponServices->m_hLastWeapon");
	schema(int, m_iAmmo, "CPlayer_WeaponServices->m_iAmmo");
	schema(float, m_flNextAttack, "CCSPlayer_WeaponServices->m_flNextAttack");
	// Inspect gate (old m_bIsLookingAtWeapon / m_bIsHoldingLookAtWeapon removed from schema)
	schema(bool, m_bBlockInspectUntilNextGraphUpdate, "CCSPlayer_WeaponServices->m_bBlockInspectUntilNextGraphUpdate");
};

class CPlayer_ObserverServices
{
public:
	schema(std::uint8_t, m_iObserverMode, "CPlayer_ObserverServices->m_iObserverMode");
	schema(CBaseHandle, m_hObserverTarget, "CPlayer_ObserverServices->m_hObserverTarget");
	// Dump +0x54 — client forces mode locally (bypasses some team filters)
	schema(bool, m_bForcedObserverMode, "CPlayer_ObserverServices->m_bForcedObserverMode");
};

class CCSWeaponBaseVData
{
public:
	SCHEMA_ADD_OFFSET(const char*, m_szName, 0x720);
	// Weapon type from VData (CSWeaponType_t) - used by Andromeda's exact IsNonGunWeapon
	schema(int, m_WeaponType, "CCSWeaponBaseVData->m_WeaponType");
	// Full-auto flag (semi pistols false) — schema dump 0x734
	schema(bool, m_bIsFullAuto, "CCSWeaponBaseVData->m_bIsFullAuto");
	// Pellets per shot (shotguns > 1). Schema: CCSWeaponBaseVData+0x738
	schema(int, m_nNumBullets, "CCSWeaponBaseVData->m_nNumBullets");
	// Damage / pen (autowall + mindamage)
	schema(int, m_nDamage, "CCSWeaponBaseVData->m_nDamage");
	schema(float, m_flArmorRatio, "CCSWeaponBaseVData->m_flArmorRatio");
	schema(float, m_flRange, "CCSWeaponBaseVData->m_flRange");
	schema(float, m_flRangeModifier, "CCSWeaponBaseVData->m_flRangeModifier");
	schema(float, m_flPenetration, "CCSWeaponBaseVData->m_flPenetration");
	schema(float, m_flHeadshotMultiplier, "CCSWeaponBaseVData->m_flHeadshotMultiplier");
	// Grenade base throw speed (HE/flash ~750, molly/inc ~700)
	schema(float, m_flThrowVelocity, "CCSWeaponBaseVData->m_flThrowVelocity");
	// Hide viewmodel while zoomed (schema dump 0x7F9) — AWP/SSG true by default
	schema(bool, m_bHideViewModelWhenZoomed, "CCSWeaponBaseVData->m_bHideViewModelWhenZoomed");
	// IDA GetCycleTime 0x180791DD0: VData+0x740 CFiringModeFloat = float[2]
	// Primary mode index = C_CSWeaponBase::m_weaponMode (0 hip / 1 alt).
	SCHEMA_ADD_OFFSET(float, m_flCycleTimePrimary, 0x740);
	SCHEMA_ADD_OFFSET(float, m_flCycleTimeSecondary, 0x744);
	// Accuracy tables — dump CCSWeaponBaseVData (client_dll.hpp). CFiringModeFloat = float[2].
	// GetInaccuracy composes these; seed gates should use live mode, not hardcoded defs.
	SCHEMA_ADD_OFFSET(float, m_flSpread0, 0x758);
	SCHEMA_ADD_OFFSET(float, m_flSpread1, 0x75C);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyStand0, 0x768);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyStand1, 0x76C);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyJump0, 0x770);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyJump1, 0x774);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyFire0, 0x788);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyFire1, 0x78C);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyMove0, 0x790);
	SCHEMA_ADD_OFFSET(float, m_flInaccuracyMove1, 0x794);
	SCHEMA_ADD_OFFSET(float, m_flRecoveryTimeStand, 0x84C);
	SCHEMA_ADD_OFFSET(float, m_flRecoveryTimeCrouch, 0x848);
};

class C_CSWeaponBase 
{
public:
	schema(bool, m_bInReload, "C_CSWeaponBase->m_bInReload");
	// Inspect anim (replaces removed CCSPlayer_WeaponServices look-at bools)
	schema(bool, m_bInspectPending, "C_CSWeaponBase->m_bInspectPending");
	schema(bool, m_bInspectShouldLoop, "C_CSWeaponBase->m_bInspectShouldLoop");
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
	// Scope zoom lives on C_CSWeaponBaseGun (not base) — dump 0x1CE0
	schema(std::int32_t, m_zoomLevel, "C_CSWeaponBaseGun->m_zoomLevel");

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