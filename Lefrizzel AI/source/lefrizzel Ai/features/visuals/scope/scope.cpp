#include "scope.h"
#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../interfaces/interfaces.h"
#include "../../../../../external/imgui/imgui.h"
#include "../../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// DrawScopeOverlay out-struct (client.dll @ 0x1808A0740)
// +0  float blend / intensity
// +4  bool  draw
// +8  float blur strength (0..1)
// +12 float lens size X
// +16 float lens size Y
// +20 int   style
// +24 int   screen metric

namespace Scope {
namespace {

// Menu/lobby still has a pawn sometimes; m_bIsScoped can stick true after disconnect.
bool InMatch() {
	if (!I::EngineClient)
		return false;
	bool ok = false;
	__try { ok = I::EngineClient->in_game() && I::EngineClient->connected(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return ok;
}

bool LocalAlivePawn(C_CSPlayerPawn*& out) {
	out = nullptr;
	if (!InMatch())
		return false;
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (!local)
		return false;
	int hp = 0;
	std::uint8_t life = 1;
	__try {
		hp = local->m_iHealth();
		life = local->m_lifeState();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (hp <= 0 || hp > 200 || life != 0)
		return false;
	out = local;
	return true;
}

bool LocalScoped() {
	C_CSPlayerPawn* local = nullptr;
	if (!LocalAlivePawn(local))
		return false;
	bool scoped = false;
	int zl = 0;
	__try {
		scoped = local->m_bIsScoped();
		// Require live zoom — stale m_bIsScoped alone draws lines in lobby
		if (C_CSWeaponBase* wep = local->GetActiveWeapon())
			zl = wep->m_zoomLevel();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return scoped && zl > 0;
}

// m_zoomLevel on active gun: 0 hip, 1 first zoom, 2 second zoom
int LocalZoomLevel() {
	C_CSPlayerPawn* local = nullptr;
	if (!LocalAlivePawn(local))
		return 0;
	int zl = 0;
	__try {
		if (!local->m_bIsScoped())
			return 0;
		C_CSWeaponBase* wep = local->GetActiveWeapon();
		if (!wep)
			return 0;
		zl = wep->m_zoomLevel();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return zl;
}

} // namespace

// Used by GetRenderFov / OverrideView
bool GetCustomZoomFov(float& outFov) {
	if (!Config::scope_zoom_fov)
		return false;
	const int zl = LocalZoomLevel();
	if (zl <= 0)
		return false;
	if (zl == 1)
		outFov = std::clamp(Config::scope_fov_1, 1.f, 90.f);
	else
		outFov = std::clamp(Config::scope_fov_2, 1.f, 90.f);
	return true;
}

// VData m_bHideViewModelWhenZoomed — engine path used by AWP/SSG.
// Force true while option on; restore stock when off/unscoped.
void ApplyHideViewmodel() {
	if (!Config::scope_hide_viewmodel)
		return;
	if (!LocalScoped())
		return;

	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (!local)
		return;

	__try {
		C_CSWeaponBase* wep = local->GetActiveWeapon();
		if (!wep)
			return;
		CCSWeaponBaseVData* vd = wep->Data();
		if (!vd)
			return;
		// Always force hide while scoped — works for rifles/pistols too
		vd->m_bHideViewModelWhenZoomed() = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

} // namespace Scope

std::int64_t __fastcall H::hkDrawScopeOverlay(void* a1, void* a2) {
	auto original = H::DrawScopeOverlay.GetOriginal();
	std::int64_t result = 0;
	if (original) {
		__try { result = original(a1, a2); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	if (!a2)
		return result;

	// Only strip stock overlay in-match while custom lines own the cross
	if (!Config::scope_custom_lines || !Scope::LocalScopedForOverlay())
		return result;

	__try {
		auto* out = reinterpret_cast<std::uint8_t*>(a2);
		out[4] = 0; // draw = false — kill bars/texture/stock lines
	} __except (EXCEPTION_EXECUTE_HANDLER) {}

	return result;
}

// Exposed for hook — same gates as DrawLines
bool Scope::LocalScopedForOverlay() {
	return LocalScoped();
}

void Scope::DrawLines() {
	if (!Config::scope_custom_lines)
		return;
	if (!LocalScoped())
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x < 8.f || ds.y < 8.f)
		return;

	const float cx = ds.x * 0.5f;
	const float cy = ds.y * 0.5f;

	const float size = std::clamp(Config::scope_line_size, 0.f, 1.f);
	const float gap = std::clamp(Config::scope_line_gap, 0.f, 80.f);
	// Hairline range: 0.1 .. 6 (was min 0.5 + heavy outline = always thick)
	const float th = std::clamp(Config::scope_line_thickness, 0.1f, 6.f);

	const float maxArmX = (std::max)(0.f, cx - gap);
	const float maxArmY = (std::max)(0.f, cy - gap);
	const float armX = maxArmX * size;
	const float armY = maxArmY * size;
	if (armX < 1.f && armY < 1.f)
		return;

	const ImVec4& c = Config::scope_line_color;
	const int a = static_cast<int>(std::clamp(c.w, 0.f, 1.f) * 255.f);
	if (a < 8)
		return;
	const ImU32 col = IM_COL32(
		static_cast<int>(std::clamp(c.x, 0.f, 1.f) * 255.f),
		static_cast<int>(std::clamp(c.y, 0.f, 1.f) * 255.f),
		static_cast<int>(std::clamp(c.z, 0.f, 1.f) * 255.f),
		a);

	// Outline only when thick enough — hairlines stay 1px
	const bool doOutline = th >= 0.75f;
	const float oth = th + 0.8f;
	const ImU32 outline = IM_COL32(0, 0, 0, (a * 2) / 3);

	auto hLine = [&](float x0, float x1) {
		if (doOutline)
			dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), outline, oth);
		dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), col, th);
	};
	auto vLine = [&](float y0, float y1) {
		if (doOutline)
			dl->AddLine(ImVec2(cx, y0), ImVec2(cx, y1), outline, oth);
		dl->AddLine(ImVec2(cx, y0), ImVec2(cx, y1), col, th);
	};

	if (armX >= 1.f) {
		hLine(cx - gap - armX, cx - gap);
		hLine(cx + gap, cx + gap + armX);
	}
	if (armY >= 1.f) {
		vLine(cy - gap - armY, cy - gap);
		vLine(cy + gap, cy + gap + armY);
	}
}
