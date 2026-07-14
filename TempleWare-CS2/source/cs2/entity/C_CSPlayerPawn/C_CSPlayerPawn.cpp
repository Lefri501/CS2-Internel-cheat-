#include "C_CSPlayerPawn.h"

#include "../../../templeware/offsets/offsets.h"
#include "../../../templeware/interfaces/interfaces.h"

C_CSPlayerPawn::C_CSPlayerPawn(uintptr_t address) : address(address) {}

Vector_t C_CSPlayerPawn::getPosition() const {
	// Prefer live scene abs origin over m_vOldOrigin (pre-move / laggy).
	const uint32_t sceneOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_pGameSceneNode"));
	const uint32_t originOff = SchemaFinder::Get(hash_32_fnv1a_const("CGameSceneNode->m_vecAbsOrigin"));
	if (sceneOff && originOff) {
		void* node = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(this) + sceneOff);
		if (node) {
			return *reinterpret_cast<Vector_t*>(
				reinterpret_cast<uintptr_t>(node) + originOff);
		}
	}
	return *reinterpret_cast<Vector_t*>(
		reinterpret_cast<uintptr_t>(this) +
		SchemaFinder::Get(hash_32_fnv1a_const("C_BasePlayerPawn->m_vOldOrigin")));
}

Vector_t C_CSPlayerPawn::getEyePosition() const {
	// NEVER m_vOldOrigin + view (UC triggerbot thread).
	// Prefer abs origin + view offset; local callers should use Bones::GetShootPos
	// (weapon history / NetClientInfo ShootPosition).
	return getPosition() + getViewOffset();
}

C_CSWeaponBase* C_CSPlayerPawn::GetActiveWeapon() const {
	if (!this)
		return nullptr;

	CCSPlayer_WeaponServices* weapon_services = this->GetWeaponServices();
	if (!weapon_services)
		return nullptr;

	if (!I::GameEntity || !I::GameEntity->Instance)
		return nullptr;

	return I::GameEntity->Instance->Get<C_CSWeaponBase>(weapon_services->m_hActiveWeapon());
}

CCSPlayer_WeaponServices* C_CSPlayerPawn::GetWeaponServices() const {
	if (!this)
		return nullptr;

	// Field lives on C_BasePlayerPawn, NOT C_CSPlayerPawn (schema hash 0x3E4F3B63 was the bad path)
	const uint32_t off = SchemaFinder::Get(hash_32_fnv1a_const("C_BasePlayerPawn->m_pWeaponServices"));
	if (!off)
		return nullptr;

	return *reinterpret_cast<CCSPlayer_WeaponServices**>(
		reinterpret_cast<uintptr_t>(this) + off);
}

uintptr_t C_CSPlayerPawn::getAddress() const {
	return reinterpret_cast<uintptr_t>(this);
}

int C_CSPlayerPawn::getHealth() const {
	return *reinterpret_cast<int*>(
		reinterpret_cast<uintptr_t>(this) +
		SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_iHealth")));
}

uint8_t C_CSPlayerPawn::getTeam() const {
	return *reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(this) +
		SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_iTeamNum")));
}

Vector_t C_CSPlayerPawn::getViewOffset() const {
	return *reinterpret_cast<Vector_t*>(
		reinterpret_cast<uintptr_t>(this) +
		SchemaFinder::Get(hash_32_fnv1a_const("C_BaseModelEntity->m_vecViewOffset")));
}
