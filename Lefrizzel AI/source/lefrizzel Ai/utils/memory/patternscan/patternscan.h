#pragma once
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <vector>
#include <string>
#include "../../console/console.h"

namespace M {
	template <typename T = std::uint8_t>
	T* abs(T* relative_address, int pre_offset = 0x0, int post_offset = 0x0)
	{
		if (!relative_address) {
			Con::Error("abs: relative_address is null");
			return nullptr;
		}
		relative_address += pre_offset;
		int32_t offset = 0;
		__try {
			offset = *reinterpret_cast<std::int32_t*>(relative_address);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("M::abs", GetExceptionCode());
			return nullptr;
		}
		relative_address += sizeof(std::int32_t) + offset;
		relative_address += post_offset;
		return relative_address;
	}
	std::uint8_t* FindPattern(const char* module_name, const std::string& byte_sequence);

	template <typename T = std::uint8_t>
	T* GetAbsoluteAddress(T* relative_address, int pre_offset = 0x0, int post_offset = 0x0)
	{
		if (!relative_address) {
			Con::Error("GetAbsoluteAddress: relative_address is null");
			return nullptr;
		}
		relative_address += pre_offset;
		int32_t offset = 0;
		__try {
			offset = *reinterpret_cast<std::int32_t*>(relative_address);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("M::GetAbsoluteAddress", GetExceptionCode());
			return nullptr;
		}
		relative_address += sizeof(std::int32_t) + offset;
		relative_address += post_offset;
		return relative_address;
	}

	template <typename T>
	inline T GetVFunc(void* thisptr, std::size_t index)
	{
		if (!thisptr) {
			Con::Error("GetVFunc: thisptr is null");
			return T{};
		}
		__try {
			return (*reinterpret_cast<T**>(thisptr))[index];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("M::GetVFunc", GetExceptionCode());
			return T{};
		}
	}

	uintptr_t patternScan(const std::string& module, const std::string& pattern);

	uint8_t* scan(const char* module_name, const char* pattern);
}