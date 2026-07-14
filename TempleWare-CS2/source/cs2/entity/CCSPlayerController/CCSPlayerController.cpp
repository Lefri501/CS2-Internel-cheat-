#include "CCSPlayerController.h"
#include "../../../templeware/utils/schema/schema.h"
#include <Windows.h>

CCSPlayerController::CCSPlayerController(uintptr_t address) : address(address) {}

uintptr_t CCSPlayerController::getAddress() const {
	return reinterpret_cast<uintptr_t>(this);
}

bool CCSPlayerController::ReadSanitizedName(char* buf, size_t bufSize) const {
	if (!buf || bufSize == 0)
		return false;
	buf[0] = '\0';
	if (!this)
		return false;

	bool ok = false;
	__try {
		const uint32_t off = SchemaFinder::Get(
			hash_32_fnv1a_const("CCSPlayerController->m_sSanitizedPlayerName"));
		if (!off)
			return false;

		// CUtlString: first field is typically the char* payload pointer.
		const char* p = *reinterpret_cast<const char* const*>(
			reinterpret_cast<const uint8_t*>(this) + off);
		if (!p)
			return false;

		size_t i = 0;
		for (; i + 1 < bufSize && p[i]; ++i)
			buf[i] = p[i];
		buf[i] = '\0';
		ok = i > 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		buf[0] = '\0';
		return false;
	}
	return ok;
}
