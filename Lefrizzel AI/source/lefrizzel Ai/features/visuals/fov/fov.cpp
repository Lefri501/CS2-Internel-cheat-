#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../keybinds/keybinds.h"
#include "../scope/scope.h"
#include "../../hitchance/hitchance.h"
#include "../../aim/aim_common.h"
#include "../../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../utils/math/vector/vector.h"
#include "../../../utils/memory/memsafe/memsafe.h"

#include <cmath>
#include <algorithm>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// CViewSetup — verified IDA client sub_180AF7810 (projection builder):
//   test [rsi+0x551], 2  → use [rsi+0x4D4] else width/height
//   FOV 0x498, origin 0x4A0, angles 0x4B8
// UC 740301/750011 listed 0x4D8/0x555 — STALE on this build (off by 4).
// No FOV scale: that is zoom, not stretch. Flag+aspect is the real path.
constexpr std::uintptr_t kViewFov = 0x498;
constexpr std::uintptr_t kViewOrigin = 0x4A0;
constexpr std::uintptr_t kViewAngles = 0x4B8;
constexpr std::uintptr_t kViewAspect = 0x4D4;       // flAspectRatio (IDA)
constexpr std::uintptr_t kViewAspectFlags = 0x551; // nSomeFlags; bit1 force aspect

// IDA camera punch path (client):
//   0x1808C3E20: eye = vtable+1368(pawn); punch = GetRemovedAimPunch(0x18088BBB0);
//                out = eye + punch  (sub_18080C360)
//   0x18088BBB0 → services@pawn+0x14B8 → 0x180812D90 dual-track compose
// CViewSetup.angles @ +0x4B8 after OverrideView are the PUNCHED camera.
// Visual no-recoil: restore raw aim (CSGOInput view) so kick is gone.
// Fire/seed still call GetRemovedAimPunch untouched.
static void StripViewAimPunch(float* angles) {
	if (!angles || !Config::remove_recoil)
		return;

	// Prefer raw input view — unpunched user aim (same source AF/RCS read).
	QAngle_t raw{};
	const bool haveRaw = AimCommon::GetViewAngles(raw) && raw.IsValid();

	C_CSPlayerPawn* lp = H::SafeLocalPlayer();
	QAngle_t punch{};
	bool havePunch = false;
	if (lp && Mem::ValidEntity(lp)) {
		// No shotsFired gate — residual punch after spray still kicks camera.
		if (HitChance::ReadAimPunch(lp, punch) && punch.IsValid())
			havePunch = true;
		else if (AimCommon::ReadAimPunch(lp, punch) && punch.IsValid())
			havePunch = true;
	}

	if (haveRaw) {
		// Force camera = raw input. Handles both:
		//  - setup already punched (old path: setup ≈ raw+punch)
		//  - setup == raw already (no-op write)
		//  - partial punch / thirdperson / prediction hitch
		angles[0] = std::clamp(raw.x, -89.f, 89.f);
		angles[1] = raw.y;
		angles[2] = 0.f;
		while (angles[1] > 180.f) angles[1] -= 360.f;
		while (angles[1] < -180.f) angles[1] += 360.f;
		return;
	}

	// Fallback when input view missing: subtract GetRemovedAimPunch from setup.
	if (!havePunch)
		return;
	const float px = std::clamp(punch.x, -12.f, 12.f);
	const float py = std::clamp(punch.y, -12.f, 12.f);
	angles[0] -= px;
	angles[1] -= py;
	if (angles[0] > 89.f) angles[0] = 89.f;
	if (angles[0] < -89.f) angles[0] = -89.f;
	while (angles[1] > 180.f) angles[1] -= 360.f;
	while (angles[1] < -180.f) angles[1] += 360.f;
	angles[2] = 0.f;
}

float H::hkGetRenderFov(void* rcx) {
	auto original = H::GetRenderFov.GetOriginal();
	float base = Config::fov;
	if (original) {
		__try { base = original(rcx); }
		__except (EXCEPTION_EXECUTE_HANDLER) { base = Config::fov; }
	}

	// Scope zoom FOV wins while zoomed (zoom1 / zoom2)
	float zoomFov = 0.f;
	if (Scope::GetCustomZoomFov(zoomFov)) {
		g_flActiveFov = zoomFov;
		return g_flActiveFov;
	}

	if (Config::fovEnabled) {
		g_flActiveFov = Config::fov;
		return g_flActiveFov;
	}

	g_flActiveFov = base;
	return g_flActiveFov;
}

// Kept for dump parity; does not stretch. Real path is OverrideView below.
float __fastcall H::hkGetScreenAspectRatio(void* thisptr, int width, int height) {
	if (Config::aspect_ratio_enabled && Config::aspect_ratio > 0.f)
		return Config::aspect_ratio;

	auto original = H::GetScreenAspectRatio.GetOriginal();
	if (!original) {
		if (height)
			return (float)width / (float)height;
		return 1.f;
	}

	float r = 1.f;
	__try { r = original(thisptr, width, height); }
	__except (EXCEPTION_EXECUTE_HANDLER) {
		if (height)
			r = (float)width / (float)height;
	}
	return r;
}

// CalcViewModel / GetViewModelOffsets — pattern CALCVIEWMODEL
void __fastcall H::hkGetViewModelOffsets(void* viewmodel, float* offsets, float* fov) {
	auto original = H::GetViewModelOffsets.GetOriginal();
	if (original) {
		__try { original(viewmodel, offsets, fov); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	}

	if (!offsets || !fov)
		return;

	// Shove viewmodel off-screen while scoped (backup if VData hide fails)
	if (Config::scope_hide_viewmodel) {
		bool scoped = false;
		__try {
			C_CSPlayerPawn* local = H::SafeLocalPlayer();
			if (local)
				scoped = local->m_bIsScoped();
		} __except (EXCEPTION_EXECUTE_HANDLER) { scoped = false; }
		if (scoped) {
			__try {
				offsets[0] = 0.f;
				offsets[1] = 0.f;
				offsets[2] = -64.f; // below camera
				*fov = 1.f;
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			return;
		}
	}

	if (!Config::viewmodel_changer)
		return;

	__try {
		offsets[0] = Config::viewmodel_x;
		offsets[1] = Config::viewmodel_y;
		offsets[2] = Config::viewmodel_z;
		*fov = Config::viewmodel_fov;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// OverrideView — third person + aspect stretch + visual no-recoil (CViewSetup)
void __fastcall H::hkOverrideView(void* rcx, void* setup) {
	auto original = H::OverrideView.GetOriginal();
	if (original) {
		__try { original(rcx, setup); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	}

	if (!setup)
		return;

	__try {
		auto* base = reinterpret_cast<std::uint8_t*>(setup);
		float* angles = reinterpret_cast<float*>(base + kViewAngles);

		// Visual recoil remove — AFTER original fills punch into view angles.
		// Fire/seed still read IDA GetRemovedAimPunch (0x18088BBB0) untouched.
		if (Config::remove_recoil)
			StripViewAimPunch(angles);

		// Projection path (sub_180AF7810): if (flags&2) aspect=*(float*)+0x4D4 else w/h.
		// Write AFTER original so game fill does not stomp.
		if (Config::aspect_ratio_enabled && Config::aspect_ratio > 0.01f) {
			*reinterpret_cast<float*>(base + kViewAspect) = Config::aspect_ratio;
			base[kViewAspectFlags] |= 2;
		} else {
			base[kViewAspectFlags] &= static_cast<std::uint8_t>(~2);
		}

		// Scope zoom FOV into CViewSetup (projection) — matches GetRenderFov
		float zoomFov = 0.f;
		if (Scope::GetCustomZoomFov(zoomFov))
			*reinterpret_cast<float*>(base + kViewFov) = zoomFov;

		if (!Config::thirdperson || !keybind.isActive(Config::thirdperson))
			return;

		float* origin = reinterpret_cast<float*>(base + kViewOrigin);

		const float pitch = angles[0] * (float)(M_PI / 180.0);
		const float yaw = angles[1] * (float)(M_PI / 180.0);
		const float cp = std::cos(pitch);
		const float sp = std::sin(pitch);
		const float cy = std::cos(yaw);
		const float sy = std::sin(yaw);

		const float fwdX = cp * cy;
		const float fwdY = cp * sy;
		const float fwdZ = -sp;

		const float dist = Config::thirdperson_distance;
		origin[0] -= fwdX * dist;
		origin[1] -= fwdY * dist;
		origin[2] -= fwdZ * dist;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}
