#pragma once
#include <cstdint>

namespace World {
	namespace Weather {
		// Resolve GameParticleManager + ResourceSystem (Hooks::init).
		void Install();
		// FRAME_RENDER_END: CMapInfo wetness + ambient rain_fx follow grid.
		void Update();
		// Present call site kept; engine particles only (no ImGui).
		void Draw();
		void Shutdown();
		void ApplyMapInfo(uintptr_t mapInfo);
	}
}
