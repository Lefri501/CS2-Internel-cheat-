#pragma once
#include "includeHooks.h"
#include "../../cs2/entity/C_AggregateSceneObject/C_AggregateSceneObject.h"
#include "../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../cs2/datatypes/cutlbuffer/cutlbuffer.h"
#include "../../cs2/datatypes/keyvalues/keyvalues.h"
#include "../../cs2/entity/C_Material/C_Material.h"
#include "../interfaces/CCSGOInput/CCSGOInput.h"

// Forward declaration
class CMeshData;
class CEntityIdentity;

namespace H {
	void __fastcall hkFrameStageNotify(void* a1, int stage);
	void* __fastcall hkLevelInit(__int64 a1, __int64 a2);
	// scenesystem DrawArray / CAnimatableSceneObjectDescRender (7 args, IDA 0x1800545D0)
	// IDA returns _UNKNOWN** — MUST propagate or caller AVs after inject.
	void* __fastcall hkChamsObject(void* pAnimatableSceneObjectDesc, void* pDx11, CMeshData* arrMeshDraw, int nDataCount, void* pSceneView, void* pSceneLayer, void* pUnk);
	void __fastcall hkDrawSkyboxArray(__int64 a1, __int64 a2, __int64 a3, int a4, int a5, __int64 a6, __int64 a7);
	std::int64_t __fastcall hkDrawAggregateSceneObjectArray(void* a1, void* a2, void* pAggregateArr);
	// GeneratePrimitives — builds mesh draw list; used to tint all world props
	void __fastcall hkGeneratePrimitives(void* a1, void* sceneObj, void* a3, void* drawList);
	void* __fastcall hkLightSceneObject(void* ptr, void* pLightScene, void* unk);
	void* __fastcall hkGlobalLightUpdate(void* pThis);
	// IDA FlashOverlay @ 0x18113C960 — "FlashbangOverlay" material path
	void __fastcall hkRenderFlashbangOverlay(void* a1, int split, void** matSys, void* a4, void* a5);
	void __fastcall hkDrawLegs(void* a1, void* a2, void* a3, void* a4, void* a5);
	std::int64_t __fastcall hkDrawSmokeVertex(void* a1, void* a2, int a3, int a4, void* a5, void* a6);
	// IDA DrawSmokeArray @ 0x180CB4190 — SmokeConstantBuffer batch (remove + color path sibling)
	std::int64_t __fastcall hkDrawSmokeArray(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6);
	void* __fastcall hkRenderDecals(void* a1, void* a2, char a3, char a4);
	// IDA CacheParticleEffect @ 0x18078EE10 — real spawn gate (old CreateParticleEffect was SetCP)
	void* __fastcall hkCacheParticleEffect(void* mgr, unsigned int* outIndex, const char* name,
		int attach, void* entity, void* a6, void* a7, int a8);
	// particles.dll ParticleDrawArray (UC Particle Modulation) — RGB floats @ a2+0x50
	// IDA sub_1802826D0 — pattern 48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55
	void* __fastcall hkParticleDrawArray(void* a1, void* a2, void* a3, void* a4, void* a5);
	// scenesystem ToneMapUpdate @ 0x1801874F0 — GPU exposure outputs @ +136/+140
	float* __fastcall hkToneMapUpdate(void* tonemapState);
	// scenesystem UpdateLightObject @ 0x180199590 — light RGB @ +0xE4
	// (old LightSceneObject short sig was this same fn — do not double-hook)
	void __fastcall hkUpdateLightObject(void* sceneSys, void* lightObj, void* a3);
	// IDA CCSGOInput::CreateMove 0x180B09520 — UC pattern after AG2
	// (input*, slot, active). NOT the old Source1 float-sample signature.
	void __fastcall hkCreateMove(void* pInput, int slot, bool active);
	void* __fastcall hkDrawGlow(void* glowProp);
	// IDA sub_180B499F0 — colour_override → float4 consumed by ManageGlowSceneObject
	void __fastcall hkGetGlowColor(void* glowProp, float* outRgba);
	// IDA sub_180B1B2B0 — force pure glow colour4 + backface mult = 1 (no mesh/chams tint)
	void* __fastcall hkManageGlowSceneObject(void** glowSceneOut, void* a2, void* sceneNode,
		float* color4, int a5, int a6, int a7, int a8);
	// IDA sub_180B04B30 — full glow apply (pre-fill → GetGlowColor → ManageGlow)
	std::int64_t __fastcall hkApplyGlowScene(void* glowProp, void* sceneNode);
	bool __fastcall hkFireEventClientSide(void* eventManager, void* gameEvent);
	std::uintptr_t __fastcall hkSetupMapInfo(std::uintptr_t mapInfo, void* unk);
	inline float g_flActiveFov;
	float hkGetRenderFov(void* rcx);
	void __fastcall hkGetViewModelOffsets(void* viewmodel, float* offsets, float* fov);
	void __fastcall hkOverrideView(void* rcx, void* setup);
	// engine2 IVEngineClient::GetScreenAspectRatio — float(this, width, height)
	float __fastcall hkGetScreenAspectRatio(void* thisptr, int width, int height);
	std::int64_t __fastcall hkDrawScopeOverlay(void* a1, void* a2);
	void* __fastcall hkGetMatrixForView(void* a1, void* a2, void* a3, void* a4, void* worldToProjection, void* a6);
	bool __fastcall hkDrawCrosshair(void* a1);
	// IDA sub_1807F5520 — sniper zoom gate; HUD does show = !this before ever calling DrawCrosshair
	bool __fastcall hkWeaponHidesCrosshair(void* zoomData);
	// IDA 0x180F5BBF0 — no meaningful args; hook signature kept for CInlineHookObj
	bool __fastcall hkShouldShowHudElements(void* a1);
	// Dead: dump UpdatePostProcessing was dem-file path. Post process via ConVar tick.
	void __fastcall hkUpdatePostProcessing(void* a1, void* a2);

	// Menu input blocking hooks (copied exactly from Andromeda-CS2-Base logic) - declare early
	void __fastcall hkIsRelativeMouseMode(void* pInputSystem, bool active);
	bool __fastcall hkMouseInputEnabled(void* rcx);

	using IsRelativeMouseModeFn = void(__fastcall*)(void*, bool);
	using MouseInputEnabledFn = bool(__fastcall*)(void*);

	inline CInlineHookObj<IsRelativeMouseModeFn> IsRelativeMouseMode = { };
	inline CInlineHookObj<MouseInputEnabledFn> MouseInputEnabled = { };
	inline void* g_pInputSystem = nullptr;
	inline bool g_wantRelativeMouse = true;

	inline CInlineHookObj<decltype(&hkChamsObject)> DrawArray = { };
	inline CInlineHookObj<decltype(&hkDrawSkyboxArray)> DrawSkyboxArray = { };
	inline CInlineHookObj<decltype(&hkDrawAggregateSceneObjectArray)> DrawAggregateSceneObjectArray = { };
	inline CInlineHookObj<decltype(&hkGeneratePrimitives)> GeneratePrimitives = { };
	inline CInlineHookObj<decltype(&hkLightSceneObject)> LightSceneObject = { };
	inline CInlineHookObj<decltype(&hkGlobalLightUpdate)> GlobalLightUpdate = { };
	inline CInlineHookObj<decltype(&hkUpdateLightObject)> UpdateLightObject = { };
	inline CInlineHookObj<decltype(&hkToneMapUpdate)> ToneMapUpdate = { };
	inline CInlineHookObj<decltype(&hkFrameStageNotify)> FrameStageNotify = { };
	inline CInlineHookObj<decltype(&hkGetRenderFov)> GetRenderFov = { };
	inline CInlineHookObj<decltype(&hkGetViewModelOffsets)> GetViewModelOffsets = { };
	inline CInlineHookObj<decltype(&hkOverrideView)> OverrideView = { };
	inline CInlineHookObj<decltype(&hkGetScreenAspectRatio)> GetScreenAspectRatio = { };
	inline CInlineHookObj<decltype(&hkDrawScopeOverlay)> DrawScopeOverlay = { };
	inline CInlineHookObj<decltype(&hkGetMatrixForView)> GetMatrixForView = { };
	inline CInlineHookObj<decltype(&hkDrawCrosshair)> DrawCrosshair = { };
	inline CInlineHookObj<decltype(&hkWeaponHidesCrosshair)> WeaponHidesCrosshair = { };
	inline CInlineHookObj<decltype(&hkShouldShowHudElements)> ShouldShowHudElements = { };
	inline CInlineHookObj<decltype(&hkUpdatePostProcessing)> UpdatePostProcessing = { };
	inline CInlineHookObj<decltype(&hkLevelInit)> LevelInit = { };
	inline CInlineHookObj<decltype(&hkRenderFlashbangOverlay)> RenderFlashBangOverlay = { };
	inline CInlineHookObj<decltype(&hkDrawLegs)> DrawLegs = { };
	inline CInlineHookObj<decltype(&hkDrawSmokeVertex)> DrawSmokeVertex = { };
	inline CInlineHookObj<decltype(&hkDrawSmokeArray)> DrawSmokeArray = { };
	inline CInlineHookObj<decltype(&hkRenderDecals)> RenderDecals = { };
	inline CInlineHookObj<decltype(&hkCacheParticleEffect)> CacheParticleEffect = { };
	inline CInlineHookObj<decltype(&hkParticleDrawArray)> ParticleDrawArray = { };
	inline CInlineHookObj<decltype(&hkCreateMove)> CreateMove = { };
	inline CInlineHookObj<decltype(&hkDrawGlow)> DrawGlow = { };
	inline CInlineHookObj<decltype(&hkGetGlowColor)> GetGlowColor = { };
	inline CInlineHookObj<decltype(&hkManageGlowSceneObject)> ManageGlowSceneObject = { };
	inline CInlineHookObj<decltype(&hkApplyGlowScene)> ApplyGlowScene = { };

	inline CInlineHookObj<decltype(&hkFireEventClientSide)> FireEventClientSide = { };
	inline CInlineHookObj<decltype(&hkSetupMapInfo)> SetupMapInfo = { };

	// Priority A — entity list + map unload
	void __fastcall hkOnAddEntity(void* entitySystem, void* entity, int handle);
	void __fastcall hkOnRemoveEntity(void* entitySystem, void* entity, int handle);
	void* __fastcall hkLevelShutdown(void* a1);
	inline CInlineHookObj<decltype(&hkOnAddEntity)> OnAddEntity = { };
	inline CInlineHookObj<decltype(&hkOnRemoveEntity)> OnRemoveEntity = { };
	inline CInlineHookObj<decltype(&hkLevelShutdown)> LevelShutdown = { };

	// inline hooks / resolved funcs
	inline int  oGetWeaponData;
	inline void* (__fastcall* ogGetBaseEntity)(void*, int);
	// Defined here; assigned in Hooks::init (also referenced by Input::get_user_cmd)
	inline C_CSPlayerPawn* (__fastcall* oGetLocalPlayer)(int) = nullptr;

	// SEH + range/vtable — use instead of bare oGetLocalPlayer on unload-sensitive paths.
	// Impl in hooks.cpp (header stays SEH-free for C2712).
	[[nodiscard]] C_CSPlayerPawn* SafeLocalPlayer() noexcept;
	// Alive + lifeState==0 only (CreateMove / combat / skins). Spec uses SafeLocalPlayer.
	[[nodiscard]] C_CSPlayerPawn* SafeLocalAlive() noexcept;

	class Hooks {
	public:
		void init();
	};
}

extern bool g_bMenuOpen; // for blocking game input when menu is open

