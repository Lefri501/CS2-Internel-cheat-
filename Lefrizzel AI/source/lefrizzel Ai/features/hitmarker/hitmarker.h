#pragma once

#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "../../../../external/imgui/imgui.h"
#include <cstdint>

namespace Hitmarker {
	void Install();
	void Shutdown();
	void OnGameEvent(void* gameEvent);
	void Draw(const ViewMatrix& vm);

	// Call on AF/TR/aim fire — world mark uses this ray until bullet_impact arrives.
	// eye = shoot origin, fireAngles = punched view used for the pellet.
	void NoteLastFire(const Vector_t& eye, const QAngle_t& fireAngles);

	// Menu live preview — COD X in box (clip, pulse, damage, world sample).
	// mode: 0=normal, 1=head, 2=kill
	void DrawPreview(ImDrawList* dl, ImVec2 boxMin, ImVec2 boxMax, int mode = 0);

	// ReportHit pattern addr for optional hook (0 if unresolved).
	uintptr_t ReportHitAddr();
}
