#pragma once

class CMeshData;

namespace World {
	// Nightmode / skybox entity path; map color + lighting via Andromeda-style inline hooks.
	void Update();
	void InstallMapColorHook();
	void InstallLightingHook();

	// DrawArray static-mesh tint (walls/props that skip Aggregate).
	void ApplyMapMeshColor(CMeshData* mesh);
}

#include "weather.h"
