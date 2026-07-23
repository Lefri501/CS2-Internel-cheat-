#pragma once

namespace Scope {
	// Custom scope crosshair lines when scoped (ImGui; replaces engine overlay).
	void DrawLines();

	// True only in-match, alive, scoped + zoomLevel>0 (not lobby stale flag).
	bool LocalScopedForOverlay();

	// If custom zoom FOV active and local is zoomed, write FOV and return true.
	bool GetCustomZoomFov(float& outFov);

	// Force hide weapon/viewmodel while scoped (VData flag + offset shove).
	// Call once per frame from HUD Present path.
	void ApplyHideViewmodel();
}
