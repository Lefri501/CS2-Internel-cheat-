#pragma once
#include <cstdint>

namespace World {
	namespace Weather {
		// Resolve GameParticleManager + ResourceSystem (Hooks::init).
		void Install();
		// Present/FSN: spawn+follow engine particles (snow/stars/ash/rain/storm).
		void Update();
		// No-op — engine particles only (no ImGui overlay).
		void Draw();
		void Shutdown();
		void ApplyMapInfo(uintptr_t mapInfo);
		// LevelInit: extract embedded .vpcf_c + re-arm warm.
		void OnLevelChange();
		// round_start / new match — particles + map wetness die each round; force rebuild.
		void OnRoundStart();
		// FireEventClientSide: round_start / round_officially_started / etc.
		void OnGameEvent(void* gameEvent);
		// FrameStageNotify (non-render): spawn+destroy warm for custom particles.
		void WarmTick();
	}
}
