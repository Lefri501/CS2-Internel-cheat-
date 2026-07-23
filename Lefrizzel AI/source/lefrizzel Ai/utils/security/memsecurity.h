#pragma once
#include <Windows.h>
#include <cstdint>

// ============================================================================
// Post-init memory hardening
// After hooks are placed:
// 1. RWX → RX on our image executable pages
// 2. Optional PE header wipe (off by default — breaks PE walks)
// 3. Scrub init-only buffers (placeholder)
// ============================================================================

namespace MemSecurity {

	// Call after all initialization is complete.
	// wipeHeader=false is the safe default (RX harden only).
	void HardenAfterInit(void* baseAddr, bool wipeHeader = false);

	void WipePEHeader(void* baseAddr);
	void RemoveWriteFromCode(void* baseAddr, size_t imageSize);
	void ScrubInitData();

} // namespace MemSecurity
