#pragma once

class CMeshData;

namespace World {
	// Nightmode / skybox entity path; map color + lighting via Andromeda-style inline hooks.
	void Update();
	void InvalidateEnvCache(); // LevelInit / map change
	void InstallMapColorHook();
	void InstallLightingHook();
	// smoke_color entity walk (~8 Hz) — also from removals ApplySmokeColorTick
	void ApplySmokeColor();

	// DrawArray / GeneratePrimitives — tint meshes (Map Color and/or Night prop darken).
	void ApplyMapMeshColor(CMeshData* mesh, int meshCount = 1);
	bool WantPropTint(); // map_color || Night
}

#include "fog_handler.h"

// Defined in removals.cpp — entity m_vSmokeColor write
void ApplySmokeColorTick();
// Defined in removals.cpp — legacy tick (no-op; tint is ParticleDrawArray hook)
void ApplyFireColorTick();

#include "weather.h"
