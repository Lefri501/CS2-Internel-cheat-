#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../keybinds/keybinds.h"

#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// CViewSetup layout (this client.dll build — IDA OverrideView @ 0x180C983D0)
constexpr std::uintptr_t kViewFov = 0x498;
constexpr std::uintptr_t kViewOrigin = 0x4A0;
constexpr std::uintptr_t kViewAngles = 0x4B8;

float H::hkGetRenderFov(void* rcx) {
	auto original = H::GetRenderFov.GetOriginal();
	if (!original) return Config::fov;

	if (Config::fovEnabled) {
		g_flActiveFov = Config::fov;
	} else {
		g_flActiveFov = original(rcx);
	}

	return g_flActiveFov;
}

// CalcViewModel / GetViewModelOffsets — pattern CALCVIEWMODEL
void __fastcall H::hkGetViewModelOffsets(void* viewmodel, float* offsets, float* fov) {
	auto original = H::GetViewModelOffsets.GetOriginal();
	if (original) {
		__try { original(viewmodel, offsets, fov); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	}

	if (!Config::viewmodel_changer || !offsets || !fov)
		return;

	__try {
		offsets[0] = Config::viewmodel_x;
		offsets[1] = Config::viewmodel_y;
		offsets[2] = Config::viewmodel_z;
		*fov = Config::viewmodel_fov;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// OverrideView — third person via camera origin pullback
void __fastcall H::hkOverrideView(void* rcx, void* setup) {
	auto original = H::OverrideView.GetOriginal();
	if (original) {
		__try { original(rcx, setup); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	}

	if (!setup || !Config::thirdperson || !keybind.isActive(Config::thirdperson))
		return;

	__try {
		auto* base = reinterpret_cast<std::uint8_t*>(setup);
		float* origin = reinterpret_cast<float*>(base + kViewOrigin);
		float* angles = reinterpret_cast<float*>(base + kViewAngles);

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
		(void)kViewFov;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}
