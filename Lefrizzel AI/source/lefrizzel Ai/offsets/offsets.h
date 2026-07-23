#pragma once
#include <cstddef>
#include <cstdint>

#include "../utils/fnv1a/fnv1a.h"
#include "../utils/schema/schema.h"

// Runtime offsets: SchemaSystem first, dump constants only as fallback.
// Call after Schema::init — before init returns fallbacks (safe if non-zero).
// Globals like dwLocalPlayerPawn are NOT schema fields (need patterns) and are
// unused here; kept only as a dump reference.
namespace Offset {

// Last-known dump values (client_dll.hpp / offsets.json). Never preferred over schema.
namespace FB {
	constexpr std::uint32_t m_iHealth              = 0x34C;
	constexpr std::uint32_t m_iTeamNum             = 0x3E7;
	constexpr std::uint32_t m_pGameSceneNode       = 0x330;
	constexpr std::uint32_t m_vecViewOffset        = 0xE78;
	constexpr std::uint32_t m_clrRender            = 0xC98;
	constexpr std::uint32_t m_nRenderMode          = 0xC78;
	constexpr std::uint32_t m_vOldOrigin           = 0x13B8;
	constexpr std::uint32_t m_vecAbsOrigin         = 0xC8;   // CGameSceneNode
	constexpr std::uint32_t m_modelState           = 0x140;  // CSkeletonInstance
	constexpr std::uint32_t m_ModelName            = 0xA8;   // CModelState (CUtlSymbolLarge)
	constexpr std::uint32_t m_pAimPunchServices    = 0x14B8;
	constexpr std::uint32_t m_iShotsFired          = 0x1C84;
	constexpr std::uint32_t m_predictableBaseAngle = 0x50;
	constexpr std::uint32_t m_predictableBaseAngleVel = 0x5C;
	constexpr std::uint32_t m_unpredictableBaseAngle = 0xA4;
	constexpr std::uint32_t m_flLastSpawnTimeIndex = 0x1404;

	// Global RVA — NOT a schema field (pattern/scanner only). Dump ref only.
	constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x23A4238;
}

// Schema if present; otherwise dump fallback. Does not sticky-cache 0 before init.
[[nodiscard]] inline std::uint32_t Sch(const char* field, std::uint32_t fallback)
{
	const std::uint32_t o = SchemaFinder::Get(hash_32_fnv1a_const(field));
	return o ? o : fallback;
}

// Cached: schema hit locks permanently. Pre-init miss returns fallback without
// locking so the next call after Schema::init can upgrade. Post-init miss locks fb.
[[nodiscard]] inline std::uint32_t SchCached(
	const char* field,
	std::uint32_t fallback,
	std::uint32_t& cache)
{
	if (cache)
		return cache;
	const std::uint32_t o = SchemaFinder::Get(hash_32_fnv1a_const(field));
	if (o) {
		cache = o;
		return o;
	}
	if (SchemaFinder::Ready()) {
		// Real miss after dump — stick to dump FB so we don't re-hash every frame
		if (fallback)
			cache = fallback;
		return fallback;
	}
	// Schema not ready yet — temporary FB, no sticky cache
	return fallback;
}

// ---- Entity / scene (schema names) ----
[[nodiscard]] inline std::uint32_t m_iHealth()
{
	static std::uint32_t c = 0;
	return SchCached("C_BaseEntity->m_iHealth", FB::m_iHealth, c);
}
[[nodiscard]] inline std::uint32_t m_iTeamNum()
{
	static std::uint32_t c = 0;
	return SchCached("C_BaseEntity->m_iTeamNum", FB::m_iTeamNum, c);
}
[[nodiscard]] inline std::uint32_t m_pGameSceneNode()
{
	static std::uint32_t c = 0;
	return SchCached("C_BaseEntity->m_pGameSceneNode", FB::m_pGameSceneNode, c);
}
[[nodiscard]] inline std::uint32_t m_vecViewOffset()
{
	static std::uint32_t c = 0;
	return SchCached("C_BaseModelEntity->m_vecViewOffset", FB::m_vecViewOffset, c);
}
[[nodiscard]] inline std::uint32_t m_clrRender()
{
	static std::uint32_t c = 0;
	return SchCached("C_BaseModelEntity->m_clrRender", FB::m_clrRender, c);
}
[[nodiscard]] inline std::uint32_t m_nRenderMode()
{
	static std::uint32_t c = 0;
	return SchCached("C_BaseModelEntity->m_nRenderMode", FB::m_nRenderMode, c);
}
[[nodiscard]] inline std::uint32_t m_vOldOrigin()
{
	static std::uint32_t c = 0;
	return SchCached("C_BasePlayerPawn->m_vOldOrigin", FB::m_vOldOrigin, c);
}
[[nodiscard]] inline std::uint32_t m_vecAbsOrigin()
{
	static std::uint32_t c = 0;
	return SchCached("CGameSceneNode->m_vecAbsOrigin", FB::m_vecAbsOrigin, c);
}
[[nodiscard]] inline std::uint32_t m_modelState()
{
	static std::uint32_t c = 0;
	return SchCached("CSkeletonInstance->m_modelState", FB::m_modelState, c);
}
[[nodiscard]] inline std::uint32_t m_ModelName()
{
	// Relative to CModelState start (embedded in skeleton)
	static std::uint32_t c = 0;
	return SchCached("CModelState->m_ModelName", FB::m_ModelName, c);
}
[[nodiscard]] inline std::uint32_t m_pAimPunchServices()
{
	static std::uint32_t c = 0;
	return SchCached("C_CSPlayerPawn->m_pAimPunchServices", FB::m_pAimPunchServices, c);
}
[[nodiscard]] inline std::uint32_t m_iShotsFired()
{
	static std::uint32_t c = 0;
	return SchCached("C_CSPlayerPawn->m_iShotsFired", FB::m_iShotsFired, c);
}
[[nodiscard]] inline std::uint32_t m_predictableBaseAngle()
{
	static std::uint32_t c = 0;
	return SchCached(
		"CCSPlayer_AimPunchServices->m_predictableBaseAngle",
		FB::m_predictableBaseAngle, c);
}
[[nodiscard]] inline std::uint32_t m_predictableBaseAngleVel()
{
	static std::uint32_t c = 0;
	return SchCached(
		"CCSPlayer_AimPunchServices->m_predictableBaseAngleVel",
		FB::m_predictableBaseAngleVel, c);
}
[[nodiscard]] inline std::uint32_t m_unpredictableBaseAngle()
{
	static std::uint32_t c = 0;
	return SchCached(
		"CCSPlayer_AimPunchServices->m_unpredictableBaseAngle",
		FB::m_unpredictableBaseAngle, c);
}
[[nodiscard]] inline std::uint32_t m_flLastSpawnTimeIndex()
{
	static std::uint32_t c = 0;
	return SchCached(
		"C_CSPlayerPawnBase->m_flLastSpawnTimeIndex",
		FB::m_flLastSpawnTimeIndex, c);
}

// Skeleton: modelState + ModelName (path string pointer)
[[nodiscard]] inline std::uint32_t SkeletonModelNameOff()
{
	return m_modelState() + m_ModelName();
}

// Backward-compat nested namespaces (constexpr dump values).
// Prefer Offset::m_*() above — these exist so older call sites still compile.
namespace C_BaseEntity {
	constexpr std::ptrdiff_t m_iHealth = FB::m_iHealth;
	constexpr std::ptrdiff_t m_iTeamNum = FB::m_iTeamNum;
	constexpr std::ptrdiff_t m_pGameSceneNode = FB::m_pGameSceneNode;
}
namespace C_BaseModelEntity {
	constexpr std::ptrdiff_t m_vecViewOffset = FB::m_vecViewOffset;
	constexpr std::ptrdiff_t m_clrRender = FB::m_clrRender;
	constexpr std::ptrdiff_t m_nRenderMode = FB::m_nRenderMode;
}
namespace C_BasePlayerPawn {
	constexpr std::ptrdiff_t m_vOldOrigin = FB::m_vOldOrigin;
}
namespace CSkeletonInstance {
	constexpr std::ptrdiff_t m_modelState = FB::m_modelState;
}
namespace C_CSPlayerPawn {
	constexpr std::ptrdiff_t m_pAimPunchServices = FB::m_pAimPunchServices;
	constexpr std::ptrdiff_t m_iShotsFired = FB::m_iShotsFired;
}
namespace CCSPlayer_AimPunchServices {
	constexpr std::ptrdiff_t m_predictableBaseAngle = FB::m_predictableBaseAngle;
	constexpr std::ptrdiff_t m_predictableBaseAngleVel = FB::m_predictableBaseAngleVel;
	constexpr std::ptrdiff_t m_unpredictableBaseAngle = FB::m_unpredictableBaseAngle;
}

// Deprecated dump global (unused by live paths — interfaces/patterns supply local pawn).
constexpr std::ptrdiff_t dwLocalPlayerPawn = FB::dwLocalPlayerPawn;

} // namespace Offset
