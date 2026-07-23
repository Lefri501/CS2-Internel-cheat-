#include "memsafe.h"

namespace Mem {

bool IsReadableHeavy(const void* p, std::size_t size) noexcept {
	if (!IsReadable(p, size))
		return false;

	const auto start = reinterpret_cast<std::uintptr_t>(p);
	const auto end = start + size;
	std::uintptr_t addr = start;
	while (addr < end) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0)
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		const DWORD prot = mbi.Protect;
		if ((prot & PAGE_NOACCESS) || (prot & PAGE_GUARD))
			return false;
		if (!(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			return false;
		const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (regionEnd <= addr)
			return false;
		addr = regionEnd;
	}
	return true;
}

} // namespace Mem
