#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <Windows.h>
#include "chams.h"
#include "../../hooks/hooks.h"
#include "../../config/config.h"
#include "../gamemode/gamemode.h"
#include "../world/world.h"
#include "../../utils/console/console.h"
#include "../../utils/security/vacdetect.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../glow/glow.h"
#include "../../../../external/imgui/imgui.h"
#include "../../utils/math/utlstronghandle/utlstronghandle.h"
#include "../../../cs2/entity/C_Material/C_Material.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../cs2/datatypes/keyvalues/keyvalues.h"
#include "../../../cs2/datatypes/cutlbuffer/cutlbuffer.h"

// IDA materialsystem2 CreateMaterial (0x18003ADE0):
//   CreateMaterial(this, outStrongHandle, name, keyValues3, resourceBinding, flags)
// create_material_from_resource allocates + SetTypeKV3(kv, 1, 6); ignores `this`.
CStrongHandle<CMaterial2> chams::create(const char* name, const char szVmatBuffer[])
{
	CStrongHandle<CMaterial2> pCustomMaterial{};
	if (!name || !szVmatBuffer || !I::CreateMaterial)
		return pCustomMaterial;

	CKeyValues3 dummy{};
	CKeyValues3* kv = dummy.create_material_from_resource();
	if (!kv) {
		Con::Error("chams::create(%s): SetTypeKV3/alloc failed", name);
		return pCustomMaterial;
	}

	kv->LoadFromBuffer(szVmatBuffer);
	// a5 resource = null, a6 flags = 1 (matches prior working call + IDA sibling path)
	I::CreateMaterial(nullptr, &pCustomMaterial, name, kv, nullptr, 1);

	if (!static_cast<CMaterial2*>(pCustomMaterial))
		Con::Error("chams::create(%s): CreateMaterial returned empty handle", name);

	return pCustomMaterial;
}

struct resource_material_t
{
	CStrongHandle<CMaterial2> mat;
	CStrongHandle<CMaterial2> mat_invs;
	// Optional additive rim/scan layer drawn after base (animated types only).
	CStrongHandle<CMaterial2> overlay;
	CStrongHandle<CMaterial2> overlay_invs;
};

static resource_material_t resourceMaterials[ChamsType::MAXCOUNT]{};
static bool g_materialsReady = false;

// Engine materialsystem2 exposes g_vTexCoordScrollSpeed — UV scrolls without CPU param writes.
// Multipass = solid body + additive fresnel overlay (real "animated" look vs flat vertex pulse).
static bool IsAnimatedMat(int type) noexcept
{
	return type == PULSE || type == RAINBOW || type == HOLO || type == ENERGY;
}

// KV3 template: additive fresnel shell with UV scroll (csgo_effects).
// scrollU/V = g_vTexCoordScrollSpeed, scale = texture repeat for visible motion on white masks.
static CStrongHandle<CMaterial2> CreateScrollOverlay(const char* name, bool xqz,
	float scrollU, float scrollV, float scale, float fresnelExp, float fresnelFall, float opacity)
{
	char buf[1800];
	std::snprintf(buf, sizeof(buf),
		R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_flOpacityScale = %.4f
	g_flFresnelExponent = %.4f
	g_flFresnelFalloff = %.4f
	g_flFresnelMax = 0.05
	g_flFresnelMin = 1.0
	g_vTexCoordScale = [%.4f, %.4f, 0.0, 0.0]
	g_vTexCoordOffset = [0.0, 0.0, 0.0, 0.0]
	g_vTexCoordScrollSpeed = [%.4f, %.4f, 0.0, 0.0]
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 0
	F_TRANSLUCENT = 1
	F_PAINT_VERTEX_COLORS = 1
	F_IGNOREZ = %d
	F_DISABLE_Z_WRITE = %d
	F_DISABLE_Z_BUFFERING = %d
	F_DISABLE_Z_PREPASS = %d
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
})",
		opacity, fresnelExp, fresnelFall, scale, scale, scrollU, scrollV,
		xqz ? 1 : 0, xqz ? 1 : 0, xqz ? 1 : 0, xqz ? 1 : 0);
	return chams::create(name, buf);
}

// Visible mats: Z on. XQZ mats: full ignore-Z + no Z-write (draw through world at any range).
bool chams::Materials::init()
{
	// Unique lefrizzel_* names — never overwrite engine materials/dev/flat|primary_white.
	// F_PAINT_VERTEX_COLORS so mesh->color tint is visible.
	// Visible: opaque + Z-write so it fully covers XQZ on LOS pixels.
	// XQZ: translucent ignore-Z, no Z-write — only reads through walls.
	resourceMaterials[FLAT] = resource_material_t{
		.mat = create("materials/dev/lefrizzel_flat_o.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_PAINT_VERTEX_COLORS = 1
            F_IGNOREZ = 0
            F_DISABLE_Z_WRITE = 0
            F_DISABLE_Z_BUFFERING = 0
            F_BLEND_MODE = 0
            F_TRANSLUCENT = 0
            F_RENDER_BACKFACES = 0
            g_vColorTint = [1.000000, 1.000000, 1.000000, 1.000000]
            g_vGlossinessRange = [0.000000, 1.000000, 0.000000, 0.000000]
            g_vNormalTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_vTexCoordOffset = [0.000000, 0.000000, 0.000000, 0.000000]
            g_vTexCoordScale = [1.000000, 1.000000, 0.000000, 0.000000]
            g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
            g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
        })"),
		.mat_invs = create("materials/dev/lefrizzel_flat_xqz.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_PAINT_VERTEX_COLORS = 1
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
		.mat = create("materials/dev/lefrizzel_illum_o.vmat",  R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 0
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
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
		.mat_invs = create("materials/dev/lefrizzel_illum_xqz.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
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
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
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
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
})")
	};

	resourceMaterials[GHOST] = resource_material_t{
		.mat = create("materials/dev/tw_ghost.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
        {
            Shader = "csgo_unlitgeneric.vfx"
            F_PAINT_VERTEX_COLORS = 1
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
            F_PAINT_VERTEX_COLORS = 1
            F_IGNOREZ = 1
            F_DISABLE_Z_WRITE = 1
            F_DISABLE_Z_BUFFERING = 1
            F_DISABLE_Z_PREPASS = 1
            F_BLEND_MODE = 1
            F_TRANSLUCENT = 1
            F_RENDER_BACKFACES = 0
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
		.mat = create("materials/dev/tw_latex_o.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 0
	F_SPECULAR = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
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
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
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

	resourceMaterials[METALLIC2] = resource_material_t{
		// Visible: Z-test + opaque cover over XQZ on LOS pixels
		.mat = create("materials/dev/metallic2_o.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_PAINT_VERTEX_COLORS = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	F_BLEND_MODE = 0
	F_TRANSLUCENT = 0
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_bFogEnabled = 0
	g_flMetalness = 1.000
	g_flModelTintAmount = 1.000
	g_nScaleTexCoordUByModelScaleAxis = 0
	g_nScaleTexCoordVByModelScaleAxis = 0
	g_nTextureAddressModeU = 0
	g_nTextureAddressModeV = 0
	g_flTexCoordRotation = 0.000
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
})"),
		.mat_invs = create("materials/dev/metallic2_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_PAINT_VERTEX_COLORS = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_PREPASS = 1
	F_DISABLE_Z_WRITE = 1
	F_BLEND_MODE = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
	g_bFogEnabled = 0
	g_flMetalness = 1.000
	g_flModelTintAmount = 1.000
	g_nScaleTexCoordUByModelScaleAxis = 0
	g_nScaleTexCoordVByModelScaleAxis = 0
	g_nTextureAddressModeU = 0
	g_nTextureAddressModeV = 0
	g_flTexCoordRotation = 0.000
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_1b833b2a.vtex"
})")
	};
	resourceMaterials[GOST2] = resource_material_t{
		.mat = create("materials/dev/gost2.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_flOpacityScale = 0.45
	g_flFresnelExponent = 0.75
	g_flFresnelFalloff = 1
	g_flFresnelMax = 0.0
	g_flFresnelMin = 1
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 0
	F_TRANSLUCENT = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	F_RENDER_BACKFACES = 1
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
})"),
		.mat_invs = create("materials/dev/gost2_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_effects.vfx"
	g_tColor = resource:"materials/dev/primary_white_color_tga_21186c76.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tMask1 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask2 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_tMask3 = resource:"materials/default/default_mask_tga_344101f8.vtex"
	g_flOpacityScale = 0.45
	g_flFresnelExponent = 0.75
	g_flFresnelFalloff = 1
	g_flFresnelMax = 0.0
	g_flFresnelMin = 1
	F_ADDITIVE_BLEND = 1
	F_BLEND_MODE = 0
	F_TRANSLUCENT = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 1.0]
})")
	};

	// Animated: opaque/self-illum body + additive UV-scroll fresnel overlay (multipass in ApplyChams).
	// g_vTexCoordScrollSpeed = engine GPU scroll (materialsystem2 string). Vertex tint still cycles colour.
	resourceMaterials[PULSE] = resource_material_t{
		.mat = create("materials/dev/tw_pulse_o.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 0
	F_SPECULAR = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 2.800000, 2.800000, 2.800000, 2.800000 ]
	g_flSelfIllumBrightness = [ 2.200000, 2.200000, 2.200000, 2.200000 ]
	g_vSelfIllumTint = [ 4.500000, 4.500000, 4.500000, 4.500000 ]
	g_flSpecularIntensity = 2.200000
	g_flSpecularExponent = 28.000000
	g_vGlossinessRange = [ 0.550000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.mat_invs = create("materials/dev/tw_pulse_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_SPECULAR = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 2.800000, 2.800000, 2.800000, 2.800000 ]
	g_flSelfIllumBrightness = [ 2.200000, 2.200000, 2.200000, 2.200000 ]
	g_vSelfIllumTint = [ 4.500000, 4.500000, 4.500000, 4.500000 ]
	g_flSpecularIntensity = 2.200000
	g_flSpecularExponent = 28.000000
	g_vGlossinessRange = [ 0.550000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.overlay = CreateScrollOverlay("materials/dev/tw_pulse_ov.vmat", false, 0.0f, 0.55f, 3.5f, 1.1f, 2.2f, 0.72f),
		.overlay_invs = CreateScrollOverlay("materials/dev/tw_pulse_ov_i.vmat", true, 0.0f, 0.55f, 3.5f, 1.1f, 2.2f, 0.55f)
	};

	resourceMaterials[RAINBOW] = resource_material_t{
		.mat = create("materials/dev/tw_rainbow_o.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 0
	F_SPECULAR = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 3.200000, 3.200000, 3.200000, 3.200000 ]
	g_flSelfIllumBrightness = [ 2.600000, 2.600000, 2.600000, 2.600000 ]
	g_vSelfIllumTint = [ 5.500000, 5.500000, 5.500000, 5.500000 ]
	g_flSpecularIntensity = 1.800000
	g_flSpecularExponent = 24.000000
	g_vGlossinessRange = [ 0.450000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.mat_invs = create("materials/dev/tw_rainbow_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_SPECULAR = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 3.200000, 3.200000, 3.200000, 3.200000 ]
	g_flSelfIllumBrightness = [ 2.600000, 2.600000, 2.600000, 2.600000 ]
	g_vSelfIllumTint = [ 5.500000, 5.500000, 5.500000, 5.500000 ]
	g_flSpecularIntensity = 1.800000
	g_flSpecularExponent = 24.000000
	g_vGlossinessRange = [ 0.450000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.overlay = CreateScrollOverlay("materials/dev/tw_rainbow_ov.vmat", false, 0.85f, 0.35f, 5.0f, 0.65f, 2.8f, 0.85f),
		.overlay_invs = CreateScrollOverlay("materials/dev/tw_rainbow_ov_i.vmat", true, 0.85f, 0.35f, 5.0f, 0.65f, 2.8f, 0.65f)
	};

	// Holo base = soft unlit body (XQZ-readable) + strong scroll fresnel shell
	resourceMaterials[HOLO] = resource_material_t{
		.mat = create("materials/dev/tw_holo.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_unlitgeneric.vfx"
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_BLEND_MODE = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 0.55]
	g_vTexCoordScale = [1.0, 1.0, 0.0, 0.0]
	g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})"),
		.mat_invs = create("materials/dev/tw_holo_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_unlitgeneric.vfx"
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_BLEND_MODE = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [1.0, 1.0, 1.0, 0.40]
	g_vTexCoordScale = [1.0, 1.0, 0.0, 0.0]
	g_tColor = resource:"materials/dev/primary_white_color_tga_f7b257f6.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
})"),
		.overlay = CreateScrollOverlay("materials/dev/tw_holo_ov.vmat", false, 1.4f, -0.55f, 8.0f, 0.45f, 3.5f, 1.0f),
		.overlay_invs = CreateScrollOverlay("materials/dev/tw_holo_ov_i.vmat", true, 1.4f, -0.55f, 8.0f, 0.45f, 3.5f, 0.80f)
	};

	resourceMaterials[ENERGY] = resource_material_t{
		.mat = create("materials/dev/tw_energy_o.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 0
	F_SPECULAR = 1
	F_IGNOREZ = 0
	F_DISABLE_Z_WRITE = 0
	F_DISABLE_Z_BUFFERING = 0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 3.800000, 3.800000, 3.800000, 3.800000 ]
	g_flSelfIllumBrightness = [ 3.200000, 3.200000, 3.200000, 3.200000 ]
	g_vSelfIllumTint = [ 7.000000, 7.000000, 7.000000, 7.000000 ]
	g_flSpecularIntensity = 5.500000
	g_flSpecularExponent = 72.000000
	g_vGlossinessRange = [ 0.920000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.mat_invs = create("materials/dev/tw_energy_i.vmat", R"(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	shader = "csgo_complex.vfx"
	F_SELF_ILLUM = 1
	F_PAINT_VERTEX_COLORS = 1
	F_TRANSLUCENT = 1
	F_SPECULAR = 1
	F_IGNOREZ = 1
	F_DISABLE_Z_WRITE = 1
	F_DISABLE_Z_BUFFERING = 1
	F_DISABLE_Z_PREPASS = 1
	F_RENDER_BACKFACES = 0
	g_vColorTint = [ 1.000000, 1.000000, 1.000000, 1.000000 ]
	g_flSelfIllumScale = [ 3.800000, 3.800000, 3.800000, 3.800000 ]
	g_flSelfIllumBrightness = [ 3.200000, 3.200000, 3.200000, 3.200000 ]
	g_vSelfIllumTint = [ 7.000000, 7.000000, 7.000000, 7.000000 ]
	g_flSpecularIntensity = 5.500000
	g_flSpecularExponent = 72.000000
	g_vGlossinessRange = [ 0.920000, 1.000000, 0.000000, 0.000000 ]
	g_tColor = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	g_tNormal = resource:"materials/default/default_normal_tga_7652cb.vtex"
	g_tSelfIllumMask = resource:"materials/default/default_mask_tga_fde710a5.vtex"
	TextureAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
	g_tAmbientOcclusion = resource:"materials/debug/particleerror.vtex"
})"),
		.overlay = CreateScrollOverlay("materials/dev/tw_energy_ov.vmat", false, 0.0f, 1.8f, 6.5f, 0.85f, 2.0f, 0.90f),
		.overlay_invs = CreateScrollOverlay("materials/dev/tw_energy_ov_i.vmat", true, 0.0f, 1.8f, 6.5f, 0.85f, 2.0f, 0.70f)
	};

	int okVis = 0, okXqz = 0;
	for (int i = 0; i < ChamsType::MAXCOUNT; ++i) {
		if (static_cast<CMaterial2*>(resourceMaterials[i].mat))
			++okVis;
		if (static_cast<CMaterial2*>(resourceMaterials[i].mat_invs))
			++okXqz;
	}

	g_materialsReady = static_cast<CMaterial2*>(resourceMaterials[FLAT].mat) != nullptr
		|| static_cast<CMaterial2*>(resourceMaterials[FLAT].mat_invs) != nullptr;

	if (g_materialsReady)
		Con::Ok("Chams materials ready (vis=%d xqz=%d / %d types)", okVis, okXqz, (int)ChamsType::MAXCOUNT);
	else
		Con::Error("Chams materials failed — CreateMaterial miss? (CreateMaterial=%p)",
			reinterpret_cast<void*>(I::CreateMaterial));
	return g_materialsReady;
}

// Clamp menu/config material index into valid ChamsType range
static int ClampMatType(int type)
{
	if (type < 0 || type >= ChamsType::MAXCOUNT)
		return FLAT;
	return type;
}

static void EnsureMaterialsReady()
{
	if (g_materialsReady)
		return;
	static bool s_reinitTried = false;
	if (!s_reinitTried && I::CreateMaterial) {
		s_reinitTried = true;
		chams::Materials re{};
		re.init();
	}
}

static CMaterial2* GetMaterial(int type, bool invisible)
{
	EnsureMaterialsReady();
	if (!g_materialsReady)
		return nullptr;
	type = ClampMatType(type);
	CMaterial2* mat = invisible
		? static_cast<CMaterial2*>(resourceMaterials[type].mat_invs)
		: static_cast<CMaterial2*>(resourceMaterials[type].mat);
	// Fallback: opposite pass, then FLAT either pass
	if (!mat) {
		mat = invisible
			? static_cast<CMaterial2*>(resourceMaterials[type].mat)
			: static_cast<CMaterial2*>(resourceMaterials[type].mat_invs);
	}
	if (!mat) {
		mat = invisible
			? static_cast<CMaterial2*>(resourceMaterials[FLAT].mat_invs)
			: static_cast<CMaterial2*>(resourceMaterials[FLAT].mat);
	}
	if (!mat)
		mat = static_cast<CMaterial2*>(resourceMaterials[FLAT].mat);
	if (!mat)
		mat = static_cast<CMaterial2*>(resourceMaterials[FLAT].mat_invs);
	return mat;
}

// Additive UV-scroll shell for animated types (null = single-pass).
static CMaterial2* GetOverlayMaterial(int type, bool invisible)
{
	EnsureMaterialsReady();
	if (!g_materialsReady)
		return nullptr;
	type = ClampMatType(type);
	if (!IsAnimatedMat(type))
		return nullptr;
	CMaterial2* ov = invisible
		? static_cast<CMaterial2*>(resourceMaterials[type].overlay_invs)
		: static_cast<CMaterial2*>(resourceMaterials[type].overlay);
	if (!ov) {
		ov = invisible
			? static_cast<CMaterial2*>(resourceMaterials[type].overlay)
			: static_cast<CMaterial2*>(resourceMaterials[type].overlay_invs);
	}
	return ov;
}

// Safe mesh write — death frames free entities mid-draw; never walk bad strides.
static bool MeshWritable(CMeshData* mesh) noexcept
{
	// Full primitive — IDA stride 0x70
	return mesh && Mem::IsReadable(mesh, CMeshData::kStride);
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
		// Always kill engine distance/LOD alpha fade — partial 0..8 gate left far meshes dim.
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
		else {
			// Prefer m_hPlayerPawn — m_hPawn is often observer while dead/spec
			auto* ctrl = reinterpret_cast<CCSPlayerController*>(e);
			h = ctrl->m_hPlayerPawn();
			if (!h.valid())
				h = ctrl->m_hPawn();
		}
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
	return H::SafeLocalPlayer();
}

__declspec(noinline) static CBaseHandle SehEntityHandle(C_BaseEntity* e)
{
	CBaseHandle h{};
	if (!e) return h;
	__try { h = e->handle(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return h;
}

static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
	h = std::fmod(h, 1.f);
	if (h < 0.f) h += 1.f;
	const float c = v * s;
	const float x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f));
	const float m = v - c;
	float rp = 0.f, gp = 0.f, bp = 0.f;
	const int sector = static_cast<int>(h * 6.f) % 6;
	switch (sector) {
	case 0: rp = c; gp = x; bp = 0.f; break;
	case 1: rp = x; gp = c; bp = 0.f; break;
	case 2: rp = 0.f; gp = c; bp = x; break;
	case 3: rp = 0.f; gp = x; bp = c; break;
	case 4: rp = x; gp = 0.f; bp = c; break;
	default: rp = c; gp = 0.f; bp = x; break;
	}
	r = rp + m;
	g = gp + m;
	b = bp + m;
}

static float SmoothPulse(float t, float speed, float lo, float hi)
{
	// Smoothstep-ish sine → punchier mid, soft edges
	const float s = 0.5f + 0.5f * std::sin(t * speed);
	const float shaped = s * s * (3.f - 2.f * s);
	return lo + (hi - lo) * shaped;
}

// overlayPass: brighter / higher-alpha tint for additive scroll shell
static void AnimateChamsTint(int matType, float& r, float& g, float& b, float& a, bool overlayPass = false)
{
	const float t = static_cast<float>(GetTickCount64()) * 0.001f;
	const float baseR = r, baseG = g, baseB = b, baseA = a;

	switch (matType) {
	case PULSE: {
		// Dual-beat heart + soft afterglow; overlay rides peaks harder
		const float beat = SmoothPulse(t, 2.4f, 0.28f, 1.0f);
		const float after = SmoothPulse(t + 0.28f, 2.4f, 0.0f, 0.42f);
		const float glow = std::clamp(beat + after * 0.85f, 0.f, 1.f);
		const float amp = overlayPass ? (0.55f + 0.90f * glow) : (0.35f + 0.75f * glow);
		r = std::clamp(baseR * amp + (overlayPass ? 0.12f * glow : 0.f), 0.f, 1.f);
		g = std::clamp(baseG * amp + (overlayPass ? 0.08f * glow : 0.f), 0.f, 1.f);
		b = std::clamp(baseB * amp + (overlayPass ? 0.15f * glow : 0.f), 0.f, 1.f);
		a = std::clamp(baseA * (overlayPass ? (0.50f + 0.50f * glow) : (0.70f + 0.30f * glow)), 0.40f, 1.f);
		break;
	}
	case RAINBOW: {
		// Fast full-body HSV + secondary phase on overlay (chrome band feel)
		const float hue = std::fmod(t * 0.55f + (overlayPass ? 0.18f : 0.f), 1.f);
		const float val = overlayPass
			? (0.88f + 0.12f * (0.5f + 0.5f * std::sin(t * 5.5f)))
			: (0.72f + 0.28f * (0.5f + 0.5f * std::sin(t * 2.4f)));
		const float sat = overlayPass ? 0.95f : 1.f;
		HsvToRgb(hue, sat, val, r, g, b);
		a = std::clamp((std::max)(baseA, overlayPass ? 0.90f : 0.88f), 0.75f, 1.f);
		break;
	}
	case HOLO: {
		// Chromatic split + high-freq scan; overlay is the moving band
		float cr = 0.f, cg = 0.f, cb = 0.f;
		float mr = 0.f, mg = 0.f, mb = 0.f;
		const float speed = overlayPass ? 0.85f : 0.48f;
		HsvToRgb(std::fmod(t * speed, 1.f), 1.f, 1.f, cr, cg, cb);
		HsvToRgb(std::fmod(t * speed + 0.33f, 1.f), 0.92f, 1.f, mr, mg, mb);
		const float scan = 0.5f + 0.5f * std::sin(t * (overlayPass ? 14.0f : 7.0f));
		const float mix = 0.25f + 0.75f * scan;
		r = cr * (1.f - mix) + mr * mix;
		g = cg * (1.f - mix) + mg * mix;
		b = cb * (1.f - mix) + mb * mix;
		if (overlayPass) {
			// Lift mid for additive shell
			r = std::clamp(r * 1.15f, 0.f, 1.f);
			g = std::clamp(g * 1.15f, 0.f, 1.f);
			b = std::clamp(b * 1.15f, 0.f, 1.f);
		}
		a = std::clamp(overlayPass ? (0.65f + 0.35f * scan) : (0.45f + 0.35f * scan), 0.40f, 1.f);
		break;
	}
	case ENERGY: {
		// Cyan↔magenta arcs + flicker spikes; overlay = hot white edges on spike
		const float arc = 0.5f + 0.5f * std::sin(t * 6.2f);
		const float hum = 0.5f + 0.5f * std::sin(t * 2.1f + 0.8f);
		const float flick = ((static_cast<int>(t * 47.f) * 1103515245 + 12345) & 255) / 255.f;
		const float spike = (flick > 0.88f) ? 1.0f : (0.50f + 0.50f * hum);
		if (overlayPass) {
			r = std::clamp(0.35f + 0.65f * arc * spike, 0.f, 1.f);
			g = std::clamp(0.70f + 0.30f * (1.f - arc) * spike, 0.f, 1.f);
			b = std::clamp(0.95f * spike + 0.05f, 0.f, 1.f);
			a = std::clamp(0.55f + 0.45f * spike, 0.50f, 1.f);
		} else {
			r = std::clamp(0.12f + 0.75f * arc * spike + baseR * 0.20f, 0.f, 1.f);
			g = std::clamp(0.45f + 0.50f * (1.f - arc) * spike + baseG * 0.12f, 0.f, 1.f);
			b = std::clamp(0.80f + 0.20f * hum * spike + baseB * 0.15f, 0.f, 1.f);
			a = std::clamp((std::max)(baseA, 0.80f) * (0.70f + 0.30f * spike), 0.60f, 1.f);
		}
		break;
	}
	default:
		break;
	}
}

// True for mats that stay translucent on the VISIBLE pass (rim/ghost/additive).
// Opaque body mats cover XQZ fully; these need alpha stacking help instead.
static bool MatKeepsVisTranslucent(int matType)
{
	switch (matType) {
	case GLOW:
	case GHOST:
	case GOST2:
	case HOLO:
		return true;
	default:
		return false;
	}
}

// dualPass = both XQZ + visible this frame (dim XQZ / solidify vis so colors don't mud)
// overlayPass = additive scroll shell (keep translucent, no opaque force)
static void ApplyChamsColor(CMeshData* mesh, int matType, float r, float g, float b, float a,
	bool xqz, bool dualPass = false, bool overlayPass = false)
{
	if (!MeshWritable(mesh))
		return;

	AnimateChamsTint(matType, r, g, b, a, overlayPass);

	float alpha = std::clamp(a, 0.f, 1.f);
	if (overlayPass) {
		// Additive shell — never force opaque; keep motion readable
		if (xqz && dualPass)
			alpha = std::clamp(alpha * 0.55f, 0.25f, 0.70f);
		else
			alpha = std::clamp(alpha, 0.40f, 1.f);
	} else if (matType == GHOST) {
		// Ghost alone stays ethereal; dual-pass XQZ even thinner
		if (xqz && dualPass)
			alpha = std::clamp(alpha * 0.12f, 0.04f, 0.16f);
		else
			alpha = std::clamp(alpha * 0.28f, 0.08f, 0.32f);
	} else if (xqz) {
		// Dual: soft wall-only silhouette so vis color owns LOS pixels
		if (dualPass)
			alpha = std::clamp(alpha * 0.42f, 0.18f, 0.48f);
		else
			alpha = std::clamp(alpha, 0.45f, 1.f);
	} else {
		// Visible: solid cover over XQZ (opaque mats ignore alpha; translucent still need it)
		if (dualPass || !MatKeepsVisTranslucent(matType))
			alpha = (std::max)(alpha, 0.92f);
	}

	SehWriteTint(mesh,
		static_cast<uint8_t>(std::clamp(r, 0.f, 1.f) * 255.0f + 0.5f),
		static_cast<uint8_t>(std::clamp(g, 0.f, 1.f) * 255.0f + 0.5f),
		static_cast<uint8_t>(std::clamp(b, 0.f, 1.f) * 255.0f + 0.5f),
		static_cast<uint8_t>(alpha * 255.0f + 0.5f));
}

// Primary + same-owner siblings. Cap avoids death-FX/particle arrays.
static constexpr int kMaxChamsMeshes = 24;

static void ApplyChamsToMeshes(
	CMeshData* base,
	int nMeshCount,
	CMaterial2* mat,
	int matType,
	float r, float g, float b, float a,
	bool xqz,
	bool dualPass = false,
	bool overlayPass = false)
{
	// Material pointers from CreateMaterial are engine-owned — null check only
	// (IsUserPtr can reject valid low-module/heaps in some layouts).
	if (!MeshWritable(base) || !mat || nMeshCount < 1)
		return;
	if (!SehSetMaterial(base, mat))
		return;
	ApplyChamsColor(base, matType, r, g, b, a, xqz, dualPass, overlayPass);

	const int walk = (nMeshCount > kMaxChamsMeshes) ? kMaxChamsMeshes : nMeshCount;
	if (walk <= 1)
		return;

	CSceneAnimatableObject* owner0 = SehSceneObj(base);
	if (!owner0)
		return;
	const CBaseHandle h0 = SehOwner(owner0);
	// Same scene-object siblings even without valid handle (some attachments)
	for (int i = 1; i < walk; ++i) {
		CMeshData* mesh = base->At(i);
		if (!MeshWritable(mesh))
			continue;
		CSceneAnimatableObject* ownerI = SehSceneObj(mesh);
		if (!ownerI)
			continue;
		if (ownerI != owner0) {
			if (!h0.valid())
				continue;
			const CBaseHandle hI = SehOwner(ownerI);
			if (!hI.valid() || hI != h0)
				continue;
		}
		if (!SehSetMaterial(mesh, mat))
			continue;
		ApplyChamsColor(mesh, matType, r, g, b, a, xqz, dualPass, overlayPass);
	}
}

using DrawArrayFn_t = void* (__fastcall*)(void*, void*, CMeshData*, int, void*, void*, void*);

// Base draw + optional additive UV-scroll overlay (second DrawArray).
// On any fault: fall through to plain original (never skip engine draw).
static void* DrawChamsPass(
	void* a1, void* a2, CMeshData* pMeshScene, int nMeshCount,
	void* pSceneView, void* pSceneLayer, void* pUnk,
	DrawArrayFn_t original,
	int matType, bool xqz,
	float r, float g, float b, float a,
	bool dualPass)
{
	if (!original)
		return nullptr;
	if (!pMeshScene || nMeshCount < 1)
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);

	void* ret = nullptr;
	__try {
		if (CMaterial2* mat = GetMaterial(matType, xqz)) {
			ApplyChamsToMeshes(pMeshScene, nMeshCount, mat, matType, r, g, b, a, xqz, dualPass, false);
			ret = original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
		}
		if (CMaterial2* ov = GetOverlayMaterial(matType, xqz)) {
			ApplyChamsToMeshes(pMeshScene, nMeshCount, ov, matType, r, g, b, a, xqz, dualPass, true);
			ret = original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		// Mat apply fault — still draw vanilla so model never disappears
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}
	return ret;
}

// Map colour must never hit players / corpses / arms / viewmodels / player gear.
// Dead pawns still report IsBasePlayer but fail SehAlivePawn → used to fall into
// INVALID and get tinted — that was the ragdoll colour leak.
__declspec(noinline) static const char* SehClassName(C_BaseEntity* e)
{
	if (!e) return nullptr;
	__try {
		SchemaClassInfoData_t* cls = nullptr;
		e->dump_class_info(&cls);
		if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
			return nullptr;
		return cls->szName;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static bool ClassIsPlayerRelated(const char* n)
{
	if (!n || !n[0])
		return false;
	if (strstr(n, "Player") || strstr(n, "Ragdoll") || strstr(n, "HudModel")
		|| strstr(n, "Viewmodel") || strstr(n, "Wearable") || strstr(n, "Observer")
		|| strstr(n, "CSGO_Player") || strstr(n, "EconEntity"))
		return true;
	// Held / world weapons still belong to combatants — leave alone
	if (strstr(n, "Weapon") || strstr(n, "Knife") || strstr(n, "Grenade")
		|| strstr(n, "C4") || strstr(n, "PlantedC4") || strstr(n, "Inferno")
		|| strstr(n, "Projectile") || strstr(n, "Molotov") || strstr(n, "Flashbang")
		|| strstr(n, "HEGrenade") || strstr(n, "SmokeGrenade") || strstr(n, "Decoy")
		|| strstr(n, "Chicken") || strstr(n, "Hostage"))
		return true;
	return false;
}

static bool OwnerIsPlayerRelated(C_BaseEntity* ent)
{
	if (!ent || !I::GameEntity || !I::GameEntity->Instance)
		return false;
	const CBaseHandle hOwner = SehHandleField(ent, true);
	if (!hOwner.valid())
		return false;
	auto* owner = I::GameEntity->Instance->Get<C_BaseEntity>(hOwner);
	if (!owner || owner == ent || !Mem::ValidEntity(owner))
		return false;
	if (SehIsBasePlayer(owner) || SehIsController(owner))
		return true;
	const char* on = SehClassName(owner);
	return ClassIsPlayerRelated(on);
}

// Engine glow outline draws share the player owner handle. If we replace their
// material/tint with chams colours, the outline becomes red (and flickers vs
// the green float4 we stamp on CGlowProperty). Never cham those draws.
//
// UC Oct 2024 (still valid): skip materials/dev/glowproperty.vmat before override.
// Do NOT match our chams mats (materials/dev/tw_glow*.vmat) — only engine glowproperty.
__declspec(noinline) static bool SehIsEngineGlowPropertyMat(CMeshData* mesh)
{
	if (!mesh) return false;
	__try {
		CMaterial2* mat = mesh->pMaterial;
		if (!mat || !Mem::IsReadable(mat, sizeof(void*)))
			return false;
		const char* n = mat->GetName();
		if (!n || !Mem::IsReadable(n, 8))
			return false;
		// Case-insensitive substring "glowproperty" (path may vary slightly by build)
		for (const char* p = n; *p; ++p) {
			char c0 = *p;
			if (c0 >= 'A' && c0 <= 'Z') c0 = static_cast<char>(c0 + 32);
			if (c0 != 'g')
				continue;
			bool ok = true;
			static const char kNeedle[] = "glowproperty";
			for (int i = 0; kNeedle[i]; ++i) {
				char c = p[i];
				if (!c) { ok = false; break; }
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
				if (c != kNeedle[i]) { ok = false; break; }
			}
			if (ok)
				return true;
		}
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static bool ShouldSkipChamsForGlow(C_BaseEntity* ent, CSceneAnimatableObject* scene, CMeshData* mesh)
{
	// 1) Exact engine glow mat (UC fix) — most reliable
	if (SehIsEngineGlowPropertyMat(mesh))
		return true;
	if (mesh) {
		for (int i = 0; i < 4; ++i) {
			CMeshData* m = mesh->At(i);
			if (!MeshWritable(m))
				break;
			if (SehIsEngineGlowPropertyMat(m))
				return true;
		}
	}
	// 2) ManageGlow scene object (magic / pointer match) — backup
	if (scene && Glow::IsEngineGlowSceneObject(ent, scene))
		return true;
	if (scene && Glow::IsEngineGlowSceneObject(scene))
		return true;
	return false;
}

// true = world geometry / map props only
static bool ShouldApplyMapColor(C_BaseEntity* ent)
{
	if (!ent)
		return true; // pure static mesh path (no owner entity)
	if (SehIsBasePlayer(ent) || SehIsController(ent)
		|| SehIsHands(ent) || SehIsViewmodel(ent))
		return false;
	const char* n = SehClassName(ent);
	if (ClassIsPlayerRelated(n))
		return false;
	if (OwnerIsPlayerRelated(ent))
		return false;
	return true;
}

// allowDead: true → return pawn even when dead (ragdoll / corpse mesh)
static C_CSPlayerPawn* ResolvePlayerPawn(C_BaseEntity* ent, bool allowDead = false)
{
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;

	if (SehIsBasePlayer(ent)) {
		auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(ent);
		if (!allowDead && !SehAlivePawn(pawn, nullptr, nullptr))
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
		if (!allowDead && !SehAlivePawn(pawn, nullptr, nullptr))
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
	if (!allowDead && !SehAlivePawn(pawn, nullptr, nullptr))
		return nullptr;
	return pawn;
}

static bool ClassIsRagdollName(const char* n)
{
	if (!n || !n[0])
		return false;
	return strstr(n, "Ragdoll") != nullptr || strstr(n, "ragdoll") != nullptr;
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

	// Named ragdoll entities (CRagdollProp / CSGO ragdoll wrappers)
	const char* cls = SehClassName(render_ent);
	if (ClassIsRagdollName(cls)) {
		if (Config::ragdollChams)
			return ChamsEntity::RAGDOLL;
		return ChamsEntity::INVALID;
	}

	// Only player pawns / controllers — never weapons/attachments.
	if (!SehIsBasePlayer(render_ent) && !SehIsController(render_ent))
		return ChamsEntity::INVALID;

	// Prefer alive; fall back to dead pawn for ragdoll chams
	C_CSPlayerPawn* player = ResolvePlayerPawn(render_ent, false);
	if (!player)
		player = ResolvePlayerPawn(render_ent, true);
	if (!player)
		return ChamsEntity::INVALID;

	const bool alive = SehAlivePawn(player, nullptr, nullptr);

	// Pointer equality alone fails when scene owner is a different C_CSPlayerPawn*
	// instance for the same handle (recreate / prediction). Compare packed handles.
	bool isLocal = (player == local);
	if (!isLocal) {
		const CBaseHandle hPlayer = SehEntityHandle(reinterpret_cast<C_BaseEntity*>(player));
		const CBaseHandle hLocal = SehEntityHandle(reinterpret_cast<C_BaseEntity*>(local));
		isLocal = hPlayer.valid() && hLocal.valid()
			&& hPlayer.index() == hLocal.index()
			&& hPlayer.serial_number() == hLocal.serial_number();
	}

	if (isLocal) {
		if (alive && Config::localChams)
			return ChamsEntity::LOCAL;
		if (!alive && Config::ragdollChams)
			return ChamsEntity::RAGDOLL;
		return ChamsEntity::INVALID;
	}

	if (!alive) {
		if (Config::ragdollChams)
			return ChamsEntity::RAGDOLL;
		return ChamsEntity::INVALID;
	}

	uint8_t team = 0, localTeam = 0;
	if (!SehTeams(player, local, &team, &localTeam))
		return ChamsEntity::INVALID;

	const bool sameTeam = GameMode::WantTeamCheck(Config::team_check)
		&& Mem::ValidTeam(static_cast<int>(team))
		&& Mem::ValidTeam(static_cast<int>(localTeam))
		&& team == localTeam;
	return sameTeam ? ChamsEntity::TEAM : ChamsEntity::ENEMY;
}

namespace chams {

using DrawArrayFn = void* (__fastcall*)(void*, void*, CMeshData*, int, void*, void*, void*);

// Always re-fetch original — never cache null from a race on first call.
// Propagate return value (IDA returns pointer; void detour left RAX garbage → crash).
static void* sehChamsHook(void* a1, void* a2, CMeshData* pMeshScene, int nMeshCount, void* pSceneView, void* pSceneLayer, void* pUnk)
{
	auto original = reinterpret_cast<DrawArrayFn>(H::DrawArray.GetOriginal());
	if (!original)
		return nullptr;

	const bool anyPlayerChams = Config::enemyChams || Config::enemyChamsInvisible
		|| Config::teamChams || Config::teamChamsInvisible
		|| Config::localChams || Config::ragdollChams;
	const bool anyLocalChams = Config::armChams || Config::viewmodelChams;
	const bool wantMapTint = World::WantPropTint();
	const bool anyChamsOrTint = anyPlayerChams || anyLocalChams || wantMapTint;

	// Pure passthrough when chams/tint off or not ready to resolve.
	if (!anyChamsOrTint
		|| VacDetect::IsSoftPaused()
		|| !I::EngineClient || !I::EngineClient->valid() || !H::oGetLocalPlayer
		|| !pMeshScene || nMeshCount < 1
		|| !Mem::IsReadable(pMeshScene, CMeshData::kStride)) {
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	const int meshWalkCount = (nMeshCount > kMaxChamsMeshes) ? kMaxChamsMeshes : nMeshCount;
	const int mapTintCount = meshWalkCount;

	CSceneAnimatableObject* sceneObj = SehSceneObj(pMeshScene);
	if (!sceneObj) {
		if (wantMapTint)
			World::ApplyMapMeshColor(pMeshScene, mapTintCount);
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	const CBaseHandle render_ent = SehOwner(sceneObj);
	if (!render_ent.valid() || !I::GameEntity || !I::GameEntity->Instance) {
		if (wantMapTint)
			World::ApplyMapMeshColor(pMeshScene, mapTintCount);
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	auto* entity = I::GameEntity->Instance->Get(render_ent);
	if (!entity || !Mem::ValidEntity(entity)) {
		if (wantMapTint)
			World::ApplyMapMeshColor(pMeshScene, mapTintCount);
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	// Map props: tint + out. Never run GetTargetType (class dumps) on world meshes.
	if (ShouldApplyMapColor(entity)) {
		if (wantMapTint)
			World::ApplyMapMeshColor(pMeshScene, mapTintCount);
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	// Player/weapon/FX mesh with no chams features — pure passthrough (map tint already excluded).
	// Full visuals + Night/map_color used to still dump class for every player draw.
	if (!anyPlayerChams && !anyLocalChams)
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);

	// Glow outline scene objects share player owner — passthrough only.
	if (ShouldSkipChamsForGlow(entity, sceneObj, pMeshScene))
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);

	// Resolve type under SEH — fault = vanilla draw only (no feature loss path)
	ChamsEntity target = ChamsEntity::INVALID;
	__try {
		target = GetTargetType(entity);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}
	if (target == ChamsEntity::INVALID)
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);

	if (target == VIEWMODEL) {
		if (Config::viewmodelChams) {
			const int mt = ClampMatType(Config::viewmodelChamsMaterial);
			if (void* r = DrawChamsPass(a1, a2, pMeshScene, meshWalkCount, pSceneView, pSceneLayer, pUnk,
				original, mt, false,
				Config::colViewmodelChams.x, Config::colViewmodelChams.y,
				Config::colViewmodelChams.z, Config::colViewmodelChams.w, false))
				return r;
		}
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	if (target == HANDS) {
		if (Config::armChams) {
			const int mt = ClampMatType(Config::armChamsMaterial);
			if (void* r = DrawChamsPass(a1, a2, pMeshScene, meshWalkCount, pSceneView, pSceneLayer, pUnk,
				original, mt, false,
				Config::colArmChams.x, Config::colArmChams.y,
				Config::colArmChams.z, Config::colArmChams.w, false))
				return r;
		}
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	if (target == LOCAL) {
		if (Config::localChams) {
			const int mt = ClampMatType(Config::localChamsMaterial);
			if (void* r = DrawChamsPass(a1, a2, pMeshScene, meshWalkCount, pSceneView, pSceneLayer, pUnk,
				original, mt, false,
				Config::colLocalChams.x, Config::colLocalChams.y,
				Config::colLocalChams.z, Config::colLocalChams.w, false))
				return r;
		}
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	if (target == RAGDOLL) {
		if (Config::ragdollChams) {
			const int mt = ClampMatType(Config::ragdollChamsMaterial);
			if (void* r = DrawChamsPass(a1, a2, pMeshScene, meshWalkCount, pSceneView, pSceneLayer, pUnk,
				original, mt, false,
				Config::colRagdollChams.x, Config::colRagdollChams.y,
				Config::colRagdollChams.z, Config::colRagdollChams.w, false))
				return r;
		}
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	const bool isEnemy = (target == ENEMY);
	const bool isTeam = (target == TEAM);
	const bool wantXQZ = isEnemy ? Config::enemyChamsInvisible
		: (isTeam && Config::teamChamsInvisible);
	const bool wantVis = isEnemy ? Config::enemyChams
		: (isTeam && Config::teamChams);

	if (!wantXQZ && !wantVis)
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);

	const int matXQZType = ClampMatType(Config::chamsMaterialXQZ);
	const int matVisType = ClampMatType(Config::chamsMaterial);
	const ImVec4& colXQZ = isEnemy ? Config::colVisualChamsIgnoreZ : Config::teamcolVisualChamsIgnoreZ;
	const ImVec4& colVis = isEnemy ? Config::colVisualChams : Config::teamcolVisualChams;

	// Two-pass: XQZ first (through walls), then visible covers LOS pixels.
	// dualPass dims XQZ alpha + solidifies vis so colors don't mud together.
	// Animated mats add a second DrawArray (UV-scroll fresnel overlay).
	const bool dualPass = wantXQZ && wantVis;
	void* ret = nullptr;
	if (wantXQZ) {
		if (void* r = DrawChamsPass(a1, a2, pMeshScene, meshWalkCount, pSceneView, pSceneLayer, pUnk,
			original, matXQZType, true,
			colXQZ.x, colXQZ.y, colXQZ.z, colXQZ.w, dualPass))
			ret = r;
	}

	if (wantVis) {
		if (void* r = DrawChamsPass(a1, a2, pMeshScene, meshWalkCount, pSceneView, pSceneLayer, pUnk,
			original, matVisType, false,
			colVis.x, colVis.y, colVis.z, colVis.w, dualPass))
			return r;
		return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	}

	if (ret)
		return ret;
	return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
}
} // namespace chams

void* __fastcall chams::hook(void* a1, void* a2, CMeshData* pMeshScene, int nMeshCount, void* pSceneView, void* pSceneLayer, void* pUnk)
{
	using Fn = void* (__fastcall*)(void*, void*, CMeshData*, int, void*, void*, void*);
	__try {
		return chams::sehChamsHook(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("chams::hook", GetExceptionCode());
		auto original = reinterpret_cast<Fn>(H::DrawArray.GetOriginal());
		if (original)
			return original(a1, a2, pMeshScene, nMeshCount, pSceneView, pSceneLayer, pUnk);
		return nullptr;
	}
}
