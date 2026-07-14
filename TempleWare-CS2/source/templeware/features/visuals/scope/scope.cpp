#include "scope.h"
#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../../external/imgui/imgui.h"
#include "../../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

#include <algorithm>
#include <cstdint>

// DrawScopeOverlay out-struct (client.dll @ 0x1808A0520)
// +0  float blend / intensity
// +4  bool  draw
// +8  float blur strength (0..1)
// +12 float lens size X
// +16 float lens size Y
// +20 int   style
// +24 int   screen metric

namespace {
	// Bars/texture share the same draw path as the game's scope lines.
	// Kill the whole overlay and redraw lines ourselves (classic clean scope).
	bool LinesOnlyMode() {
		return Config::scope_remove_bars || Config::scope_remove_texture;
	}
}

std::int64_t __fastcall H::hkDrawScopeOverlay(void* a1, void* a2) {
	auto original = H::DrawScopeOverlay.GetOriginal();
	std::int64_t result = 0;
	if (original) {
		__try { result = original(a1, a2); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	if (!a2)
		return result;

	const bool any =
		Config::scope_no_overlay ||
		Config::scope_remove_blur ||
		Config::scope_remove_bars ||
		Config::scope_remove_texture ||
		Config::scope_custom_look;
	if (!any)
		return result;

	__try {
		auto* out = reinterpret_cast<std::uint8_t*>(a2);

		// Full kill — no bars, no dirt, no engine lines
		if (Config::scope_no_overlay || LinesOnlyMode()) {
			out[4] = 0;
			return result;
		}

		if (Config::scope_remove_blur)
			*reinterpret_cast<float*>(out + 8) = 0.f;

		if (Config::scope_custom_look) {
			const float scale = (std::max)(0.1f, Config::scope_size_scale);
			*reinterpret_cast<float*>(out + 12) *= scale;
			*reinterpret_cast<float*>(out + 16) *= scale;

			if (!Config::scope_remove_blur) {
				float blur = Config::scope_blur_amount;
				if (blur < 0.f) blur = 0.f;
				if (blur > 1.f) blur = 1.f;
				*reinterpret_cast<float*>(out + 8) = blur;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}

	return result;
}

void Scope::DrawLines() {
	if (Config::scope_no_overlay)
		return;
	if (!LinesOnlyMode())
		return;
	if (!H::oGetLocalPlayer)
		return;

	C_CSPlayerPawn* local = nullptr;
	__try { local = H::oGetLocalPlayer(0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (!local)
		return;

	bool scoped = false;
	__try { scoped = local->m_bIsScoped(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (!scoped)
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	const float cx = ds.x * 0.5f;
	const float cy = ds.y * 0.5f;
	const float gap = 4.f; // small center hole like stock scope
	const float th = 1.f;
	const ImU32 col = IM_COL32(0, 0, 0, 255);

	// Horizontal
	dl->AddLine(ImVec2(0.f, cy), ImVec2(cx - gap, cy), col, th);
	dl->AddLine(ImVec2(cx + gap, cy), ImVec2(ds.x, cy), col, th);
	// Vertical
	dl->AddLine(ImVec2(cx, 0.f), ImVec2(cx, cy - gap), col, th);
	dl->AddLine(ImVec2(cx, cy + gap), ImVec2(cx, ds.y), col, th);
}
