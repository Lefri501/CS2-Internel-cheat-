#include "C_CSWeaponBase.h"
#include "..\..\..\templeware\hooks\hooks.h"
#include <cstring>
#include "..\C_EntityInstance\C_EntityInstance.h"
CCSWeaponBaseVData* C_CSWeaponBase::Data()
{
	// return pointer to weapon data
	return *reinterpret_cast<CCSWeaponBaseVData**>((uintptr_t)this + H::oGetWeaponData);
}

bool C_CSWeaponBase::IsNonGunWeapon() const
{
	// Class-name first (stable). VData type enum shifts between patches.
	auto* wEntity = reinterpret_cast<CEntityInstance*>(const_cast<C_CSWeaponBase*>(this));
	SchemaClassInfoData_t* wClass = nullptr;
	if (wEntity)
		wEntity->dump_class_info(&wClass);
	if (wClass && wClass->szName) {
		const char* n = wClass->szName;
		if (strstr(n, "Knife") || strstr(n, "Taser") || strstr(n, "Grenade") || strstr(n, "C4") ||
			strstr(n, "Flash") || strstr(n, "Smoke") || strstr(n, "Molotov") || strstr(n, "Decoy") ||
			strstr(n, "Shield") || strstr(n, "BaseItem") || strstr(n, "Tablet") || strstr(n, "Fists") ||
			strstr(n, "Melee") || strstr(n, "Healthshot") || strstr(n, "BumpMine"))
			return true;
		// Named gun classes: C_Weapon*, C_DEagle, C_AK47, etc.
		if (strstr(n, "Weapon") || strstr(n, "DEagle") || strstr(n, "AK47") || strstr(n, "M4A") ||
			strstr(n, "SSG") || strstr(n, "AWP") || strstr(n, "Glock") || strstr(n, "USP") ||
			strstr(n, "P250") || strstr(n, "FiveSeven") || strstr(n, "Tec9") || strstr(n, "Elite") ||
			strstr(n, "Revolver") || strstr(n, "Negev") || strstr(n, "M249") || strstr(n, "Nova") ||
			strstr(n, "XM1014") || strstr(n, "MAG7") || strstr(n, "Sawed") || strstr(n, "MAC10") ||
			strstr(n, "MP") || strstr(n, "P90") || strstr(n, "Bizon") || strstr(n, "UMP") ||
			strstr(n, "Galil") || strstr(n, "Famas") || strstr(n, "SG556") || strstr(n, "AUG") ||
			strstr(n, "SCAR") || strstr(n, "G3SG") || strstr(n, "Scout"))
			return false;
	}

	auto* vdata = const_cast<C_CSWeaponBase*>(this)->Data();
	if (vdata) {
		const int wtype = vdata->m_WeaponType();
		// CS2 CCSWeaponType: knife=0, pistol=1, submachinegun=2, rifle=3, shotgun=4, sniper=5, machinegun=6, c4=7, taser=8, grenade=9, equipment=10...
		if (wtype >= 1 && wtype <= 6)
			return false;
		return true;
	}

	// Unknown: allow aimbot attempt rather than hard-block
	return false;
}
