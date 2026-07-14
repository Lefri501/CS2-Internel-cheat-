#pragma once
#include <cstdint>

namespace M {
	uintptr_t getAbsoluteAddress(uintptr_t addr, const int nPreOffset, const int nPostOffset = 0);
}