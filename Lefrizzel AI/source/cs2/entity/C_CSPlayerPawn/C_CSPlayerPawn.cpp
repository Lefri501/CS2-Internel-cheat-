#include "C_CSPlayerPawn.h"

#include "../../../lefrizzel Ai/offsets/offsets.h"
#include "../../../lefrizzel Ai/interfaces/interfaces.h"
#include "../../../lefrizzel Ai/utils/memory/memsafe/memsafe.h"

#include <Windows.h>

C_CSPlayerPawn::C_CSPlayerPawn(uintptr_t address) : address(address) {}

Vector_t C_CSPlayerPawn::getPosition() const {
	// Prefer live scene abs origin over m_vOldOrigin (pre-move / laggy).
	// SEH: death/respawn frees scene node mid-frame (TDM).
	Vector_t out{};
	if (!this)
		return out;
	__try {
		const uint32_t sceneOff = Offset::m_pGameSceneNode();
		const uint32_t originOff = Offset::m_vecAbsOrigin();
		if (sceneOff && originOff) {
			void* node = *reinterpret_cast<void**>(
				reinterpret_cast<uintptr_t>(this) + sceneOff);
			if (node && Mem::IsUserPtr(node)) {
				out = *reinterpret_cast<Vector_t*>(
					reinterpret_cast<uintptr_t>(node) + originOff);
				return out;
			}
		}
		const uint32_t oldOff = Offset::m_vOldOrigin();
		if (oldOff)
			out = *reinterpret_cast<Vector_t*>(
				reinterpret_cast<uintptr_t>(this) + oldOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out = Vector_t{};
	}
	return out;
}

Vector_t C_CSPlayerPawn::getEyePosition() const {
	// NEVER m_vOldOrigin + view (UC triggerbot thread).
	// Prefer abs origin + view offset; local callers should use Bones::GetShootPos
	// (weapon history / NetClientInfo ShootPosition).
	return getPosition() + getViewOffset();
}

C_CSWeaponBase* C_CSPlayerPawn::GetActiveWeapon() const {
	if (!this || !Mem::IsUserPtr(this))
		return nullptr;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return nullptr;

	C_CSWeaponBase* wpn = nullptr;
	__try {
		CCSPlayer_WeaponServices* weapon_services = this->GetWeaponServices();
		if (!weapon_services || !Mem::IsUserPtr(weapon_services))
			return nullptr;
		const CBaseHandle h = weapon_services->m_hActiveWeapon();
		if (!h.valid())
			return nullptr;
		wpn = I::GameEntity->Instance->Get<C_CSWeaponBase>(h);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	// Free'd / recycled handle: Get can return non-null garbage
	if (!wpn || !Mem::ValidEntity(wpn))
		return nullptr;
	return wpn;
}

CCSPlayer_WeaponServices* C_CSPlayerPawn::GetWeaponServices() const {
	if (!this || !Mem::IsUserPtr(this))
		return nullptr;

	// Field lives on C_BasePlayerPawn, NOT C_CSPlayerPawn (schema hash 0x3E4F3B63 was the bad path)
	const uint32_t off = SchemaFinder::Get(hash_32_fnv1a_const("C_BasePlayerPawn->m_pWeaponServices"));
	if (!off)
		return nullptr;

	CCSPlayer_WeaponServices* ws = nullptr;
	__try {
		ws = *reinterpret_cast<CCSPlayer_WeaponServices**>(
			reinterpret_cast<uintptr_t>(this) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!ws || !Mem::IsUserPtr(ws))
		return nullptr;
	return ws;
}

uintptr_t C_CSPlayerPawn::getAddress() const {
	return reinterpret_cast<uintptr_t>(this);
}

int C_CSPlayerPawn::getHealth() const {
	if (!this || !Mem::IsUserPtr(this))
		return 0;
	const uint32_t off = Offset::m_iHealth();
	if (!off)
		return 0;
	int hp = 0;
	__try {
		hp = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(this) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return hp;
}

uint8_t C_CSPlayerPawn::getTeam() const {
	if (!this || !Mem::IsUserPtr(this))
		return 0;
	const uint32_t off = Offset::m_iTeamNum();
	if (!off)
		return 0;
	uint8_t team = 0;
	__try {
		team = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(this) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return team;
}

Vector_t C_CSPlayerPawn::getViewOffset() const {
	if (!this || !Mem::IsUserPtr(this))
		return Vector_t{};
	const uint32_t off = Offset::m_vecViewOffset();
	if (!off)
		return Vector_t{};
	Vector_t v{};
	__try {
		v = *reinterpret_cast<Vector_t*>(reinterpret_cast<uintptr_t>(this) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return Vector_t{};
	}
	return v;
}
