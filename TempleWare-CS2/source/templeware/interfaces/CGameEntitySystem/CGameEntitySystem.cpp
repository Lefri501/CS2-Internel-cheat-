#include "CGameEntitySystem.h"
#include "..\..\hooks\hooks.h"
#include "..\..\utils\memory\memsafe\memsafe.h"

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
