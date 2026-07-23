#pragma once

// Custom paintkit colors (UC silvhook method):
// Mid-hook UpdateCompositeMaterialSet → append g_vColor0..3 COLOR4 loose vars.

namespace CustomPaint {
	bool Install();
	bool Ready();

	enum class SeedStatus : int { Fail = 0, Ok = 1, Pending = 2 };

	// Fill out[16] (4×RGBA floats) for the colour bugs.
	// Named kits → kit color0..3.
	// Filler kits → sample dominant colours from the panorama preview (all skins).
	// Pending = preview still loading — caller should retry next frame (do not lock).
	SeedStatus SeedFromPaintKit(int paintKitId, float outRgba[16],
		const char* simpleName = nullptr, const char* kitToken = nullptr);

	// Neutral white fallback when kit is missing.
	void SetNeutral(float outRgba[16]);

	// Restore CPaintKit color0..3 to the snapshot taken before we mutated them.
	void RestoreKitColors(int paintKitId);
}
