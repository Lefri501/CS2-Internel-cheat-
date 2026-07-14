#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

void __fastcall H::hkRenderFlashbangOverlay(void* a1, void* a2, void* a3, void* a4, void* a5) {
	float amount = Config::antiflash_amount;
	if (amount < 0.f) amount = 0.f;
	if (amount > 100.f) amount = 100.f;

	// 100% = skip overlay entirely (old antiflash bool behavior)
	if (amount >= 99.5f)
		return;

	// Partial: scale pawn flash alphas so the overlay draws weaker
	if (amount > 0.01f && H::oGetLocalPlayer) {
		C_CSPlayerPawn* local = nullptr;
		__try { local = H::oGetLocalPlayer(0); }
		__except (EXCEPTION_EXECUTE_HANDLER) { local = nullptr; }

		if (local) {
			const float keep = 1.f - (amount * 0.01f);
			__try {
				float& overlay = local->m_flFlashOverlayAlpha();
				float& maxA = local->m_flFlashMaxAlpha();
				overlay *= keep;
				maxA *= keep;
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

	auto original = RenderFlashBangOverlay.GetOriginal();
	if (original) original(a1, a2, a3, a4, a5);
}
