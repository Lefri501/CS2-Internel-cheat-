#include "fog_handler.h"
#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace World {
namespace Fog {
namespace {

// client_dll.hpp C_GradientFog (build dump)
constexpr std::uintptr_t kFogStart      = 0x608;
constexpr std::uintptr_t kFogEnd        = 0x60C;
constexpr std::uintptr_t kFogMaxOpacity = 0x620;
constexpr std::uintptr_t kFogFalloff    = 0x624;
constexpr std::uintptr_t kFogColor      = 0x62C; // Color rgba
constexpr std::uintptr_t kFogStrength   = 0x630;
constexpr std::uintptr_t kFogEnabled    = 0x639; // bool m_bIsEnabled

// CreateEntityByClassName — IDA 0x1814E9A40
// void* fn(CGameEntitySystem*, int slot, const char* name, int, int, int, char)
using FnCreateEntity = void* (__fastcall*)(
	void* es, int slot, const char* name, int a4, int a5, int a6, char a7);

FnCreateEntity g_createEntity = nullptr;
void* g_fogEnt = nullptr;
float g_lastStart = -1.f, g_lastEnd = -1.f, g_lastFall = -1.f;
float g_lastCol[4] = { -1.f, -1.f, -1.f, -1.f };
bool g_ready = false;

uint8_t ToU8(float v) {
	v = std::clamp(v, 0.f, 1.f);
	return static_cast<uint8_t>(v * 255.f + 0.5f);
}

void EnsureFns() {
	if (g_ready)
		return;
	g_ready = true;
	// Unique-ish prologue for CreateEntityByClassName
	const uintptr_t addr = M::patternScan("client",
		"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC ? 49 8B F8 44 8B F2");
	if (addr)
		g_createEntity = reinterpret_cast<FnCreateEntity>(addr);
	if (!g_createEntity)
		Con::OffsetMiss("Fog CreateEntityByClassName");
	else
		Con::Ok("Fog CreateEntityByClassName @ 0x%p", (void*)addr);
}

void* CreateFogEntity() {
	EnsureFns();
	if (!g_createEntity || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	void* ent = nullptr;
	__try {
		ent = g_createEntity(I::GameEntity->Instance, -1, "env_gradient_fog", 0, -1, -1, 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ent = nullptr;
	}
	return ent;
}

void ApplyFields(void* ent, float start, float end, float falloff, const float col[4]) {
	if (!ent || !Mem::Valid(ent, kFogEnabled + 1))
		return;
	__try {
		*reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ent) + kFogStart) = start;
		*reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ent) + kFogEnd) = end;
		*reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ent) + kFogFalloff) = falloff;
		*reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ent) + kFogStrength) = 1.f;
		*reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ent) + kFogMaxOpacity) = col[3];
		auto* c = reinterpret_cast<std::uint8_t*>(ent) + kFogColor;
		c[0] = ToU8(col[0]);
		c[1] = ToU8(col[1]);
		c[2] = ToU8(col[2]);
		c[3] = ToU8(col[3]);
		*reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(ent) + kFogEnabled) = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void DisableFields(void* ent) {
	if (!ent || !Mem::Valid(ent, kFogEnabled + 1))
		return;
	__try {
		*reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(ent) + kFogEnabled) = false;
		*reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(ent) + kFogStrength) = 0.f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

} // namespace

void Update() {
	if (Config::loading.load(std::memory_order_acquire))
		return;

	if (!Config::custom_fog) {
		if (g_fogEnt) {
			DisableFields(g_fogEnt);
			g_fogEnt = nullptr;
			g_lastStart = -1.f;
		}
		return;
	}

	const float start = Config::custom_fog_start;
	const float end = Config::custom_fog_end;
	const float fall = Config::custom_fog_falloff;
	const float col[4] = {
		Config::custom_fog_color.x, Config::custom_fog_color.y,
		Config::custom_fog_color.z, Config::custom_fog_color.w
	};

	const bool changed =
		g_lastStart != start || g_lastEnd != end || g_lastFall != fall
		|| g_lastCol[0] != col[0] || g_lastCol[1] != col[1]
		|| g_lastCol[2] != col[2] || g_lastCol[3] != col[3];

	// Param change: disable old, re-create (celerity remove_fog)
	if (changed && g_fogEnt) {
		DisableFields(g_fogEnt);
		g_fogEnt = nullptr;
	}

	if (!g_fogEnt) {
		g_fogEnt = CreateFogEntity();
		if (!g_fogEnt)
			return;
		// New entity — must write fields
		ApplyFields(g_fogEnt, start, end, fall, col);
		g_lastStart = start;
		g_lastEnd = end;
		g_lastFall = fall;
		g_lastCol[0] = col[0]; g_lastCol[1] = col[1];
		g_lastCol[2] = col[2]; g_lastCol[3] = col[3];
		return;
	}

	// Params stable + entity live — skip rewrite (was every FSN frame)
	if (!changed)
		return;

	ApplyFields(g_fogEnt, start, end, fall, col);
	g_lastStart = start;
	g_lastEnd = end;
	g_lastFall = fall;
	g_lastCol[0] = col[0]; g_lastCol[1] = col[1];
	g_lastCol[2] = col[2]; g_lastCol[3] = col[3];
}

void Shutdown() {
	if (g_fogEnt) {
		DisableFields(g_fogEnt);
		g_fogEnt = nullptr;
	}
	g_lastStart = -1.f;
}

} // namespace Fog
} // namespace World
