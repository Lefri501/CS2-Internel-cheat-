#include "world.h"
#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/handle.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_Material/C_Material.h"

#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/gaa/gaa.h"

// Nightmode: camera services → post/tonemap exposure.
// Skybox: DrawSkyboxArray (scenesystem) RGB float tint; entity C_EnvSky path as fallback.
// Map color: DrawAggregateSceneObjectArray → shared lightData buffer (Andromeda path).
// Lighting: LightSceneObject (+0xE4 *3) + GlobalLightUpdate (source triples + dirty 0x69).

namespace {
	bool g_wasNight = false;
	bool g_wasSkybox = false;
	bool g_wasLighting = false;

	struct Color4 {
		uint8_t r = 255, g = 255, b = 255, a = 255;
	};

	struct SavedFloats {
		void* ent = nullptr;
		float minExp = 0, maxExp = 0, minLog = 0, maxLog = 0, comp = 0;
		float fadeUp = 0, fadeDown = 0, fadeDur = 0, evSmooth = 0;
		float autoMin = 0, autoMax = 0, adaptUp = 0, adaptDown = 0, toneEvSmooth = 0;
		float skyBright = 1.f;
		Color4 skyTint{};
		Color4 skyTintLight{};
		Color4 lightColor{};
		Color4 ambient1{};
		Color4 ambient2{};
		Color4 ambient3{};
		Color4 specular{};
		bool expCtrl = false, master = false;
		bool isPost = false;
		bool isTone = false;
		bool isSkyBright = false;
		bool isSkyTint = false;
		bool isLight = false;
		bool valid = false;
	};

	constexpr int kMaxSaved = 32;
	SavedFloats g_saved[kMaxSaved];
	int g_savedCount = 0;

	uint32_t Off(const char* field) {
		return SchemaFinder::Get(hash_32_fnv1a_const(field));
	}

	bool ReadFloat(void* ent, const char* field, float& out) {
		const uint32_t off = Off(field);
		if (!off || !ent)
			return false;
		out = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(ent) + off);
		return true;
	}

	bool WriteFloat(void* ent, const char* field, float v) {
		const uint32_t off = Off(field);
		if (!off || !ent)
			return false;
		*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(ent) + off) = v;
		return true;
	}

	bool ReadBool(void* ent, const char* field, bool& out) {
		const uint32_t off = Off(field);
		if (!off || !ent)
			return false;
		out = *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(ent) + off);
		return true;
	}

	bool WriteBool(void* ent, const char* field, bool v) {
		const uint32_t off = Off(field);
		if (!off || !ent)
			return false;
		*reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(ent) + off) = v;
		return true;
	}

	bool ReadColor(void* ent, const char* field, Color4& out) {
		const uint32_t off = Off(field);
		if (!off || !ent)
			return false;
		std::memcpy(&out, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + off), 4);
		return true;
	}

	bool WriteColor(void* ent, const char* field, const Color4& c) {
		const uint32_t off = Off(field);
		if (!off || !ent)
			return false;
		std::memcpy(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + off), &c, 4);
		return true;
	}

	Color4 ImToColor4(const ImVec4& v) {
		Color4 c{};
		c.r = static_cast<uint8_t>(std::clamp(v.x, 0.f, 1.f) * 255.f + 0.5f);
		c.g = static_cast<uint8_t>(std::clamp(v.y, 0.f, 1.f) * 255.f + 0.5f);
		c.b = static_cast<uint8_t>(std::clamp(v.z, 0.f, 1.f) * 255.f + 0.5f);
		c.a = static_cast<uint8_t>(std::clamp(v.w, 0.f, 1.f) * 255.f + 0.5f);
		return c;
	}

	SavedFloats* FindSaved(void* ent) {
		for (int i = 0; i < g_savedCount; ++i)
			if (g_saved[i].ent == ent)
				return &g_saved[i];
		return nullptr;
	}

	SavedFloats* AllocSaved(void* ent) {
		if (SavedFloats* s = FindSaved(ent))
			return s;
		if (g_savedCount >= kMaxSaved)
			return nullptr;
		SavedFloats& s = g_saved[g_savedCount++];
		s = {};
		s.ent = ent;
		return &s;
	}

	void ExposureFromDarkness(float darkness, float& minExp, float& maxExp, float& compensation) {
		const float d = std::clamp(darkness, 0.f, 1.f);
		const float exp = std::clamp(1.0f - d * 0.88f, 0.12f, 1.0f);
		minExp = exp;
		maxExp = exp;
		compensation = -2.2f * d;
	}

	void ForceInstantExposure(void* ent, bool post) {
		if (!ent)
			return;
		if (post) {
			WriteFloat(ent, "C_PostProcessingVolume->m_flExposureFadeSpeedUp", 1.0e6f);
			WriteFloat(ent, "C_PostProcessingVolume->m_flExposureFadeSpeedDown", 1.0e6f);
			WriteFloat(ent, "C_PostProcessingVolume->m_flTonemapEVSmoothingRange", 0.f);
			WriteFloat(ent, "C_PostProcessingVolume->m_flFadeDuration", 0.f);
		}
		else {
			WriteFloat(ent, "C_TonemapController2->m_flExposureAdaptationSpeedUp", 1.0e6f);
			WriteFloat(ent, "C_TonemapController2->m_flExposureAdaptationSpeedDown", 1.0e6f);
			WriteFloat(ent, "C_TonemapController2->m_flTonemapEVSmoothingRange", 0.f);
		}
	}

	int ApplyPostVolume(void* ent, float minExp, float maxExp, float compensation) {
		if (!ent)
			return 0;

		SavedFloats* s = AllocSaved(ent);
		if (s && !s->valid) {
			s->isPost = true;
			ReadFloat(ent, "C_PostProcessingVolume->m_flMinExposure", s->minExp);
			ReadFloat(ent, "C_PostProcessingVolume->m_flMaxExposure", s->maxExp);
			ReadFloat(ent, "C_PostProcessingVolume->m_flMinLogExposure", s->minLog);
			ReadFloat(ent, "C_PostProcessingVolume->m_flMaxLogExposure", s->maxLog);
			ReadFloat(ent, "C_PostProcessingVolume->m_flExposureCompensation", s->comp);
			ReadFloat(ent, "C_PostProcessingVolume->m_flExposureFadeSpeedUp", s->fadeUp);
			ReadFloat(ent, "C_PostProcessingVolume->m_flExposureFadeSpeedDown", s->fadeDown);
			ReadFloat(ent, "C_PostProcessingVolume->m_flTonemapEVSmoothingRange", s->evSmooth);
			ReadFloat(ent, "C_PostProcessingVolume->m_flFadeDuration", s->fadeDur);
			ReadBool(ent, "C_PostProcessingVolume->m_bExposureControl", s->expCtrl);
			ReadBool(ent, "C_PostProcessingVolume->m_bMaster", s->master);
			s->valid = true;
		}

		ForceInstantExposure(ent, true);

		const float locked = 0.5f * (minExp + maxExp);
		const float lo = (std::min)(minExp, locked);
		const float hi = (std::max)(maxExp, locked);

		int ok = 0;
		if (WriteFloat(ent, "C_PostProcessingVolume->m_flMinExposure", lo)) ++ok;
		if (WriteFloat(ent, "C_PostProcessingVolume->m_flMaxExposure", hi)) ++ok;
		const float minLog = (lo > 0.01f) ? lo : 0.01f;
		const float maxLog = (hi > 0.01f) ? hi : 0.01f;
		if (WriteFloat(ent, "C_PostProcessingVolume->m_flMinLogExposure", std::log2(minLog))) ++ok;
		if (WriteFloat(ent, "C_PostProcessingVolume->m_flMaxLogExposure", std::log2(maxLog))) ++ok;
		if (WriteFloat(ent, "C_PostProcessingVolume->m_flExposureCompensation", compensation)) ++ok;
		if (WriteBool(ent, "C_PostProcessingVolume->m_bExposureControl", true)) ++ok;
		WriteBool(ent, "C_PostProcessingVolume->m_bMaster", true);
		ForceInstantExposure(ent, true);
		return ok;
	}

	int ApplyTonemap(void* ent, float minExp, float maxExp) {
		if (!ent)
			return 0;

		SavedFloats* s = AllocSaved(ent);
		if (s && !s->valid) {
			s->isTone = true;
			ReadFloat(ent, "C_TonemapController2->m_flAutoExposureMin", s->autoMin);
			ReadFloat(ent, "C_TonemapController2->m_flAutoExposureMax", s->autoMax);
			ReadFloat(ent, "C_TonemapController2->m_flExposureAdaptationSpeedUp", s->adaptUp);
			ReadFloat(ent, "C_TonemapController2->m_flExposureAdaptationSpeedDown", s->adaptDown);
			ReadFloat(ent, "C_TonemapController2->m_flTonemapEVSmoothingRange", s->toneEvSmooth);
			s->valid = true;
		}

		ForceInstantExposure(ent, false);
		int ok = 0;
		if (WriteFloat(ent, "C_TonemapController2->m_flAutoExposureMin", minExp)) ++ok;
		if (WriteFloat(ent, "C_TonemapController2->m_flAutoExposureMax", maxExp)) ++ok;
		ForceInstantExposure(ent, false);
		return ok;
	}

	void ApplySkyBrightness(void* ent, float darkness) {
		if (!ent)
			return;
		SavedFloats* s = AllocSaved(ent);
		if (s && !s->isSkyBright) {
			ReadFloat(ent, "C_EnvSky->m_flBrightnessScale", s->skyBright);
			s->isSkyBright = true;
			s->valid = true;
		}
		const float scale = std::clamp(1.f - darkness * 0.75f, 0.25f, 1.f);
		WriteFloat(ent, "C_EnvSky->m_flBrightnessScale", scale);
	}

	using FnUpdateSkybox = __int64(__fastcall*)(void*);

	// SEH-only helper — must not contain C++ objects (C2712)
	static int CallUpdateSkyboxSeh(FnUpdateSkybox fn, void* ent) {
		if (!fn || !ent)
			return 0;
		__try {
			fn(ent);
			return 1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	FnUpdateSkybox ResolveUpdateSkybox() {
		static FnUpdateSkybox s_fn = nullptr;
		static bool s_done = false;
		if (!s_done) {
			s_done = true;
			const uintptr_t addr = M::patternScan("client",
				"48 89 5C 24 ? 57 48 83 EC ? 48 8B F9 E8 ? ? ? ? 48 8B 47");
			if (addr)
				s_fn = reinterpret_cast<FnUpdateSkybox>(addr);
		}
		return s_fn;
	}

	void ApplySkyTint(void* ent, const Color4& tint) {
		if (!ent)
			return;
		SavedFloats* s = AllocSaved(ent);
		if (s && !s->isSkyTint) {
			ReadColor(ent, "C_EnvSky->m_vTintColor", s->skyTint);
			ReadColor(ent, "C_EnvSky->m_vTintColorLightingOnly", s->skyTintLight);
			s->isSkyTint = true;
			s->valid = true;
		}
		WriteColor(ent, "C_EnvSky->m_vTintColor", tint);
		WriteColor(ent, "C_EnvSky->m_vTintColorLightingOnly", tint);
		WriteBool(ent, "C_EnvSky->m_bEnabled", true);
		CallUpdateSkyboxSeh(ResolveUpdateSkybox(), ent);
	}

	// IDA: C_GlobalLight wrapper calls UpdateState(this + 0x600)
	constexpr uintptr_t kGlobalLightBaseOff = 0x600;

	void* GlobalLightBase(void* ent) {
		if (!ent)
			return nullptr;
		return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + kGlobalLightBaseOff);
	}

	bool ReadColorAt(void* base, uint32_t off, Color4& out) {
		if (!base || !off)
			return false;
		std::memcpy(&out, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + off), 4);
		return true;
	}

	bool WriteColorAt(void* base, uint32_t off, const Color4& c) {
		if (!base || !off)
			return false;
		std::memcpy(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + off), &c, 4);
		return true;
	}

	// Schema often mis-resolves embedded CGlobalLightBase — use IDA offsets only.
	// IDA CGlobalLightBase layout (UpdateState reads these)
	constexpr uint32_t kOffSpecular = 0x64;
	constexpr uint32_t kOffEnabled = 0x69;
	constexpr uint32_t kOffLight = 0x6A;
	constexpr uint32_t kOffAmbient1 = 0x6E;
	constexpr uint32_t kOffAmbient2 = 0x72;
	constexpr uint32_t kOffAmbient3 = 0x76;

	struct HookLightSave {
		void* base = nullptr;
		Color4 light{}, a1{}, a2{}, a3{}, spec{};
		bool valid = false;
	};
	constexpr int kMaxHookLights = 8;
	HookLightSave g_hookLights[kMaxHookLights];

	HookLightSave* FindHookLight(void* base) {
		for (int i = 0; i < kMaxHookLights; ++i)
			if (g_hookLights[i].valid && g_hookLights[i].base == base)
				return &g_hookLights[i];
		return nullptr;
	}

	HookLightSave* AllocHookLight(void* base) {
		if (HookLightSave* s = FindHookLight(base))
			return s;
		for (int i = 0; i < kMaxHookLights; ++i) {
			if (!g_hookLights[i].valid) {
				g_hookLights[i] = {};
				g_hookLights[i].base = base;
				g_hookLights[i].valid = true;
				return &g_hookLights[i];
			}
		}
		return nullptr;
	}

	void ApplyLightTintToBase(void* base, const Color4& col) {
		if (!base || !Mem::IsReadable(base, 0x280))
			return;

		if (!FindHookLight(base)) {
			if (HookLightSave* s = AllocHookLight(base)) {
				ReadColorAt(base, kOffLight, s->light);
				ReadColorAt(base, kOffAmbient1, s->a1);
				ReadColorAt(base, kOffAmbient2, s->a2);
				ReadColorAt(base, kOffAmbient3, s->a3);
				ReadColorAt(base, kOffSpecular, s->spec);
			}
		}

		// Byte triples UpdateState reads; dirty 0x69 forces recompute (Andromeda).
		WriteColorAt(base, kOffSpecular, col);
		WriteColorAt(base, kOffLight, col);
		WriteColorAt(base, kOffAmbient1, col);
		WriteColorAt(base, kOffAmbient2, col);
		WriteColorAt(base, kOffAmbient3, col);
		*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(base) + kOffEnabled) = 1;
	}

	void RestoreHookLights() {
		for (int i = 0; i < kMaxHookLights; ++i) {
			HookLightSave& s = g_hookLights[i];
			if (!s.valid || !s.base)
				continue;
			if (Mem::IsReadable(s.base, 0x80)) {
				WriteColorAt(s.base, kOffLight, s.light);
				WriteColorAt(s.base, kOffAmbient1, s.a1);
				WriteColorAt(s.base, kOffAmbient2, s.a2);
				WriteColorAt(s.base, kOffAmbient3, s.a3);
				WriteColorAt(s.base, kOffSpecular, s.spec);
				*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(s.base) + kOffEnabled) = 1;
			}
			s = {};
		}
	}

	void ApplyLighting(void* ent, const Color4& col) {
		void* base = GlobalLightBase(ent);
		if (!base)
			return;

		SavedFloats* s = AllocSaved(ent);
		if (s && !s->isLight) {
			ReadColorAt(base, kOffLight, s->lightColor);
			ReadColorAt(base, kOffAmbient1, s->ambient1);
			ReadColorAt(base, kOffAmbient2, s->ambient2);
			ReadColorAt(base, kOffAmbient3, s->ambient3);
			ReadColorAt(base, kOffSpecular, s->specular);
			s->isLight = true;
			s->valid = true;
		}

		ApplyLightTintToBase(base, col);
	}

	void RestoreLightingColors(SavedFloats& s) {
		if (!s.isLight || !s.ent)
			return;
		void* base = GlobalLightBase(s.ent);
		if (!base)
			return;
		WriteColorAt(base, kOffLight, s.lightColor);
		WriteColorAt(base, kOffAmbient1, s.ambient1);
		WriteColorAt(base, kOffAmbient2, s.ambient2);
		WriteColorAt(base, kOffAmbient3, s.ambient3);
		WriteColorAt(base, kOffSpecular, s.specular);
		*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(base) + kOffEnabled) = 1;
		s.isLight = false;
	}

	bool IsGlobalLightClass(const char* name) {
		if (!name || !name[0])
			return false;
		const uint32_t h = HASH(name);
		if (h == HASH("C_GlobalLight") || h == HASH("CGlobalLight"))
			return true;
		return strstr(name, "GlobalLight") != nullptr;
	}

	bool IsGlobalLightDesigner(const char* name) {
		if (!name || !name[0])
			return false;
		return strcmp(name, "env_global_light") == 0
			|| strstr(name, "global_light") != nullptr;
	}

	// Identity-slot designer (same as nade_pred) — works even when dump_class_info fails
	const char* DesignerAtIndex(int index) {
		if (!I::GameEntity || !I::GameEntity->Instance || !Mem::ValidEntityIndex(index))
			return nullptr;
		auto* es = I::GameEntity->Instance;
		if ((unsigned)index > 0x7FFE || (unsigned)(index >> 9) > 0x3F)
			return nullptr;
		void* chunk = nullptr;
		if (!Mem::ReadField(es, 0x10 + 8ull * (index >> 9), chunk) || !chunk)
			return nullptr;
		const uintptr_t slot = reinterpret_cast<uintptr_t>(chunk) + 0x70ull * (index & 0x1FF);
		if (!Mem::IsReadable(reinterpret_cast<void*>(slot), 0x28))
			return nullptr;
		const char* p = nullptr;
		if (!Mem::ReadField(reinterpret_cast<void*>(slot), 0x20, p) || !p)
			return nullptr;
		if (!Mem::IsReadable(p, 2) || !p[0])
			return nullptr;
		return p;
	}

	void* EntityAtIndexLoose(int index) {
		if (!I::GameEntity || !I::GameEntity->Instance || !Mem::ValidEntityIndex(index))
			return nullptr;
		if (void* e = I::GameEntity->Instance->Get(index))
			return e;
		auto* es = I::GameEntity->Instance;
		if ((unsigned)index > 0x7FFE || (unsigned)(index >> 9) > 0x3F)
			return nullptr;
		void* chunk = nullptr;
		if (!Mem::ReadField(es, 0x10 + 8ull * (index >> 9), chunk) || !chunk)
			return nullptr;
		const uintptr_t slot = reinterpret_cast<uintptr_t>(chunk) + 0x70ull * (index & 0x1FF);
		void* ent = nullptr;
		if (!Mem::ReadField(reinterpret_cast<void*>(slot), 0, ent) || !Mem::ValidEntity(ent))
			return nullptr;
		return ent;
	}

	bool IsEnvSkyClass(const char* name) {
		if (!name || !name[0]) return false;
		return HASH(name) == HASH("C_EnvSky")
			|| HASH(name) == HASH("CEnvSky")
			|| strstr(name, "EnvSky") != nullptr;
	}

	bool IsEnvSkyDesigner(const char* name) {
		return name && name[0] && strcmp(name, "env_sky") == 0;
	}

	const char* SafeDumpClassName(void* ent) {
		if (!ent) return nullptr;
		__try {
			SchemaClassInfoData_t* cls = nullptr;
			reinterpret_cast<CEntityInstance*>(ent)->dump_class_info(&cls);
			if (cls && cls->szName && Mem::IsReadable(cls->szName, 1))
				return cls->szName;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return nullptr;
	}

	void RestoreNightOnly() {
		// Compact: restore post/tonemap/brightness; keep tint saves if skybox still on
		int write = 0;
		for (int i = 0; i < g_savedCount; ++i) {
			SavedFloats& s = g_saved[i];
			if (!s.valid || !s.ent)
				continue;

			if (s.isPost) {
				ForceInstantExposure(s.ent, true);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMinExposure", s.minExp);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMaxExposure", s.maxExp);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMinLogExposure", s.minLog);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMaxLogExposure", s.maxLog);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flExposureCompensation", s.comp);
				WriteBool(s.ent, "C_PostProcessingVolume->m_bExposureControl", s.expCtrl);
				WriteBool(s.ent, "C_PostProcessingVolume->m_bMaster", s.master);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flExposureFadeSpeedUp", s.fadeUp);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flExposureFadeSpeedDown", s.fadeDown);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flTonemapEVSmoothingRange", s.evSmooth);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flFadeDuration", s.fadeDur);
				s.isPost = false;
			}
			if (s.isTone) {
				ForceInstantExposure(s.ent, false);
				WriteFloat(s.ent, "C_TonemapController2->m_flAutoExposureMin", s.autoMin);
				WriteFloat(s.ent, "C_TonemapController2->m_flAutoExposureMax", s.autoMax);
				WriteFloat(s.ent, "C_TonemapController2->m_flExposureAdaptationSpeedUp", s.adaptUp);
				WriteFloat(s.ent, "C_TonemapController2->m_flExposureAdaptationSpeedDown", s.adaptDown);
				WriteFloat(s.ent, "C_TonemapController2->m_flTonemapEVSmoothingRange", s.toneEvSmooth);
				s.isTone = false;
			}
			if (s.isSkyBright) {
				WriteFloat(s.ent, "C_EnvSky->m_flBrightnessScale", s.skyBright);
				s.isSkyBright = false;
			}

			const bool keep = s.isSkyTint || s.isLight;
			if (keep) {
				s.isPost = false;
				s.isTone = false;
				s.isSkyBright = false;
				g_saved[write++] = s;
			}
		}
		g_savedCount = write;
	}

	void RestoreSkyTintOnly() {
		int write = 0;
		for (int i = 0; i < g_savedCount; ++i) {
			SavedFloats& s = g_saved[i];
			if (!s.valid || !s.ent)
				continue;

			if (s.isSkyTint) {
				WriteColor(s.ent, "C_EnvSky->m_vTintColor", s.skyTint);
				WriteColor(s.ent, "C_EnvSky->m_vTintColorLightingOnly", s.skyTintLight);
				CallUpdateSkyboxSeh(ResolveUpdateSkybox(), s.ent);
				s.isSkyTint = false;
			}

			const bool keep = s.isPost || s.isTone || s.isSkyBright || s.isLight;
			if (keep)
				g_saved[write++] = s;
		}
		g_savedCount = write;
	}

	void RestoreLightingOnly() {
		RestoreHookLights();
		int write = 0;
		for (int i = 0; i < g_savedCount; ++i) {
			SavedFloats& s = g_saved[i];
			if (!s.valid || !s.ent)
				continue;

			if (s.isLight)
				RestoreLightingColors(s);

			const bool keep = s.isPost || s.isTone || s.isSkyBright || s.isSkyTint;
			if (keep)
				g_saved[write++] = s;
		}
		g_savedCount = write;
	}

	void RestoreAll() {
		RestoreHookLights();
		for (int i = 0; i < g_savedCount; ++i) {
			SavedFloats& s = g_saved[i];
			if (!s.valid || !s.ent)
				continue;
			if (s.isPost) {
				ForceInstantExposure(s.ent, true);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMinExposure", s.minExp);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMaxExposure", s.maxExp);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMinLogExposure", s.minLog);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flMaxLogExposure", s.maxLog);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flExposureCompensation", s.comp);
				WriteBool(s.ent, "C_PostProcessingVolume->m_bExposureControl", s.expCtrl);
				WriteBool(s.ent, "C_PostProcessingVolume->m_bMaster", s.master);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flExposureFadeSpeedUp", s.fadeUp);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flExposureFadeSpeedDown", s.fadeDown);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flTonemapEVSmoothingRange", s.evSmooth);
				WriteFloat(s.ent, "C_PostProcessingVolume->m_flFadeDuration", s.fadeDur);
			}
			if (s.isTone) {
				ForceInstantExposure(s.ent, false);
				WriteFloat(s.ent, "C_TonemapController2->m_flAutoExposureMin", s.autoMin);
				WriteFloat(s.ent, "C_TonemapController2->m_flAutoExposureMax", s.autoMax);
				WriteFloat(s.ent, "C_TonemapController2->m_flExposureAdaptationSpeedUp", s.adaptUp);
				WriteFloat(s.ent, "C_TonemapController2->m_flExposureAdaptationSpeedDown", s.adaptDown);
				WriteFloat(s.ent, "C_TonemapController2->m_flTonemapEVSmoothingRange", s.toneEvSmooth);
			}
			if (s.isSkyBright)
				WriteFloat(s.ent, "C_EnvSky->m_flBrightnessScale", s.skyBright);
			if (s.isSkyTint) {
				WriteColor(s.ent, "C_EnvSky->m_vTintColor", s.skyTint);
				WriteColor(s.ent, "C_EnvSky->m_vTintColorLightingOnly", s.skyTintLight);
				CallUpdateSkyboxSeh(ResolveUpdateSkybox(), s.ent);
			}
			if (s.isLight)
				RestoreLightingColors(s);
		}
		g_savedCount = 0;
	}

	CBaseHandle ReadHandle(void* base, const char* field) {
		const uint32_t off = Off(field);
		if (!off || !base)
			return CBaseHandle();
		return *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uintptr_t>(base) + off);
	}

	void* ReadPtr(void* base, const char* field) {
		const uint32_t off = Off(field);
		if (!off || !base)
			return nullptr;
		return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + off);
	}

	int ApplyPostVector(void* camera, float minExp, float maxExp, float compensation) {
		const uint32_t off = Off("CPlayer_CameraServices->m_PostProcessingVolumes");
		if (!off || !camera || !I::GameEntity || !I::GameEntity->Instance)
			return 0;

		const uintptr_t vec = reinterpret_cast<uintptr_t>(camera) + off;
		int sizeA = *reinterpret_cast<int*>(vec);
		CBaseHandle* dataA = *reinterpret_cast<CBaseHandle**>(vec + 8);
		CBaseHandle* dataB = *reinterpret_cast<CBaseHandle**>(vec);
		int sizeB = *reinterpret_cast<int*>(vec + 8);

		CBaseHandle* data = nullptr;
		int size = 0;
		if (sizeA > 0 && sizeA < 64 && dataA) { data = dataA; size = sizeA; }
		else if (sizeB > 0 && sizeB < 64 && dataB) { data = dataB; size = sizeB; }
		else return 0;

		int fields = 0;
		for (int i = 0; i < size; ++i) {
			if (!data[i].valid()) continue;
			if (void* vol = I::GameEntity->Instance->Get(data[i]))
				fields += ApplyPostVolume(vol, minExp, maxExp, compensation);
		}
		return fields;
	}

	int ApplyFromCamera(void* camera, float minExp, float maxExp, float compensation) {
		if (!camera || !I::GameEntity || !I::GameEntity->Instance)
			return 0;
		int fields = 0;

		CBaseHandle hActive = ReadHandle(camera, "CPlayer_CameraServices->m_hActivePostProcessingVolume");
		if (hActive.valid()) {
			if (void* vol = I::GameEntity->Instance->Get(hActive))
				fields += ApplyPostVolume(vol, minExp, maxExp, compensation);
		}
		fields += ApplyPostVector(camera, minExp, maxExp, compensation);

		CBaseHandle hTone = ReadHandle(camera, "CPlayer_CameraServices->m_hTonemapController");
		if (hTone.valid()) {
			if (void* tone = I::GameEntity->Instance->Get(hTone))
				fields += ApplyTonemap(tone, minExp, maxExp);
		}
		return fields;
	}

	bool Ready() {
		if (!I::EngineClient || !I::EngineClient->valid())
			return false;
		if (!I::GameEntity || !I::GameEntity->Instance)
			return false;
		return true;
	}

}

// DrawSkyboxArray (scenesystem): RGB floats at sceneObj+0xE8 used as material tint.
// Snapshot originals per entry so disabling restores the map's real sky colours.
namespace {
	constexpr uintptr_t kSkyTintRgb = 0xE8; // 232 — same as Andromeda
	constexpr size_t kMaxSkySnaps = 32;

	struct SkyEntrySnap { float r, g, b; };
	std::unordered_map<void*, SkyEntrySnap> g_skySnap;

	static void WriteSkyObjTintSeh(void* skyObj, float r, float g, float b) {
		__try {
			if (!skyObj)
				return;
			float* rgb = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(skyObj) + kSkyTintRgb);
			rgb[0] = r;
			rgb[1] = g;
			rgb[2] = b;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	static bool ReadSkyObjTintSeh(void* skyObj, float out[3]) {
		__try {
			if (!skyObj)
				return false;
			float* rgb = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(skyObj) + kSkyTintRgb);
			out[0] = rgb[0];
			out[1] = rgb[1];
			out[2] = rgb[2];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Andromeda: ppEntry = a3 + a4 * 0x68 - 0x50 (a4 = 1-based entry index)
	static void* SkyEntryAt(uintptr_t a3, int index1Based) {
		__try {
			if (!a3 || index1Based <= 0)
				return nullptr;
			return *reinterpret_cast<void**>(a3 + static_cast<__int64>(index1Based) * 0x68 - 0x50);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	static void ApplyOrRestoreSkyEntry(void* pEntry) {
		if (!pEntry)
			return;

		float cur[3]{};
		if (!ReadSkyObjTintSeh(pEntry, cur))
			return;

		if (Config::skybox) {
			auto it = g_skySnap.find(pEntry);
			if (it == g_skySnap.end() && g_skySnap.size() < kMaxSkySnaps)
				it = g_skySnap.emplace(pEntry, SkyEntrySnap{ cur[0], cur[1], cur[2] }).first;

			WriteSkyObjTintSeh(pEntry,
				Config::skybox_color.x, Config::skybox_color.y, Config::skybox_color.z);
		}
		else {
			auto it = g_skySnap.find(pEntry);
			if (it != g_skySnap.end()) {
				WriteSkyObjTintSeh(pEntry, it->second.r, it->second.g, it->second.b);
				g_skySnap.erase(it);
			}
		}
	}
}

void __fastcall H::hkDrawSkyboxArray(__int64 a1, __int64 a2, __int64 a3, int a4, int a5, __int64 a6, __int64 a7)
{
	auto original = DrawSkyboxArray.GetOriginal();

	// a4 is the 1-based entry index (engine skips a4 <= 0). Patch before original.
	if (a4 > 0 && a3)
		ApplyOrRestoreSkyEntry(SkyEntryAt(static_cast<uintptr_t>(a3), a4));

	if (original)
		original(a1, a2, a3, a4, a5, a6, a7);
}

// Map color: DrawAggregateSceneObjectArray → lightData (Andromeda).
// Lighting: LightSceneObject (+0xE4 *3) + GlobalLightUpdate (triples + dirty 0x69).
namespace {
	// Aggregate arr +0x08 → data; data+0x04 count; data+0x30 lightData index
	constexpr uintptr_t kAggArrData = 0x08;
	constexpr uintptr_t kAggCount = 0x04;
	constexpr uintptr_t kAggIndex = 0x30;
	constexpr int kLightEntryShift = 5; // 0x20 stride

	// SceneSystem_002 fallback (Andromeda); primary path is IDA xmmword+8 → +0x18
	constexpr uintptr_t kSceneSysData = 0x2A28;
	constexpr uintptr_t kLightDataOff = 0x18;

	constexpr uintptr_t kLightColorOff = 0xE4;
	constexpr float kLightColorBoost = 3.0f;

	constexpr uintptr_t kGlColorOffs[] = { 0x64, 0x6A, 0x6E, 0x72, 0x76 };
	constexpr uintptr_t kGlDirty = 0x69;

	// Resolved once: pointer to the QWORD holding the CMemoryStack* used by AggregateArray
	void** g_ppLightStackObj = nullptr;

	std::unordered_map<void*, std::array<float, 4>> g_lightSnap;
	constexpr size_t kMaxLightSnaps = 256;

	bool g_glSnapReady = false;
	void* g_glSnapObj = nullptr;
	uint8_t g_glSnap[5][3]{};

	void ResolveLightStackFromAggregate(uintptr_t fn) {
		if (!fn || g_ppLightStackObj)
			return;
		// Prefer hit inside this function (IDA: mov r14,[rip]; shl eax,5; … add rdx,[r14+18h])
		uintptr_t hit = 0;
		for (uintptr_t p = fn; p + 16 < fn + 0xC00; ++p) {
			const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
			if (b[0] != 0x4C || b[1] != 0x8B || b[2] != 0x35)
				continue;
			if (b[7] == 0xC1 && b[8] == 0xE0 && b[9] == 0x05
				&& b[10] == 0x48 && b[11] == 0x63 && b[12] == 0xD0
				&& b[13] == 0x49 && b[14] == 0x03 && b[15] == 0x56 && b[16] == 0x18) {
				hit = p;
				break;
			}
		}
		if (!hit)
			hit = M::patternScan("scenesystem",
				"4C 8B 35 ? ? ? ? C1 E0 05 48 63 D0 49 03 56 18");
		if (!hit) {
			Con::OffsetMiss("AggregateLightData rip");
			return;
		}
		g_ppLightStackObj = reinterpret_cast<void**>(M::getAbsoluteAddress(hit, 3));
		Con::Ok("AggregateLightData stack @ 0x%p", (void*)g_ppLightStackObj);
	}

	void* GetSceneSystem() {
		static void* s = nullptr;
		if (!s)
			s = I::Get<void>("scenesystem.dll", "SceneSystem_002");
		return s;
	}

	uint8_t* GetLightDataNoSeh() {
		if (g_ppLightStackObj) {
			void* stackObj = *g_ppLightStackObj;
			if (stackObj)
				return *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(stackObj) + kLightDataOff);
		}
		void* scene = GetSceneSystem();
		if (!scene)
			return nullptr;
		void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(scene) + kSceneSysData);
		if (!data)
			return nullptr;
		return *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(data) + kLightDataOff);
	}

	uint8_t* GetLightDataSeh() {
		uint8_t* out = nullptr;
		__try { out = GetLightDataNoSeh(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
		return out;
	}

	float NightLightFactor() {
		if (!Config::Night)
			return 1.f;
		const float e = std::clamp(Config::night_exposure, 0.01f, 1.f);
		return 0.15f + (1.f - e) * 0.85f;
	}

	void WriteTriple(void* pThis, uintptr_t off, uint8_t r, uint8_t g, uint8_t b) {
		auto* p = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pThis) + off);
		p[0] = r;
		p[1] = g;
		p[2] = b;
	}

	uint8_t FloatToByte(float v) {
		v = std::clamp(v, 0.f, 255.f);
		return static_cast<uint8_t>(v);
	}

	void TintAggregateLightDataSeh(void* pAggregateArr, uint8_t r, uint8_t g, uint8_t b) {
		__try {
			void* pData = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pAggregateArr) + kAggArrData);
			if (!pData)
				return;
			const int nCount = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pData) + kAggCount);
			const int nIndex = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pData) + kAggIndex);
			if (nIndex < 0 || nCount <= 0 || nCount > 8192)
				return;
			uint8_t* pLightData = GetLightDataNoSeh();
			if (!pLightData)
				return;
			for (int i = 0; i < nCount; ++i) {
				uint8_t* entry = pLightData + (static_cast<uintptr_t>(nIndex + i) << kLightEntryShift);
				entry[0] = r;
				entry[1] = g;
				entry[2] = b;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	bool ReadLightFloatsSeh(void* pLightScene, float out[4]) {
		__try {
			float* p = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pLightScene) + kLightColorOff);
			out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	void WriteLightFloatsSeh(void* pLightScene, const float in[4]) {
		__try {
			float* p = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pLightScene) + kLightColorOff);
			p[0] = in[0]; p[1] = in[1]; p[2] = in[2]; p[3] = in[3];
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void ApplyGlobalLightSeh(void* pThis, bool bColor, bool bNight, float flNight,
		float cr, float cg, float cb)
	{
		__try {
			if (!g_glSnapReady || g_glSnapObj != pThis) {
				for (int i = 0; i < 5; ++i) {
					const auto* p = reinterpret_cast<const uint8_t*>(
						reinterpret_cast<uintptr_t>(pThis) + kGlColorOffs[i]);
					g_glSnap[i][0] = p[0];
					g_glSnap[i][1] = p[1];
					g_glSnap[i][2] = p[2];
				}
				g_glSnapObj = pThis;
				g_glSnapReady = true;
			}

			if (bColor || bNight) {
				for (int i = 0; i < 5; ++i) {
					float fr, fg, fb;
					if (bColor) {
						fr = cr * 255.f;
						fg = cg * 255.f;
						fb = cb * 255.f;
					}
					else {
						fr = static_cast<float>(g_glSnap[i][0]);
						fg = static_cast<float>(g_glSnap[i][1]);
						fb = static_cast<float>(g_glSnap[i][2]);
					}
					fr *= flNight;
					fg *= flNight;
					fb *= flNight;
					WriteTriple(pThis, kGlColorOffs[i], FloatToByte(fr), FloatToByte(fg), FloatToByte(fb));
				}
				*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pThis) + kGlDirty) = 1;
			}
			else if (g_glSnapReady && g_glSnapObj == pThis) {
				for (int i = 0; i < 5; ++i)
					WriteTriple(pThis, kGlColorOffs[i], g_glSnap[i][0], g_glSnap[i][1], g_glSnap[i][2]);
				*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pThis) + kGlDirty) = 1;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
}

std::int64_t __fastcall H::hkDrawAggregateSceneObjectArray(void* a1, void* a2, void* pAggregateArr)
{
	auto original = DrawAggregateSceneObjectArray.GetOriginal();
	if (!original)
		return 0;

	const std::int64_t result = original(a1, a2, pAggregateArr);

	if (!Config::map_color || !pAggregateArr)
		return result;

	const auto& c = Config::map_color_value;
	const uint8_t r = static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f + 0.5f);
	const uint8_t g = static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f + 0.5f);
	const uint8_t b = static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f + 0.5f);
	TintAggregateLightDataSeh(pAggregateArr, r, g, b);
	return result;
}

void* __fastcall H::hkLightSceneObject(void* ptr, void* pLightScene, void* unk)
{
	auto original = LightSceneObject.GetOriginal();
	if (!original)
		return nullptr;

	if (pLightScene && reinterpret_cast<uintptr_t>(pLightScene) >= 0x10000ull) {
		const bool bColor = Config::lighting;
		const bool bNight = Config::Night;
		float cur[4]{};
		if (ReadLightFloatsSeh(pLightScene, cur)) {
			if (bColor || bNight) {
				auto snapIt = g_lightSnap.find(pLightScene);
				if (snapIt == g_lightSnap.end()) {
					if (g_lightSnap.size() < kMaxLightSnaps) {
						snapIt = g_lightSnap.emplace(
							pLightScene,
							std::array<float, 4>{ cur[0], cur[1], cur[2], cur[3] }).first;
					}
				}
				if (snapIt != g_lightSnap.end()) {
					const float flNight = NightLightFactor();
					const float src[4] = {
						Config::lighting_color.x, Config::lighting_color.y,
						Config::lighting_color.z, Config::lighting_color.w
					};
					float out[4]{};
					for (int i = 0; i < 4; ++i) {
						const float flBase = bColor
							? (src[i] * kLightColorBoost)
							: snapIt->second[static_cast<size_t>(i)];
						out[i] = (i < 3) ? (flBase * flNight) : flBase;
					}
					WriteLightFloatsSeh(pLightScene, out);
				}
			}
			else {
				auto it = g_lightSnap.find(pLightScene);
				if (it != g_lightSnap.end()) {
					WriteLightFloatsSeh(pLightScene, it->second.data());
					g_lightSnap.erase(it);
				}
			}
		}
	}

	return original(ptr, pLightScene, unk);
}

void* __fastcall H::hkGlobalLightUpdate(void* pThis)
{
	auto original = GlobalLightUpdate.GetOriginal();
	if (!original)
		return nullptr;
	if (!pThis || reinterpret_cast<uintptr_t>(pThis) < 0x10000ull)
		return original(pThis);

	ApplyGlobalLightSeh(
		pThis, Config::lighting, Config::Night, NightLightFactor(),
		Config::lighting_color.x, Config::lighting_color.y, Config::lighting_color.z);

	return original(pThis);
}

void World::InstallMapColorHook()
{
	const uintptr_t addr = M::patternScan("scenesystem",
		"48 8B C4 48 89 50 ? 48 89 48 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70");
	if (!addr) {
		Con::OffsetMiss("DrawAggregateSceneObjectArray");
		return;
	}
	ResolveLightStackFromAggregate(addr);
	if (!H::DrawAggregateSceneObjectArray.Add(reinterpret_cast<void*>(addr),
		reinterpret_cast<void*>(&H::hkDrawAggregateSceneObjectArray)))
		Con::Error("DrawAggregateSceneObjectArray hook.Add failed @ 0x%p", (void*)addr);
	else
		Con::Ok("DrawAggregateSceneObjectArray @ 0x%p", (void*)addr);
}

void World::InstallLightingHook()
{
	{
		const uintptr_t addr = M::patternScan("scenesystem", "48 89 54 24 ? 55 57 41 56 48 83 EC");
		if (!addr)
			Con::OffsetMiss("LightSceneObject");
		else if (!H::LightSceneObject.Add(reinterpret_cast<void*>(addr),
			reinterpret_cast<void*>(&H::hkLightSceneObject)))
			Con::Error("LightSceneObject hook.Add failed @ 0x%p", (void*)addr);
		else
			Con::Ok("LightSceneObject @ 0x%p", (void*)addr);
	}

	{
		const uintptr_t addr = M::patternScan("client",
			"40 57 48 81 EC C0 00 00 00 48 8B F9 BA FF FF FF FF 48 8D 0D");
		if (!addr)
			Con::OffsetMiss("GlobalLightUpdateState");
		else if (!H::GlobalLightUpdate.Add(reinterpret_cast<void*>(addr),
			reinterpret_cast<void*>(&H::hkGlobalLightUpdate)))
			Con::Error("GlobalLightUpdateState hook.Add failed @ 0x%p", (void*)addr);
		else
			Con::Ok("GlobalLightUpdateState @ 0x%p", (void*)addr);
	}
}

void World::ApplyMapMeshColor(CMeshData* mesh)
{
	// Static world meshes in DrawArray (no skeletal owner) — walls/props that skip Aggregate.
	if (!Config::map_color || !mesh)
		return;
	const auto& c = Config::map_color_value;
	__try {
		mesh->color.r = static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f + 0.5f);
		mesh->color.g = static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f + 0.5f);
		mesh->color.b = static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f + 0.5f);
		mesh->color.a = static_cast<uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f + 0.5f);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void World::Update()
{
	const bool wantNight = Config::Night;
	const bool wantSky = Config::skybox;
	const bool wantLight = Config::lighting;

	// Nothing active → full restore if we previously modified anything
	if (!wantNight && !wantSky && !wantLight) {
		if (g_wasNight || g_wasSkybox || g_wasLighting) {
			__try { RestoreAll(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("World restore all", GetExceptionCode());
				g_savedCount = 0;
			}
			g_wasNight = false;
			g_wasSkybox = false;
			g_wasLighting = false;
		}
		return;
	}

	if (!Ready())
		return;

	if (!wantNight && g_wasNight) {
		__try { RestoreNightOnly(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("NightMode restore", GetExceptionCode());
		}
		g_wasNight = false;
	}

	if (!wantSky && g_wasSkybox) {
		__try { RestoreSkyTintOnly(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("Skybox restore", GetExceptionCode());
		}
		g_wasSkybox = false;
	}

	if (!wantLight && g_wasLighting) {
		__try { RestoreLightingOnly(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("Lighting restore", GetExceptionCode());
		}
		g_wasLighting = false;
	}

	__try {
		const bool doNight = wantNight;
		const bool doSky = wantSky;
		const bool doLight = wantLight;
		float minExp = 0.1f, maxExp = 0.3f, compensation = -1.f;
		Color4 tint{};
		Color4 lightCol{};
		if (doNight)
			ExposureFromDarkness(Config::night_exposure, minExp, maxExp, compensation);
		if (doSky)
			tint = ImToColor4(Config::skybox_color);
		if (doLight)
			lightCol = ImToColor4(Config::lighting_color);

		if (doNight) {
			g_wasNight = true;
			if (H::oGetLocalPlayer) {
				C_CSPlayerPawn* lp = H::oGetLocalPlayer(0);
				if (lp) {
					void* camera = ReadPtr(lp, "C_BasePlayerPawn->m_pCameraServices");
					if (!camera)
						camera = ReadPtr(lp, "C_CSPlayerPawn->m_pCameraServices");
					if (camera)
						ApplyFromCamera(camera, minExp, maxExp, compensation);
				}
			}
		}
		if (doSky)
			g_wasSkybox = true;
		if (doLight)
			g_wasLighting = true;

		const int maxEnt = I::GameEntity->Instance->GetHighestEntityIndex();
		if (maxEnt > 0 && maxEnt <= 8192) {
			for (int i = 0; i <= maxEnt; ++i) {
				const char* designer = DesignerAtIndex(i);
				void* ent = EntityAtIndexLoose(i);
				const char* clsName = SafeDumpClassName(ent);

				if (doNight && ent && clsName) {
					const uint32_t h = HASH(clsName);
					if (h == HASH("C_EnvSky") || h == HASH("CEnvSky") || strstr(clsName, "EnvSky"))
						ApplySkyBrightness(ent, Config::night_exposure);
					else if (h == HASH("C_PostProcessingVolume") || h == HASH("CPostProcessingVolume"))
						ApplyPostVolume(ent, minExp, maxExp, compensation);
					else if (h == HASH("C_TonemapController2") ||
						h == HASH("C_TonemapController2Alias_env_tonemap_controller2") ||
						h == HASH("CTonemapController2"))
						ApplyTonemap(ent, minExp, maxExp);
				}

				if (doSky && ent && (IsEnvSkyClass(clsName) || IsEnvSkyDesigner(designer)))
					ApplySkyTint(ent, tint);

				if (doLight && ent && (IsGlobalLightClass(clsName) || IsGlobalLightDesigner(designer)))
					ApplyLighting(ent, lightCol);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("World::Update", GetExceptionCode());
	}
}
