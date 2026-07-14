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
	void __fastcall hkChamsObject(void* pAnimatableSceneObjectDesc, void* pDx11, CMeshData* arrMeshDraw, int nDataCount, void* pSceneView, void* pSceneLayer, void* pUnk, void* pUnk2);
	void __fastcall hkDrawSkyboxArray(__int64 a1, __int64 a2, __int64 a3, int a4, int a5, __int64 a6, __int64 a7);
	std::int64_t __fastcall hkDrawAggregateSceneObjectArray(void* a1, void* a2, void* pAggregateArr);
	void* __fastcall hkLightSceneObject(void* ptr, void* pLightScene, void* unk);
	void* __fastcall hkGlobalLightUpdate(void* pThis);
	void __fastcall hkRenderFlashbangOverlay(void* a1, void* a2, void* a3, void* a4, void* a5);
	void __fastcall hkDrawLegs(void* a1, void* a2, void* a3, void* a4, void* a5);
	std::int64_t __fastcall hkDrawSmokeVertex(void* a1, void* a2, int a3, int a4, void* a5, void* a6);
	void* __fastcall hkRenderDecals(void* a1, void* a2, char a3, char a4);
	// Misnamed: pattern is SetParticleControl TRANSFORM (IDA sub_1809C50F0).
	// Signature MUST keep full 64-bit mgr pointer — int a1 truncated this and crashed on shoot FX.
	std::int64_t __fastcall hkCreateParticleEffect(void* mgr, int effectIndex, unsigned int cp, void* transform, float time);
	void __fastcall hkCreateMove(void* rcx, int slot, float flInputSampleTime, bool active);
	void* __fastcall hkDrawGlow(void* glowProp);
	bool __fastcall hkFireEventClientSide(void* eventManager, void* gameEvent);
	std::uintptr_t __fastcall hkSetupMapInfo(std::uintptr_t mapInfo, void* unk);
	inline float g_flActiveFov;
	float hkGetRenderFov(void* rcx);
	void __fastcall hkGetViewModelOffsets(void* viewmodel, float* offsets, float* fov);
	void __fastcall hkOverrideView(void* rcx, void* setup);
	std::int64_t __fastcall hkDrawScopeOverlay(void* a1, void* a2);

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
	inline CInlineHookObj<decltype(&hkLightSceneObject)> LightSceneObject = { };
	inline CInlineHookObj<decltype(&hkGlobalLightUpdate)> GlobalLightUpdate = { };
	inline CInlineHookObj<decltype(&hkFrameStageNotify)> FrameStageNotify = { };
	inline CInlineHookObj<decltype(&hkGetRenderFov)> GetRenderFov = { };
	inline CInlineHookObj<decltype(&hkGetViewModelOffsets)> GetViewModelOffsets = { };
	inline CInlineHookObj<decltype(&hkOverrideView)> OverrideView = { };
	inline CInlineHookObj<decltype(&hkDrawScopeOverlay)> DrawScopeOverlay = { };
	inline CInlineHookObj<decltype(&hkLevelInit)> LevelInit = { };
	inline CInlineHookObj<decltype(&hkRenderFlashbangOverlay)> RenderFlashBangOverlay = { };
	inline CInlineHookObj<decltype(&hkDrawLegs)> DrawLegs = { };
	inline CInlineHookObj<decltype(&hkDrawSmokeVertex)> DrawSmokeVertex = { };
	inline CInlineHookObj<decltype(&hkRenderDecals)> RenderDecals = { };
	inline CInlineHookObj<decltype(&hkCreateParticleEffect)> CreateParticleEffect = { };
	inline CInlineHookObj<decltype(&hkCreateMove)> CreateMove = { };
	inline CInlineHookObj<decltype(&hkDrawGlow)> DrawGlow = { };


	inline CInlineHookObj<decltype(&hkFireEventClientSide)> FireEventClientSide = { };
	inline CInlineHookObj<decltype(&hkSetupMapInfo)> SetupMapInfo = { };

	// inline hooks / resolved funcs
	inline int  oGetWeaponData;
	inline void* (__fastcall* ogGetBaseEntity)(void*, int);
	// Defined here; assigned in Hooks::init (also referenced by Input::get_user_cmd)
	inline C_CSPlayerPawn* (__fastcall* oGetLocalPlayer)(int) = nullptr;

	class Hooks {
	public:
		void init();
	};
}

extern bool g_bMenuOpen; // for blocking game input when menu is open

