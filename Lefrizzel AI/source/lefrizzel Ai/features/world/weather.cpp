#include "weather.h"
#include "world.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/math/vector/vector.h"
#include "../../utils/schema/schema.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../visuals/visuals.h"
#include "../bones/bones.h"
#include "../../utils/security/vacdetect.h"
#include "../../utils/console/console.h"
#include "../../../debug/debug.h"
#include "../../../../external/imgui/imgui.h"
#include "world_particle_assets.hpp"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <string>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>

// Engine rain_fx is either an invisible baked disc or stacked MAIN_VIEW that tanks FPS.
// Visible weather = ImGui Draw(); Update() only drives CMapInfo wetness.

namespace {
	constexpr int kInvalidFx = -1;
	constexpr int kAttachWorldOrigin = 8;     // PATTACH_WORLDORIGIN
	constexpr int kAttachCustomOrigin = 2;    // PATTACH_CUSTOMORIGIN
	constexpr int kAttachAbsOriginFollow = 1; // PATTACH_ABSORIGIN_FOLLOW
	constexpr int kAttachMainView = 11;       // PATTACH_MAIN_VIEW — camera volume
	constexpr unsigned int kCpOrigin = 0;
	constexpr unsigned int kCpEnable = 0x1Cu;
	constexpr std::size_t kCBufferStringBytes = 256;
	constexpr std::uint32_t kCBufferAllocEmpty = 0xC0000088u;

	// Rain full grid: 3 MAIN_VIEW + 3 center + 4 cardinal + 4 corners + 4 ring = 18
	constexpr int kMaxFx = 24;
	constexpr float kTwoPi = 6.28318530718f;

	struct FxSlot {
		int index = kInvalidFx;
		float ox = 0.f;
		float oy = 0.f;
		float oz = 0.f;
		const char* path = nullptr;
		int attach = kAttachCustomOrigin;
	};

	// UC: CacheParticleEffect(this, pEffectIndex, szName, attach, entity, 0,0,0)
	using FnCacheParticle = unsigned int* (__fastcall*)(
		void* mgr, unsigned int* outIndex, const char* name, int attachType,
		void* entity, void* a6, void* a7, int a8);
	using FnDestroyParticle = void (__fastcall*)(
		void* mgr, int index, unsigned char a3, char a4);
	// UC "CreateParticleEffect" = SetEffect float3 — IDA sub_1809C54E0
	using FnSetEffectF3 = char (__fastcall*)(
		void* mgr, int index, unsigned int cp, float* vec3, float time);
	// Celerity SetParticleSettings — IDA 0x1809C5540
	using FnSetParticleSettings = bool (__fastcall*)(
		void* mgr, unsigned int index, int cp, void* vec3, int a5);
	using FnGetParticleMgr = void* (__fastcall*)();
	using FnCBufferStringInit = char (__fastcall*)(void* buf, const char* str);
	using FnBlockingLoad = void* (__fastcall*)(void* rs, void* bufferString, const char* ext);
	using FnCBufferPurge = void (__fastcall*)(void* buf, int keepCapacity);

	void** g_ppParticleMgr = nullptr;
	FnGetParticleMgr g_getParticleMgr = nullptr;
	void* g_resourceSystem = nullptr;
	FnCacheParticle g_cacheParticle = nullptr;
	FnSetEffectF3 g_setCpF3 = nullptr;
	FnSetParticleSettings g_setSettings = nullptr;
	FnDestroyParticle g_destroyParticle = nullptr;
	FnCBufferStringInit g_bufInit = nullptr;
	FnBlockingLoad g_blockingLoad = nullptr;
	FnCBufferPurge g_bufPurge = nullptr;
	bool g_ready = false;

	FxSlot g_fx[kMaxFx]{};
	int g_fxCount = 0;
	int g_activeKind = 0;
	int g_activeLayerBucket = -1;
	float g_activeIntensity = 0.f;
	int g_createFails = 0;
	uintptr_t g_cachedMapInfo = 0;
	bool g_precacheDone[4]{};
	int g_recreateCooldown = 0;

	// Last-good follow origin (never freeze emitters if one frame fails)
	float g_lastX = 0.f, g_lastY = 0.f, g_lastZ = 0.f;
	float g_spawnX = 0.f, g_spawnY = 0.f, g_spawnZ = 0.f;
	bool g_haveLastOrigin = false;
	const char* g_lastOriginSrc = "none";
	int g_followTicks = 0;
	int g_setCpFailStreak = 0;
	C_CSPlayerPawn* g_followPawn = nullptr;
	float g_rawAbsZ = 0.f, g_rawEyeZ = 0.f;

	// rain.vpcf = CreateWithinSphereTransform → ground impact disc that IGNORES CP0 after spawn.
	// Camera volume (MAIN_VIEW) is how CS2 ambient precip fills the view and follows.
	// Camera volumes = what you actually see. World sheets sit just above the head (not +260).
	struct KindPaths {
		const char* camera;   // MAIN_VIEW primary (storm_screen / snow / ash)
		const char* camera2;  // MAIN_VIEW secondary (edge / outer / eddy)
		const char* camera3;  // MAIN_VIEW fill (mist / drift)
		const char* world;    // CUSTOMORIGIN near-head sheet
	};

	KindPaths PathsFor(int mode) {
		switch (mode) {
		case 1:
			return {
				"particles/rain_fx/rain_storm_screen.vpcf",
				"particles/rain_fx/rain_edge.vpcf",
				"particles/rain_fx/rain_mist.vpcf",
				"particles/rain_fx/rain_sheet.vpcf"
			};
		case 2:
			return {
				"particles/rain_fx/snow.vpcf",
				"particles/rain_fx/snow_outer.vpcf",
				"particles/rain_fx/snow_drift.vpcf",
				"particles/rain_fx/snow_outer.vpcf"
			};
		case 3:
			return {
				"particles/rain_fx/ash_outer.vpcf",
				"particles/rain_fx/ash_eddy.vpcf",
				"particles/rain_fx/ash_eddy_b.vpcf",
				"particles/rain_fx/ash.vpcf"
			};
		default:
			return { nullptr, nullptr, nullptr, nullptr };
		}
	}

	int LayerBucket(float intensity01) {
		if (intensity01 < 0.34f) return 1;
		if (intensity01 < 0.67f) return 2;
		return 3;
	}

	float DensityCp(float intensity01) {
		return std::clamp(0.65f + 0.35f * intensity01, 0.65f, 1.f);
	}

	// celerity world_effects: custom .vpcf_c → csgo/bin/, spawn via CacheParticleEffect
	// Paths: bin/<name>.vpcf resolves to csgo/bin/<name>.vpcf_c
	// Menu: 1=snow 2=stars 3=ash (embedded) | 4=rain (engine rain_fx full stack + wetness)
	constexpr const char* kSnowPath  = "bin/falling_snow1.vpcf";
	constexpr const char* kStarsPath = "bin/nomove_stars.vpcf";
	constexpr const char* kEmberA    = "bin/falling_ember1.vpcf";
	constexpr const char* kEmberB    = "bin/falling_ember2.vpcf";
	// Stock CS2 rain_fx (pak01) — UC particle-directories + vpcf analysis:
	//   rain_storm.vpcf  = official rain (CreateWithinSphere + PRECIPITATIONBLOCKER)
	//   rain.vpcf        = rain_streak sphere
	//   rain_storm_outer = wider storm layer
	//   rain_edge/volume = camera-space fill (CreateWithinBox / Continuous)
	//   rain_storm_screen = sparse screen drip — DO NOT use as main rain
	// Density CP2 is ONLY for custom bin/* snow/ash (celerity). Stock rain_fx
	// has no density CP — visibility = multi-emitter grid + MAIN_VIEW fill.
	constexpr const char* kRainStorm  = "particles/rain_fx/rain_storm.vpcf";
	constexpr const char* kRainMain   = "particles/rain_fx/rain.vpcf";
	constexpr const char* kRainOuter  = "particles/rain_fx/rain_storm_outer.vpcf";
	constexpr const char* kRainVol    = "particles/rain_fx/rain_volume.vpcf";
	constexpr const char* kRainEdge   = "particles/rain_fx/rain_edge.vpcf";
	constexpr const char* kRainSheet  = "particles/rain_fx/rain_sheet.vpcf";
	constexpr unsigned int kCpDensity = 2;

	// 1 Snow, 2 Stars, 3 Ash, 4 Rain
	constexpr int kWeatherModeMax = 4;

	std::mutex g_warmMutex;
	std::unordered_set<std::string> g_warmed;
	bool g_warmPending = false;
	int  g_warmAttempts = 0;
	bool g_assetsWritten = false;
	bool g_mapReset = false;

	std::filesystem::path ResolveCsgoBin() {
		wchar_t buffer[MAX_PATH];
		const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (len == 0 || len >= MAX_PATH)
			return {};
		std::error_code ec;
		std::filesystem::path exe(buffer);
		std::filesystem::path game = exe.parent_path().parent_path().parent_path();
		if (game.empty() || !std::filesystem::exists(game / "csgo", ec))
			return {};
		return game / "csgo" / "bin";
	}

	void EnsureParticleAssets() {
		{
			std::lock_guard<std::mutex> lock(g_warmMutex);
			if (g_assetsWritten)
				return;
			g_assetsWritten = true;
		}
		const auto bin = ResolveCsgoBin();
		if (bin.empty())
			return;
		std::error_code ec;
		std::filesystem::create_directories(bin, ec);
		struct Asset { const char* name; const unsigned char* data; std::size_t size; };
		const Asset assets[] = {
			{ "falling_snow1.vpcf_c",  world_particle_assets::falling_snow1,  sizeof(world_particle_assets::falling_snow1) },
			{ "falling_ember1.vpcf_c", world_particle_assets::falling_ember1, sizeof(world_particle_assets::falling_ember1) },
			{ "falling_ember2.vpcf_c", world_particle_assets::falling_ember2, sizeof(world_particle_assets::falling_ember2) },
			// stars optional — not mapped to menu mode yet
			{ "nomove_stars.vpcf_c",   world_particle_assets::nomove_stars,   sizeof(world_particle_assets::nomove_stars) },
		};
		for (const auto& a : assets) {
			const auto path = bin / a.name;
			if (std::filesystem::exists(path, ec))
				continue;
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if (f)
				f.write(reinterpret_cast<const char*>(a.data), static_cast<std::streamsize>(a.size));
		}
		Con::Ok("Weather assets written under csgo/bin");
	}

	bool PathWarmed(const char* path) {
		std::lock_guard<std::mutex> lock(g_warmMutex);
		return g_warmed.find(path) != g_warmed.end();
	}

	void* GetMgr();
	void DestroyOneSeh(void* mgr, int index, unsigned char force);

	bool WarmOne(const char* path) {
		// spawn+destroy outside render frame (FSN) — block-load legal here
		if (!g_cacheParticle || !path)
			return false;
		void* mgr = GetMgr();
		if (!mgr)
			return false;
		unsigned int idx = static_cast<unsigned int>(-1);
		__try {
			g_cacheParticle(mgr, &idx, path, kAttachWorldOrigin, nullptr, nullptr, nullptr, 0);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (idx == static_cast<unsigned int>(-1) || idx == 0)
			return false;
		DestroyOneSeh(mgr, static_cast<int>(idx), 1);
		return true;
	}


	// Horizontal distance before we tear down + respawn world emitters
	constexpr float kRecreateMoveDist = 64.f;

	void* GetMgr() {
		// Celerity: GetGameParticleManager() first
		if (g_getParticleMgr) {
			__try {
				if (void* m = g_getParticleMgr())
					return m;
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}
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

	// Prefer celerity SetParticleSettings; fall back to SetEffect F3
	char SetCpF3Seh(void* mgr, int index, unsigned int cp, float a, float b, float c) {
		if (!mgr || index == kInvalidFx || index <= 0)
			return 0;
		float v[3] = { a, b, c };
		char ok = 0;
		if (g_setSettings) {
			__try {
				ok = g_setSettings(mgr, static_cast<unsigned int>(index), static_cast<int>(cp), v, 0) ? 1 : 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				ok = 0;
			}
			if (ok)
				return ok;
		}
		if (!g_setCpF3)
			return 0;
		__try {
			ok = g_setCpF3(mgr, index, cp, v, 0.f);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			ok = 0;
		}
		return ok;
	}

	struct OriginSample {
		float x = 0.f, y = 0.f, z = 0.f;
		const char* src = "none";
		C_CSPlayerPawn* pawn = nullptr;
		bool ok = false;
	};

	OriginSample SampleLocalOrigin();
	void UpdatePositions(float px, float py, float pz);

	void DestroyOneSeh(void* mgr, int index, unsigned char force) {
		if (!mgr || !g_destroyParticle || index == kInvalidFx)
			return;
		__try {
			g_destroyParticle(mgr, index, force, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
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
			g_fx[i] = FxSlot{};
		}
		for (int i = 0; i < count; ++i)
			DestroyOneSeh(mgr, indices[i], 0);
		for (int i = 0; i < count; ++i)
			DestroyOneSeh(mgr, indices[i], 1);

		if (count > 0) {
			ADLogf("W", "weather.cpp:DestroyAll", "cleared",
				"{\"n\":%d,\"mgr\":%llu}", count, (unsigned long long)(uintptr_t)mgr);
		}

		g_fxCount = 0;
		g_activeKind = 0;
		g_activeLayerBucket = -1;
		g_activeIntensity = 0.f;
		g_followPawn = nullptr;
		g_setCpFailStreak = 0;
	}

	int CreateOne(void* mgr, const char* path, float ox, float oy, float oz,
		int attach, void* entity)
	{
		if (!mgr || !g_cacheParticle || !path || g_fxCount >= kMaxFx)
			return kInvalidFx;

		unsigned int idx = static_cast<unsigned int>(-1);
		int seh = 0;
		__try {
			g_cacheParticle(mgr, &idx, path, attach, entity, nullptr, nullptr, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			seh = static_cast<int>(GetExceptionCode());
		}
		if (seh || idx == 0 || idx == static_cast<unsigned int>(-1)) {
			ADLogf("W", "weather.cpp:CreateOne", "cache_fail",
				"{\"path\":\"%s\",\"attach\":%d,\"seh\":%u,\"idx\":%u,\"ent\":%llu}",
				path, attach, (unsigned)seh, idx, (unsigned long long)(uintptr_t)entity);
			Con::Error("Weather Cache fail path=%s attach=%d seh=0x%X", path, attach, seh);
			return kInvalidFx;
		}

		const int iIdx = static_cast<int>(idx);
		const float wx = g_haveLastOrigin ? (g_lastX + ox) : ox;
		const float wy = g_haveLastOrigin ? (g_lastY + oy) : oy;
		const float wz = g_haveLastOrigin ? (g_lastZ + oz) : (oz + 72.f);

		// Custom celerity particles: CP0 = world origin only (density set by CreateKind on CP2)
		const char ori = SetCpF3Seh(mgr, iIdx, kCpOrigin, wx, wy, wz);
		const char en = 1;

		FxSlot& s = g_fx[g_fxCount++];
		s.index = iIdx;
		s.ox = ox;
		s.oy = oy;
		s.oz = oz;
		s.path = path;
		s.attach = attach;

		ADLogf("W", "weather.cpp:CreateOne", "spawned",
			"{\"path\":\"%s\",\"idx\":%d,\"attach\":%d,\"ent\":%llu,"
			"\"pos\":[%.1f,%.1f,%.1f],\"off\":[%.1f,%.1f,%.1f],"
			"\"cp28\":%d,\"cp0\":%d,\"src\":\"%s\",\"slot\":%d}",
			path, iIdx, attach, (unsigned long long)(uintptr_t)entity,
			wx, wy, wz, ox, oy, oz, (int)en, (int)ori, g_lastOriginSrc, g_fxCount - 1);
		Con::Ok("Weather spawn %s idx=%d attach=%d @ (%.0f,%.0f,%.0f) cp28=%d cp0=%d",
			path, iIdx, attach, wx, wy, wz, (int)en, (int)ori);
		return iIdx;
	}

	void PrecacheKind(int kind, const KindPaths& p, bool force = false) {
		if (kind < 0 || kind > 3)
			return;
		if (g_precacheDone[kind] && !force)
			return;
		int n = 0;
		const char* all[] = { p.camera, p.camera2, p.camera3, p.world };
		for (const char* path : all) {
			if (!path)
				continue;
			if (PrecachePathSeh(path))
				++n;
		}
		ADLogf("W", "weather.cpp:PrecacheKind", "precache",
			"{\"kind\":%d,\"ok\":%d,\"force\":%d}", kind, n, force ? 1 : 0);
		if (n > 0)
			g_precacheDone[kind] = true;
	}

	bool CreateKind(int kind, float intensity01) {
		// 1=snow 2=stars 3=ash (bin/* + CP2 density)
		// 4=rain (stock rain_fx: CP0 origin grid + MAIN_VIEW fill — no CP2)
		DestroyAll();
		g_activeIntensity = intensity01;
		g_activeLayerBucket = LayerBucket(intensity01);
		g_createFails = 0;

		if (kind < 1 || kind > kWeatherModeMax) {
			g_activeKind = 0;
			return false;
		}

		void* mgr = GetMgr();
		if (!mgr || !g_cacheParticle || (!g_setSettings && !g_setCpF3)) {
			++g_createFails;
			g_activeKind = 0;
			return false;
		}

		const float t = std::clamp(intensity01, 0.f, 1.f);
		// bin/* only — custom particles read CP2 as density
		const float dens = t * 1000.f;

		auto spawn_world = [&](const char* path) -> int {
			if (!path || !PathWarmed(path))
				return kInvalidFx;
			return CreateOne(mgr, path, 0.f, 0.f, 0.f, kAttachWorldOrigin, nullptr);
		};
		// rain_fx VPK — no warm gate, no density CP
		auto spawn_rain = [&](const char* path, int attach, float ox, float oy, float oz) -> int {
			if (!path)
				return kInvalidFx;
			return CreateOne(mgr, path, ox, oy, oz, attach, nullptr);
		};
		auto setDens = [&](int idx) {
			if (idx != kInvalidFx)
				SetCpF3Seh(mgr, idx, kCpDensity, dens, 0.f, 0.f);
		};

		if (kind == 1) {
			setDens(spawn_world(kSnowPath));
		} else if (kind == 2) {
			setDens(spawn_world(kStarsPath));
		} else if (kind == 3) {
			setDens(spawn_world(kEmberA));
			setDens(spawn_world(kEmberB));
		} else if (kind == 4) {
			// UC: particles/rain_fx/rain_storm.vpcf is the rain particle.
			// rain_storm = CreateWithinSphere around CP0 → must follow player +
			// multi-emitter grid (single sphere is a small disc).
			// rain_edge / rain_volume = camera fill (MAIN_VIEW) so FOV is wet.
			// lift so sphere sits above head; streaks fall through view.
			const float lift = 180.f;
			const float step = 96.f; // grid spacing (sphere radius-ish)

			// Camera volumes first — always on, fill the screen like map precip
			(void)spawn_rain(kRainEdge, kAttachMainView, 0.f, 0.f, 0.f);
			(void)spawn_rain(kRainVol,  kAttachMainView, 0.f, 0.f, 0.f);
			(void)spawn_rain(kRainSheet, kAttachMainView, 0.f, 0.f, 0.f);

			// Center storm + base rain (official paths)
			(void)spawn_rain(kRainStorm, kAttachWorldOrigin, 0.f, 0.f, lift);
			(void)spawn_rain(kRainMain,  kAttachWorldOrigin, 0.f, 0.f, lift);
			(void)spawn_rain(kRainOuter, kAttachWorldOrigin, 0.f, 0.f, lift);

			// Intensity → more WORLDORIGIN storm emitters around player
			// bucket1: center only | bucket2: +4 cardinal | bucket3: full 3x3
			const int bucket = LayerBucket(t);
			if (bucket >= 2) {
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin,  step, 0.f, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, -step, 0.f, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, 0.f,  step, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, 0.f, -step, lift);
			}
			if (bucket >= 3) {
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin,  step,  step, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin,  step, -step, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, -step,  step, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, -step, -step, lift);
				// second ring for full intensity
				const float step2 = step * 2.f;
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin,  step2, 0.f, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, -step2, 0.f, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, 0.f,  step2, lift);
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, 0.f, -step2, lift);
			}

			if (g_fxCount == 0)
				(void)spawn_rain(kRainStorm, kAttachWorldOrigin, 0.f, 0.f, lift);
		}

		const bool ok = g_fxCount > 0;
		g_activeKind = ok ? kind : 0;
		ADLogf("W", "weather.cpp:CreateKind", "engine_particles",
			"{\"kind\":%d,\"intensity\":%.2f,\"fx\":%d}",
			kind, intensity01, g_fxCount);
		if (!ok)
			Con::Warn("Weather CreateKind %d failed (fx=0) — rain_fx may be missing from VPK", kind);
		return ok;
	}


	void UpdatePositions(float px, float py, float pz) {
		void* mgr = GetMgr();
		if (!mgr || g_fxCount <= 0)
			return;
		// bin/* snow/ash: density CP2. rain_fx: origin only (no density CP).
		const float dens = std::clamp(g_activeIntensity, 0.f, 1.f) * 1000.f;
		const bool rain = (g_activeKind == 4);
		for (int i = 0; i < g_fxCount; ++i) {
			FxSlot& s = g_fx[i];
			if (s.index == kInvalidFx)
				continue;
			// MAIN_VIEW tracks camera — do not stomp CP0
			if (s.attach != kAttachMainView)
				SetCpF3Seh(mgr, s.index, kCpOrigin, px + s.ox, py + s.oy, pz + s.oz);
			if (!rain)
				SetCpF3Seh(mgr, s.index, kCpDensity, dens, 0.f, 0.f);
		}
	}

	OriginSample SampleLocalOrigin() {
		OriginSample o{};
		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local)
			return o;
		o.pawn = local;

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
			return o;
		}

		g_rawAbsZ = gotAbs ? abs.z : 0.f;
		g_rawEyeZ = gotEye ? eye.z : 0.f;

		if (gotEye) {
			o.x = eye.x; o.y = eye.y; o.z = eye.z;
			o.src = "eye"; o.ok = true;
			return o;
		}
		if (gotShoot) {
			o.x = shoot.x; o.y = shoot.y; o.z = shoot.z;
			o.src = "shoot"; o.ok = true;
			return o;
		}
		if (gotAbs && (std::fabs(abs.x) > 0.1f || std::fabs(abs.y) > 0.1f)) {
			o.x = abs.x; o.y = abs.y;
			o.z = (abs.z < 32.f) ? (abs.z + 72.f) : (abs.z + 64.f);
			if (o.z < 48.f)
				o.z = 72.f;
			o.src = "abs+lift"; o.ok = true;
			return o;
		}
		if (gotOld) {
			o.x = old.x; o.y = old.y; o.z = old.z + 72.f;
			o.src = "old+72"; o.ok = true;
			return o;
		}
		return o;
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

			// Rain (4): full wetness + puddles. Others restore.
			if (kind == 4) {
				const float wet = std::clamp(0.70f + 0.30f * intensity01, 0.f, 1.f);
				if (offRainTrace)
					*reinterpret_cast<bool*>(base + offRainTrace) = true;
				if (offRainStr)
					*reinterpret_cast<float*>(base + offRainStr) = wet;
				if (offWet)
					*reinterpret_cast<float*>(base + offWet) = wet;
				if (offPuddle)
					*reinterpret_cast<float*>(base + offPuddle) = wet;
				if (offPuddleDir)
					*reinterpret_cast<float*>(base + offPuddleDir) = 1.f;
				if (offDry)
					*reinterpret_cast<float*>(base + offDry) = 0.0001f;
			} else {
				// Snow/stars/ash: leave map wetness at saved (not rain wet)
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
	g_ppParticleMgr = nullptr;
	g_getParticleMgr = nullptr;
	g_cacheParticle = nullptr;
	g_setCpF3 = nullptr;
	g_setSettings = nullptr;
	g_destroyParticle = nullptr;
	g_bufInit = nullptr;
	g_blockingLoad = nullptr;
	g_bufPurge = nullptr;
	g_ready = false;

	g_resourceSystem = I::Get<void>("resourcesystem.dll", "ResourceSystem013");
	if (!g_resourceSystem)
		Con::OffsetMiss("Weather ResourceSystem013");

	// GetGameParticleManager — IDA 0x180997580 (celerity)
	const uintptr_t getMgr = M::patternScan("client",
		"48 8B 05 ? ? ? ? C3 ? ? ? ? ? ? ? ? 48 89 5C 24 ? 57 B8");
	if (getMgr)
		g_getParticleMgr = reinterpret_cast<FnGetParticleMgr>(getMgr);
	// fallback: pParticleManager qword via RIP
	const uintptr_t mgrRip = M::patternScan("client",
		"48 8B 0D ? ? ? ? 41 B8 ? ? ? ? F3 0F 11 74 24 ? 48 C7 44 24");
	if (mgrRip)
		g_ppParticleMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(mgrRip, 3));
	if (!g_getParticleMgr && !g_ppParticleMgr)
		Con::OffsetMiss("Weather GetGameParticleManager");
	else
		Con::Ok("Weather particle mgr get=%p ptr=%p", (void*)g_getParticleMgr, (void*)g_ppParticleMgr);

	// CacheParticleEffect / CreateParticle — IDA sub_18078EE10
	uintptr_t cacheAddr = M::patternScan("client",
		"4C 8B DC 53 48 81 EC ? ? ? ? F2 0F 10 05");
	if (!cacheAddr)
		cacheAddr = M::patternScan("client",
			"4C 8B DC 53 48 81 EC 90 00 00 00 F2 0F 10 05");
	if (cacheAddr)
		g_cacheParticle = reinterpret_cast<FnCacheParticle>(cacheAddr);
	if (!g_cacheParticle)
		Con::OffsetMiss("Weather CacheParticleEffect");
	else
		Con::Ok("Weather CacheParticleEffect @ 0x%p", (void*)cacheAddr);

	// SetParticleSettings — celerity / IDA 0x1809C5540
	const uintptr_t setSettings = M::patternScan("client",
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? F3 0F 10 1D ? ? ? ? 41 8B F8 8B DA 4C 8D 05");
	if (setSettings)
		g_setSettings = reinterpret_cast<FnSetParticleSettings>(setSettings);
	// SetEffect float3 fallback — IDA sub_1809C54E0
	uintptr_t setF3 = M::patternScan("client",
		"48 83 EC 58 F3 41 0F 10 51 04 F3 41 0F 10 09 F3 41 0F 10 59 08");
	if (!setF3)
		setF3 = M::patternScan("client",
			"48 83 EC 58 F3 41 0F 10 51 ? 0F 28 05");
	if (setF3)
		g_setCpF3 = reinterpret_cast<FnSetEffectF3>(setF3);
	if (!g_setSettings && !g_setCpF3)
		Con::OffsetMiss("Weather SetParticleSettings/SetEffect");
	else
		Con::Ok("Weather setCP settings=%p f3=%p", (void*)g_setSettings, (void*)g_setCpF3);

	// DestroyParticle — patterns.hpp (unique @ 0x180989F40)
	const uintptr_t destroyAddr = M::patternScan("client",
		"83 FA FF 0F 84 ? ? ? ? 41 54 41 56 41 57 48");
	if (destroyAddr)
		g_destroyParticle = reinterpret_cast<FnDestroyParticle>(destroyAddr);
	if (!g_destroyParticle)
		Con::OffsetMiss("Weather DestroyParticle");
	else
		Con::Ok("Weather DestroyParticle @ 0x%p", (void*)destroyAddr);

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

	// FindParticleSystem (particles.dll) — reserved for engine fog/dust names later
	const uintptr_t findPs = M::patternScan("particles",
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC 40");
	if (findPs)
		Con::Ok("Weather FindParticleSystem @ 0x%p (fog/dust names ready)", (void*)findPs);
	else
		Con::OffsetMiss("Weather FindParticleSystem");

	// Modes: snow/stars/ash (bin) + rain (rain_fx VPK volumes).
	g_ready = true;
	EnsureParticleAssets();
	{
		std::lock_guard<std::mutex> lock(g_warmMutex);
		g_warmPending = true;
		g_warmAttempts = 0;
	}
	Con::Ok("Weather ready (snow/stars/ash/rain)");
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
	if (!Config::weather || Config::weather_mode < 1 || Config::weather_mode > kWeatherModeMax)
		return;
	g_cachedMapInfo = mapInfo;
	// Wetness for rain only
	if (Config::weather_mode == 4)
		ApplyMapInfoFields(mapInfo, Config::weather_mode,
			std::clamp(Config::weather_intensity, 0.f, 1.f));
}

void World::Weather::Draw()
{
	// Engine particles only (same path as snow/stars/ash). No ImGui.
}

void World::Weather::Update()
{
	if (Config::loading.load(std::memory_order_acquire))
		return;

	if (VacDetect::IsSoftPaused()) {
		if (g_fxCount > 0)
			DestroyAll();
		RestoreMapInfoFields();
		return;
	}

	// Modes: 1 snow 2 stars 3 ash 4 rain
	// Legacy Storm (5) → Rain
	if (Config::weather_mode == 5)
		Config::weather_mode = 4;

	const bool want = Config::weather
		&& Config::weather_mode >= 1
		&& Config::weather_mode <= kWeatherModeMax;

	if (!want) {
		if (g_fxCount > 0)
			DestroyAll();
		RestoreMapInfoFields();
		g_haveLastOrigin = false;
		g_activeKind = 0;
		return;
	}

	int kind = Config::weather_mode;
	if (kind < 1 || kind > kWeatherModeMax) {
		kind = 1;
		Config::weather_mode = 1;
	}
	const float intensity = std::clamp(Config::weather_intensity, 0.f, 1.f);

	// Honor map/round reset from WarmTick / OnRoundStart (stale particle indices)
	bool reset = false;
	{
		std::lock_guard<std::mutex> lock(g_warmMutex);
		reset = g_mapReset;
		g_mapReset = false;
	}
	if (reset) {
		// old manager / round wipe — drop indices without destroy
		for (int i = 0; i < g_fxCount; ++i)
			g_fx[i] = FxSlot{};
		g_fxCount = 0;
		g_activeKind = 0;
		g_cachedMapInfo = 0; // force mapinfo rescan for wetness
	}

	// Mode change: rebuild
	if (g_activeKind != 0 && g_activeKind != kind)
		DestroyAll();

	// Rain extra storm copies gated by intensity bucket — rebuild stack on change
	const int bucket = LayerBucket(intensity);
	if (kind == 4 && g_activeKind == kind
		&& g_activeLayerBucket != bucket && g_fxCount > 0)
		DestroyAll();

	g_activeIntensity = intensity;
	g_activeLayerBucket = bucket;

	// Always recreate when empty or kind mismatch (round wipe leaves count 0)
	if (g_fxCount <= 0 || g_activeKind != kind)
		(void)CreateKind(kind, intensity);
	const OriginSample o = SampleLocalOrigin();
	if (o.ok) {
		g_lastX = o.x; g_lastY = o.y; g_lastZ = o.z;
		g_haveLastOrigin = true;
		g_lastOriginSrc = o.src;
		UpdatePositions(o.x, o.y, o.z);
	} else if (g_haveLastOrigin) {
		UpdatePositions(g_lastX, g_lastY, g_lastZ);
	}

	if (!g_cachedMapInfo) {
		static int s_scanCooldown = 0;
		if (--s_scanCooldown <= 0) {
			s_scanCooldown = 64;
			g_cachedMapInfo = FindMapInfoEntity();
		}
	}
	// Wetness for rain only
	if (g_cachedMapInfo && kind == 4)
		ApplyMapInfoFields(g_cachedMapInfo, kind, intensity);
	else if (kind != 4)
		RestoreMapInfoFields();
}


void World::Weather::OnLevelChange()
{
	EnsureParticleAssets();
	std::lock_guard<std::mutex> lock(g_warmMutex);
	g_warmed.clear();
	g_warmPending = true;
	g_warmAttempts = 0;
	g_mapReset = true;
}

void World::Weather::OnRoundStart()
{
	// Round wipe kills particle system handles + mapinfo wetness.
	// LevelInit only runs on map load — must re-arm every round.
	if (!Config::weather || Config::weather_mode < 1 || Config::weather_mode > kWeatherModeMax)
		return;

	// Drop stale indices without destroy (manager may already have wiped them)
	for (int i = 0; i < g_fxCount; ++i)
		g_fx[i] = FxSlot{};
	g_fxCount = 0;
	g_activeKind = 0;
	g_activeLayerBucket = -1;
	g_createFails = 0;
	g_haveLastOrigin = false;
	g_followPawn = nullptr;
	g_setCpFailStreak = 0;
	// Map entity often recreated — force rescan + re-apply wetness
	g_cachedMapInfo = 0;
	g_wetSaved = false;
	g_wetSavedFor = 0;
	{
		std::lock_guard<std::mutex> lock(g_warmMutex);
		g_mapReset = true;
		// Keep warmed set — assets already on disk; just force CreateKind next Update
		g_warmPending = true;
		g_warmAttempts = 0;
	}
	// Immediate rebuild this frame if Update already ran — next FSN Update will CreateKind
}

// IDA: IGameEvent::GetName = vtable[1]
static const char* WeatherEventName(void* gameEvent) {
	if (!gameEvent)
		return nullptr;
	const char* name = nullptr;
	__try {
		void** vt = *reinterpret_cast<void***>(gameEvent);
		if (!vt || !vt[1])
			return nullptr;
		using FnGetName = const char* (__fastcall*)(void*);
		name = reinterpret_cast<FnGetName>(vt[1])(gameEvent);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return (name && name[0]) ? name : nullptr;
}

void World::Weather::OnGameEvent(void* gameEvent) {
	const char* name = WeatherEventName(gameEvent);
	if (!name)
		return;
	// Same round boundary set as nade_pred — LevelInit does not fire each round
	if (std::strcmp(name, "round_start") == 0
		|| std::strcmp(name, "round_officially_started") == 0
		|| std::strcmp(name, "round_prestart") == 0
		|| std::strcmp(name, "game_newmap") == 0
		|| std::strcmp(name, "begin_new_match") == 0
		|| std::strcmp(name, "cs_match_end_restart") == 0) {
		OnRoundStart();
	}
}

void World::Weather::WarmTick()
{
	if (!g_ready || !g_cacheParticle)
		return;
	if (!GetMgr())
		return;

	// Embedded bin/* must warm. rain_fx from VPK — try warm, CreateKind still
	// spawns without PathWarmed for rain attach path.
	constexpr const char* kAll[] = {
		kSnowPath, kStarsPath, kEmberA, kEmberB,
		kRainStorm, kRainMain, kRainOuter, kRainVol, kRainEdge, kRainSheet
	};
	constexpr int kNeedMin = 4; // snow+stars+embers enough to stop spinning
	constexpr int kMaxAttempts = 600;

	std::vector<const char*> todo;
	{
		std::lock_guard<std::mutex> lock(g_warmMutex);
		if (!g_warmPending)
			return;
		for (const char* path : kAll) {
			if (g_warmed.find(path) == g_warmed.end())
				todo.push_back(path);
		}
	}
	if (todo.empty()) {
		std::lock_guard<std::mutex> lock(g_warmMutex);
		g_warmPending = false;
		return;
	}

	std::vector<std::string> just;
	for (const char* path : todo) {
		if (WarmOne(path))
			just.emplace_back(path);
	}

	std::lock_guard<std::mutex> lock(g_warmMutex);
	for (auto& s : just)
		g_warmed.insert(std::move(s));
	if (!just.empty()) {
		g_mapReset = true;
		g_warmAttempts = 0;
		Con::Ok("Weather warm +%d total=%zu", (int)just.size(), g_warmed.size());
	}
	// Stop when core bin assets ready, or all paths tried enough times
	const bool coreReady =
		g_warmed.count(kSnowPath) && g_warmed.count(kStarsPath)
		&& g_warmed.count(kEmberA);
	if (coreReady || static_cast<int>(g_warmed.size()) >= kNeedMin
		|| ++g_warmAttempts >= kMaxAttempts)
		g_warmPending = false;
}
