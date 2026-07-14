#include "weather.h"
#include "world.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/math/vector/vector.h"
#include "../../utils/schema/schema.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../visuals/visuals.h"
#include "../bones/bones.h"
#include "../../../debug/debug.h"
#include "../../utils/security/vacdetect.h"

#include <Windows.h>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

// IDA client particle path (precip sub_1809B1600):
//   CacheParticleEffect(attach=8 WORLDORIGIN), SetCP 0x1C enable, SetCP CP0 transform
// VPK rain_fx names: rain_storm_screen, rain_volume, rain_storm_outer, snow, snow_outer,
//   snow_drift, snow_hit_player_screeneffect, ash, ash_outer, ash_eddy_b
// NEVER spawn rain_storm (full) — puddle child sticks as yellow pot at create origin.

namespace {
	constexpr int kInvalidFx = -1;
	constexpr int kAttachWorldOrigin = 8;  // PATTACH_WORLDORIGIN (precip uses 8)
	constexpr int kAttachMainView = 11;    // PATTACH_MAIN_VIEW
	constexpr unsigned int kCpOrigin = 0;
	constexpr unsigned int kCpEnable = 0x1Cu;
	constexpr std::size_t kCBufferStringBytes = 256;
	constexpr std::uint32_t kCBufferAllocEmpty = 0xC0000088u;

	// HARD CAP — logs showed 60–72 systems → half FPS. Keep tiny.
	constexpr int kMaxFx = 16;
	constexpr float kCloudRadius = 80.f;
	constexpr float kLiftMin = 20.f;
	constexpr float kLiftMax = 160.f;

	struct FxSlot {
		int index = kInvalidFx;
		int attach = kAttachWorldOrigin;
		float ox = 0.f;
		float oy = 0.f;
		float oz = 0.f;
		bool follow = true;
	};

	using FnCacheParticle = unsigned int* (__fastcall*)(
		void* mgr, unsigned int* outIndex, const char* name, int attachType,
		void* entity, void* a6, void* a7, int a8);
	// IDA: void DestroyParticle(mgr, index, a3=0|1, a4=0) — a3=1 force
	using FnDestroyParticle = void (__fastcall*)(
		void* mgr, int index, unsigned char a3, char a4);
	using FnSetEffectF3 = char (__fastcall*)(
		void* mgr, int index, unsigned int cp, float* vec3, float time);
	using FnCBufferStringInit = char (__fastcall*)(void* buf, const char* str);
	using FnBlockingLoad = void* (__fastcall*)(void* rs, void* bufferString, const char* ext);
	using FnCBufferPurge = void (__fastcall*)(void* buf, int keepCapacity);

	void** g_ppParticleMgr = nullptr;
	void* g_resourceSystem = nullptr;
	FnCacheParticle g_cacheParticle = nullptr;
	FnSetEffectF3 g_setCpF3 = nullptr;
	FnDestroyParticle g_destroyParticle = nullptr;
	FnCBufferStringInit g_bufInit = nullptr;
	FnBlockingLoad g_blockingLoad = nullptr;
	FnCBufferPurge g_bufPurge = nullptr;
	bool g_ready = false;

	FxSlot g_fx[kMaxFx]{};
	int g_fxCount = 0;
	int g_activeKind = 0;
	int g_activeLayerBucket = -1;
	int g_createFails = 0;
	uintptr_t g_cachedMapInfo = 0;
	bool g_precacheDone[4]{}; // per-kind
	int g_recreateCooldown = 0;

	// Safe paths only (VPK rain_fx). NEVER rain_storm / rain_storm_outer / rain_spot
	// — those spawn ground "yellow pot" child FX we can't track/destroy.
	struct KindPaths {
		const char* screen; // MAIN_VIEW — primary visible
		const char* world;  // WORLDORIGIN volume (1–few)
		const char* world2; // optional second volume
	};

	KindPaths PathsFor(int mode) {
		switch (mode) {
		case 1: // Rain
			return {
				"particles/rain_fx/rain_storm_screen.vpcf",
				"particles/rain_fx/rain_volume.vpcf",
				nullptr
			};
		case 2: // Snow — screen effect is the only one that reads in FP
			return {
				"particles/rain_fx/snow_hit_player_screeneffect.vpcf",
				"particles/rain_fx/snow.vpcf",
				"particles/rain_fx/snow_outer.vpcf"
			};
		case 3: // Ash — MAIN_VIEW outer + light world ash
			return {
				"particles/rain_fx/ash_outer.vpcf",
				"particles/rain_fx/ash.vpcf",
				"particles/rain_fx/ash_eddy_b.vpcf"
			};
		default:
			return { nullptr, nullptr, nullptr };
		}
	}

	// Coarse intensity tiers so slider doesn't thrash recreate every tick
	int LayerBucket(float intensity01) {
		if (intensity01 < 0.34f) return 1;
		if (intensity01 < 0.67f) return 2;
		return 3;
	}

	void* GetMgr() {
		if (!g_ppParticleMgr)
			return nullptr;
		__try {
			return *g_ppParticleMgr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	char BufInitSeh(void* buf, const char* path) {
		char ok = 0;
		__try {
			ok = g_bufInit(buf, path);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			ok = 0;
		}
		return ok;
	}

	void* BlockingLoadSeh(void* buf) {
		void* result = nullptr;
		__try {
			if (g_blockingLoad && g_resourceSystem)
				result = g_blockingLoad(g_resourceSystem, buf, "");
			else if (g_resourceSystem) {
				auto** vt = *reinterpret_cast<void***>(g_resourceSystem);
				if (vt && vt[41])
					result = reinterpret_cast<FnBlockingLoad>(vt[41])(g_resourceSystem, buf, "");
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			result = nullptr;
		}
		return result;
	}

	void BufPurgeSeh(void* buf) {
		if (!g_bufPurge || !buf)
			return;
		__try {
			g_bufPurge(buf, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	bool PrecachePathSeh(const char* path) {
		if (!g_resourceSystem || !path || !g_bufInit)
			return false;
		alignas(16) unsigned char storage[kCBufferStringBytes];
		std::memset(storage, 0, sizeof(storage));
		*reinterpret_cast<std::int32_t*>(storage + 0) = 0;
		*reinterpret_cast<std::uint32_t*>(storage + 4) = kCBufferAllocEmpty;
		if (!BufInitSeh(storage, path)) {
			BufPurgeSeh(storage);
			return false;
		}
		void* result = BlockingLoadSeh(storage);
		BufPurgeSeh(storage);
		return result != nullptr;
	}

	char SetCpF3Seh(void* mgr, int index, unsigned int cp, float a, float b, float c) {
		if (!mgr || !g_setCpF3 || index == kInvalidFx)
			return 0;
		float v[3] = { a, b, c };
		char ok = 0;
		__try {
			ok = g_setCpF3(mgr, index, cp, v, 0.f);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			ok = 0;
		}
		return ok;
	}

	// IDA sub_180989AF0 — soft (0,0) then force (1,0). No extra vtable release.
	void DestroyOneSeh(void* mgr, int index, unsigned char force) {
		if (!mgr || !g_destroyParticle || index == kInvalidFx)
			return;
		__try {
			g_destroyParticle(mgr, index, force, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			ADLogf("E", "weather.cpp:DestroyOneSeh", "seh",
				"{\"code\":%u,\"idx\":%d,\"force\":%u}", GetExceptionCode(), index, force);
		}
	}

	void DestroyAll() {
		void* mgr = GetMgr();
		const int n = g_fxCount;
		int indices[kMaxFx];
		int count = 0;
		for (int i = 0; i < n && count < kMaxFx; ++i) {
			if (g_fx[i].index != kInvalidFx)
				indices[count++] = g_fx[i].index;
			g_fx[i].index = kInvalidFx;
		}
		// Soft stop
		for (int i = 0; i < count; ++i)
			DestroyOneSeh(mgr, indices[i], 0);
		// Force stop leftovers (ash ground splash, rain_storm debris)
		for (int i = 0; i < count; ++i)
			DestroyOneSeh(mgr, indices[i], 1);

		g_fxCount = 0;
		g_activeKind = 0;
		g_activeLayerBucket = -1;
		ADLogf("C", "weather.cpp:DestroyAll", "done", "{\"n\":%d}", count);
	}

	int CreateOne(void* mgr, const char* path, int attach, float ox, float oy, float oz, bool follow) {
		if (!mgr || !g_cacheParticle || !path || g_fxCount >= kMaxFx)
			return kInvalidFx;

		unsigned int idx = static_cast<unsigned int>(-1);
		int seh = 0;
		__try {
			g_cacheParticle(mgr, &idx, path, attach, nullptr, nullptr, nullptr, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			seh = static_cast<int>(GetExceptionCode());
		}
		if (seh || idx == static_cast<unsigned int>(-1)) {
			ADLogf("C", "weather.cpp:CreateOne", "fail",
				"{\"path\":\"%s\",\"attach\":%d,\"seh\":%d,\"idx\":%u}",
				path, attach, seh, idx);
			return kInvalidFx;
		}

		const int iIdx = static_cast<int>(idx);
		// Enable only — CP0 world pos written in UpdatePositions
		SetCpF3Seh(mgr, iIdx, kCpEnable, 1.f, 0.f, 0.f);

		FxSlot& s = g_fx[g_fxCount++];
		s.index = iIdx;
		s.attach = attach;
		s.ox = ox;
		s.oy = oy;
		s.oz = oz;
		s.follow = follow;
		return iIdx;
	}

	bool GetLocalOrigin(float& x, float& y, float& z);
	void UpdatePositions(float px, float py, float pz);

	void PrecacheKind(int kind, const KindPaths& p, bool force = false) {
		if (kind < 0 || kind > 3)
			return;
		if (g_precacheDone[kind] && !force)
			return;
		int n = 0;
		const char* all[] = { p.screen, p.world, p.world2 };
		for (const char* path : all) {
			if (!path)
				continue;
			if (PrecachePathSeh(path))
				++n;
		}
		// Snow screen alts
		if (kind == 2) {
			if (PrecachePathSeh("particles/rain_fx/snow_hit_player_screeneffect_b.vpcf"))
				++n;
			if (PrecachePathSeh("particles/rain_fx/snow_hit_player_screeneffect_bb.vpcf"))
				++n;
		}
		if (n > 0)
			g_precacheDone[kind] = true;
		ADLogf("C", "weather.cpp:PrecacheKind", "done",
			"{\"kind\":%d,\"ok\":%d,\"screen\":\"%s\"}", kind, n, p.screen ? p.screen : "");
	}

	// Few world emitters above the player (not a 50-point spiral)
	int SpawnWorldRing(void* mgr, const char* path, int count, float radius, float zOff) {
		if (!path || count <= 0)
			return 0;
		int made = 0;
		// Always one at center (above head)
		if (CreateOne(mgr, path, kAttachWorldOrigin, 0.f, 0.f, zOff, true) != kInvalidFx)
			++made;
		for (int i = 1; i < count && g_fxCount < kMaxFx; ++i) {
			const float ang = (6.2831853f * static_cast<float>(i)) / static_cast<float>(count);
			const float ox = std::cos(ang) * radius;
			const float oy = std::sin(ang) * radius;
			if (CreateOne(mgr, path, kAttachWorldOrigin, ox, oy, zOff + 20.f * (i & 1), true) != kInvalidFx)
				++made;
		}
		return made;
	}

	int SpawnScreen(void* mgr, const char* path, int count) {
		if (!path || count <= 0)
			return 0;
		int made = 0;
		for (int s = 0; s < count && g_fxCount < kMaxFx; ++s) {
			if (CreateOne(mgr, path, kAttachMainView, 0.f, 0.f, 0.f, false) != kInvalidFx)
				++made;
		}
		return made;
	}

	bool CreateKind(int kind, float intensity01) {
		DestroyAll();

		const KindPaths paths = PathsFor(kind);
		if (!paths.world && !paths.screen)
			return false;

		PrecacheKind(kind, paths, !g_precacheDone[kind]);

		void* mgr = GetMgr();
		if (!mgr || !g_cacheParticle) {
			ADLog("C", "weather.cpp:CreateKind", "no_mgr", "{}");
			++g_createFails;
			return false;
		}

		const int bucket = LayerBucket(intensity01);
		// Tiny counts — intensity only adds a couple emitters
		// bucket 1/2/3 → screen 1/2/3, world 2/3/4
		const int screenN = bucket;           // 1..3
		const int worldN  = 1 + bucket;       // 2..4
		int made = 0;

		if (kind == 1) {
			// Rain: 1–3 camera sheets + 2–4 volume (NO storm/outer → no yellow pot)
			made += SpawnScreen(mgr, paths.screen, screenN);
			made += SpawnWorldRing(mgr, paths.world, worldN, 45.f + 15.f * bucket, 90.f);
		} else if (kind == 2) {
			// Snow: screen is mandatory for visibility; world flakes optional depth
			made += SpawnScreen(mgr, paths.screen, screenN + 1); // 2..4 screen
			if (made == 0) {
				made += SpawnScreen(mgr, "particles/rain_fx/snow_hit_player_screeneffect_b.vpcf", screenN + 1);
			}
			if (made == 0) {
				made += SpawnScreen(mgr, "particles/rain_fx/snow_hit_player_screeneffect_bb.vpcf", screenN + 1);
			}
			// A few world flakes above player (cheap)
			made += SpawnWorldRing(mgr, paths.world, worldN, 50.f, 100.f);
			if (paths.world2 && bucket >= 2)
				made += SpawnWorldRing(mgr, paths.world2, 1, 70.f, 120.f);
		} else {
			// Ash: MAIN_VIEW outer (visible) + few world ash
			made += SpawnScreen(mgr, paths.screen, screenN + 1);
			made += SpawnWorldRing(mgr, paths.world, worldN, 55.f, 80.f);
			if (paths.world2 && bucket >= 2)
				made += SpawnWorldRing(mgr, paths.world2, 1, 40.f, 60.f);
		}

		float px = 0.f, py = 0.f, pz = 0.f;
		if (GetLocalOrigin(px, py, pz))
			UpdatePositions(px, py, pz);
		else {
			for (int i = 0; i < g_fxCount; ++i) {
				if (g_fx[i].index != kInvalidFx)
					SetCpF3Seh(mgr, g_fx[i].index, kCpEnable, 1.f, 0.f, 0.f);
			}
		}

		g_activeKind = g_fxCount > 0 ? kind : 0;
		g_activeLayerBucket = bucket;
		g_recreateCooldown = 90; // don't thrash on slider
		const int i0 = (g_fxCount > 0) ? g_fx[0].index : -1;
		const int i1 = (g_fxCount > 1) ? g_fx[1].index : -1;
		ADLogf("C", "weather.cpp:CreateKind", "create",
			"{\"kind\":%d,\"n\":%d,\"made\":%d,\"bucket\":%d,\"path\":\"%s\",\"px\":%.1f,\"py\":%.1f,\"pz\":%.1f,\"i0\":%d,\"i1\":%d}",
			kind, g_fxCount, made, bucket, paths.screen ? paths.screen : (paths.world ? paths.world : ""),
			px, py, pz, i0, i1);

		if (g_fxCount == 0) {
			++g_createFails;
			g_precacheDone[kind] = false;
			return false;
		}
		g_createFails = 0;
		return true;
	}

	void UpdatePositions(float px, float py, float pz) {
		void* mgr = GetMgr();
		if (!mgr)
			return;

		for (int i = 0; i < g_fxCount; ++i) {
			FxSlot& s = g_fx[i];
			if (s.index == kInvalidFx)
				continue;

			SetCpF3Seh(mgr, s.index, kCpEnable, 1.f, 0.f, 0.f);

			// Only WORLDORIGIN follow — leave MAIN_VIEW alone (camera-bound)
			if (s.follow && s.attach == kAttachWorldOrigin) {
				SetCpF3Seh(mgr, s.index, kCpOrigin,
					px + s.ox, py + s.oy, pz + s.oz);
			}
		}
	}

	bool GetLocalOrigin(float& x, float& y, float& z) {
		if (!H::oGetLocalPlayer)
			return false;
		C_CSPlayerPawn* local = nullptr;
		__try {
			local = H::oGetLocalPlayer(0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (!local)
			return false;

		Vector_t abs{};
		Vector_t eye{};
		Vector_t old{};
		Vector_t shoot{};
		bool gotAbs = false, gotEye = false, gotOld = false, gotShoot = false;

		__try {
			gotAbs = Bones::GetOrigin(local, abs) && Bones::IsValidPos(abs);
			eye = Bones::GetEyePos(local);
			gotEye = Bones::IsValidPos(eye);
			old = local->m_vOldOrigin();
			gotOld = std::isfinite(old.x) && std::isfinite(old.y)
				&& (std::fabs(old.x) > 0.1f || std::fabs(old.y) > 0.1f);
			shoot = Bones::GetShootPos(local);
			gotShoot = Bones::IsValidPos(shoot);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}

		// Prefer eye/shoot (camera) so cloud offsets sit in FOV, not feet
		if (gotEye) {
			x = eye.x; y = eye.y; z = eye.z;
			return true;
		}
		if (gotShoot) {
			x = shoot.x; y = shoot.y; z = shoot.z;
			return true;
		}
		if (gotAbs && (std::fabs(abs.x) > 0.1f || std::fabs(abs.y) > 0.1f || std::fabs(abs.z) > 0.1f)) {
			x = abs.x; y = abs.y; z = abs.z + 64.f; // lift feet origin to chest
			return true;
		}
		if (gotOld) {
			x = old.x; y = old.y; z = old.z + 64.f;
			return true;
		}
		return false;
	}

	// ---- CMapInfo wetness (rain materials only when kind==1) ----

	bool  g_wetSaved = false;
	bool  g_savedRainTrace = false;
	float g_savedRainStr = 0.f;
	float g_savedWet = 0.f;
	float g_savedPuddle = 0.f;
	float g_savedPuddleDir = 0.f;
	float g_savedDry = 0.f;
	uintptr_t g_wetSavedFor = 0;

	void ApplyMapInfoFields(uintptr_t mapInfo, int kind, float intensity01) {
		if (!mapInfo)
			return;
		__try {
			const uint32_t offRainTrace = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_bRainTraceToSkyEnabled"));
			const uint32_t offRainStr = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvRainStrength"));
			const uint32_t offWet = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvWetnessCoverage"));
			const uint32_t offPuddle = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvPuddleRippleStrength"));
			const uint32_t offPuddleDir = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvPuddleRippleDirection"));
			const uint32_t offDry = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvWetnessDryingAmount"));
			auto* base = reinterpret_cast<uint8_t*>(mapInfo);

			if (!g_wetSaved || g_wetSavedFor != mapInfo) {
				if (offRainTrace)
					g_savedRainTrace = *reinterpret_cast<bool*>(base + offRainTrace);
				if (offRainStr)
					g_savedRainStr = *reinterpret_cast<float*>(base + offRainStr);
				if (offWet)
					g_savedWet = *reinterpret_cast<float*>(base + offWet);
				if (offPuddle)
					g_savedPuddle = *reinterpret_cast<float*>(base + offPuddle);
				if (offPuddleDir)
					g_savedPuddleDir = *reinterpret_cast<float*>(base + offPuddleDir);
				if (offDry)
					g_savedDry = *reinterpret_cast<float*>(base + offDry);
				g_wetSaved = true;
				g_wetSavedFor = mapInfo;
			}

			if (kind == 1) {
				// Strong wetness so rain materials + puddles read clearly
				const float wet = std::clamp(0.75f + 0.25f * intensity01, 0.f, 1.f);
				if (offRainTrace)
					*reinterpret_cast<bool*>(base + offRainTrace) = true;
				if (offRainStr)
					*reinterpret_cast<float*>(base + offRainStr) = wet;
				if (offWet)
					*reinterpret_cast<float*>(base + offWet) = wet;
				if (offPuddle)
					*reinterpret_cast<float*>(base + offPuddle) = wet;
				if (offPuddleDir)
					*reinterpret_cast<float*>(base + offPuddleDir) = 0.65f;
				if (offDry)
					*reinterpret_cast<float*>(base + offDry) = 0.001f;
			} else {
				// Snow/ash: leave map wetness at saved (not rain wet)
				if (offRainTrace)
					*reinterpret_cast<bool*>(base + offRainTrace) = g_savedRainTrace;
				if (offRainStr)
					*reinterpret_cast<float*>(base + offRainStr) = g_savedRainStr;
				if (offWet)
					*reinterpret_cast<float*>(base + offWet) = g_savedWet;
				if (offPuddle)
					*reinterpret_cast<float*>(base + offPuddle) = g_savedPuddle;
				if (offPuddleDir)
					*reinterpret_cast<float*>(base + offPuddleDir) = g_savedPuddleDir;
				if (offDry)
					*reinterpret_cast<float*>(base + offDry) = g_savedDry;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void RestoreMapInfoFields() {
		if (!g_wetSaved || !g_wetSavedFor)
			return;
		__try {
			const uint32_t offRainTrace = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_bRainTraceToSkyEnabled"));
			const uint32_t offRainStr = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvRainStrength"));
			const uint32_t offWet = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvWetnessCoverage"));
			const uint32_t offPuddle = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvPuddleRippleStrength"));
			const uint32_t offPuddleDir = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvPuddleRippleDirection"));
			const uint32_t offDry = SchemaFinder::Get(
				hash_32_fnv1a_const("CMapInfo->m_flEnvWetnessDryingAmount"));
			auto* base = reinterpret_cast<uint8_t*>(g_wetSavedFor);
			if (offRainTrace)
				*reinterpret_cast<bool*>(base + offRainTrace) = g_savedRainTrace;
			if (offRainStr)
				*reinterpret_cast<float*>(base + offRainStr) = g_savedRainStr;
			if (offWet)
				*reinterpret_cast<float*>(base + offWet) = g_savedWet;
			if (offPuddle)
				*reinterpret_cast<float*>(base + offPuddle) = g_savedPuddle;
			if (offPuddleDir)
				*reinterpret_cast<float*>(base + offPuddleDir) = g_savedPuddleDir;
			if (offDry)
				*reinterpret_cast<float*>(base + offDry) = g_savedDry;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		g_wetSaved = false;
		g_wetSavedFor = 0;
	}

	uintptr_t FindMapInfoEntity() {
		if (!I::GameEntity || !I::GameEntity->Instance || !H::ogGetBaseEntity)
			return 0;
		const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
		const uint32_t want = hash_32_fnv1a_const("CMapInfo");
		for (int i = 0; i <= nMax && i < 4096; ++i) {
			void* raw = nullptr;
			__try {
				raw = H::ogGetBaseEntity(I::GameEntity->Instance, i);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				continue;
			}
			if (!raw)
				continue;
			auto* ent = reinterpret_cast<C_BaseEntity*>(raw);
			SchemaClassInfoData_t* cls = nullptr;
			__try {
				ent->dump_class_info(&cls);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				continue;
			}
			if (!cls || !cls->szName)
				continue;
			if (hash_32_fnv1a_const(cls->szName) == want)
				return reinterpret_cast<uintptr_t>(raw);
		}
		return 0;
	}

	FnBlockingLoad BindBlockingLoadFromVtableSeh(void* rs) {
		FnBlockingLoad out = nullptr;
		if (!rs)
			return out;
		__try {
			void*** vt = reinterpret_cast<void***>(rs);
			void** table = *vt;
			if (table && table[41])
				out = reinterpret_cast<FnBlockingLoad>(table[41]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out = nullptr;
		}
		return out;
	}
}

void World::Weather::Install()
{
	DestroyAll();
	g_createFails = 0;
	for (int i = 0; i < 4; ++i)
		g_precacheDone[i] = false;

	g_resourceSystem = I::Get<void>("resourcesystem.dll", "ResourceSystem013");

	// Patterns from cs2-sdk dump (patterns.hpp) — verified IDA client 2b230e4d
	const uintptr_t mgrRip = M::patternScan("client",
		"48 8B 0D ? ? ? ? 41 B8 ? ? ? ? F3 0F 11 74 24 ? 48 C7 44 24");
	if (mgrRip)
		g_ppParticleMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(mgrRip, 3));
	else
		Con::OffsetMiss("Weather::ParticleManager");

	// CacheParticleEffect — pattern::client::CacheParticleEffect → 0x18078EE10
	const uintptr_t cacheAddr = M::patternScan("client",
		"4C 8B DC 53 48 81 EC 90 00 00 00 F2 0F 10 05");
	if (cacheAddr)
		g_cacheParticle = reinterpret_cast<FnCacheParticle>(cacheAddr);
	else
		Con::OffsetMiss("Weather::CacheParticleEffect");

	// SetParticleControl float3 — IDA sub_1809C5090
	const uintptr_t setF3 = M::patternScan("client",
		"48 83 EC 58 F3 41 0F 10 51 04 F3 41 0F 10 09 F3 41 0F 10 59 08");
	if (setF3)
		g_setCpF3 = reinterpret_cast<FnSetEffectF3>(setF3);
	else
		Con::OffsetMiss("Weather::SetParticleControlF3");

	// DestroyParticle — pattern::client::DestroyParticle → 0x180989AF0
	const uintptr_t destroyAddr = M::patternScan("client",
		"83 FA FF 0F 84 ? ? ? ? 41 54 41 56 41 57 48");
	if (destroyAddr)
		g_destroyParticle = reinterpret_cast<FnDestroyParticle>(destroyAddr);
	else
		Con::OffsetMiss("Weather::DestroyParticle");

	// CBufferStringInit — pattern::client::CBufferStringInit
	const uintptr_t bufInit = M::patternScan("client",
		"48 89 5C 24 10 57 48 83 EC 30 8B 41 04 48 8D 79");
	if (bufInit)
		g_bufInit = reinterpret_cast<FnCBufferStringInit>(bufInit);

	const uintptr_t loadAddr = M::patternScan("resourcesystem",
		"40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA 48 8B D9 FF 90");
	if (loadAddr)
		g_blockingLoad = reinterpret_cast<FnBlockingLoad>(loadAddr);
	else if (g_resourceSystem)
		g_blockingLoad = BindBlockingLoadFromVtableSeh(g_resourceSystem);

	if (const HMODULE tier0 = GetModuleHandleA("tier0.dll")) {
		g_bufPurge = reinterpret_cast<FnCBufferPurge>(
			GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z"));
	}

	g_ready = g_ppParticleMgr && g_cacheParticle && g_setCpF3 && g_destroyParticle;
	Con::Info("[Weather] Install ready=%d destroy=%p (mode-isolated rain/snow/ash)",
		g_ready ? 1 : 0, reinterpret_cast<void*>(g_destroyParticle));
	ADLogf("B", "weather.cpp:Install", "ready",
		"{\"ready\":%d,\"mgr\":%d,\"cache\":%d,\"setcp\":%d,\"destroy\":%d}",
		g_ready ? 1 : 0, g_ppParticleMgr ? 1 : 0, g_cacheParticle ? 1 : 0,
		g_setCpF3 ? 1 : 0, g_destroyParticle ? 1 : 0);
}

void World::Weather::Shutdown()
{
	DestroyAll();
	g_cachedMapInfo = 0;
	g_wetSaved = false;
	g_wetSavedFor = 0;
	g_createFails = 0;
}

void World::Weather::ApplyMapInfo(uintptr_t mapInfo)
{
	if (!mapInfo)
		return;
	if (!Config::weather || Config::weather_mode < 1 || Config::weather_mode > 3)
		return;
	g_cachedMapInfo = mapInfo;
	ApplyMapInfoFields(mapInfo, Config::weather_mode,
		std::clamp(Config::weather_intensity, 0.f, 1.f));
}

void World::Weather::Draw()
{
	// Engine particles only
}

void World::Weather::Update()
{
	// Soft-pause: kill particles + restore map wetness (no engine weather while scanning)
	if (VacDetect::IsSoftPaused()) {
		if (g_fxCount > 0)
			DestroyAll();
		RestoreMapInfoFields();
		return;
	}

	const bool want = Config::weather
		&& Config::weather_mode >= 1
		&& Config::weather_mode <= 3;

	if (!want) {
		if (g_fxCount > 0)
			DestroyAll();
		RestoreMapInfoFields();
		return;
	}

	const int kind = Config::weather_mode;
	const float intensity = std::clamp(Config::weather_intensity, 0.f, 1.f);
	const int layers = LayerBucket(intensity);

	if (!g_cachedMapInfo) {
		static int s_scanCooldown = 0;
		if (--s_scanCooldown <= 0) {
			s_scanCooldown = 64;
			g_cachedMapInfo = FindMapInfoEntity();
		}
	}
	if (g_cachedMapInfo)
		ApplyMapInfoFields(g_cachedMapInfo, kind, intensity);

	// Soft fail budget — keep retrying (snow often fails first frames before precache sticks)
	if (!g_ready || g_createFails >= 24)
		return;

	if (g_recreateCooldown > 0)
		--g_recreateCooldown;

	// Mode change always; density bucket after short cooldown
	const bool modeChanged = (g_activeKind != kind);
	const bool densChanged = (g_activeLayerBucket != layers);
	const bool needCreate = (g_fxCount == 0)
		|| modeChanged
		|| (densChanged && g_recreateCooldown <= 0 && std::abs(layers - g_activeLayerBucket) >= 1);

	if (needCreate) {
		if (modeChanged)
			g_precacheDone[kind] = false;
		if (!CreateKind(kind, intensity))
			return;
	}

	float px = 0.f, py = 0.f, pz = 0.f;
	if (!GetLocalOrigin(px, py, pz))
		return;

	UpdatePositions(px, py, pz);

	static int s_updLog = 0;
	if (s_updLog < 8) {
		ADLogf("B", "weather.cpp:Update", "follow",
			"{\"kind\":%d,\"n\":%d,\"bucket\":%d,\"px\":%.2f,\"py\":%.2f,\"pz\":%.2f}",
			kind, g_fxCount, layers, px, py, pz);
		++s_updLog;
	}
}
