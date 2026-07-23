#include "patternscan.h"

#include "../../module/module.h"
#include "../../console/console.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Module resolve
//
// Call sites mix short registry names ("client") and PE names ("client.dll").
// GetModuleHandleA("client") fails — game loads as "client.dll".
// patternScan used Modules registry (short OK); FindPattern/scan did not → miss.
// ---------------------------------------------------------------------------
static HMODULE ResolveModuleHandle(const char* name)
{
	if (!name || !name[0])
		return nullptr;

	// 1) Modules registry (short keys: "client", "engine2", ...)
	if (const uintptr_t reg = modules.getModule(name))
		return reinterpret_cast<HMODULE>(reg);

	// 2) Exact PE name
	if (HMODULE h = GetModuleHandleA(name))
		return h;

	char buf[160]{};
	const size_t len = strnlen(name, 140);
	if (len == 0 || len >= 140)
		return nullptr;

	const bool hasDot = (strchr(name, '.') != nullptr);

	// 3) short → short.dll
	if (!hasDot) {
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s.dll", name);
		if (HMODULE h = GetModuleHandleA(buf))
			return h;
		// registry already tried exact short; also try after dll form via getModule base name
		if (const uintptr_t reg = modules.getModule(name))
			return reinterpret_cast<HMODULE>(reg);
		return nullptr;
	}

	// 4) "client.dll" → strip .dll → registry short name
	if (len > 4) {
		const char* ext = name + (len - 4);
		if (_stricmp(ext, ".dll") == 0) {
			memcpy(buf, name, len - 4);
			buf[len - 4] = '\0';
			if (const uintptr_t reg = modules.getModule(buf))
				return reinterpret_cast<HMODULE>(reg);
			if (HMODULE h = GetModuleHandleA(buf))
				return h;
		}
	}

	return nullptr;
}

static bool GetModuleBounds(HMODULE mod, uintptr_t& outBase, size_t& outSize)
{
	outBase = 0;
	outSize = 0;
	if (!mod)
		return false;

	outBase = reinterpret_cast<uintptr_t>(mod);

	MODULEINFO mi{};
	if (GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))
		&& mi.SizeOfImage > 0) {
		outBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll ? mi.lpBaseOfDll : mod);
		outSize = static_cast<size_t>(mi.SizeOfImage);
		return true;
	}

	// Fallback: PE headers (works when Psapi fails)
	__try {
		const auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
			reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		outSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
		return outSize > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// pair: (byte, must_match) — must_match=false → wildcard
static std::vector<std::pair<uint8_t, bool>> PatternToBytes(const std::string& pattern)
{
	std::vector<std::pair<uint8_t, bool>> patternBytes;
	const char* start = pattern.c_str();
	const char* end = start + pattern.size();

	for (const char* current = start; current < end; ++current) {
		if (*current == ' ')
			continue;
		if (*current == '?') {
			patternBytes.emplace_back(0, false);
			if (current + 1 < end && *(current + 1) == '?')
				++current;
		} else {
			if (!std::isxdigit(static_cast<unsigned char>(*current)))
				continue;
			char* next = nullptr;
			const auto val = static_cast<uint8_t>(strtoul(current, &next, 16));
			patternBytes.emplace_back(val, true);
			// strtoul advances past hex digits; loop will ++current so step back one
			if (next && next > current)
				current = next - 1;
		}
	}

	return patternBytes;
}

// pair: (byte, is_wildcard) — opposite flag of PatternToBytes (legacy FindPattern)
static std::vector<std::pair<uint8_t, bool>> PatternToBytesWildcard(const std::string& pattern)
{
	std::vector<std::pair<uint8_t, bool>> out;
	std::stringstream stream(pattern);
	std::string token;
	while (stream >> token) {
		if (token.empty())
			continue;
		if (token[0] == '?') {
			out.emplace_back(0u, true);
			continue;
		}
		if (token.size() < 2
			|| !std::isxdigit(static_cast<unsigned char>(token[0]))
			|| !std::isxdigit(static_cast<unsigned char>(token[1])))
			continue;
		out.emplace_back(static_cast<uint8_t>(std::strtoul(token.data(), nullptr, 16)), false);
	}
	return out;
}

static uintptr_t patternScanRaw(uintptr_t base, size_t size, const std::pair<uint8_t, bool>* pattern, size_t len)
{
	if (!base || !pattern || len == 0 || size < len)
		return 0;

	const size_t last = size - len;
	__try {
		for (size_t i = 0; i <= last; ++i) {
			bool found = true;
			for (size_t j = 0; j < len; ++j) {
				// second=true → must match byte
				if (pattern[j].second
					&& pattern[j].first != *reinterpret_cast<uint8_t*>(base + i + j)) {
					found = false;
					break;
				}
			}
			if (found)
				return base + i;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("patternScan scan", GetExceptionCode());
	}
	return 0;
}

uintptr_t M::patternScan(const std::string& module, const std::string& pattern)
{
	HMODULE hModule = ResolveModuleHandle(module.c_str());
	if (!hModule)
		return 0;

	uintptr_t baseAddress = 0;
	size_t moduleSize = 0;
	if (!GetModuleBounds(hModule, baseAddress, moduleSize))
		return 0;

	const auto patternBytes = PatternToBytes(pattern);
	if (patternBytes.empty())
		return 0;

	const uintptr_t result = patternScanRaw(
		baseAddress, moduleSize, patternBytes.data(), patternBytes.size());
	if (!result)
		Con::PatternMiss(module.c_str(), pattern.c_str());
	return result;
}

static std::vector<int> ida_to_bytes(const char* pattern)
{
	std::vector<int> bytes;
	if (!pattern)
		return bytes;

	const char* start = pattern;
	const char* end = pattern + strlen(pattern);

	for (const char* current = start; current < end; ) {
		if (*current == ' ' || *current == '\t') {
			++current;
			continue;
		}
		if (*current == '?') {
			++current;
			if (current < end && *current == '?')
				++current;
			bytes.push_back(-1);
			continue;
		}
		if (!std::isxdigit(static_cast<unsigned char>(*current))) {
			++current;
			continue;
		}
		char* next = nullptr;
		bytes.push_back(static_cast<int>(strtoul(current, &next, 16)));
		current = (next && next > current) ? next : current + 1;
	}

	return bytes;
}

static uint8_t* scanRaw(uint8_t* base, size_t size, const int* pattern, size_t len)
{
	if (!base || !pattern || len == 0 || size < len)
		return nullptr;

	const size_t last = size - len;
	__try {
		for (size_t i = 0; i <= last; ++i) {
			bool found = true;
			for (size_t j = 0; j < len; ++j) {
				if (pattern[j] == -1)
					continue;
				if (base[i + j] != static_cast<uint8_t>(pattern[j])) {
					found = false;
					break;
				}
			}
			if (found)
				return &base[i];
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("M::scan", GetExceptionCode());
	}
	return nullptr;
}

uint8_t* M::scan(const char* module_name, const char* pattern)
{
	HMODULE module_handle = ResolveModuleHandle(module_name);
	if (!module_handle)
		return nullptr;

	uintptr_t base = 0;
	size_t size_of_image = 0;
	if (!GetModuleBounds(module_handle, base, size_of_image))
		return nullptr;

	const auto pattern_bytes = ida_to_bytes(pattern);
	if (pattern_bytes.empty())
		return nullptr;

	uint8_t* result = scanRaw(
		reinterpret_cast<uint8_t*>(base),
		size_of_image,
		pattern_bytes.data(),
		pattern_bytes.size());
	if (!result)
		Con::PatternMiss(module_name, pattern);
	return result;
}

static uint8_t* findPatternRaw(
	uint8_t* base, uint8_t* end,
	const std::pair<uint8_t, bool>* pattern, size_t len)
{
	if (!base || !end || !pattern || len == 0 || end <= base)
		return nullptr;
	if (static_cast<size_t>(end - base) < len)
		return nullptr;

	__try {
		// pair.second = is_wildcard
		auto ret = std::search(base, end, pattern, pattern + len,
			[](uint8_t byte, const std::pair<uint8_t, bool>& seq) {
				return seq.second || byte == seq.first;
			});
		if (ret != end)
			return ret;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("M::FindPattern", GetExceptionCode());
	}
	return nullptr;
}

std::uint8_t* M::FindPattern(const char* module_name, const std::string& byte_sequence)
{
	const HMODULE module = ResolveModuleHandle(module_name);
	if (!module)
		return nullptr;

	uintptr_t baseAddr = 0;
	size_t imageSize = 0;
	if (!GetModuleBounds(module, baseAddr, imageSize))
		return nullptr;

	// Full image scan (was SizeOfCode only → missed some code/data patterns)
	auto* m_base = reinterpret_cast<std::uint8_t*>(baseAddr);
	auto* end = m_base + imageSize;

	const auto byte_sequence_vec = PatternToBytesWildcard(byte_sequence);
	if (byte_sequence_vec.empty())
		return nullptr;

	uint8_t* result = findPatternRaw(
		m_base, end, byte_sequence_vec.data(), byte_sequence_vec.size());
	if (!result)
		Con::PatternMiss(module_name, byte_sequence.c_str());
	return result;
}
