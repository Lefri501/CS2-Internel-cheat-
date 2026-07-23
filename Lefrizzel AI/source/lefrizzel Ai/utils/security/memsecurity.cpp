#include "memsecurity.h"
#include "../console/console.h"
#include <cstring>

namespace MemSecurity {

static size_t QueryAllocationSize(void* baseAddr) {
	if (!baseAddr)
		return 0;

	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(baseAddr, &mbi, sizeof(mbi)) == 0)
		return 0;

	void* allocBase = mbi.AllocationBase;
	size_t imageSize = 0;
	auto addr = reinterpret_cast<uint8_t*>(allocBase);
	while (true) {
		if (VirtualQuery(addr + imageSize, &mbi, sizeof(mbi)) == 0)
			break;
		if (mbi.AllocationBase != allocBase)
			break;
		imageSize += mbi.RegionSize;
	}
	return imageSize;
}

static size_t SizeFromPeHeaders(void* baseAddr) {
	if (!baseAddr)
		return 0;
	__try {
		auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(baseAddr);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;
		auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
			reinterpret_cast<uint8_t*>(baseAddr) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return 0;
		return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

void WipePEHeader(void* baseAddr) {
	if (!baseAddr)
		return;

	auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(baseAddr);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;

	auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
		reinterpret_cast<uint8_t*>(baseAddr) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return;

	const size_t headerSize = nt->OptionalHeader.SizeOfHeaders;
	if (!headerSize || headerSize > 0x10000)
		return;

	DWORD oldProt = 0;
	if (VirtualProtect(baseAddr, headerSize, PAGE_READWRITE, &oldProt)) {
		SecureZeroMemory(baseAddr, headerSize);
		VirtualProtect(baseAddr, headerSize, oldProt, &oldProt);
		Con::Ok("PE header wiped (%zu bytes)", headerSize);
	}
}

void RemoveWriteFromCode(void* baseAddr, size_t imageSize) {
	if (!baseAddr || !imageSize)
		return;

	MEMORY_BASIC_INFORMATION mbi{};
	auto addr = reinterpret_cast<uint8_t*>(baseAddr);
	const auto end = addr + imageSize;
	int pagesHardened = 0;

	// Regions belong to the allocation that contains baseAddr
	MEMORY_BASIC_INFORMATION baseMbi{};
	if (VirtualQuery(baseAddr, &baseMbi, sizeof(baseMbi)) == 0)
		return;
	void* allocBase = baseMbi.AllocationBase;

	while (addr < end) {
		if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0)
			break;

		if (mbi.AllocationBase == allocBase) {
			// Only RWX → RX. Leave pure RW data alone (globals, config).
			if (mbi.Protect == PAGE_EXECUTE_READWRITE
				|| mbi.Protect == PAGE_EXECUTE_WRITECOPY) {
				DWORD oldProt = 0;
				if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READ, &oldProt))
					++pagesHardened;
			}
		}

		if (mbi.RegionSize == 0)
			break;
		addr += mbi.RegionSize;
	}

	if (pagesHardened > 0)
		Con::Ok("Memory hardened: %d regions -> RX", pagesHardened);
}

void ScrubInitData() {
	// Pattern strings use xorstr. No durable plaintext init buffers to scrub yet.
}

void HardenAfterInit(void* baseAddr, bool wipeHeader) {
	if (!baseAddr)
		return;

	// Size BEFORE any header wipe
	size_t imageSize = SizeFromPeHeaders(baseAddr);
	if (!imageSize)
		imageSize = QueryAllocationSize(baseAddr);
	if (!imageSize) {
		Con::Error("MemSecurity: could not resolve image size");
		return;
	}

	// Safe default: RX only. SafetyHook trampolines live in external allocs.
	RemoveWriteFromCode(baseAddr, imageSize);

	// Optional PE wipe — only after SEH registered; breaks late PE walks
	if (wipeHeader)
		WipePEHeader(baseAddr);

	ScrubInitData();
	Con::Ok("MemSecurity harden done (wipe=%d size=%zu)", wipeHeader ? 1 : 0, imageSize);
}

} // namespace MemSecurity
