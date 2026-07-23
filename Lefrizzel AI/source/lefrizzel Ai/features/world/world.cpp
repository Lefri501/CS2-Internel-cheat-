#include "world.h"
#include "fog_handler.h"
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

// Nightmode: entity post/tonemap + scenesystem ToneMapUpdate (GPU outputs +136/+140).
// Skybox: DrawSkyboxArray (scenesystem) RGB float tint; entity C_EnvSky path as fallback.
// Map color: DrawAggregateSceneObjectArray → shared lightData buffer (Andromeda path).
//          + DrawArray mesh tint + GeneratePrimitives draw-list tint (all world props).
// Lighting: LightSceneObject (+0xE4 *3) + GlobalLightUpdate + UpdateLightObject.

namespace {
	bool g_wasNight = false;
	bool g_wasSkybox = false;
	bool g_wasLighting = false;

	// Env entities (sky / tonemap / light) — full entity walk is expensive; cache indices.
	// kind bit flags so Update() never dump_class_info every frame (big FPS win).
	constexpr int kMaxEnvIdx = 48;
	constexpr std::uint8_t kEnvSky = 1;
	constexpr std::uint8_t kEnvLight = 2;
	constexpr std::uint8_t kEnvPost = 4;
	constexpr std::uint8_t kEnvTone = 8;
	struct EnvSlot {
		int idx = 0;
		std::uint8_t kind = 0;
	};
	EnvSlot g_envSlots[kMaxEnvIdx]{};
	int g_envCount = 0;
	unsigned long long g_envScanMs = 0;

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
		if (!off || !ent || !Mem::IsUserPtr(ent))
			return false;
		return Mem::ReadField(ent, off, out);
	}

	bool WriteFloat(void* ent, const char* field, float v) {
		const uint32_t off = Off(field);
		if (!off || !ent || !Mem::IsUserPtr(ent))
			return false;
		// Range-ok only; SEH for free'd env entities mid-apply
		__try {
			*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(ent) + off) = v;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool ReadBool(void* ent, const char* field, bool& out) {
		const uint32_t off = Off(field);
		if (!off || !ent || !Mem::IsUserPtr(ent))
			return false;
		return Mem::ReadField(ent, off, out);
	}

	bool WriteBool(void* ent, const char* field, bool v) {
		const uint32_t off = Off(field);
		if (!off || !ent || !Mem::IsUserPtr(ent))
			return false;
		__try {
			*reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(ent) + off) = v;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool ReadColor(void* ent, const char* field, Color4& out) {
		const uint32_t off = Off(field);
		if (!off || !ent || !Mem::IsUserPtr(ent))
			return false;
		return Mem::Read(reinterpret_cast<const void*>(
			reinterpret_cast<uintptr_t>(ent) + off), out);
	}

	bool WriteColor(void* ent, const char* field, const Color4& c) {
		const uint32_t off = Off(field);
		if (!off || !ent || !Mem::IsUserPtr(ent))
			return false;
		__try {
			std::memcpy(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + off), &c, 4);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
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

	// IDA client C_EnvSky (schema dump build): tint bytes — do NOT call UpdateSkybox.
	// UpdateSkybox @ 0x26A880 loads a material via resource system and stores the
	// handle at ent+0xFF8; that path crashes when identity/scene state is mid-frame.
	// Visual tint is applied safely in hkDrawSkyboxArray (scenesystem +0xE8 floats).
	constexpr uint32_t kEnvSkyTint = 0xFC1;
	constexpr uint32_t kEnvSkyTintLight = 0xFC5;
	constexpr uint32_t kEnvSkyEnabled = 0xFE4;

	bool WriteEnvSkyTintBytes(void* ent, uint32_t off, const Color4& c) {
		if (!ent || !Mem::IsUserPtr(ent))
			return false;
		void* p = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + off);
		if (!Mem::IsReadable(p, 4))
			return false;
		std::memcpy(p, &c, 4);
		return true;
	}

	bool ReadEnvSkyTintBytes(void* ent, uint32_t off, Color4& out) {
		if (!ent || !Mem::IsUserPtr(ent))
			return false;
		void* p = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + off);
		if (!Mem::IsReadable(p, 4))
			return false;
		std::memcpy(&out, p, 4);
		return true;
	}

	void ApplySkyTint(void* ent, const Color4& tint) {
		if (!ent || !Mem::ValidEntity(ent))
			return;
		SavedFloats* s = AllocSaved(ent);
		if (s && !s->isSkyTint) {
			if (!ReadEnvSkyTintBytes(ent, kEnvSkyTint, s->skyTint))
				ReadColor(ent, "C_EnvSky->m_vTintColor", s->skyTint);
			if (!ReadEnvSkyTintBytes(ent, kEnvSkyTintLight, s->skyTintLight))
				ReadColor(ent, "C_EnvSky->m_vTintColorLightingOnly", s->skyTintLight);
			s->isSkyTint = true;
			s->valid = true;
		}
		// Entity-side tint only (lighting). No UpdateSkybox — draw hook owns the look.
		WriteEnvSkyTintBytes(ent, kEnvSkyTint, tint);
		WriteEnvSkyTintBytes(ent, kEnvSkyTintLight, tint);
		if (Mem::IsReadable(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ent) + kEnvSkyEnabled), 1))
			*reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(ent) + kEnvSkyEnabled) = true;
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
			// Map change frees light bases — never write into recycled/freed memory
			if (reinterpret_cast<uintptr_t>(s.base) >= 0x10000ull
				&& Mem::IsUserPtr(s.base)
				&& Mem::IsReadable(s.base, 0x80))
			{
				__try {
					WriteColorAt(s.base, kOffLight, s.light);
					WriteColorAt(s.base, kOffAmbient1, s.a1);
					WriteColorAt(s.base, kOffAmbient2, s.a2);
					WriteColorAt(s.base, kOffAmbient3, s.a3);
					WriteColorAt(s.base, kOffSpecular, s.spec);
					*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(s.base) + kOffEnabled) = 1;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
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

	// Snap exposure back — write values while fades forced instant; restore fade last.
	void RestorePostInstant(void* ent, const SavedFloats& s) {
		if (!ent)
			return;
		ForceInstantExposure(ent, true);
		WriteFloat(ent, "C_PostProcessingVolume->m_flMinExposure", s.minExp);
		WriteFloat(ent, "C_PostProcessingVolume->m_flMaxExposure", s.maxExp);
		WriteFloat(ent, "C_PostProcessingVolume->m_flMinLogExposure", s.minLog);
		WriteFloat(ent, "C_PostProcessingVolume->m_flMaxLogExposure", s.maxLog);
		WriteFloat(ent, "C_PostProcessingVolume->m_flExposureCompensation", s.comp);
		WriteBool(ent, "C_PostProcessingVolume->m_bExposureControl", s.expCtrl);
		WriteBool(ent, "C_PostProcessingVolume->m_bMaster", s.master);
		ForceInstantExposure(ent, true); // re-force after writes so engine snaps, no long blend
		WriteFloat(ent, "C_PostProcessingVolume->m_flFadeDuration", 0.f);
		WriteFloat(ent, "C_PostProcessingVolume->m_flTonemapEVSmoothingRange", 0.f);
		// Keep fade speeds instant so disable is immediate (don't restore slow map fades)
		WriteFloat(ent, "C_PostProcessingVolume->m_flExposureFadeSpeedUp", 1.0e6f);
		WriteFloat(ent, "C_PostProcessingVolume->m_flExposureFadeSpeedDown", 1.0e6f);
	}

	void RestoreToneInstant(void* ent, const SavedFloats& s) {
		if (!ent)
			return;
		ForceInstantExposure(ent, false);
		WriteFloat(ent, "C_TonemapController2->m_flAutoExposureMin", s.autoMin);
		WriteFloat(ent, "C_TonemapController2->m_flAutoExposureMax", s.autoMax);
		ForceInstantExposure(ent, false);
		WriteFloat(ent, "C_TonemapController2->m_flTonemapEVSmoothingRange", 0.f);
		WriteFloat(ent, "C_TonemapController2->m_flExposureAdaptationSpeedUp", 1.0e6f);
		WriteFloat(ent, "C_TonemapController2->m_flExposureAdaptationSpeedDown", 1.0e6f);
	}

	// True only for live entity system objects (map change frees old ones).
	bool SavedEntLive(void* ent) {
		if (!ent || reinterpret_cast<uintptr_t>(ent) < 0x10000ull)
			return false;
		if (!Mem::IsUserPtr(ent))
			return false;
		// ValidEntity walks handle/identity — false after LevelShutdown free
		return Mem::ValidEntity(ent);
	}

	void RestoreNightOnly() {
		// Compact: restore post/tonemap/brightness; keep tint saves if skybox still on
		int write = 0;
		for (int i = 0; i < g_savedCount; ++i) {
			SavedFloats& s = g_saved[i];
			if (!s.valid || !s.ent)
				continue;
			if (!SavedEntLive(s.ent))
				continue; // drop stale map entry

			if (s.isPost) {
				RestorePostInstant(s.ent, s);
				s.isPost = false;
			}
			if (s.isTone) {
				RestoreToneInstant(s.ent, s);
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
			if (!SavedEntLive(s.ent))
				continue;

			if (s.isSkyTint) {
				WriteEnvSkyTintBytes(s.ent, kEnvSkyTint, s.skyTint);
				WriteEnvSkyTintBytes(s.ent, kEnvSkyTintLight, s.skyTintLight);
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
			if (!SavedEntLive(s.ent))
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
			if (!s.valid || !s.ent || !SavedEntLive(s.ent))
				continue;
			if (s.isPost)
				RestorePostInstant(s.ent, s);
			if (s.isTone)
				RestoreToneInstant(s.ent, s);
			if (s.isSkyBright)
				WriteFloat(s.ent, "C_EnvSky->m_flBrightnessScale", s.skyBright);
			if (s.isSkyTint) {
				WriteEnvSkyTintBytes(s.ent, kEnvSkyTint, s.skyTint);
				WriteEnvSkyTintBytes(s.ent, kEnvSkyTintLight, s.skyTintLight);
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
// Fixed snap table — no heap allocs on the render thread.
namespace {
	constexpr uintptr_t kSkyTintRgb = 0xE8; // IDA / Andromeda
	constexpr int kMaxSkySnaps = 32;

	struct SkyEntrySnap {
		void* ptr = nullptr;
		float r = 1.f, g = 1.f, b = 1.f;
	};
	SkyEntrySnap g_skySnap[kMaxSkySnaps]{};
	int g_skySnapCount = 0;

	static SkyEntrySnap* FindSkySnap(void* p) {
		for (int i = 0; i < g_skySnapCount; ++i)
			if (g_skySnap[i].ptr == p)
				return &g_skySnap[i];
		return nullptr;
	}

	static void WriteSkyObjTintSeh(void* skyObj, float r, float g, float b) {
		__try {
			if (!skyObj || !Mem::IsUserPtr(skyObj))
				return;
			float* rgb = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(skyObj) + kSkyTintRgb);
			if (!Mem::IsReadable(rgb, sizeof(float) * 3))
				return;
			rgb[0] = r;
			rgb[1] = g;
			rgb[2] = b;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	static bool ReadSkyObjTintSeh(void* skyObj, float out[3]) {
		__try {
			if (!skyObj || !Mem::IsUserPtr(skyObj))
				return false;
			float* rgb = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(skyObj) + kSkyTintRgb);
			if (!Mem::IsReadable(rgb, sizeof(float) * 3))
				return false;
			out[0] = rgb[0];
			out[1] = rgb[1];
			out[2] = rgb[2];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// IDA DrawSkyboxArray: v8 = *(a3 + a4 * 0x70 - 0x58)
	static void* SkyEntryAt(uintptr_t a3, int index1Based) {
		__try {
			if (!a3 || !Mem::IsUserPtr(reinterpret_cast<void*>(a3)) || index1Based <= 0 || index1Based > 64)
				return nullptr;
			void** slot = reinterpret_cast<void**>(
				a3 + static_cast<__int64>(index1Based) * 0x70 - 0x58);
			if (!Mem::IsReadable(slot, sizeof(void*)))
				return nullptr;
			void* p = *slot;
			if (!p || !Mem::IsUserPtr(p))
				return nullptr;
			return p;
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
		if (!std::isfinite(cur[0]) || !std::isfinite(cur[1]) || !std::isfinite(cur[2]))
			return;
		if (cur[0] < -8.f || cur[0] > 8.f || cur[1] < -8.f || cur[1] > 8.f
			|| cur[2] < -8.f || cur[2] > 8.f)
			return;

		if (Config::skybox) {
			if (!FindSkySnap(pEntry) && g_skySnapCount < kMaxSkySnaps) {
				SkyEntrySnap& s = g_skySnap[g_skySnapCount++];
				s.ptr = pEntry;
				s.r = cur[0];
				s.g = cur[1];
				s.b = cur[2];
			}

			const float r = std::clamp(Config::skybox_color.x, 0.f, 1.f);
			const float g = std::clamp(Config::skybox_color.y, 0.f, 1.f);
			const float b = std::clamp(Config::skybox_color.z, 0.f, 1.f);
			WriteSkyObjTintSeh(pEntry, r, g, b);
		}
		else if (SkyEntrySnap* s = FindSkySnap(pEntry)) {
			WriteSkyObjTintSeh(pEntry, s->r, s->g, s->b);
			*s = g_skySnap[--g_skySnapCount];
		}
	}

	void ClearSkySnaps() {
		// Restore originals before wipe (toggle-off path clears before next draw)
		for (int i = 0; i < g_skySnapCount; ++i) {
			if (g_skySnap[i].ptr)
				WriteSkyObjTintSeh(g_skySnap[i].ptr, g_skySnap[i].r, g_skySnap[i].g, g_skySnap[i].b);
			g_skySnap[i] = {};
		}
		g_skySnapCount = 0;
	}
}

void __fastcall H::hkDrawSkyboxArray(__int64 a1, __int64 a2, __int64 a3, int a4, int a5, __int64 a6, __int64 a7)
{
	auto original = DrawSkyboxArray.GetOriginal();

	__try {
		// a4 = 1-based entry index. Tint floats at sceneObj+0xE8 (IDA confirmed).
		if (a4 > 0 && a3 && !Config::loading.load(std::memory_order_acquire))
			ApplyOrRestoreSkyEntry(SkyEntryAt(static_cast<uintptr_t>(a3), a4));
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("hkDrawSkyboxArray", GetExceptionCode());
	}

	if (original)
		original(a1, a2, a3, a4, a5, a6, a7);
}

// Map color: DrawAggregateSceneObjectArray → lightData.
// IDA (scenesystem 0x18003B8B0): a3[1]+4=count, a3[1]+0x30=index;
//   light base = *(stackObj+0x18), entry = base + (index<<5), RGB at +0.
// Lighting: LightSceneObject (+0xE4) + GlobalLightUpdate (triples + dirty 0x69).
namespace {
	// Aggregate: pAggregateArr is __int64* — [1] is data blob (IDA v18)
	constexpr uintptr_t kAggArrData = 0x08;
	constexpr uintptr_t kAggCount = 0x04;
	constexpr uintptr_t kAggIndex = 0x30;
	constexpr int kLightEntryShift = 5; // 0x20 stride
	constexpr uintptr_t kLightDataOff = 0x18;
	constexpr int kMaxAggLightCount = 2048; // tighter than 8192 — bad counts crash

	constexpr uintptr_t kLightColorOff = 0xE4;
	constexpr float kLightColorBoost = 3.0f;

	constexpr uintptr_t kGlColorOffs[] = { 0x64, 0x6A, 0x6E, 0x72, 0x76 };
	constexpr uintptr_t kGlDirty = 0x69;

	// IDA-proven only: rip → CMemoryStack* (xmmword high qword), then +0x18 → light bytes
	// SceneSystem_002+0x2A28 removed — unverified on current build, corrupt/crash risk
	void** g_ppLightStackObj = nullptr;

	std::unordered_map<void*, std::array<float, 4>> g_lightSnap;
	constexpr size_t kMaxLightSnaps = 256;

	bool g_glSnapReady = false;
	void* g_glSnapObj = nullptr;
	uint8_t g_glSnap[5][3]{};

	void ResolveLightStackFromAggregate(uintptr_t fn) {
		if (!fn || g_ppLightStackObj)
			return;
		// Prefer hit inside DrawAggregate (IDA 0x18003C1D9: mov r14,[rip]; shl; … [r14+18h])
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

	uint8_t* GetLightDataFromStackNoSeh() {
		if (!g_ppLightStackObj || !Mem::IsReadable(g_ppLightStackObj, sizeof(void*)))
			return nullptr;
		void* stackObj = *g_ppLightStackObj;
		if (!stackObj || !Mem::IsReadable(stackObj, kLightDataOff + sizeof(void*)))
			return nullptr;
		uint8_t* base = *reinterpret_cast<uint8_t**>(
			reinterpret_cast<uintptr_t>(stackObj) + kLightDataOff);
		if (!base || reinterpret_cast<uintptr_t>(base) < 0x10000ull)
			return nullptr;
		return base;
	}

	uint8_t* GetLightDataSeh() {
		uint8_t* out = nullptr;
		__try { out = GetLightDataFromStackNoSeh(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
		return out;
	}

	float NightLightFactor() {
		if (!Config::Night)
			return 1.f;
		const float e = std::clamp(Config::night_exposure, 0.01f, 1.f);
		return 0.15f + (1.f - e) * 0.85f;
	}

	// Night darkness 0..1 → prop/mesh brightness scale (keeps slight visibility at max)
	float NightPropBright() {
		if (!Config::Night)
			return 1.f;
		const float d = std::clamp(Config::night_exposure, 0.f, 1.f);
		return std::clamp(1.f - d * 0.88f, 0.12f, 1.f);
	}

	// Frame-hot: DrawArray / Aggregate / GenPrim all call this — cache until config moves.
	struct WorldTintCache {
		bool mapOn = false;
		bool nightOn = false;
		float mx = -1.f, my = -1.f, mz = -1.f, mw = -1.f;
		float nightExp = -1.f;
		uint8_t r = 255, g = 255, b = 255;
		bool valid = false;
	};
	WorldTintCache g_tintRgb{};

	// Combined Map Color + Night → RGB bytes for props / aggregate light
	void ComputeWorldTintRgb(uint8_t& r, uint8_t& g, uint8_t& b) {
		const bool mapOn = Config::map_color;
		const bool nightOn = Config::Night;
		const auto& c = Config::map_color_value;
		const float nightExp = Config::night_exposure;
		if (g_tintRgb.valid
			&& g_tintRgb.mapOn == mapOn && g_tintRgb.nightOn == nightOn
			&& (!mapOn || (g_tintRgb.mx == c.x && g_tintRgb.my == c.y
				&& g_tintRgb.mz == c.z && g_tintRgb.mw == c.w))
			&& (!nightOn || g_tintRgb.nightExp == nightExp)) {
			r = g_tintRgb.r;
			g = g_tintRgb.g;
			b = g_tintRgb.b;
			return;
		}

		float rf = 1.f, gf = 1.f, bf = 1.f;
		if (mapOn) {
			const float strength = std::clamp(c.w, 0.f, 1.f);
			rf = (1.f - strength) + c.x * strength;
			gf = (1.f - strength) + c.y * strength;
			bf = (1.f - strength) + c.z * strength;
		}
		const float night = NightPropBright();
		rf *= night;
		gf *= night;
		bf *= night;
		r = static_cast<uint8_t>(std::clamp(rf * 255.f, 0.f, 255.f) + 0.5f);
		g = static_cast<uint8_t>(std::clamp(gf * 255.f, 0.f, 255.f) + 0.5f);
		b = static_cast<uint8_t>(std::clamp(bf * 255.f, 0.f, 255.f) + 0.5f);

		g_tintRgb.mapOn = mapOn;
		g_tintRgb.nightOn = nightOn;
		g_tintRgb.mx = c.x; g_tintRgb.my = c.y; g_tintRgb.mz = c.z; g_tintRgb.mw = c.w;
		g_tintRgb.nightExp = nightExp;
		g_tintRgb.r = r; g_tintRgb.g = g; g_tintRgb.b = b;
		g_tintRgb.valid = true;
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

	void TintOneLightBuffer(uint8_t* pLightData, int nIndex, int nCount, uint8_t r, uint8_t g, uint8_t b) {
		if (!pLightData || nIndex < 0 || nCount <= 0 || nCount > kMaxAggLightCount)
			return;
		// Guard full span before any write (OOB = random crash later)
		const size_t bytes = static_cast<size_t>(nIndex + nCount) << kLightEntryShift;
		if (bytes > (64ull << 20) || !Mem::IsReadable(pLightData, bytes))
			return;
		for (int i = 0; i < nCount; ++i) {
			uint8_t* entry = pLightData + (static_cast<uintptr_t>(nIndex + i) << kLightEntryShift);
			entry[0] = r;
			entry[1] = g;
			entry[2] = b;
			// leave entry[3] alpha / packing untouched
		}
	}

	void TintAggregateLightDataSeh(void* pAggregateArr, uint8_t r, uint8_t g, uint8_t b) {
		__try {
			if (!pAggregateArr || !Mem::IsReadable(pAggregateArr, 0x10))
				return;
			// IDA: a3[1] = aggregate data (count @+4, light index @+0x30)
			void* pData = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pAggregateArr) + kAggArrData);
			if (!pData || !Mem::IsReadable(pData, 0x40))
				return;
			const int nCount = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pData) + kAggCount);
			const int nIndex = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pData) + kAggIndex);
			// IDA uses -1 when slot unallocated — never write
			if (nIndex < 0 || nCount <= 0 || nCount > kMaxAggLightCount)
				return;
			TintOneLightBuffer(GetLightDataSeh(), nIndex, nCount, r, g, b);
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

	// true = restore originals into live light objects; false = map unload (ptrs dead).
	void FlushLightSnapsInstant(bool restore = true) {
		for (auto it = g_lightSnap.begin(); it != g_lightSnap.end(); ) {
			if (restore && it->first
				&& reinterpret_cast<uintptr_t>(it->first) >= 0x10000ull
				&& Mem::IsUserPtr(it->first)
				&& Mem::IsReadable(it->first, kLightColorOff + 16))
			{
				WriteLightFloatsSeh(it->first, it->second.data());
			}
			it = g_lightSnap.erase(it);
		}
		if (g_glSnapReady && g_glSnapObj) {
			if (restore
				&& reinterpret_cast<uintptr_t>(g_glSnapObj) >= 0x10000ull
				&& Mem::IsUserPtr(g_glSnapObj)
				&& Mem::IsReadable(g_glSnapObj, 0x80))
			{
				ApplyGlobalLightSeh(g_glSnapObj, false, false, 1.f, 1.f, 1.f, 1.f);
			}
			g_glSnapReady = false;
			g_glSnapObj = nullptr;
		}
	}

	// Map unload / LevelInit — NEVER write back into freed workshop entities.
	void DropAllWorldState() {
		g_envCount = 0;
		g_envScanMs = 0;
		g_savedCount = 0;
		for (int i = 0; i < kMaxSaved; ++i)
			g_saved[i] = {};
		// Hook light bases die with the map
		for (int i = 0; i < kMaxHookLights; ++i)
			g_hookLights[i] = {};
		FlushLightSnapsInstant(false);
		// Sky draw snaps: clear without restore (scene objs gone)
		g_skySnapCount = 0;
		for (int i = 0; i < kMaxSkySnaps; ++i)
			g_skySnap[i] = {};
		g_wasNight = false;
		g_wasSkybox = false;
		g_wasLighting = false;
	}
}

std::int64_t __fastcall H::hkDrawAggregateSceneObjectArray(void* a1, void* a2, void* pAggregateArr)
{
	auto original = DrawAggregateSceneObjectArray.GetOriginal();
	if (!original)
		return 0;

	// original FIRST (IDA fills light stack), then overwrite RGB
	const std::int64_t result = original(a1, a2, pAggregateArr);

	if ((Config::map_color || Config::Night) && pAggregateArr && g_ppLightStackObj) {
		uint8_t r = 255, g = 255, b = 255;
		ComputeWorldTintRgb(r, g, b);
		TintAggregateLightDataSeh(pAggregateArr, r, g, b);
	}

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
	// IDA scenesystem DrawAggregateSceneObjectArray @ 0x18003B8B0 (dump patterns.hpp)
	{
		uintptr_t addr = M::patternScan("scenesystem",
			"48 8B C4 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 D8 F8");
		if (!addr)
			addr = M::patternScan("scenesystem",
				"48 8B C4 48 89 50 ? 48 89 48 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70");
		if (!addr) {
			Con::OffsetMiss("DrawAggregateSceneObjectArray");
		} else {
			ResolveLightStackFromAggregate(addr);
			if (!g_ppLightStackObj)
				Con::Warn("MapColor: light stack unresolved — aggregate tint disabled");
			if (!H::DrawAggregateSceneObjectArray.Add(reinterpret_cast<void*>(addr),
				reinterpret_cast<void*>(&H::hkDrawAggregateSceneObjectArray)))
				Con::Error("DrawAggregateSceneObjectArray hook.Add failed @ 0x%p", (void*)addr);
			else
				Con::Ok("DrawAggregateSceneObjectArray @ 0x%p", (void*)addr);
		}
	}

	// GeneratePrimitives @ IDA 0x180075640 — per-entry prop tint (scene match only)
	{
		const uintptr_t addr = M::patternScan("scenesystem",
			"48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FE FF FF 48 81");
		if (!addr)
			Con::OffsetMiss("GeneratePrimitives");
		else if (!H::GeneratePrimitives.Add(reinterpret_cast<void*>(addr),
			reinterpret_cast<void*>(&H::hkGeneratePrimitives)))
			Con::Error("GeneratePrimitives hook.Add failed @ 0x%p", (void*)addr);
		else
			Con::Ok("GeneratePrimitives @ 0x%p", (void*)addr);
	}
}

bool World::WantPropTint()
{
	if (Config::loading.load(std::memory_order_acquire))
		return false;
	return Config::map_color || Config::Night;
}

void World::ApplyMapMeshColor(CMeshData* mesh, int meshCount)
{
	// Map / prop meshes — Map Color and/or Night prop darken (caller excludes players).
	// Same safety as chams ApplyChamsToMeshes: material check, same-owner siblings,
	// large batches → primary only. Blind 64-slot walk corrupted FX → AV in DrawArray.
	if (!WantPropTint() || !mesh || meshCount < 1)
		return;
	uint8_t r = 255, g = 255, b = 255;
	ComputeWorldTintRgb(r, g, b);
	const uint8_t a = 255;

	__try {
		if (!Mem::IsReadable(mesh, 0x58))
			return;
		if (!mesh->pMaterial || !Mem::IsUserPtr(mesh->pMaterial))
			return;

		mesh->color.r = r;
		mesh->color.g = g;
		mesh->color.b = b;
		mesh->color.a = a;

		// Death FX / fat batches — tint primary only
		if (meshCount <= 1 || meshCount > 12)
			return;

		void* owner0 = mesh->pSceneAnimatableObject;
		for (int i = 1; i < meshCount; ++i) {
			CMeshData* m = mesh->At(i);
			if (!m || !Mem::IsReadable(m, 0x58))
				break;
			if (!m->pMaterial || !Mem::IsUserPtr(m->pMaterial))
				break;
			if (m->pSceneAnimatableObject != owner0)
				break;
			m->color.r = r;
			m->color.g = g;
			m->color.b = b;
			m->color.a = a;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// GeneratePrimitives draw entry: 112 bytes (IDA UtlVector alloc), scene @+24, RGBA @+80
static constexpr int kGenPrimStride = 112;
static constexpr int kGenPrimSceneOff = 24;
static constexpr int kGenPrimColorOff = 80;

// Only tint when scene owns a world prop entity. Ownerless lists are often particles /
// FX — writing color there corrupts combat FX and crashes on hit/kill.
// Open-addressing cache (256) — old 64-slot linear scan re-dumped class on thrash.
struct TintableCacheEntry {
	void* sceneObj = nullptr;
	std::uint64_t expireMs = 0;
	bool tintable = false;
};
static TintableCacheEntry g_tintCache[256]{};

static int TintCacheSlot(void* p)
{
	// pointer mix — stable for open addressing
	const auto u = reinterpret_cast<std::uintptr_t>(p);
	return static_cast<int>(((u >> 4) ^ (u >> 12)) & 255u);
}

static bool SceneObjIsTintableWorldProp(void* sceneObj)
{
	if (!sceneObj || !Mem::IsReadable(sceneObj, 0xC8))
		return false;

	const std::uint64_t now = GetTickCount64();
	const int home = TintCacheSlot(sceneObj);
	// Probe home + 3 neighbors (cheap, no full 256 walk)
	for (int p = 0; p < 4; ++p) {
		const TintableCacheEntry& e = g_tintCache[(home + p) & 255];
		if (e.sceneObj == sceneObj && now < e.expireMs)
			return e.tintable;
	}

	bool tintable = false;
	__try {
		const CBaseHandle h = *reinterpret_cast<CBaseHandle*>(
			reinterpret_cast<uintptr_t>(sceneObj) + 0xC0);
		if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance) {
			tintable = false;
		} else {
			auto* ent = I::GameEntity->Instance->Get(h);
			if (!ent || !Mem::ValidEntity(ent)) {
				tintable = false;
			} else {
				SchemaClassInfoData_t* cls = nullptr;
				ent->dump_class_info(&cls);
				if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1)) {
					tintable = false;
				} else {
					const char* n = cls->szName;
					if (std::strstr(n, "Player") || std::strstr(n, "Ragdoll") || std::strstr(n, "HudModel")
						|| std::strstr(n, "Viewmodel") || std::strstr(n, "Wearable") || std::strstr(n, "Observer")
						|| std::strstr(n, "CSGO_Player") || std::strstr(n, "EconEntity"))
						tintable = false;
					else if (std::strstr(n, "Weapon") || std::strstr(n, "Knife") || std::strstr(n, "Grenade")
						|| std::strstr(n, "C4") || std::strstr(n, "PlantedC4") || std::strstr(n, "Inferno")
						|| std::strstr(n, "Projectile") || std::strstr(n, "Molotov") || std::strstr(n, "Flashbang")
						|| std::strstr(n, "HEGrenade") || std::strstr(n, "SmokeGrenade") || std::strstr(n, "Decoy")
						|| std::strstr(n, "Chicken") || std::strstr(n, "Hostage") || std::strstr(n, "Particle")
						|| std::strstr(n, "Effect") || std::strstr(n, "Sprite") || std::strstr(n, "Beam")
						|| std::strstr(n, "Smoke") || std::strstr(n, "Fire") || std::strstr(n, "EnvDetail"))
						tintable = false;
					else
						tintable = true;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		tintable = false;
	}

	// Prefer empty / expired / same-key slot near home
	int write = home;
	for (int p = 0; p < 4; ++p) {
		const int i = (home + p) & 255;
		if (!g_tintCache[i].sceneObj || g_tintCache[i].sceneObj == sceneObj
			|| now >= g_tintCache[i].expireMs) {
			write = i;
			break;
		}
	}
	TintableCacheEntry& slot = g_tintCache[write];
	slot.sceneObj = sceneObj;
	slot.tintable = tintable;
	// 3s TTL — props stable; miss cost is dump_class_info
	slot.expireMs = now + 3000ull;
	return tintable;
}

// Tint only entries that belong to this sceneObj (IDA: entry+24 = a2).
// Old path recolored whole drawList → particle/FX corruption + crash.
static void TintGenPrimVecForScene(void* base, int count, void* sceneObj, uint32_t packed)
{
	if (!base || !sceneObj || count <= 0 || count > 256)
		return;
	if (!Mem::IsReadable(base, static_cast<size_t>(count) * kGenPrimStride))
		return;
	auto* bytes = reinterpret_cast<uint8_t*>(base);
	for (int i = 0; i < count; ++i) {
		uint8_t* e = bytes + i * kGenPrimStride;
		void* entryScene = *reinterpret_cast<void**>(e + kGenPrimSceneOff);
		if (entryScene != sceneObj)
			continue;
		*reinterpret_cast<uint32_t*>(e + kGenPrimColorOff) = packed;
	}
}

static void TintGenPrimListSeh(void* drawList, void* sceneObj, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (!drawList || !sceneObj || !Mem::IsReadable(drawList, 0x28))
		return;
	__try {
		const uint32_t packed = static_cast<uint32_t>(r)
			| (static_cast<uint32_t>(g) << 8)
			| (static_cast<uint32_t>(b) << 16)
			| (static_cast<uint32_t>(a) << 24);

		// IDA drawList: primary vec base@+0 count@+12; overflow vec base@+24 count@+16
		void* base0 = *reinterpret_cast<void**>(drawList);
		const int count0 = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(drawList) + 12);
		TintGenPrimVecForScene(base0, count0, sceneObj, packed);

		void* base1 = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(drawList) + 24);
		const int count1 = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(drawList) + 16);
		TintGenPrimVecForScene(base1, count1, sceneObj, packed);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void __fastcall H::hkGeneratePrimitives(void* a1, void* sceneObj, void* a3, void* drawList)
{
	auto original = GeneratePrimitives.GetOriginal();
	if (!original)
		return;

	original(a1, sceneObj, a3, drawList);

	if (!World::WantPropTint() || !drawList || !sceneObj)
		return;
	if (!SceneObjIsTintableWorldProp(sceneObj))
		return;

	uint8_t r = 255, g = 255, b = 255;
	ComputeWorldTintRgb(r, g, b);
	TintGenPrimListSeh(drawList, sceneObj, r, g, b, 255);
}

// IDA ToneMapUpdate @ 0x1801874F0 — writes resolved min/max exposure to a1+136 / a1+140
float* __fastcall H::hkToneMapUpdate(void* tonemapState)
{
	auto original = ToneMapUpdate.GetOriginal();
	float* result = original ? original(tonemapState) : nullptr;
	if (!tonemapState || !Config::Night)
		return result;
	if (reinterpret_cast<uintptr_t>(tonemapState) < 0x10000ull)
		return result;

	const float d = std::clamp(Config::night_exposure, 0.f, 1.f);
	// Match entity ExposureFromDarkness: darker → lower exposure
	const float exp = std::clamp(1.0f - d * 0.88f, 0.12f, 1.0f);
	__try {
		float* base = reinterpret_cast<float*>(tonemapState);
		// outputs after resolve: +136 min, +140 max (bytes)
		float* outMin = reinterpret_cast<float*>(
			reinterpret_cast<char*>(tonemapState) + 136);
		float* outMax = reinterpret_cast<float*>(
			reinterpret_cast<char*>(tonemapState) + 140);
		if (Mem::IsReadable(outMin, 8)) {
			*outMin = exp;
			*outMax = exp;
		}
		(void)base;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return result;
}

// IDA UpdateLightObject @ 0x180199590 — second arg is light scene object
void __fastcall H::hkUpdateLightObject(void* sceneSys, void* lightObj, void* a3)
{
	auto original = UpdateLightObject.GetOriginal();
	// Tint light RGB before original enqueues (same +0xE4 as LightSceneObject)
	if (lightObj && reinterpret_cast<uintptr_t>(lightObj) >= 0x10000ull
		&& Mem::IsUserPtr(lightObj)
		&& (Config::lighting || Config::Night)) {
		float cur[4]{};
		if (ReadLightFloatsSeh(lightObj, cur)) {
			auto snapIt = g_lightSnap.find(lightObj);
			if (snapIt == g_lightSnap.end()) {
				if (g_lightSnap.size() < kMaxLightSnaps) {
					snapIt = g_lightSnap.emplace(
						lightObj,
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
					const float flBase = Config::lighting
						? (src[i] * kLightColorBoost)
						: snapIt->second[static_cast<size_t>(i)];
					out[i] = (i < 3) ? (flBase * flNight) : flBase;
				}
				WriteLightFloatsSeh(lightObj, out);
			}
		}
	}
	else if (lightObj && !Config::lighting && !Config::Night
		&& reinterpret_cast<uintptr_t>(lightObj) >= 0x10000ull
		&& Mem::IsUserPtr(lightObj)) {
		auto it = g_lightSnap.find(lightObj);
		if (it != g_lightSnap.end()) {
			if (Mem::IsReadable(lightObj, kLightColorOff + 16))
				WriteLightFloatsSeh(lightObj, it->second.data());
			g_lightSnap.erase(it);
		}
	}
	if (original)
		original(sceneSys, lightObj, a3);
}

void World::InstallLightingHook()
{
	// IDA scenesystem 0x180199590 UpdateLightObject (light RGB @ +0xE4).
	// UNIQUE sig only — short "48 89 54 24 ? 55 57 41 56 48 83 EC" is the SAME
	// function. Old LightSceneObject used that short sig first → SafetyHook
	// rewrote prologue → UpdateLightObject full scan PatternMiss.
	// Celerity misnames this DrawLightScene with the short pattern.
	{
		uintptr_t addr = M::patternScan("scenesystem",
			"48 89 54 24 10 55 57 41 56 48 83 EC 50 48 8B FA 48 8B E9 BA FF FF FF FF");
		if (!addr)
			addr = M::patternScan("scenesystem",
				"48 89 54 24 ? 55 57 41 56 48 83 EC 50 48 8B FA 48 8B E9 BA FF FF FF FF");
		if (!addr)
			addr = M::patternScan("scenesystem",
				"48 89 54 24 10 55 57 41 56 48 83 EC 50 48 8B FA");
		if (!addr)
			Con::OffsetMiss("UpdateLightObject");
		else if (!H::UpdateLightObject.Add(reinterpret_cast<void*>(addr),
			reinterpret_cast<void*>(&H::hkUpdateLightObject)))
			Con::Error("UpdateLightObject hook.Add failed @ 0x%p", (void*)addr);
		else
			Con::Ok("UpdateLightObject @ 0x%p", (void*)addr);
	}

	// LightSceneObject: do NOT scan short UpdateLightObject sig again (double-hook).
	// Keep hook object for ABI; lighting path is UpdateLightObject only.

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

	// ToneMapUpdate — GPU exposure resolve (IDA 0x1801874F0)
	{
		const uintptr_t addr = M::patternScan("scenesystem",
			"40 53 48 83 EC 60 48 8B D9 0F 29 74 24 50 48 8D");
		if (!addr)
			Con::OffsetMiss("ToneMapUpdate");
		else if (!H::ToneMapUpdate.Add(reinterpret_cast<void*>(addr),
			reinterpret_cast<void*>(&H::hkToneMapUpdate)))
			Con::Error("ToneMapUpdate hook.Add failed @ 0x%p", (void*)addr);
		else
			Con::Ok("ToneMapUpdate @ 0x%p", (void*)addr);
	}
}

void World::ApplySmokeColor()
{
	ApplySmokeColorTick();
}

// declared in removals.cpp
void ApplyPostProcessRemovalTick();

void World::InvalidateEnvCache()
{
	// LevelInit / LevelShutdown / map change:
	// Drop ALL night/sky/light saves + light snaps without writing back.
	// Writing original exposure into freed workshop post/tonemap entities
	// is the crash when Night is toggled off after a map switch.
	__try {
		DropAllWorldState();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_envCount = 0;
		g_envScanMs = 0;
		g_savedCount = 0;
		g_wasNight = false;
		g_wasSkybox = false;
		g_wasLighting = false;
	}
}

static void RefreshEnvIndexCache()
{
	const unsigned long long now = GetTickCount64();
	// Re-scan ~1.5 Hz (or immediately if empty) — env ents rarely move slots mid-map
	if (g_envCount > 0 && g_envScanMs != 0 && now < g_envScanMs + 1500ull)
		return;
	g_envScanMs = now;
	g_envCount = 0;

	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return;

	const int maxEnt = I::GameEntity->Instance->GetHighestEntityIndex();
	if (maxEnt <= 0 || maxEnt > 8192)
		return;

	for (int i = 0; i <= maxEnt && g_envCount < kMaxEnvIdx; ++i) {
		const char* designer = DesignerAtIndex(i);
		// Fast path: designer alone often enough for sky/light
		std::uint8_t kind = 0;
		if (IsEnvSkyDesigner(designer))
			kind |= kEnvSky;
		if (IsGlobalLightDesigner(designer))
			kind |= kEnvLight;

		void* ent = nullptr;
		const char* clsName = nullptr;
		// Only dump class when designer unknown — dump is the expensive bit
		if (!kind) {
			ent = EntityAtIndexLoose(i);
			if (!ent)
				continue;
			clsName = SafeDumpClassName(ent);
			if (!clsName || !clsName[0])
				continue;
		} else {
			// still resolve entity for later apply; skip class dump
			ent = EntityAtIndexLoose(i);
			if (!ent) {
				kind = 0;
			}
		}

		if (clsName) {
			if (IsEnvSkyClass(clsName))
				kind |= kEnvSky;
			if (IsGlobalLightClass(clsName))
				kind |= kEnvLight;
			const uint32_t h = HASH(clsName);
			if (h == HASH("C_PostProcessingVolume") || h == HASH("CPostProcessingVolume"))
				kind |= kEnvPost;
			else if (h == HASH("C_TonemapController2")
				|| h == HASH("C_TonemapController2Alias_env_tonemap_controller2")
				|| h == HASH("CTonemapController2"))
				kind |= kEnvTone;
			else if (strstr(clsName, "EnvSky"))
				kind |= kEnvSky;
		}

		if (!kind)
			continue;
		g_envSlots[g_envCount].idx = i;
		g_envSlots[g_envCount].kind = kind;
		++g_envCount;
	}
}

void World::Update()
{
	// Config load briefly clears Night — do not RestoreAll mid-apply
	if (Config::loading.load(std::memory_order_acquire))
		return;

	// Gradient fog (env_gradient_fog) — independent of Night/sky
	Fog::Update();

	// Smoke volume tint (entity m_vSmokeColor) — independent of Night/sky
	ApplySmokeColorTick();
	// Molotov/incendiary particle CP tint
	ApplyFireColorTick();
	// cl_disable_postprocessing ConVar poke (dump UpdatePostProcessing was wrong)
	ApplyPostProcessRemovalTick();

	const bool wantNight = Config::Night;
	const bool wantSky = Config::skybox;
	const bool wantLight = Config::lighting;

	// Nothing active → full restore if we previously modified anything
	if (!wantNight && !wantSky && !wantLight) {
		if (g_wasNight || g_wasSkybox || g_wasLighting) {
			__try {
				if (Ready()) {
					RestoreAll();
					FlushLightSnapsInstant(true);
				} else {
					// Not in-game / mid map swap — drop state, no restore writes
					DropAllWorldState();
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("World restore all", GetExceptionCode());
				DropAllWorldState();
			}
			g_wasNight = false;
			g_wasSkybox = false;
			g_wasLighting = false;
		}
		return;
	}

	if (!Ready()) {
		// Map unload with features still ON — purge stale saves so next map
		// re-snapshots live entities instead of reusing workshop pointers.
		if (g_wasNight || g_wasSkybox || g_wasLighting || g_savedCount > 0 || g_envCount > 0)
			DropAllWorldState();
		return;
	}

	if (!wantNight && g_wasNight) {
		__try {
			RestoreNightOnly();
			// Only flush light RGB when lighting feature is also off —
			// otherwise night-only disable must not wipe lighting snaps.
			if (!wantLight)
				FlushLightSnapsInstant(true);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("NightMode restore", GetExceptionCode());
			// Failed restore (stale ents) — drop remaining night-related saves
			g_savedCount = 0;
		}
		g_wasNight = false;
	}

	if (!wantSky && g_wasSkybox) {
		__try { RestoreSkyTintOnly(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("Skybox restore", GetExceptionCode());
		}
		ClearSkySnaps();
		g_wasSkybox = false;
	}

	if (!wantLight && g_wasLighting) {
		__try { RestoreLightingOnly(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("Lighting restore", GetExceptionCode());
		}
		g_wasLighting = false;
	}

	// Skip full env re-write when toggles + colors stable (game still fights exposure
	// slowly — re-apply every ~250ms so night/sky stick without every-frame schema writes).
	static bool s_lastN = false, s_lastS = false, s_lastL = false;
	static float s_lastNightExp = -1.f;
	static ImVec4 s_lastSky{}, s_lastLight{};
	static unsigned long long s_lastApplyMs = 0;
	const float nightExp = Config::night_exposure;
	const ImVec4 skyCol = Config::skybox_color;
	const ImVec4 lightColIm = Config::lighting_color;
	const unsigned long long nowMs = GetTickCount64();
	const bool cfgChanged =
		s_lastN != wantNight || s_lastS != wantSky || s_lastL != wantLight
		|| (wantNight && s_lastNightExp != nightExp)
		|| (wantSky && (s_lastSky.x != skyCol.x || s_lastSky.y != skyCol.y
			|| s_lastSky.z != skyCol.z || s_lastSky.w != skyCol.w))
		|| (wantLight && (s_lastLight.x != lightColIm.x || s_lastLight.y != lightColIm.y
			|| s_lastLight.z != lightColIm.z || s_lastLight.w != lightColIm.w));
	const bool dueRefresh = (s_lastApplyMs == 0) || (nowMs - s_lastApplyMs >= 250ull);
	if (!cfgChanged && !dueRefresh && g_envCount > 0
		&& (g_wasNight || g_wasSkybox || g_wasLighting))
		return;

	__try {
		const bool doNight = wantNight;
		const bool doSky = wantSky;
		const bool doLight = wantLight;
		float minExp = 0.1f, maxExp = 0.3f, compensation = -1.f;
		Color4 tint{};
		Color4 lightCol{};
		if (doNight)
			ExposureFromDarkness(nightExp, minExp, maxExp, compensation);
		if (doSky)
			tint = ImToColor4(skyCol);
		if (doLight)
			lightCol = ImToColor4(lightColIm);

		if (doNight) {
			g_wasNight = true;
			// Alive only — dead/respawn pawn camera services free mid-frame (TDM crash)
			if (C_CSPlayerPawn* lp = H::SafeLocalAlive()) {
				void* camera = nullptr;
				__try {
					camera = ReadPtr(lp, "C_BasePlayerPawn->m_pCameraServices");
					if (!camera)
						camera = ReadPtr(lp, "C_CSPlayerPawn->m_pCameraServices");
					if (camera && Mem::IsUserPtr(camera))
						ApplyFromCamera(camera, minExp, maxExp, compensation);
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
		}
		if (doSky)
			g_wasSkybox = true;
		if (doLight)
			g_wasLighting = true;

		RefreshEnvIndexCache();
		// Apply via cached kind bits — NO dump_class_info on hot path
		for (int k = 0; k < g_envCount; ++k) {
			const EnvSlot& slot = g_envSlots[k];
			void* ent = EntityAtIndexLoose(slot.idx);
			// Skip freed/recycled slots — never snapshot or write dead workshop ents
			if (!ent || !Mem::ValidEntity(ent))
				continue;

			if (doNight) {
				if (slot.kind & kEnvSky)
					ApplySkyBrightness(ent, nightExp);
				if (slot.kind & kEnvPost)
					ApplyPostVolume(ent, minExp, maxExp, compensation);
				if (slot.kind & kEnvTone)
					ApplyTonemap(ent, minExp, maxExp);
			}

			if (doSky && (slot.kind & kEnvSky))
				ApplySkyTint(ent, tint);

			if (doLight && (slot.kind & kEnvLight))
				ApplyLighting(ent, lightCol);
		}

		s_lastN = wantNight;
		s_lastS = wantSky;
		s_lastL = wantLight;
		s_lastNightExp = nightExp;
		s_lastSky = skyCol;
		s_lastLight = lightColIm;
		s_lastApplyMs = nowMs;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("World::Update", GetExceptionCode());
	}
}
