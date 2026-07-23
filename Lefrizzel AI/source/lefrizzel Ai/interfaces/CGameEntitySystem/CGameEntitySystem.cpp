#include "CGameEntitySystem.h"
#include "..\..\hooks\hooks.h"
#include "..\..\utils\memory\memsafe\memsafe.h"
#include <Windows.h>

void* CGameEntitySystem::GetEntityByIndex(int nIndex) {
	if (!Mem::ValidEntityIndex(nIndex))
		return nullptr;
	if (!H::ogGetBaseEntity || !Mem::Valid(this, sizeof(void*)))
		return nullptr;

	void* result = nullptr;
	__try {
		result = H::ogGetBaseEntity(this, nIndex);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}

	if (!Mem::ValidEntity(result))
		return nullptr;
	return result;
}

bool CGameEntitySystem::MatchHandleSerial(void* entity, const CBaseHandle& expected) noexcept
{
	if (!entity || !expected.valid())
		return false;
	CBaseHandle actual{};
	__try {
		actual = reinterpret_cast<CEntityInstance*>(entity)->handle();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return actual.valid()
		&& actual.index() == expected.index()
		&& actual.serial_number() == expected.serial_number();
}
