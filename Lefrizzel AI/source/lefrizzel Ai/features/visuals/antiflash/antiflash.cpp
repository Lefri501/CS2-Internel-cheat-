#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

// IDA FlashOverlay @ 0x18113C960 — builds "FlashbangOverlay" material.
// 100% = skip entirely. Partial = scale pawn flash alphas then call original.
void __fastcall H::hkRenderFlashbangOverlay(void* a1, int split, void** matSys, void* a4, void* a5) {
	float amount = Config::antiflash_amount;
	if (amount < 0.f) amount = 0.f;
	if (amount > 100.f) amount = 100.f;

	if (amount >= 99.5f)
		return;

	if (amount > 0.01f) {
		if (C_CSPlayerPawn* local = H::SafeLocalPlayer()) {
			const float keep = 1.f - (amount * 0.01f);
			__try {
				float& overlay = local->m_flFlashOverlayAlpha();
				float& maxA = local->m_flFlashMaxAlpha();
				// also damp duration residual so partial stays stable across frames
				float& dur = local->m_flFlashDuration();
				overlay *= keep;
				maxA *= keep;
				if (dur > 0.f)
					dur *= keep;
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

	auto original = RenderFlashBangOverlay.GetOriginal();
	if (original) original(a1, split, matSys, a4, a5);
}
