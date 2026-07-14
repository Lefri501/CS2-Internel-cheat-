#include <algorithm>
#include <iostream>
#include "chams.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../gamemode/gamemode.h"
#include "../world/world.h"
#include "../../utils/console/console.h"
#include "../../utils/security/vacdetect.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../../../external/imgui/imgui.h"
#include "../../utils/math/utlstronghandle/utlstronghandle.h"
#include "../../../cs2/entity/C_Material/C_Material.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../cs2/datatypes/keyvalues/keyvalues.h"
#include "../../../cs2/datatypes/cutlbuffer/cutlbuffer.h"

CStrongHandle<CMaterial2> chams::create(const char* name, const char szVmatBuffer[])
{
	CKeyValues3* pKeyValues3 = nullptr;
	pKeyValues3 = pKeyValues3->create_material_from_resource();
	pKeyValues3->LoadFromBuffer(szVmatBuffer);

	CStrongHandle<CMaterial2> pCustomMaterial = {};
	if (I::CreateMaterial)
		I::CreateMaterial(nullptr, &pCustomMaterial, name, pKeyValues3, 0, 1);
	return pCustomMaterial;
}

struct resource_material_t
{
	CStrongHandle<CMaterial2> mat;
	CStrongHandle<CMaterial2> mat_invs;
};

static resource_material_t resourceMaterials[ChamsType::MAXCOUNT]{};
static bool g_materialsReady = false;

// Visible mats: Z on. XQZ mats: full ignore-Z + no Z-write (draw through world at any range).
bool chams::Materials::init()
{
	// flat
	resourceMaterials[FLAT] = resource_material_t{
		.mat = create("materials/dev/flat.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_IGNOREZ = 0
            F_DISABLE_Z_WRITE = 0
            F_DISABLE_Z_BUFFERING = 0
            F_BLEND_MODE = 1
            F_TRANSLUCENT = 1
            F_RENDER_BACKFACES = 0
            g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
            g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
            g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
            g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
            g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
        })"),
		.mat_invs = create("materials/dev/flat_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_IGNOREZ = 1
            F_DISABLE_Z_WRITE = 1
            F_DISABLE_Z_BUFFERING = 1
            F_BLEND_MODE = 1
            F_TRANSLUCENT = 1
            F_RENDER_BACKFACES = 1
            g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
            g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
            g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
            g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
            g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
        })")
	};
	resourceMaterials[ILLUMINATE] = resource_material_t{
		.mat = create("materials/dev/primary_white.vmat",  R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
	g_flSelfIllumBrightness = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
    g_vSelfIllumTint = [ 10.000000, 10.000000, 10.000000, 10.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.mat_invs = create("materials/dev/primary_white_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_RENDER_BACKFACES = 1
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
	g_flSelfIllumBrightness = [ 3.000000, 3.000000, 3.000000, 3.000000 ]
	g_vSelfIllumTint = [ 10.000000, 10.000000, 10.000000, 10.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})")
	};

	resourceMaterials[GLOW] = resource_material_t{
		.mat = create("materials/dev/tw_glow.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	g_flFresnelExponent = 7.0
	g_flFresnelFalloff = 10.0
	g_flFresnelMax = 0.1
	g_flFresnelMin = 1.0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_flToolsVisCubemapReflectionRoughness = 1.0
	g_flBeginMixingRoughness = 1.0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 0 ]
	F_IGNOREZ = 0
	F_TRANSLUCENT = 1
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	F_RENDER_BACKFACES = 0
})"),
		.mat_invs = create("materials/dev/tw_glow_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	g_flFresnelExponent = 7.0
	g_flFresnelFalloff = 10.0
	g_flFresnelMax = 0.1
	g_flFresnelMin = 1.0
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tSceneDepth = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_flToolsVisCubemapReflectionRoughness = 1.0
	g_flBeginMixingRoughness = 1.0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	F_IGNOREZ = 1
	F_TRANSLUCENT = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_RENDER_BACKFACES = 1
})")
	};

	resourceMaterials[GHOST] = resource_material_t{
		.mat = create("materials/dev/tw_ghost.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_IGNOREZ = 0
            F_DISABLE_Z_WRITE = 1
            F_DISABLE_Z_BUFFERING = 0
            F_BLEND_MODE = 1
            F_TRANSLUCENT = 1
            F_RENDER_BACKFACES = 1
            g_vColorTint = [1.000000, 1.000000, 1.000000, 0.200000]
            g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
            g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
            g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
            g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
        })"),
		.mat_invs = create("materials/dev/tw_ghost_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_IGNOREZ = 1
            F_DISABLE_Z_WRITE = 1
            F_DISABLE_Z_BUFFERING = 1
            F_BLEND_MODE = 1
            F_TRANSLUCENT = 1
            F_RENDER_BACKFACES = 1
            g_vColorTint = [1.000000, 1.000000, 1.000000, 0.200000]
            g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
            g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
            g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
            g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
        })")
	};

	resourceMaterials[LATEX] = resource_material_t{
		.mat = create("materials/dev/tw_latex.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_SPECULAR = 1
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 1.500000, 1.500000, 1.500000, 1.500000 ]
	g_flSelfIllumBrightness = [ 1.200000, 1.200000, 1.200000, 1.200000 ]
	g_vSelfIllumTint = [ 2.000000, 2.000000, 2.000000, 2.000000 ]
	g_flSpecularIntensity = 2.500000
	g_flSpecularExponent = 32.000000
	g_vGlossinessRange = [ 0.750000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.mat_invs = create("materials/dev/tw_latex_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_SPECULAR = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_RENDER_BACKFACES = 1
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 1.500000, 1.500000, 1.500000, 1.500000 ]
	g_flSelfIllumBrightness = [ 1.200000, 1.200000, 1.200000, 1.200000 ]
	g_vSelfIllumTint = [ 2.000000, 2.000000, 2.000000, 2.000000 ]
	g_flSpecularIntensity = 2.500000
	g_flSpecularExponent = 32.000000
	g_vGlossinessRange = [ 0.750000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})")
	};

	// Require at least flat XQZ material — fallback path uses FLAT
	g_materialsReady = static_cast<CMaterial2*>(resourceMaterials[FLAT].mat_invs) != nullptr
		|| static_cast<CMaterial2*>(resourceMaterials[FLAT].mat) != nullptr;

	if (g_materialsReady)
		Con::Ok("Chams materials ready");
	else
		Con::Error("Chams materials failed — CreateMaterial miss?");
	return g_materialsReady;
}

static CMaterial2* GetMaterial(int type, bool invisible)
{
	if (!g_materialsReady)
		return nullptr;
	if (type < 0 || type >= ChamsType::MAXCOUNT)
		type = FLAT;
	CMaterial2* mat = invisible
		? static_cast<CMaterial2*>(resourceMaterials[type].mat_invs)
		: static_cast<CMaterial2*>(resourceMaterials[type].mat);
	// Fallback to flat if selected type failed to create
	if (!mat) {
		mat = invisible
			? static_cast<CMaterial2*>(resourceMaterials[FLAT].mat_invs)
			: static_cast<CMaterial2*>(resourceMaterials[FLAT].mat);
	}
	return mat;
}

// Safe mesh write — death frames free entities mid-draw; never walk bad strides.
static bool MeshWritable(CMeshData* mesh) noexcept
{
	return mesh && Mem::IsReadable(mesh, 0x58);
}

// SEH helpers must be noinline, no C++ objects with dtors (MSVC C2712).
__declspec(noinline) static bool SehSetMaterial(CMeshData* mesh, CMaterial2* mat)
{
	if (!mesh || !mat) return false;
	__try { mesh->pMaterial = mat; return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SehWriteTint(CMeshData* mesh, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (!mesh) return false;
	__try {
		mesh->color.r = r;
		mesh->color.g = g;
		mesh->color.b = b;
		mesh->color.a = a;
		const float s = mesh->alphaScale;
		if (s >= 0.f && s <= 8.f)
			mesh->alphaScale = 1.0f;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static CSceneAnimatableObject* SehSceneObj(CMeshData* mesh)
{
	if (!mesh) return nullptr;
	__try { return mesh->pSceneAnimatableObject; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) static CBaseHandle SehOwner(CSceneAnimatableObject* obj)
{
	CBaseHandle h{};
	if (!obj) return h;
	__try { h = obj->Owner(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return h;
}

__declspec(noinline) static bool SehIsBasePlayer(C_BaseEntity* e)
{
	if (!e) return false;
	__try { return e->IsBasePlayer(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SehIsController(C_BaseEntity* e)
{
	if (!e) return false;
	__try { return e->IsPlayerController(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SehIsHands(C_BaseEntity* e)
{
	if (!e) return false;
	__try { return e->IsViewmodelAttachment(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SehIsViewmodel(C_BaseEntity* e)
{
	if (!e) return false;
	__try { return e->IsViewmodel(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool SehAlivePawn(C_CSPlayerPawn* pawn, int* outHp, uint8_t* outLife)
{
	if (!pawn) return false;
	__try {
		const int hp = pawn->m_iHealth();
		const uint8_t life = pawn->m_lifeState();
		if (outHp) *outHp = hp;
		if (outLife) *outLife = life;
		return hp > 0 && life == 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static CBaseHandle SehHandleField(C_BaseEntity* e, bool ownerEntity)
{
	CBaseHandle h{};
	if (!e) return h;
	__try {
		if (ownerEntity)
			h = e->m_hOwnerEntity();
		else
			h = reinterpret_cast<CCSPlayerController*>(e)->m_hPawn();
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return h;
}

__declspec(noinline) static bool SehTeams(C_CSPlayerPawn* a, C_CSPlayerPawn* b, uint8_t* ta, uint8_t* tb)
{
	if (!a || !b) return false;
	__try {
		*ta = a->m_iTeamNum();
		*tb = b->m_iTeamNum();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static C_CSPlayerPawn* SehLocal()
{
	if (!H::oGetLocalPlayer) return nullptr;
	__try { return H::oGetLocalPlayer(0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void ApplyChamsColor(CMeshData* mesh, int matType, float r, float g, float b, float a, bool xqz)
{
	if (!MeshWritable(mesh))
		return;
	float alpha = std::clamp(a, 0.f, 1.f);
	if (matType == GHOST)
		alpha = std::clamp(alpha * 0.28f, 0.08f, 0.32f);
	else if (xqz)
		alpha = std::clamp(alpha, 0.45f, 1.f);

	SehWriteTint(mesh,
		static_cast<uint8_t>(std::clamp(r, 0.f, 1.f) * 255.0f + 0.5f),
		static_cast<uint8_t>(std::clamp(g, 0.f, 1.f) * 255.0f + 0.5f),
		static_cast<uint8_t>(std::clamp(b, 0.f, 1.f) * 255.0f + 0.5f),
		static_cast<uint8_t>(alpha * 255.0f + 0.5f));
}

// Primary mesh + same-owner siblings only. Large counts → primary only (death FX).
static void ApplyChamsToMeshes(
	CMeshData* base,
	int nMeshCount,
	CMaterial2* mat,
	int matType,
	float r, float g, float b, float a,
	bool xqz)
{
	if (!MeshWritable(base) || !mat || !Mem::IsUserPtr(mat) || nMeshCount < 1)
		return;
	if (!SehSetMaterial(base, mat))
		return;
	ApplyChamsColor(base, matType, r, g, b, a, xqz);

	if (nMeshCount <= 1 || nMeshCount > 12)
		return;

	CSceneAnimatableObject* owner0 = SehSceneObj(base);
	if (!owner0 || !Mem::IsUserPtr(owner0))
		return;

	for (int i = 1; i < nMeshCount; ++i) {
		CMeshData* mesh = base->At(i);
		if (!MeshWritable(mesh))
			break;
		CSceneAnimatableObject* ownerI = SehSceneObj(mesh);
		if (ownerI != owner0)
			break;
		if (!SehSetMaterial(mesh, mat))
			break;
		ApplyChamsColor(mesh, matType, r, g, b, a, xqz);
	}
}

static C_CSPlayerPawn* ResolvePlayerPawn(C_BaseEntity* ent)
{
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;

	if (SehIsBasePlayer(ent)) {
		auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(ent);
		if (!SehAlivePawn(pawn, nullptr, nullptr))
			return nullptr;
		return pawn;
	}

	if (SehIsController(ent)) {
		if (!I::GameEntity || !I::GameEntity->Instance)
			return nullptr;
		const CBaseHandle hPawn = SehHandleField(ent, false);
		if (!hPawn.valid())
			return nullptr;
		auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
		if (!pawn || !Mem::ValidEntity(pawn))
			return nullptr;
		if (!SehAlivePawn(pawn, nullptr, nullptr))
			return nullptr;
		return pawn;
	}

	if (!I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	const CBaseHandle hOwner = SehHandleField(ent, true);
	if (!hOwner.valid())
		return nullptr;
	auto* owner = I::GameEntity->Instance->Get<C_BaseEntity>(hOwner);
	if (!owner || owner == ent || !Mem::ValidEntity(owner))
		return nullptr;
	if (!SehIsBasePlayer(owner))
		return nullptr;
	auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(owner);
	if (!SehAlivePawn(pawn, nullptr, nullptr))
		return nullptr;
	return pawn;
}

ChamsEntity chams::GetTargetType(C_BaseEntity* render_ent) noexcept
{
	if (!render_ent || !Mem::ValidEntity(render_ent) || !H::oGetLocalPlayer)
		return ChamsEntity::INVALID;

	C_CSPlayerPawn* local = SehLocal();
	if (!local || !Mem::ValidEntity(local))
		return ChamsEntity::INVALID;

	if (SehIsHands(render_ent))
		return ChamsEntity::HANDS;
	if (SehIsViewmodel(render_ent))
		return ChamsEntity::VIEWMODEL;

	C_CSPlayerPawn* player = ResolvePlayerPawn(render_ent);
	if (!player || player == local)
		return ChamsEntity::INVALID;

	uint8_t team = 0, localTeam = 0;
	if (!SehTeams(player, local, &team, &localTeam))
		return ChamsEntity::INVALID;

	const bool sameTeam = Mem::ValidTeam(static_cast<int>(team))
		&& Mem::ValidTeam(static_cast<int>(localTeam))
		&& team == localTeam;
	return sameTeam ? ChamsEntity::TEAM : ChamsEntity::ENEMY;
}

namespace chams {
static void sehChamsHook(void* a1, void* a2, CMeshData* pMeshScene, int nMeshCount, void* pSceneView, void* pSceneLayer, void* pUnk, void* pUnk2)
{
	static auto original = H::DrawArray.GetOriginal();
	if (!original)
		return;

	if (VacDetect::IsSoftPaused()
		|| !I::EngineClient || !I::EngineClient->valid() || !H::oGetLocalPlayer
		|| !pMeshScene || nMeshCount < 1 || !Mem::IsReadable(pMeshScene, 0x58)) {
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	// Material walk only — never alter nMeshCount for original
	const int meshWalkCount = (nMeshCount > 12) ? 1 : nMeshCount;

	CSceneAnimatableObject* sceneObj = SehSceneObj(pMeshScene);
	if (!sceneObj) {
		World::ApplyMapMeshColor(pMeshScene);
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}
	if (!Mem::IsUserPtr(sceneObj)) {
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	const CBaseHandle render_ent = SehOwner(sceneObj);
	if (!render_ent.valid() || !I::GameEntity || !I::GameEntity->Instance) {
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	auto* entity = I::GameEntity->Instance->Get(render_ent);
	if (!entity || !Mem::ValidEntity(entity)) {
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	const ChamsEntity target = GetTargetType(entity);
	if (target == ChamsEntity::INVALID) {
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	if (target == VIEWMODEL && Config::viewmodelChams) {
		if (CMaterial2* mat = GetMaterial(Config::viewmodelChamsMaterial, false)) {
			ApplyChamsToMeshes(pMeshScene, meshWalkCount, mat, Config::viewmodelChamsMaterial,
				Config::colViewmodelChams.x, Config::colViewmodelChams.y,
				Config::colViewmodelChams.z, Config::colViewmodelChams.w, false);
		}
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	if (target == HANDS && Config::armChams) {
		if (CMaterial2* mat = GetMaterial(Config::armChamsMaterial, false)) {
			ApplyChamsToMeshes(pMeshScene, meshWalkCount, mat, Config::armChamsMaterial,
				Config::colArmChams.x, Config::colArmChams.y,
				Config::colArmChams.z, Config::colArmChams.w, false);
		}
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	const bool isEnemy = (target == ENEMY);
	const bool isTeam = (target == TEAM);
	const bool wantXQZ = isEnemy ? Config::enemyChamsInvisible
		: (isTeam && Config::teamChamsInvisible);
	const bool wantVis = isEnemy ? Config::enemyChams
		: (isTeam && Config::teamChams);

	if (!wantXQZ && !wantVis) {
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
		return;
	}

	const int matXQZType = Config::chamsMaterialXQZ;
	const int matVisType = Config::chamsMaterial;
	const ImVec4& colXQZ = isEnemy ? Config::colVisualChamsIgnoreZ : Config::teamcolVisualChamsIgnoreZ;
	const ImVec4& colVis = isEnemy ? Config::colVisualChams : Config::teamcolVisualChams;

	bool drew = false;
	if (wantXQZ) {
		if (CMaterial2* matXQZ = GetMaterial(matXQZType, true)) {
			ApplyChamsToMeshes(pMeshScene, meshWalkCount, matXQZ, matXQZType,
				colXQZ.x, colXQZ.y, colXQZ.z, colXQZ.w, true);
			original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
			drew = true;
		}
	}
	if (wantVis) {
		if (CMaterial2* matVis = GetMaterial(matVisType, false)) {
			ApplyChamsToMeshes(pMeshScene, meshWalkCount, matVis, matVisType,
				colVis.x, colVis.y, colVis.z, colVis.w, false);
			original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
			drew = true;
		}
	}
	if (!drew)
		original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
}
} // namespace chams

void __fastcall chams::hook(void* a1, void* a2, CMeshData* pMeshScene, int nMeshCount, void* pSceneView, void* pSceneLayer, void* pUnk, void* pUnk2)
{
	__try {
		chams::sehChamsHook(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("chams::hook", GetExceptionCode());
		auto original = H::DrawArray.GetOriginal();
		if (original)
			original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk, pUnk2);
	}
}
