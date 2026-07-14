#pragma once
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "../../../templeware/utils/schema/schema.h"
#include "../C_CSWeaponBase/C_CSWeaponBase.h"
#include "../C_BaseEntity/C_BaseEntity.h"

#include <cstdint>

class ViewAngleServerChange_t {
public:
	schema(QAngle_t*, qAngle, "ViewAngleServerChange_t->qAngle");
};

class C_CSPlayerPawn : public C_BaseEntity {
public:
	schema(Vector_t, m_vOldOrigin, "C_BasePlayerPawn->m_vOldOrigin");
	schema(Vector_t, m_vecViewOffset, "C_BaseModelEntity->m_vecViewOffset");
	schema(CCSPlayer_WeaponServices*, m_pWeaponServices, "C_BasePlayerPawn->m_pWeaponServices");
	schema(CPlayer_ObserverServices*, m_pObserverServices, "C_BasePlayerPawn->m_pObserverServices");
	schema(void*, m_pMovementServices, "C_BasePlayerPawn->m_pMovementServices");  // CCSPlayer_MovementServices*
	// CCSPlayer_ItemServices* — m_bHasHelmet @ +0x49 (autowall ScaleDamage)
	schema(void*, m_pItemServices, "C_BasePlayerPawn->m_pItemServices");
	// Live dump: both live on C_BaseEntity (not C_BasePlayerPawn) — wrong class = schema miss 0x0A6DB2C3
	schema(Vector_t, m_vecVelocity, "C_BaseEntity->m_vecVelocity");
	schema(Vector_t, m_vecAbsVelocity, "C_BaseEntity->m_vecAbsVelocity");
	schema(ViewAngleServerChange_t*, m_ServerViewAngleChanges, "C_BasePlayerPawn->m_ServerViewAngleChanges");
	schema(std::int32_t, m_ArmorValue, "C_CSPlayerPawn->m_ArmorValue");
	schema(bool, m_bIsScoped, "C_CSPlayerPawn->m_bIsScoped");
	schema(bool, m_bIsDefusing, "C_CSPlayerPawn->m_bIsDefusing");
	schema(bool, m_bIsGrabbingHostage, "C_CSPlayerPawn->m_bIsGrabbingHostage");
	schema(bool, m_bWaitForNoAttack, "C_CSPlayerPawn->m_bWaitForNoAttack");
	schema(float, m_flFlashDuration, "C_CSPlayerPawnBase->m_flFlashDuration");
	schema(float, m_flFlashOverlayAlpha, "C_CSPlayerPawnBase->m_flFlashOverlayAlpha");
	schema(float, m_flFlashMaxAlpha, "C_CSPlayerPawnBase->m_flFlashMaxAlpha");
	C_CSPlayerPawn(uintptr_t address);

	C_CSWeaponBase* GetActiveWeapon()const;
	CCSPlayer_WeaponServices* GetWeaponServices()const;
	Vector_t getPosition() const;
	Vector_t getEyePosition() const;

	uintptr_t getAddress() const;
	int getHealth() const;
	uint8_t getTeam() const;
	Vector_t getViewOffset() const;
private:
	uintptr_t address;
};
