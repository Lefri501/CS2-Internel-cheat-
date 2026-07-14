#include "gaa.h"
#include "../../console/console.h"
#include <windows.h>

uintptr_t M::getAbsoluteAddress(uintptr_t addr, const int nPreOffset, const int nPostOffset) {
	if (!addr) {
		Con::Error("getAbsoluteAddress: addr is null");
		return 0;
	}
	addr += nPreOffset;
	int32_t nRva = 0;
	__try {
		nRva = *reinterpret_cast<int32_t*>(addr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("getAbsoluteAddress", GetExceptionCode());
		return 0;
	}
	addr += nPostOffset + sizeof(uint32_t) + nRva;
	return addr;
}
