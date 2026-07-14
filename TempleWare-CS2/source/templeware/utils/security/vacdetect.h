#pragma once
#include <Windows.h>
#include <cstddef>

// ============================================================================
// VAC module load detection (LdrRegisterDllNotification) + soft-pause.
// Soft-pause: stop high-risk feature logic for a window. Hooks stay in place
// (no unhook thrash). Not a VAC bypass — lowers activity while a scanner DLL loads.
// ============================================================================

namespace VacDetect {

	using VacLoadCallback = void(*)(const wchar_t* dllPath);

	bool Install(VacLoadCallback onVacLoad = nullptr);
	void Uninstall();

	bool WasVacDetected();
	int  GetDetectionCount();
	bool GetLastDetectedPath(wchar_t* out, size_t cch);

	// Soft-pause API (thread-safe)
	// Default window: 60s from last detection; refreshed on each hit.
	void SoftPause(unsigned ms = 60000);
	void SoftResume();
	bool IsSoftPaused();
	// Remaining ms (0 if not paused)
	unsigned SoftPauseRemainingMs();

} // namespace VacDetect
