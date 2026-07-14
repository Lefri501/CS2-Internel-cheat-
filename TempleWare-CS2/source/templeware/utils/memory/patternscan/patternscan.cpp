#include "patternscan.h"

#include "../../module/module.h"
#include "../../console/console.h"

#include <Windows.h>
#include <vector>
#include <string>
#include <iostream>
#include <span>
#include <sstream>

#include <Psapi.h>
#include <algorithm>
#include <string_view>
#include <cstdint>
#include <functional>
#include <string>

#define WINCALL(func) func
std::vector<std::pair<uint8_t, bool>> PatternToBytes(const std::string& pattern) {
    std::vector<std::pair<uint8_t, bool>> patternBytes;
    const char* start = pattern.c_str();
    const char* end = start + pattern.size();

    for (const char* current = start; current < end; ++current) {
        if (*current == ' ') continue;
        if (*current == '?') {
            patternBytes.emplace_back(0, false);
            if (*(current + 1) == '?') ++current;
        }
        else {
            patternBytes.emplace_back(static_cast<uint8_t>(strtoul(current, nullptr, 16)), true);
            if (*(current + 1) != ' ') ++current;
        }
    }

    return patternBytes;
}

static uintptr_t patternScanRaw(uintptr_t base, size_t size, const std::pair<uint8_t, bool>* pattern, size_t len) {
    __try {
        for (size_t i = 0; i < size - len; ++i) {
            bool found = true;
            for (size_t j = 0; j < len; ++j) {
                if (pattern[j].second && pattern[j].first != *reinterpret_cast<uint8_t*>(base + i + j)) {
                    found = false;
                    break;
                }
            }
            if (found) return base + i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("patternScan scan", GetExceptionCode());
    }
    return 0;
}

uintptr_t M::patternScan(const std::string& module, const std::string& pattern) {
    uintptr_t baseAddress = modules.getModule(module);
    HMODULE hModule = reinterpret_cast<HMODULE>(baseAddress);

    if (!hModule) return 0;

    MODULEINFO moduleInfo;
    GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(MODULEINFO));

    size_t moduleSize = moduleInfo.SizeOfImage;

    std::vector<std::pair<uint8_t, bool>> patternBytes = PatternToBytes(pattern);
    if (patternBytes.empty()) return 0;

    uintptr_t result = patternScanRaw(baseAddress, moduleSize, patternBytes.data(), patternBytes.size());
    if (!result)
        Con::PatternMiss(module.c_str(), pattern.c_str());
    return result;
}

std::vector<int> ida_to_bytes(const char* pattern)
{
	std::vector<int> bytes = std::vector<int>{};
	char* start = const_cast<char*>(pattern);
	char* end = const_cast<char*>(pattern) + strlen(pattern);

	for (char* current = start; current < end; ++current) {
		if (*current == '?') {
			++current;

			if (*current == '?')
				++current;

			bytes.push_back(-1);
		}
		else {
			bytes.push_back(strtoul(current, &current, 16));
		}
	}

	return bytes;
}

static uint8_t* scanRaw(uint8_t* base, size_t size, const int* pattern, size_t len) {
    __try {
        for (unsigned int i = 0; i < size - len; i++) {
            bool found = true;
            for (unsigned int j = 0; j < len; ++j) {
                if (pattern[j] == -1) continue;
                if (base[i + j] != static_cast<uint8_t>(pattern[j])) {
                    found = false;
                    break;
                }
            }
            if (found) return &base[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("M::scan", GetExceptionCode());
    }
    return nullptr;
}

uint8_t* M::scan(const char* module_name, const char* pattern)
{
	void* module_handle = WINCALL(GetModuleHandle)(module_name);
	if (module_handle == nullptr)
		return nullptr;

	PIMAGE_DOS_HEADER dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(module_handle);
	PIMAGE_NT_HEADERS nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t*>(module_handle) + dos_header->e_lfanew);

	auto size_of_image = nt_headers->OptionalHeader.SizeOfImage;
	auto pattern_bytes = ida_to_bytes(pattern);
	auto scan_bytes = reinterpret_cast<uint8_t*>(module_handle);

	if (pattern_bytes.empty()) return nullptr;

	uint8_t* result = scanRaw(scan_bytes, size_of_image, pattern_bytes.data(), pattern_bytes.size());
	if (!result)
		Con::PatternMiss(module_name, pattern);
	return result;
}


static uint8_t* findPatternRaw(uint8_t* base, uint8_t* end, const std::pair<uint8_t, bool>* pattern, size_t len) {
    __try {
        auto ret = std::search(base, end, pattern, pattern + len,
            [](uint8_t byte, const std::pair<uint8_t, bool>& seq) {
                return seq.second || byte == seq.first;
            });
        if (ret != end) return ret;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("M::FindPattern", GetExceptionCode());
    }
    return nullptr;
}

std::uint8_t* M::FindPattern(const char* module_name, const std::string& byte_sequence)
{
	// retrieve the handle to the specified module
	const HMODULE module = GetModuleHandleA(module_name);
	if (module == nullptr)
		return nullptr;

	// retrieve the DOS header of the module
	const auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
	if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return nullptr;

	// retrieve the NT headers of the module
	const auto nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<std::uint8_t*>(module) + dos_header->e_lfanew);
	if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
		return nullptr;

	// get the size and base address of the code section
	DWORD m_size = nt_headers->OptionalHeader.SizeOfCode;
	std::uint8_t* m_base = reinterpret_cast<std::uint8_t*>(module) + nt_headers->OptionalHeader.BaseOfCode;

	using SeqByte_t = std::pair< std::uint8_t, bool >;

	std::string str{ };
	std::vector< std::pair< std::uint8_t, bool > > byte_sequence_vec{ };
	std::stringstream stream(byte_sequence);
	// parse the byte sequence string into a vector of byte sequence elements
	while (stream >> str)
	{
		// wildcard byte
		if (str[0u] == '?')
		{
			byte_sequence_vec.emplace_back(0u, true);
			continue;
		}

		// invalid hex digit, skip this byte
		if (!std::isxdigit(str[0u]) || !std::isxdigit(str[1u]))
			continue;

		byte_sequence_vec.emplace_back(static_cast<std::uint8_t>(std::strtoul(str.data(), nullptr, 16)), false);
	}

	if (byte_sequence_vec.empty()) return nullptr;

	// end pointer of the code section
	const auto end = reinterpret_cast<std::uint8_t*>(m_base + m_size);

	uint8_t* result = findPatternRaw(m_base, end, byte_sequence_vec.data(), byte_sequence_vec.size());
	if (!result)
		Con::PatternMiss(module_name, byte_sequence.c_str());
	return result;
}