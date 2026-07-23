#include "custom_paint.h"
#include "skinchanger.h"
#include "skin_preview.h"

#include "../../config/config.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/security/vacdetect.h"
#include "../../../../external/safetyhook/safetyhook.hpp"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace CustomPaint {
namespace {

// Schema: CompositeMaterialInputLooseVariable_t size 0x288
// m_strName @ 0x0 (CUtlString), m_nVariableType @ 0x40, m_cValueColor4 @ 0x8C
// LOOSE_VARIABLE_TYPE_COLOR4 = 9
// CPaintKit: id@0, name@8, style@0x48, color0..3@0x4C (IDA sub_18109D630)

enum : int32_t {
	kLooseVarFloat3 = 7,
	kLooseVarColor4 = 9,
};
constexpr std::uintptr_t kOffPaintKits = 0x2F0;
constexpr std::uintptr_t kOffKitStyle  = 0x48;
constexpr std::uintptr_t kOffKitColors = 0x4C;

#pragma pack(push, 1)
struct CompositeMaterialInputLooseVariable_t {
	char*    m_strName;              // 0x00 CUtlString
	uint8_t  pad_008[0x38];
	int32_t  m_nVariableType;        // 0x40
	uint8_t  pad_044[0x18];
	float    m_flValueFloatX;        // 0x5C  (also used by FLOAT4 mats)
	float    m_flValueFloatX_Min;    // 0x60
	float    m_flValueFloatX_Max;    // 0x64
	float    m_flValueFloatY;        // 0x68
	float    m_flValueFloatY_Min;    // 0x6C
	float    m_flValueFloatY_Max;    // 0x70
	float    m_flValueFloatZ;        // 0x74
	float    m_flValueFloatZ_Min;    // 0x78
	float    m_flValueFloatZ_Max;    // 0x7C
	float    m_flValueFloatW;        // 0x80
	float    m_flValueFloatW_Min;    // 0x84
	float    m_flValueFloatW_Max;    // 0x88
	uint32_t m_cValueColor4;         // 0x8C
	uint8_t  pad_090[0x288 - 0x90];
};
#pragma pack(pop)
static_assert(sizeof(CompositeMaterialInputLooseVariable_t) == 0x288, "loose var size");
static_assert(offsetof(CompositeMaterialInputLooseVariable_t, m_nVariableType) == 0x40, "type off");
static_assert(offsetof(CompositeMaterialInputLooseVariable_t, m_flValueFloatX) == 0x5C, "floatx off");
static_assert(offsetof(CompositeMaterialInputLooseVariable_t, m_cValueColor4) == 0x8C, "color off");

struct PaintKitNode {
	int left, right, pad8, pad12, key, pad20;
	void* value;
};
static_assert(sizeof(PaintKitNode) == 32, "node");

struct PaintKitMap {
	int count;
	int capacity_flags;
	PaintKitNode* elements;
};

using AppendFn = void(__fastcall*)(void* vector, const CompositeMaterialInputLooseVariable_t* var);
using AllocFn  = void*(__fastcall*)(size_t);

static AppendFn              g_append = nullptr;
static AllocFn               g_alloc  = nullptr;
static safetyhook::MidHook   g_midWeapon{};
static safetyhook::MidHook   g_midGlove{};
static bool                  g_ready  = false;
static volatile LONG         g_midHits = 0;
static volatile LONG         g_injectHits = 0;
static volatile LONG         g_resolveFail = 0;

static void* FindPaintKitPtr(int paintKitId);

static uint8_t ClampU8(float v) {
	if (v < 0.f) v = 0.f;
	if (v > 1.f) v = 1.f;
	return static_cast<uint8_t>(v * 255.f + 0.5f);
}

static uint32_t PackColor(float r, float g, float b, float /*a*/) {
	// Always opaque — half-alpha (seed 0x80) made solid_color look washed / wrong.
	return static_cast<uint32_t>(ClampU8(r))
		| (static_cast<uint32_t>(ClampU8(g)) << 8)
		| (static_cast<uint32_t>(ClampU8(b)) << 16)
		| 0xFF000000u;
}

static uint32_t PackColorArr(const float* rgba) {
	return PackColor(rgba[0], rgba[1], rgba[2], rgba[3]);
}

static bool IsKnifeDef(std::uint16_t d) {
	return d == 42 || d == 59 || (d >= 500 && d <= 526);
}

static bool IsGloveDef(std::uint16_t d) {
	// Broken Fang=4725, Bloodhound..Hydra=5027..5035, Operation gloves range
	if (d == 4725) return true;
	if (d >= 5027 && d <= 5035) return true;
	// Some builds stamp glove econ def on world model outside that set
	if (d >= 4700 && d <= 5100) return true;
	return false;
}

// Pure white / unset pickers — inject would wash the whole mesh white.
static bool ColorsAreUnsetWhite(const float* rgba16) {
	if (!rgba16) return true;
	for (int i = 0; i < 4; ++i) {
		if (rgba16[i * 4 + 0] < 0.97f || rgba16[i * 4 + 1] < 0.97f
			|| rgba16[i * 4 + 2] < 0.97f)
			return false;
	}
	return true;
}

// Midhook fast-bail: checkbox on only (seed may still be pending).
static bool AnyCustomColorActive() {
	if (Config::knife_custom_color || Config::glove_custom_color)
		return true;
	if (Config::custom_paint_color)
		return true;
	if (!Config::weapon_skins)
		return false;
	for (int d = 1; d <= 70; ++d) {
		if (Config::weapon_skin[d].custom_color)
			return true;
	}
	return false;
}

// pathHint: 0=auto, 1=weapon mid, 2=glove mid
// REQUIRE colors_active (seed finished) OR userEdited — never inject default white.
static const char* ResolvePath(void* weapon, std::uint16_t def, int pathHint,
	const float*& cols, bool& enabled)
{
	cols = nullptr;
	enabled = false;

	if (pathHint == 2) {
		if (Config::glove_custom_color
			&& (Config::glove_colors_active || Config::glove_colors_edited)
			&& !ColorsAreUnsetWhite(Config::glove_colors)) {
			enabled = true;
			cols = Config::glove_colors;
			return "glove";
		}
		return "off";
	}

	if (IsKnifeDef(def) && Config::knife_custom_color
		&& (Config::knife_colors_active || Config::knife_colors_edited)
		&& !ColorsAreUnsetWhite(Config::knife_colors)) {
		enabled = true;
		cols = Config::knife_colors;
		return "knife";
	}
	if (IsGloveDef(def) && Config::glove_custom_color
		&& (Config::glove_colors_active || Config::glove_colors_edited)
		&& !ColorsAreUnsetWhite(Config::glove_colors)) {
		enabled = true;
		cols = Config::glove_colors;
		return "glove";
	}
	if (def >= 1 && def <= 70 && !IsKnifeDef(def) && Config::weapon_skins
		&& Config::weapon_skin[def].custom_color
		&& (Config::weapon_skin[def].colors_active || Config::weapon_skin[def].colors_edited)
		&& !ColorsAreUnsetWhite(Config::weapon_skin[def].colors)) {
		enabled = true;
		cols = Config::weapon_skin[def].colors;
		return "weapon";
	}
	// Glove composite owner often has def=0 on weapon mid — still inject gloves.
	if (Config::glove_custom_color
		&& (Config::glove_colors_active || Config::glove_colors_edited)
		&& !ColorsAreUnsetWhite(Config::glove_colors)
		&& !IsKnifeDef(def) && !(def >= 1 && def <= 70)) {
		enabled = true;
		cols = Config::glove_colors;
		return "glove";
	}
	if (Config::custom_paint_color) {
		enabled = true;
		cols = nullptr;
		return "global";
	}
	(void)weapon;
	return "off";
}

static bool ResolveColors(void* weapon, int pathHint, uint32_t out[4],
	std::uint16_t* outDef, const char** outPath)
{
	std::uint16_t def = 0;
	if (weapon) {
		__try {
			def = SkinChanger::Internal::DefIndexOf(weapon);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			def = 0;
		}
	}
	if (outDef) *outDef = def;

	const float* cols = nullptr;
	bool enabled = false;
	const char* path = ResolvePath(weapon, def, pathHint, cols, enabled);
	if (outPath) *outPath = path;
	if (!enabled) return false;

	if (cols) {
		// Final white guard — never wash mesh
		if (ColorsAreUnsetWhite(cols))
			return false;
		out[0] = PackColorArr(cols + 0);
		out[1] = PackColorArr(cols + 4);
		out[2] = PackColorArr(cols + 8);
		out[3] = PackColorArr(cols + 12);
	} else {
		out[0] = PackColor(Config::custom_color0.x, Config::custom_color0.y, Config::custom_color0.z, Config::custom_color0.w);
		out[1] = PackColor(Config::custom_color1.x, Config::custom_color1.y, Config::custom_color1.z, Config::custom_color1.w);
		out[2] = PackColor(Config::custom_color2.x, Config::custom_color2.y, Config::custom_color2.z, Config::custom_color2.w);
		out[3] = PackColor(Config::custom_color3.x, Config::custom_color3.y, Config::custom_color3.z, Config::custom_color3.w);
	}
	return true;
}

static char* Tier0Dup(const char* s) {
	if (!g_alloc || !s) return nullptr;
	const size_t n = std::strlen(s) + 1;
	char* p = static_cast<char*>(g_alloc(n));
	if (p) std::memcpy(p, s, n);
	return p;
}

// CUtlVector<LooseVar>: size@+0, memory@+8, capacity@+16, grow@+20; elem size 0x288
struct LooseVarVector {
	int     size;
	int     pad4;
	uint8_t* memory;
	int     capacity;
	int     grow_flags;
};

// UC: only type + m_cValueColor4. Keep float fill as extra for FLOAT4 mats.
static void FillColorFields(CompositeMaterialInputLooseVariable_t& v, uint32_t rgba, int32_t type = kLooseVarColor4) {
	v.m_nVariableType = type;
	v.m_cValueColor4  = rgba;
	const float r = (rgba & 0xFF) / 255.f;
	const float g = ((rgba >> 8) & 0xFF) / 255.f;
	const float b = ((rgba >> 16) & 0xFF) / 255.f;
	const float a = ((rgba >> 24) & 0xFF) / 255.f;
	v.m_flValueFloatX = r; v.m_flValueFloatY = g; v.m_flValueFloatZ = b; v.m_flValueFloatW = a;
	v.m_flValueFloatX_Min = 0.f; v.m_flValueFloatX_Max = 1.f;
	v.m_flValueFloatY_Min = 0.f; v.m_flValueFloatY_Max = 1.f;
	v.m_flValueFloatZ_Min = 0.f; v.m_flValueFloatZ_Max = 1.f;
	v.m_flValueFloatW_Min = 0.f; v.m_flValueFloatW_Max = 1.f;
}

// Gloves sample g_vColorTint as FLOAT3 (UC) — not COLOR4.
static void FillFloat3Fields(CompositeMaterialInputLooseVariable_t& v, float r, float g, float b) {
	v.m_nVariableType = kLooseVarFloat3;
	v.m_flValueFloatX = r; v.m_flValueFloatY = g; v.m_flValueFloatZ = b; v.m_flValueFloatW = 1.f;
	v.m_flValueFloatX_Min = 0.f; v.m_flValueFloatX_Max = 1.f;
	v.m_flValueFloatY_Min = 0.f; v.m_flValueFloatY_Max = 1.f;
	v.m_flValueFloatZ_Min = 0.f; v.m_flValueFloatZ_Max = 1.f;
	v.m_flValueFloatW_Min = 0.f; v.m_flValueFloatW_Max = 1.f;
	v.m_cValueColor4 = PackColor(r, g, b, 1.f);
}

static bool SetNamedColor(void* vector, const char* name, uint32_t rgba, int32_t type = kLooseVarColor4) {
	if (!vector || !name || !g_append) return false;

	__try {
		auto* vec = reinterpret_cast<LooseVarVector*>(vector);
		if (vec->memory && vec->size > 0 && vec->size < 256) {
			for (int i = 0; i < vec->size; ++i) {
				auto* elem = reinterpret_cast<CompositeMaterialInputLooseVariable_t*>(
					vec->memory + (size_t)i * 0x288);
				const char* n = elem->m_strName;
				if (!n || !Mem::IsReadable(n, 1)) continue;
				if (std::strcmp(n, name) != 0) continue;
				FillColorFields(*elem, rgba, type);
				return true;
			}
		}

		CompositeMaterialInputLooseVariable_t v{};
		v.m_strName = Tier0Dup(name);
		if (!v.m_strName) {
			Con::Rate("cp_alloc", 1000, "CustomPaint: Tier0Dup failed for %s", name);
			return false;
		}
		FillColorFields(v, rgba, type);
		g_append(vector, &v);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Rate("cp_app_seh", 1000, "CustomPaint: set color SEH 0x%08X name=%s",
			GetExceptionCode(), name);
		return false;
	}
}

static bool SetNamedFloat3(void* vector, const char* name, float r, float g, float b) {
	if (!vector || !name || !g_append) return false;

	__try {
		auto* vec = reinterpret_cast<LooseVarVector*>(vector);
		if (vec->memory && vec->size > 0 && vec->size < 256) {
			for (int i = 0; i < vec->size; ++i) {
				auto* elem = reinterpret_cast<CompositeMaterialInputLooseVariable_t*>(
					vec->memory + (size_t)i * 0x288);
				const char* n = elem->m_strName;
				if (!n || !Mem::IsReadable(n, 1)) continue;
				if (std::strcmp(n, name) != 0) continue;
				FillFloat3Fields(*elem, r, g, b);
				return true;
			}
		}

		CompositeMaterialInputLooseVariable_t v{};
		v.m_strName = Tier0Dup(name);
		if (!v.m_strName) {
			Con::Rate("cp_alloc", 1000, "CustomPaint: Tier0Dup failed for %s", name);
			return false;
		}
		FillFloat3Fields(v, r, g, b);
		g_append(vector, &v);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Rate("cp_app_seh", 1000, "CustomPaint: set float3 SEH 0x%08X name=%s",
			GetExceptionCode(), name);
		return false;
	}
}

// Dump loose-var names already in the econ_instance vector (wear/seed + any prior).
static void LogVectorSnapshot(void* vector, const char* tag) {
	if (!vector) return;
	__try {
		auto* vec = reinterpret_cast<LooseVarVector*>(vector);
		Con::Info("CustomPaint %s vec size=%d cap=%d mem=%p",
			tag, vec->size, vec->capacity, (void*)vec->memory);
		if (!vec->memory || vec->size <= 0 || vec->size > 64) return;
		for (int i = 0; i < vec->size; ++i) {
			auto* elem = reinterpret_cast<CompositeMaterialInputLooseVariable_t*>(
				vec->memory + (size_t)i * 0x288);
			const char* n = elem->m_strName;
			if (!n || !Mem::IsReadable(n, 1)) n = "?";
			Con::Info("  [%d] name=%s type=%d color=%08X",
				i, n, elem->m_nVariableType, elem->m_cValueColor4);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Only touch exact g_vColor0..3. Never rewrite random COLOR4 / "olor" names
// (wear helpers, mask tints, etc.) — that crushed albedo on many paints.
static void OverwriteNamedGvColors(void* vector, const uint32_t colors[4])
{
	if (!vector || !colors)
		return;
	__try {
		auto* vec = reinterpret_cast<LooseVarVector*>(vector);
		if (!vec->memory || vec->size <= 0 || vec->size > 256)
			return;
		int n = 0;
		for (int i = 0; i < vec->size; ++i) {
			auto* elem = reinterpret_cast<CompositeMaterialInputLooseVariable_t*>(
				vec->memory + (size_t)i * 0x288);
			const char* nname = elem->m_strName;
			if (!nname || !Mem::IsReadable(nname, 9))
				continue;
			// g_vColor0 .. g_vColor3 only
			if (std::strncmp(nname, "g_vColor", 8) != 0)
				continue;
			if (nname[8] < '0' || nname[8] > '3' || nname[9] != '\0')
				continue;
			const int idx = nname[8] - '0';
			FillColorFields(*elem, colors[idx], kLooseVarColor4);
			++n;
		}
		if (n > 0)
			Con::Rate("cp_ovw", 2000, "CustomPaint: overwrote %d g_vColorN", n);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Lift dark inject channels so pattern albedo is not crushed when seed is dark.
static void LiftInjectU32(uint32_t colors[4], bool aggressive)
{
	if (!colors) return;
	const float floorV = aggressive ? 0.72f : 0.55f;
	for (int i = 0; i < 4; ++i) {
		float r = (colors[i] & 0xFF) / 255.f;
		float g = ((colors[i] >> 8) & 0xFF) / 255.f;
		float b = ((colors[i] >> 16) & 0xFF) / 255.f;
		const float mx = (std::max)(r, (std::max)(g, b));
		if (mx < 1e-4f) {
			r = g = b = floorV;
		} else if (mx < floorV) {
			const float s = floorV / mx;
			r = (std::min)(1.f, r * s);
			g = (std::min)(1.f, g * s);
			b = (std::min)(1.f, b * s);
		}
		colors[i] = PackColor(r, g, b, 1.f);
	}
}

static void AppendColorVars(void* vector, const uint32_t colors[4], bool isGlove) {
	if (!vector || !colors) return;
	OverwriteNamedGvColors(vector, colors);
	// IDA solid_color / gunsmith: g_vColor0..3 (string "g_vColor" + digit)
	for (int i = 0; i < 4; ++i) {
		char name[16];
		std::snprintf(name, sizeof(name), "g_vColor%d", i);
		SetNamedColor(vector, name, colors[i], kLooseVarColor4);
	}
	// Also bare g_vColor (some glove mats) as color0
	SetNamedColor(vector, "g_vColor", colors[0], kLooseVarColor4);
	// Celerity glove path: g_vGloveColorTop/Bottom + UC g_vColorTint FLOAT3.
	if (isGlove) {
		for (int i = 0; i < 4; ++i) {
			char top[32], bot[32];
			std::snprintf(top, sizeof(top), "g_vGloveColorTop%d", i);
			std::snprintf(bot, sizeof(bot), "g_vGloveColorBottom%d", i);
			SetNamedColor(vector, top, colors[i], kLooseVarColor4);
			SetNamedColor(vector, bot, colors[i], kLooseVarColor4);
		}
		const float r0 = (colors[0] & 0xFF) / 255.f;
		const float g0 = ((colors[0] >> 8) & 0xFF) / 255.f;
		const float b0 = ((colors[0] >> 16) & 0xFF) / 255.f;
		const float r1 = (colors[1] & 0xFF) / 255.f;
		const float g1 = ((colors[1] >> 8) & 0xFF) / 255.f;
		const float b1 = ((colors[1] >> 16) & 0xFF) / 255.f;
		SetNamedFloat3(vector, "g_vColorTint", r0, g0, b0);
		// Some glove paints use color0/color1 as top/bottom tints
		SetNamedFloat3(vector, "g_vColor0", r0, g0, b0);
		SetNamedFloat3(vector, "g_vColor1", r1, g1, b1);
	}
}

static void MaybeWriteKitForPath(const char* path, std::uint16_t def, bool userEdited);
static bool UserEditedForPath(const char* path, std::uint16_t def);
static void RestoreKitColorsImpl(int paintKitId);
static void EnsureCompositeStyleForInject(int paintKitId);

// Snapshot ORIGINAL color0..3 once — defined early so OnBuildMaterial can read style.
struct KitColorSnap {
	int id = 0;
	int style = 0;
	uint8_t raw[16]{};
	bool filler = true;
	bool ok = false;
};
static KitColorSnap* FindSnap(int paintKitId);
static KitColorSnap* EnsureSnap(int paintKitId);
static bool StyleIsTexturePaintJob(int style) {
	return style == 6 || style == 7;
}

static void OnBuildMaterial(safetyhook::Context& ctx, int pathHint) {
	// Fast bail: midhook hits every composite build — no work/logs when all off.
	if (!AnyCustomColorActive())
		return;

	InterlockedIncrement(&g_midHits);

	if (VacDetect::IsSoftPaused())
		return;
	if (!g_append || !g_alloc)
		return;

	void* vector = reinterpret_cast<void*>(ctx.r8);
	// Weapon path: rbx = weapon. Glove path: rdi = parent entity, rbx = world-model gloves.
	void* entity = (pathHint == 2)
		? reinterpret_cast<void*>(ctx.rdi)
		: reinterpret_cast<void*>(ctx.rbx);

	if (!vector)
		return;

	uint32_t colors[4]{};
	std::uint16_t def = 0;
	const char* path = "n/a";
	if (!ResolveColors(entity, pathHint, colors, &def, &path)) {
		// Glove mid: also try rbx if rdi failed
		if (pathHint == 2 && ctx.rbx) {
			if (ResolveColors(reinterpret_cast<void*>(ctx.rbx), pathHint, colors, &def, &path))
				goto inject;
		}
		InterlockedIncrement(&g_resolveFail);
		return;
	}

inject:
	// Resolve paint kit id for optional schema kit write + logging
	int paintKitId = 0;
	if (path && std::strcmp(path, "knife") == 0)
		paintKitId = Config::knife_paint_kit_id;
	else if (path && std::strcmp(path, "glove") == 0)
		paintKitId = Config::glove_paint_kit_id;
	else if (path && std::strcmp(path, "weapon") == 0) {
		const int d = (def > 0 && def < 71) ? (int)def : Config::weapon_selected;
		if (d > 0 && d < 71) paintKitId = Config::weapon_skin[d].paint_kit_id;
	}
	if (paintKitId <= 0 && pathHint == 2)
		paintKitId = Config::glove_paint_kit_id;

	const bool userEdited = UserEditedForPath(path, def);
	const bool isGlove = (path && std::strcmp(path, "glove") == 0) || pathHint == 2;

	// Texture paint jobs (Ice Coaled = style 7): midhook inject multiplies g_vColor onto
	// texture and washes white unless user deliberately edited pickers.
	int kitStyle = -1;
	if (paintKitId > 0) {
		if (KitColorSnap* s = FindSnap(paintKitId))
			kitStyle = s->style;
		else if (KitColorSnap* s = EnsureSnap(paintKitId))
			kitStyle = s->style;
	}
	if (!isGlove && StyleIsTexturePaintJob(kitStyle) && !userEdited)
		return; // leave stock texture alone

	// Gunsmith only — never remap 6/7 (that was the Ice Coaled → white bug).
	if (paintKitId > 0 && !isGlove)
		EnsureCompositeStyleForInject(paintKitId);

	// Kit schema write for solid/gunsmith; user edit opens pattern tint write.
	MaybeWriteKitForPath(path, def, userEdited);

	// Lift greys for solid styles only. Texture jobs: use raw picker (user tint).
	if (!isGlove && !StyleIsTexturePaintJob(kitStyle))
		LiftInjectU32(colors, /*aggressive=*/!userEdited);
	else if (isGlove && !userEdited)
		LiftInjectU32(colors, /*aggressive=*/false);

	AppendColorVars(vector, colors, isGlove);

	const LONG inj = InterlockedIncrement(&g_injectHits);
	// Rare debug only — was spamming console every composite rebuild.
	if (inj <= 3 || (inj % 300) == 0) {
		Con::Rate("cp_inj", 2000,
			"CustomPaint INJECT#%ld def=%u path=%s kit=%d glove=%d edit=%d",
			inj, (unsigned)def, path, paintKitId, (int)isGlove, (int)userEdited);
	}
}

static void OnWeaponMid(safetyhook::Context& ctx) { OnBuildMaterial(ctx, 1); }
static void OnGloveMid(safetyhook::Context& ctx)  { OnBuildMaterial(ctx, 2); }

static PaintKitMap* GetPaintMap() {
	void* schema = SkinChanger::Internal::ItemSchema();
	if (!schema || !Mem::IsUserPtr(schema)) return nullptr;
	__try {
		auto* map = reinterpret_cast<PaintKitMap*>((std::uint8_t*)schema + kOffPaintKits);
		if (!map || map->count <= 0 || map->count > 20000) return nullptr;
		if (!map->elements || !Mem::IsUserPtr(map->elements)) return nullptr;
		return map;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static void* FindPaintKitPtr(int paintKitId) {
	if (paintKitId <= 0) return nullptr;
	PaintKitMap* map = GetPaintMap();
	if (!map) return nullptr;

	__try {
		for (int i = 0; i < map->count; ++i) {
			if (map->elements[i].key != paintKitId) continue;
			void* kit = map->elements[i].value;
			if (kit && Mem::IsUserPtr(kit) && Mem::IsReadable(kit, kOffKitColors + 16))
				return kit;
		}
		for (int i = 0; i < map->count; ++i) {
			void* kit = map->elements[i].value;
			if (!kit || !Mem::IsUserPtr(kit) || !Mem::IsReadable(kit, 4)) continue;
			if (*(int*)kit != paintKitId) continue;
			if (Mem::IsReadable(kit, kOffKitColors + 16))
				return kit;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

static bool RawIsFillerColors(const uint8_t raw[16]) {
	for (int i = 0; i < 16; ++i) {
		if (raw[i] != 0x80)
			return false;
	}
	return true;
}

// KitColorSnap declared above OnBuildMaterial.
constexpr int kSnapSlots = 64;
static KitColorSnap g_snaps[kSnapSlots]{};

static KitColorSnap* FindSnap(int paintKitId) {
	for (int i = 0; i < kSnapSlots; ++i) {
		if (g_snaps[i].ok && g_snaps[i].id == paintKitId)
			return &g_snaps[i];
	}
	return nullptr;
}

static KitColorSnap* EnsureSnap(int paintKitId) {
	if (KitColorSnap* s = FindSnap(paintKitId))
		return s;
	void* kit = FindPaintKitPtr(paintKitId);
	if (!kit) return nullptr;

	KitColorSnap tmp{};
	tmp.id = paintKitId;
	__try {
		tmp.style = *(int*)((std::uint8_t*)kit + kOffKitStyle);
		std::memcpy(tmp.raw, (std::uint8_t*)kit + kOffKitColors, 16);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	tmp.filler = RawIsFillerColors(tmp.raw);
	// Pattern / texture finishes (2 hydro, 3 spray, 5 airbrush, 6/7 custom paint job):
	// color0..3 are often 0x80 filler OR junk that is NOT the visible palette.
	// Do NOT wipe live kit colours here — that nuked stock look on some kits.
	// Only mark filler so Seed prefers preview sample over raw kit bytes.
	const bool patternStyle =
		tmp.style == 2 || tmp.style == 3 || tmp.style == 5
		|| tmp.style == 6 || tmp.style == 7;
	if (patternStyle)
		tmp.filler = true; // seed via preview; leave schema colors alone
	// Undo legacy force-to-9 / force-to-8 so texture paint jobs render stock again.
	__try {
		const int liveStyle = *(int*)((std::uint8_t*)kit + kOffKitStyle);
		if (liveStyle != tmp.style
			&& (liveStyle == 8 || liveStyle == 9)
			&& (tmp.style == 7 || tmp.style == 6 || tmp.style == 2
				|| tmp.style == 3 || tmp.style == 5 || tmp.style == 9))
			*(int*)((std::uint8_t*)kit + kOffKitStyle) = tmp.style;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	tmp.ok = true;

	for (int i = 0; i < kSnapSlots; ++i) {
		if (!g_snaps[i].ok) {
			g_snaps[i] = tmp;
			return &g_snaps[i];
		}
	}
	static int s_rr = 0;
	g_snaps[s_rr++ % kSnapSlots] = tmp;
	return &g_snaps[(s_rr - 1) % kSnapSlots];
}

// Styles that sample CPaintKit color0..3 as paint (solid / gunsmith / patina / gunsmith).
// IDA CPaintKit parse: style@0x48, color0@0x4C..color3@0x58 (RGBA bytes).
// Pattern styles (2,3,5,6,7) ignore kit colours — only loose g_vColor midhook works
// when the composite path runs (IDA: styles 0-6,8 only; style 7 skips entirely).
static bool StyleUsesKitColors(int style)
{
	return style == 0 || style == 1 || style == 4 || style == 8 || style == 9;
}

// IDA UpdateCompositeMaterialSet (0x1807C5CA0): switch style 0-6,8 → econ_instance midhook.
// Style 7 (custom_paint_job / Ice Coaled etc.) SKIPS the midhook — that is correct:
// those paints are texture-driven; remapping to style 8 (patina) washes them WHITE
// because patina multiplies g_vColor onto a blank base. NEVER remap 6/7.
// Only style 9 (gunsmith) skips midhook but still uses kit colours — map 9→8.
static void EnsureCompositeStyleForInject(int paintKitId)
{
	if (paintKitId <= 0) return;
	KitColorSnap* snap = FindSnap(paintKitId);
	if (!snap) snap = EnsureSnap(paintKitId);
	if (!snap) return;
	// 0-6,8 already build econ_instance
	if (snap->style >= 0 && snap->style <= 6)
		return;
	if (snap->style == 8)
		return;
	// Texture paint jobs: leave style alone (no midhook — inject won't help / would break)
	if (snap->style == 6 || snap->style == 7)
		return;
	// Gunsmith only: force midhook-eligible style
	if (snap->style != 9)
		return;
	void* kit = FindPaintKitPtr(paintKitId);
	if (!kit) return;
	__try {
		*(int*)((uint8_t*)kit + kOffKitStyle) = 8;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Write color0..3 into live CPaintKit. For pattern styles still write when user
// edited (some materials sample kit colours as tint even on hydrographic).
// NEVER force style → 9 (kills midhook).
static void WriteKitColors(int paintKitId, const float* rgba16, bool allowFillerWrite) {
	if (paintKitId <= 0 || !rgba16) return;
	KitColorSnap* snap = FindSnap(paintKitId);
	if (!snap) snap = EnsureSnap(paintKitId);
	if (!snap) return;
	// Pattern: only write on explicit user edit (tint path)
	if (!StyleUsesKitColors(snap->style) && !allowFillerWrite)
		return;
	if (snap->filler && !allowFillerWrite)
		return;

	void* kit = FindPaintKitPtr(paintKitId);
	if (!kit) return;
	__try {
		uint8_t* base = (uint8_t*)kit + kOffKitColors;
		for (int i = 0; i < 4; ++i) {
			base[i * 4 + 0] = ClampU8(rgba16[i * 4 + 0]);
			base[i * 4 + 1] = ClampU8(rgba16[i * 4 + 1]);
			base[i * 4 + 2] = ClampU8(rgba16[i * 4 + 2]);
			base[i * 4 + 3] = 0xFF;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void RestoreKitColorsImpl(int paintKitId) {
	if (paintKitId <= 0) return;
	KitColorSnap* snap = FindSnap(paintKitId);
	if (!snap) return;
	void* kit = FindPaintKitPtr(paintKitId);
	if (!kit) return;
	__try {
		// Always restore style (undo any legacy force-to-9) + original colours.
		*(int*)((uint8_t*)kit + kOffKitStyle) = snap->style;
		if (snap->filler)
			std::memset((uint8_t*)kit + kOffKitColors, 0x80, 16);
		else
			std::memcpy((uint8_t*)kit + kOffKitColors, snap->raw, 16);
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void MaybeWriteKitForPath(const char* path, std::uint16_t def, bool userEdited) {
	if (!path) return;
	// Solid/gunsmith: write kit colours when seeded. Pattern/filler: only on user edit
	// (writing filler 0x80→user white washed Ice Coaled-class paints).
	auto writeFor = [&](int paintId, const float* cols, bool active) {
		if (paintId <= 0 || !cols) return;
		KitColorSnap* s = FindSnap(paintId);
		if (!s) s = EnsureSnap(paintId);
		const bool named = s && !s->filler && StyleUsesKitColors(s->style);
		if (named && (active || userEdited))
			WriteKitColors(paintId, cols, true);
		else if (userEdited)
			WriteKitColors(paintId, cols, true);
	};
	if (std::strcmp(path, "knife") == 0)
		writeFor(Config::knife_paint_kit_id, Config::knife_colors, Config::knife_colors_active);
	else if (std::strcmp(path, "glove") == 0)
		writeFor(Config::glove_paint_kit_id, Config::glove_colors, Config::glove_colors_active);
	else if (std::strcmp(path, "weapon") == 0) {
		const int d = (def > 0 && def < 71) ? (int)def : Config::weapon_selected;
		if (d > 0 && d < 71)
			writeFor(Config::weapon_skin[d].paint_kit_id, Config::weapon_skin[d].colors,
				Config::weapon_skin[d].colors_active);
	}
}

static bool UserEditedForPath(const char* path, std::uint16_t def) {
	if (!path) return false;
	if (std::strcmp(path, "knife") == 0) return Config::knife_colors_edited;
	if (std::strcmp(path, "glove") == 0) return Config::glove_colors_edited;
	if (std::strcmp(path, "weapon") == 0) {
		const int d = (def > 0 && def < 71) ? (int)def : Config::weapon_selected;
		if (d > 0 && d < 71) return Config::weapon_skin[d].colors_edited;
	}
	// Global / unknown: treat as edited so named-kit writes can run when applicable
	if (std::strcmp(path, "global") == 0) return true;
	return false;
}

} // namespace

void SetNeutral(float outRgba[16]) {
	if (!outRgba) return;
	for (int i = 0; i < 4; ++i) {
		outRgba[i * 4 + 0] = 1.f;
		outRgba[i * 4 + 1] = 1.f;
		outRgba[i * 4 + 2] = 1.f;
		outRgba[i * 4 + 3] = 1.f;
	}
}

void RestoreKitColors(int paintKitId) {
	RestoreKitColorsImpl(paintKitId);
}

// Preview sampling pulls shadow greys — lift hard so enable does not darken.
static void LiftPaintPalette(float outRgba[16], int style) {
	if (!outRgba) return;
	for (int i = 0; i < 4; ++i) {
		float* c = outRgba + i * 4;
		const float mx = (std::max)(c[0], (std::max)(c[1], c[2]));
		if (mx < 1e-4f) {
			// Never seed pure black into pickers (inject multiplies → black gun).
			c[0] = c[1] = c[2] = (i == 3) ? 0.55f : 0.78f;
			c[3] = 1.f;
			continue;
		}
		// All styles: high floor. Dark c3 from Asiimov outlines was crushing guns.
		float floorV = 0.62f;
		if (style == 9 || style == 0 || style == 1)
			floorV = (i == 3) ? 0.50f : 0.75f;
		else
			floorV = (i == 3) ? 0.48f : 0.68f;
		if (mx < floorV) {
			const float s = floorV / mx;
			c[0] = (std::min)(1.f, c[0] * s);
			c[1] = (std::min)(1.f, c[1] * s);
			c[2] = (std::min)(1.f, c[2] * s);
		}
		c[3] = 1.f;
	}
}

// Filler kits have no baked color0..3 — vary starters by kit id so switching skins
// visibly changes the colour bugs (not the same red/green every time).
static void SetEditableDefaults(float outRgba[16], int style, int paintKitId) {
	if (!outRgba) return;
	float base[16];
	if (style == 9) {
		const float p[16] = {
			1.00f, 0.78f, 0.20f, 1.f,
			0.92f, 0.90f, 0.85f, 1.f,
			0.45f, 0.38f, 0.28f, 1.f,
			0.12f, 0.10f, 0.08f, 1.f,
		};
		std::memcpy(base, p, sizeof(p));
	} else {
		const float p[16] = {
			0.90f, 0.20f, 0.20f, 1.f,
			0.20f, 0.85f, 0.30f, 1.f,
			0.20f, 0.35f, 0.95f, 1.f,
			0.95f, 0.90f, 0.20f, 1.f,
		};
		std::memcpy(base, p, sizeof(p));
	}
	// Rotate channels by kit id so each skin gets a distinct picker set
	const int rot = (paintKitId > 0 ? paintKitId : 0) % 4;
	for (int i = 0; i < 4; ++i) {
		const int s = (i + rot) % 4;
		outRgba[i * 4 + 0] = base[s * 4 + 0];
		outRgba[i * 4 + 1] = base[s * 4 + 1];
		outRgba[i * 4 + 2] = base[s * 4 + 2];
		outRgba[i * 4 + 3] = 1.f;
	}
}

// True if outRgba still default pure white (seed never filled real stock colours).
static bool IsAllWhite(const float rgba[16]) {
	if (!rgba) return true;
	for (int i = 0; i < 4; ++i) {
		if (rgba[i * 4 + 0] < 0.97f || rgba[i * 4 + 1] < 0.97f || rgba[i * 4 + 2] < 0.97f)
			return false;
	}
	return true;
}

SeedStatus SeedFromPaintKit(int paintKitId, float outRgba[16],
	const char* simpleName, const char* kitToken)
{
	if (!outRgba)
		return SeedStatus::Fail;
	// Always start from kit-id palette (never pure white in pickers).
	SetEditableDefaults(outRgba, 0, paintKitId);
	if (paintKitId <= 0) {
		SetNeutral(outRgba);
		return SeedStatus::Fail;
	}

	auto* snap = EnsureSnap(paintKitId);
	if (!snap) {
		// Schema not ready — non-white placeholders, retry for real stock colours
		Con::Rate("cp_seed", 2000, "CustomPaint::Seed kit=%d map miss (pending)", paintKitId);
		return SeedStatus::Pending;
	}
	// Refresh defaults with real style
	SetEditableDefaults(outRgba, snap->style, paintKitId);

	// Named kits (solid / gunsmith / anodized with real color0..3): schema palette.
	if (!snap->filler) {
		for (int i = 0; i < 4; ++i) {
			outRgba[i * 4 + 0] = snap->raw[i * 4 + 0] / 255.f;
			outRgba[i * 4 + 1] = snap->raw[i * 4 + 1] / 255.f;
			outRgba[i * 4 + 2] = snap->raw[i * 4 + 2] / 255.f;
			outRgba[i * 4 + 3] = 1.f;
		}
		// Near-white schema → try panorama sample (Lightning Strike etc. often OK named)
		if (IsAllWhite(outRgba) && simpleName && *simpleName) {
			float prev[16]{};
			if (SkinPreview::SamplePaintPalette(simpleName, kitToken, paintKitId, prev)
				&& !IsAllWhite(prev)) {
				std::memcpy(outRgba, prev, sizeof(float) * 16);
				LiftPaintPalette(outRgba, snap->style);
				Con::Rate("cp_seed", 1500, "CustomPaint::Seed kit=%d style=%d named+preview",
					snap->id, snap->style);
				return SeedStatus::Ok;
			}
			if (SkinPreview::PreviewPending())
				return SeedStatus::Pending; // keep non-white defaults in outRgba
		}
		// 0x80 was misclassified — raw all ~0.5 is filler grey, not real palette
		const bool greyFiller =
			std::fabs(outRgba[0] - 0.5f) < 0.02f
			&& std::fabs(outRgba[1] - 0.5f) < 0.02f
			&& std::fabs(outRgba[2] - 0.5f) < 0.02f;
		if (greyFiller && simpleName && *simpleName) {
			float prev[16]{};
			if (SkinPreview::SamplePaintPalette(simpleName, kitToken, paintKitId, prev)
				&& !IsAllWhite(prev)) {
				std::memcpy(outRgba, prev, sizeof(float) * 16);
				LiftPaintPalette(outRgba, snap->style);
				return SeedStatus::Ok;
			}
			if (SkinPreview::PreviewPending())
				return SeedStatus::Pending;
			SetEditableDefaults(outRgba, snap->style, paintKitId);
		}
		LiftPaintPalette(outRgba, snap->style);
		Con::Rate("cp_seed", 1500, "CustomPaint::Seed kit=%d style=%d named=1",
			snap->id, snap->style);
		return SeedStatus::Ok;
	}

	// Pattern / filler: preview texture = closest to stock look for Ice Coaled etc.
	if (simpleName && *simpleName) {
		if (SkinPreview::SamplePaintPalette(simpleName, kitToken, paintKitId, outRgba)
			&& !IsAllWhite(outRgba)) {
			LiftPaintPalette(outRgba, snap->style);
			Con::Rate("cp_seed", 1500, "CustomPaint::Seed kit=%d style=%d preview sn=%s",
				snap->id, snap->style, simpleName);
			return SeedStatus::Ok;
		}
		if (SkinPreview::PreviewPending()) {
			// Keep kit-id defaults in pickers (not white) while GPU loads
			SetEditableDefaults(outRgba, snap->style, paintKitId);
			LiftPaintPalette(outRgba, snap->style);
			return SeedStatus::Pending;
		}
	} else {
		SetEditableDefaults(outRgba, snap->style, paintKitId);
		LiftPaintPalette(outRgba, snap->style);
		return SeedStatus::Pending;
	}

	// BC / missing preview: distinctive per-kit defaults (never pure white)
	SetEditableDefaults(outRgba, snap->style, paintKitId);
	LiftPaintPalette(outRgba, snap->style);
	Con::Rate("cp_seed", 1500, "CustomPaint::Seed kit=%d style=%d defaults sn=%s",
		snap->id, snap->style, simpleName ? simpleName : "?");
	return SeedStatus::Ok;
}

bool Install() {
	if (g_ready) return true;

	Con::Section("CustomPaint");

	if (HMODULE t0 = GetModuleHandleA("tier0.dll"))
		g_alloc = reinterpret_cast<AllocFn>(GetProcAddress(t0, "MemAlloc_AllocFunc"));
	if (!g_alloc) {
		Con::Error("CustomPaint: MemAlloc_AllocFunc missing");
		return false;
	}
	Con::Ok("CustomPaint alloc @ 0x%p", (void*)g_alloc);

	auto* appendCall = M::FindPattern("client",
		"E8 ? ? ? ? 0F 28 B4 24 ? ? ? ? 4C 39 A5");
	if (!appendCall) {
		Con::Warn("CustomPaint: append pattern A miss, trying B");
		appendCall = M::FindPattern("client",
			"E8 ? ? ? ? 0F 28 B4 24 ? ? ? ? 48 8B");
	}
	if (appendCall)
		g_append = reinterpret_cast<AppendFn>(M::GetAbsoluteAddress(appendCall, 1, 0));
	if (!g_append) {
		Con::Error("CustomPaint: append pattern miss (both)");
		return false;
	}
	Con::Ok("CustomPaint append call @ 0x%p -> fn 0x%p", (void*)appendCall, (void*)g_append);

	// Weapon: UpdateCompositeMaterialSet → lea rdx,"econ_instance" (r8=vector, rbx=weapon)
	auto* weaponMid = M::FindPattern("client",
		"48 8D 15 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 48 8B D0 48 8D 8B");
	if (!weaponMid) {
		Con::Error("CustomPaint: weapon midhook pattern miss");
		return false;
	}
	Con::Ok("CustomPaint weapon mid @ 0x%p", (void*)weaponMid);

	g_midWeapon = safetyhook::create_mid(weaponMid, &OnWeaponMid);
	if (!g_midWeapon) {
		Con::Error("CustomPaint: create_mid weapon failed @ 0x%p", (void*)weaponMid);
		return false;
	}

	// Glove: sub_180BFA550 @ IDA 0x180BFA550
	// Call site: lea r8, vector; lea rdx, "econ_instance"; lea rcx, ...; call sub_180744CF0
	// Prefer unique gloves path (48 8D 4E after call) over multi-hit lea r8 pattern.
	auto* gloveMid = M::FindPattern("client",
		"48 8D 15 ? ? ? ? 48 8D 4D ? E8 ? ? ? ? 48 8B D0 48 8D 4E");
	if (!gloveMid) {
		gloveMid = M::FindPattern("client",
			"4C 8D 44 24 ? 48 8D 15 ? ? ? ? 48 8D 4D ? E8");
		if (gloveMid && gloveMid[0] == 0x4C && gloveMid[1] == 0x8D)
			gloveMid += 5; // land on lea rdx after lea r8
	}
	if (gloveMid) {
		g_midGlove = safetyhook::create_mid(gloveMid, &OnGloveMid);
		if (g_midGlove)
			Con::Ok("CustomPaint glove mid @ 0x%p", (void*)gloveMid);
		else
			Con::Warn("CustomPaint: create_mid glove failed @ 0x%p", (void*)gloveMid);
	} else {
		Con::Warn("CustomPaint: glove mid pattern miss (gloves may not recolor)");
	}

	g_ready = true;
	Con::Ok("CustomPaint READY weapon=%p glove_ok=%d append=%p",
		(void*)weaponMid, (int)(bool)g_midGlove, (void*)g_append);
	return true;
}

bool Ready() { return g_ready; }

} // namespace CustomPaint
