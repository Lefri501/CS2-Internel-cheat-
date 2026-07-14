#include "nade_pred.h"

#include "../../config/config.h"
#include "../../interfaces/interfaces.h"
#include "../../hooks/hooks.h"
#include "../trace/trace.h"
#include "../bones/bones.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/handle.h"
#include "../../../cs2/datatypes/schema/ISchemaClass/ISchemaClass.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../menu/menu.h"
#include "../visuals/assets/weapon_icons.hpp"
#include "../visuals/visuals.h"
#include "../gamemode/gamemode.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "../../../../external/imgui/imgui.h"

namespace NadePred {

// Defined below; used by CollectProjectiles (anon) + OnGameEvent/Reset
void RoundBoundaryReset(const char* reason, bool force = false);

namespace {

// Physics constants from Andromeda CGrenadePrediction (CS2-accurate)
constexpr float kGravity = 320.f;
constexpr float kElasticity = 0.45f;
constexpr float kSimDt = 1.f / 64.f;
constexpr int   kMaxTicks = 1024;
constexpr int   kTicksPerPoint = 4;
constexpr float kStopSpeedSq = 2036.f; // ~45.1 u/s
constexpr float kGrenadeHull = 2.f;
constexpr int   kMaxBounces = 20;

// CS2 gameplay radii (units)
constexpr float kRadiusHE = 350.f;       // HE damage radius
constexpr float kRadiusFlash = 0.f;      // no ring (engine flash is huge / not circular)
// IDA SmokeVolume density: outer = cl_smoke_torus_ring_radius + subradius
constexpr float kSmokeRingRadius = 61.f; // cl_smoke_torus_ring_radius default
constexpr float kSmokeRingSub = 88.f;    // cl_smoke_torus_ring_subradius default
constexpr float kRadiusSmoke = kSmokeRingRadius + kSmokeRingSub; // 149 — outer volume edge
constexpr float kRadiusMollyBase = 50.f; // per-fire core pad
constexpr float kRadiusMollyMax = 250.f;

// CS2 effect durations (seconds) — Valve defaults / IDA-backed
constexpr float kDurSmoke = 18.f;
constexpr float kDurMolly = 7.0f;          // fallback; prefer C_Inferno->m_nFireLifetime
constexpr float kDurDecoy = 15.f;
// Valve GRENADE_TIMER = 1.5s; Think_Detonate gates with (tick - 8) * dt > 1.5
constexpr float kGrenadeTimer = 1.5f;
constexpr int   kFuseDelayTicks = 8; // ~0.125s before fuse clock starts
// Wall-clock badge pad so late entity discovery still shows until explode flags clear
constexpr float kDurHeFlashFuse = 1.6f;
// Molly / inc: shatter when normal.z >= this (cos ~30°)
constexpr float kMollyMaxSlopeZ = 0.866f;
// Molly projectile air only; inferno entity owns burn warn
constexpr float kDurMollyProjectile = 2.5f;

// Dump-stable schema fallbacks (client.dll.i64 schema tables)
constexpr uint32_t kOffAbsVel = 0x3F8;
constexpr uint32_t kOffInitPos = 0x11C8;
constexpr uint32_t kOffInitVel = 0x11D4;
constexpr uint32_t kOffBounces = 0x11E0;
constexpr uint32_t kOffThrower = 0x11A8;
constexpr uint32_t kOffPinPulled = 0x1CE3;
constexpr uint32_t kOffHeldByPlayer = 0x1CE2;
constexpr uint32_t kOffThrowStrength = 0x1CF0;
constexpr uint32_t kOffDidSmoke = 0x127C;           // C_SmokeGrenadeProjectile->m_bDidSmokeEffect
constexpr uint32_t kOffSmokeTick = 0x1278;          // m_nSmokeEffectTickBegin
constexpr uint32_t kOffFireCount = 0x1960;          // C_Inferno->m_fireCount
constexpr uint32_t kOffFireLifetime = 0x1968;       // m_nFireLifetime (float seconds)
constexpr uint32_t kOffFirePostFx = 0x196C;         // m_bInPostEffectTime
constexpr uint32_t kOffFireTick = 0x1974;           // m_nFireEffectTickBegin
constexpr uint32_t kOffFirePos = 0x1020;            // m_firePositions VectorWS[64]
constexpr uint32_t kOffFireBurning = 0x1620;        // m_bFireIsBurning bool[64]
constexpr uint32_t kOffMaxFireHalfW = 0x858C;       // m_maxFireHalfWidth
constexpr uint32_t kOffDetonateTime = 0x1188;       // C_BaseGrenade->m_flDetonateTime
// C_BaseCSGrenadeProjectile explode — instant clear (IDA schema 0x11F0 / 0x1214)
constexpr uint32_t kOffExplodeTickBegin = 0x11F0;   // m_nExplodeEffectTickBegin
constexpr uint32_t kOffExplodeBegan = 0x1214;       // m_bExplodeEffectBegan

std::vector<Path> g_paths;

// Bumps every round/map so recycled handles never inherit expired fuse clocks.
uint32_t g_roundEpoch = 1;

// Collect scan diagnostics — cleared on round boundary (stale "seen" cache hid
// reclassified utility on recycled indices after round 1).
std::unordered_set<std::string> s_collectSeenNames;
int s_collectPrevMax = 0;
int s_collectPeak = 0;
int s_collectLowFrames = 0;
int s_collectEmptyFrames = 0;

// Effect timers: handle + throw signature + round epoch.
// Without epoch, round 2/3 reuses handles → wrong badge / sped-up / missing warn.
struct FxTrack {
	float start = 0.f;
	NadeType type = NadeType::Unknown;
	uint32_t throwSig = 0; // m_vInitialPosition hash — new throw = new life
	bool expired = false;  // fuse done — stay dead until throwSig/epoch change
};
std::unordered_map<uint32_t, FxTrack> g_fxStart;
std::unordered_set<uint32_t> g_fxSeen;

uint32_t HandleKey(CEntityInstance* ent, int fallbackIdx) {
	// Prefer raw identity handle @+0x10 (index | serial<<15) — avoids schema/flags quirks
	if (ent && Mem::ValidEntity(ent)) {
		CEntityIdentity* id = nullptr;
		if (Mem::ReadField(ent, 0x10, id) && id && Mem::Valid(id, 0x14)) {
			uint32_t raw = 0;
			if (Mem::ReadField(id, 0x10, raw) && (raw & 0x7FFF) != 0)
				return raw;
		}
		const CBaseHandle h = ent->handle();
		if (h.valid())
			return static_cast<uint32_t>(h.index())
				| (static_cast<uint32_t>(h.serial_number()) << 15);
	}
	return static_cast<uint32_t>(fallbackIdx);
}

uint32_t ThrowSigFromPos(const Vector_t& p) {
	// Quantize so tiny float noise doesn't churn keys
	const int x = static_cast<int>(p.x * 0.5f);
	const int y = static_cast<int>(p.y * 0.5f);
	const int z = static_cast<int>(p.z * 0.5f);
	uint32_t h = 2166136261u;
	h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
	h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
	h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
	return h ? h : 1u;
}

uint32_t FxKey(uint32_t handleKey, uint32_t throwSig, NadeType type) {
	// Mix throw origin + round epoch + type so recycled slots never inherit
	// wrong utility clocks (smoke handle → molly next throw after round 1).
	return handleKey
		^ (throwSig * 0x9E3779B9u)
		^ (g_roundEpoch * 0x85EBCA6Bu)
		^ (static_cast<uint32_t>(type) * 0xC2B2AE3Du);
}

float Dot(const Vector_t& a, const Vector_t& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

float LenSq(const Vector_t& v) {
	return v.x * v.x + v.y * v.y + v.z * v.z;
}

float Len2D(const Vector_t& v) {
	return std::sqrt(v.x * v.x + v.y * v.y);
}

Vector_t Cross(const Vector_t& a, const Vector_t& b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

bool FiniteVec(const Vector_t& v) {
	return Mem::Finite(v.x) && Mem::Finite(v.y) && Mem::Finite(v.z);
}

Vector_t ReadAbsOrigin(C_BaseEntity* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return Vector_t{};
	// Hardcoded dump offsets first (schema can be 0 before init / wrong class hash)
	constexpr uint32_t kSceneNode = 0x330;
	constexpr uint32_t kAbsOrigin = 0xC8;
	CGameSceneNode* node = nullptr;
	if (!Mem::ReadField(ent, kSceneNode, node) || !Mem::Valid(node, kAbsOrigin + 12)) {
		node = ent->m_pGameSceneNode();
		if (!node || !Mem::Valid(node, kAbsOrigin + 12))
			return Vector_t{};
	}
	Vector_t o{};
	if (Mem::ReadField(node, kAbsOrigin, o) && FiniteVec(o))
		return o;
	o = node->m_vecAbsOrigin();
	return FiniteVec(o) ? o : Vector_t{};
}

// Prefer dump offset when schema is 0 OR returns nonsense (wrong class hash)
uint32_t NadeOff(const char* field, uint32_t fallback) {
	const uint32_t off = SchemaFinder::Get(hash_32_fnv1a_const(field));
	// reject tiny/non-field values; nade fields are always > 0x100
	if (off >= 0x100 && off < 0x20000)
		return off;
	return fallback;
}

uint32_t SchemaOr(const char* field, uint32_t fallback) {
	return NadeOff(field, fallback);
}

// SEH helpers — must stay free of C++ objects with destructors (MSVC C2712)
float SehReadFloat(const float* p) {
	__try { return *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

int SehReadInt(const int* p) {
	__try { return *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint8_t SehReadU8(const uint8_t* p) {
	__try { return *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

CEntityInstance* GetEntityByIndexRaw(int index); // defined below
CEntityInstance* GetEntityByIndexLoose(int index);
const char* GetDesignerAtIndex(int index);

CEntityInstance* SehGetEntity(int i) {
	__try {
		if (!I::GameEntity || !I::GameEntity->Instance)
			return nullptr;
		CEntityInstance* e = I::GameEntity->Instance->Get(i);
		if (Mem::ValidEntity(e))
			return e;
		// Fallback: direct chunk walk if ogGetBaseEntity missing / rejects
		e = GetEntityByIndexRaw(i);
		if (Mem::ValidEntity(e))
			return e;
		// Projectile predict slots can fail handle-serial match
		return GetEntityByIndexLoose(i);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Returns lit count as float; outMax = max horiz dist of lit fire + core pad.
float SehInfernoScan(void* ent, float ox, float oy, int n, float* outMax) {
	*outMax = 0.f;
	const auto* base = reinterpret_cast<const uint8_t*>(ent) + kOffFirePos;
	const auto* burning = reinterpret_cast<const uint8_t*>(ent) + kOffFireBurning;
	if (!Mem::IsReadable(base, static_cast<size_t>(n) * 12) || !Mem::IsReadable(burning, n))
		return 0.f;
	__try {
		int lit = 0;
		for (int i = 0; i < n; ++i) {
			if (!burning[i])
				continue;
			++lit;
			const float* xyz = reinterpret_cast<const float*>(base + i * 12);
			const float dx = xyz[0] - ox;
			const float dy = xyz[1] - oy;
			const float r = std::sqrt(dx * dx + dy * dy) + kRadiusMollyBase;
			if (r > *outMax) *outMax = r;
		}
		return static_cast<float>(lit);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.f;
	}
}

Vector_t SehReadVec(const Vector_t* p) {
	__try {
		const Vector_t v = *p;
		if (!Mem::Finite(v.x) || !Mem::Finite(v.y) || !Mem::Finite(v.z))
			return Vector_t{};
		return v;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return Vector_t{};
	}
}

// Wall-clock only — never mix game curtime (round resets / stale gv → every-other-round bugs)
float NowSeconds() {
	return static_cast<float>(ImGui::GetTime());
}

float EffectTimeLeft(uint32_t fxKey, NadeType type, float duration, uint32_t throwSig = 0) {
	const float now = NowSeconds();
	g_fxSeen.insert(fxKey);
	auto it = g_fxStart.find(fxKey);

	const bool newThrow = (throwSig != 0 && it != g_fxStart.end()
		&& it->second.throwSig != 0 && it->second.throwSig != throwSig);
	const bool typeChange = (it != g_fxStart.end() && it->second.type != type);

	// Fresh track, new throw, or type change → full duration
	if (it == g_fxStart.end() || newThrow || typeChange) {
		g_fxStart[fxKey] = FxTrack{ now, type, throwSig, false };
		return duration;
	}

	// Dead from prior fuse on same recycled key — stay hidden until throwSig/epoch changes
	if (it->second.expired)
		return 0.f;

	if (throwSig != 0)
		it->second.throwSig = throwSig;

	const float age = now - it->second.start;
	// Clock hitch / pause only — never re-anchor "impossibly old" (that revived round-2 ghosts)
	if (age < -0.05f) {
		it->second.start = now;
		return duration;
	}
	if (age >= duration) {
		it->second.expired = true;
		return 0.f;
	}
	return duration - age;
}

// While airborne, drop settle-fuse track so landing gets a fresh fuse window
void ClearFxTrack(uint32_t fxKey) {
	g_fxStart.erase(fxKey);
	g_fxSeen.erase(fxKey);
}

void PurgeFxTracks() {
	for (auto it = g_fxStart.begin(); it != g_fxStart.end(); ) {
		if (g_fxSeen.find(it->first) == g_fxSeen.end())
			it = g_fxStart.erase(it);
		else
			++it;
	}
	g_fxSeen.clear();
}

void ResetFxTracks() {
	g_fxStart.clear();
	g_fxSeen.clear();
}

Vector_t ReadInitVelocity(C_BaseEntity* ent) {
	if (!ent) return {};
	const uint32_t off = NadeOff("C_BaseCSGrenadeProjectile->m_vInitialVelocity", kOffInitVel);
	auto* p = reinterpret_cast<Vector_t*>(reinterpret_cast<uint8_t*>(ent) + off);
	if (!Mem::IsReadable(p, sizeof(Vector_t)))
		return {};
	return SehReadVec(p);
}

Vector_t ReadInitPosition(C_BaseEntity* ent) {
	if (!ent) return {};
	const uint32_t off = NadeOff("C_BaseCSGrenadeProjectile->m_vInitialPosition", kOffInitPos);
	auto* p = reinterpret_cast<Vector_t*>(reinterpret_cast<uint8_t*>(ent) + off);
	if (!Mem::IsReadable(p, sizeof(Vector_t)))
		return {};
	return SehReadVec(p);
}

bool ReadBoolOff(void* ent, uint32_t off) {
	if (!ent || !off) return false;
	auto* p = reinterpret_cast<uint8_t*>(ent) + off;
	if (!Mem::IsReadable(p, 1)) return false;
	return SehReadU8(p) != 0;
}

int ReadIntOff(void* ent, uint32_t off) {
	if (!ent || !off) return 0;
	auto* p = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ent) + off);
	if (!Mem::IsReadable(p, sizeof(int))) return 0;
	return SehReadInt(p);
}

float ReadFloatOff(void* ent, uint32_t off) {
	if (!ent || !off) return 0.f;
	auto* p = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ent) + off);
	if (!Mem::IsReadable(p, sizeof(float))) return 0.f;
	const float v = SehReadFloat(p);
	return Mem::Finite(v) ? v : 0.f;
}

// Inferno footprint from lit fire points only. ageSec = seconds since first see (warm-up).
// Clears when no fires lit or post-effect — halfW alone kept warn after visual die.
float InfernoRadius(void* ent, const Vector_t& origin, float ageSec = 999.f) {
	const uint32_t offFc = SchemaOr("C_Inferno->m_fireCount", kOffFireCount);
	const int fc = ReadIntOff(ent, offFc);
	if (fc <= 0)
		return 0.f;
	// Post-effect = fire dying; clear with the game
	if (ReadBoolOff(ent, kOffFirePostFx))
		return 0.f;

	const int n = (fc < 64) ? fc : 64;
	float maxR = 0.f;
	const float litF = SehInfernoScan(ent, origin.x, origin.y, n, &maxR);
	const float halfW = ReadFloatOff(ent, kOffMaxFireHalfW);

	if (litF >= 0.5f) {
		if (halfW > 15.f && halfW < 200.f && halfW > maxR)
			maxR = halfW;
		if (maxR < kRadiusMollyBase)
			maxR = kRadiusMollyBase;
		return (maxR < kRadiusMollyMax) ? maxR : kRadiusMollyMax;
	}

	// First ~0.35s burn flags can lag — show base ring only during warm-up
	if (ageSec < 0.35f)
		return kRadiusMollyBase;
	return 0.f; // no lit fires → fire is out
}

// Explode flags: always trust when settled. Mid-air only if both flags look real
// (single garbage tick used to wipe HE instantly while flying).
bool ProjectileExplodedSettled(void* ent, bool flying) {
	const int tick = ReadIntOff(ent, kOffExplodeTickBegin);
	const bool began = ReadBoolOff(ent, kOffExplodeBegan);
	if (!flying)
		return tick > 0 || began;
	// Air: require both, or a large tick (real detonate sequence)
	return began && tick > 1;
}

float InfernoLifeSeconds(void* ent) {
	// Prefer full burn length; m_nFireLifetime can be ticks/junk — only trust ~5–8s
	const float life = ReadFloatOff(ent, kOffFireLifetime);
	if (life >= 5.f && life <= 8.5f)
		return life;
	return kDurMolly; // 7s CS2 default
}

Vector_t ReadEntityVelocity(C_BaseEntity* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return Vector_t{};
	// dump-stable first (schema has mis-resolved this field before)
	auto tryOff = [&](uint32_t off) -> Vector_t {
		if (!off) return {};
		auto* p = reinterpret_cast<Vector_t*>(reinterpret_cast<uint8_t*>(ent) + off);
		if (!Mem::IsReadable(p, sizeof(Vector_t)))
			return {};
		return SehReadVec(p);
	};
	Vector_t v = tryOff(kOffAbsVel);
	if (v.Length() >= 5.f)
		return v;
	const uint32_t sch = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_vecAbsVelocity"));
	if (sch && sch != kOffAbsVel) {
		Vector_t v2 = tryOff(sch);
		if (v2.Length() >= 5.f)
			return v2;
	}
	// m_vecVelocity (sometimes populated when abs is not)
	const uint32_t offVel = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_vecVelocity"));
	if (offVel) {
		Vector_t v3 = tryOff(offVel);
		if (v3.Length() >= 5.f)
			return v3;
	}
	return v;
}

void AngleVectors(const QAngle_t& ang, Vector_t& forward) {
	const float pitch = ang.x * (3.14159265f / 180.f);
	const float yaw = ang.y * (3.14159265f / 180.f);
	const float cp = std::cos(pitch);
	const float sp = std::sin(pitch);
	const float cy = std::cos(yaw);
	const float sy = std::sin(yaw);
	forward = { cp * cy, cp * sy, -sp };
}

bool GetLocalViewAngles(QAngle_t& out) {
	if (!Input::GetViewAngles || !Input::viewAngleContext)
		return false;
	const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
	if (!viewPtr || !Mem::IsReadable(reinterpret_cast<void*>(viewPtr), sizeof(Vector_t)))
		return false;
	const Vector_t v = SehReadVec(reinterpret_cast<const Vector_t*>(viewPtr));
	if (!std::isfinite(v.x) || !std::isfinite(v.y))
		return false;
	out = { v.x, v.y, 0.f };
	return true;
}

float TypeRadius(NadeType t) {
	switch (t) {
	case NadeType::HE: return kRadiusHE;
	case NadeType::Flash: return kRadiusFlash;
	case NadeType::Smoke: return kRadiusSmoke;
	case NadeType::Molly: return 130.f; // flying molly pred ring; live fire uses InfernoRadius
	default: return 0.f;
	}
}

ImVec4 TypeColor(NadeType t, bool local) {
	if (local)
		return Config::nade_pred_local_color;
	switch (t) {
	case NadeType::HE: return Config::world_esp_he_color;
	case NadeType::Flash: return Config::world_esp_flash_color;
	case NadeType::Smoke: return Config::world_esp_smoke_color;
	case NadeType::Molly: return Config::world_esp_molotov_color;
	case NadeType::Decoy: return Config::world_esp_decoy_color;
	default: return Config::nade_pred_color;
	}
}

// Robust: class + designer; accept loose "projectile" / base class names
NadeType ClassifyProjectile(const char* cls, const char* designer, bool* isInferno = nullptr) {
	if (isInferno) *isInferno = false;
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";

	auto has = [](const char* s, const char* k) -> bool {
		return s && k && k[0] && std::strstr(s, k) != nullptr;
	};

	// Ground fire first
	if (has(a, "Inferno") || has(a, "FireCrackerBlast") || has(b, "inferno")) {
		if (isInferno) *isInferno = true;
		return NadeType::Molly;
	}

	// Class name (C_HEGrenadeProjectile etc.)
	if (has(a, "HEGrenadeProjectile") || has(a, "FragGrenadeProjectile"))
		return NadeType::HE;
	if (has(a, "FlashbangProjectile"))
		return NadeType::Flash;
	if (has(a, "SmokeGrenadeProjectile"))
		return NadeType::Smoke;
	if (has(a, "MolotovProjectile") || has(a, "IncendiaryGrenadeProjectile")
		|| has(a, "IncGrenadeProjectile"))
		return NadeType::Molly;
	if (has(a, "DecoyProjectile"))
		return NadeType::Decoy;

	// Designer: hegrenade_projectile / smokegrenade_projectile / ...
	// Also accept without requiring both substrings if name is clearly a nade projectile
	if (has(b, "projectile") || has(a, "Projectile") || has(a, "GrenadeProjectile")) {
		if (has(b, "smoke") || has(a, "Smoke"))
			return NadeType::Smoke;
		if (has(b, "molotov") || has(b, "incendiary") || has(b, "incgrenade")
			|| has(a, "Molotov") || has(a, "Incendiary"))
			return NadeType::Molly;
		if (has(b, "hegrenade") || has(b, "he_grenade") || has(a, "HEGrenade") || has(a, "Frag"))
			return NadeType::HE;
		if (has(b, "flash") || has(a, "Flash"))
			return NadeType::Flash;
		if (has(b, "decoy") || has(a, "Decoy"))
			return NadeType::Decoy;
		// Unknown projectile subclass — still treat as HE for warn (better than silent miss)
		if (has(a, "BaseCSGrenadeProjectile") || has(a, "GrenadeProjectile"))
			return NadeType::HE;
	}

	// Designer-only without "projectile" token (some builds strip it)
	if (has(b, "hegrenade") && !has(b, "weapon_"))
		return NadeType::HE;
	if (has(b, "flashbang") && !has(b, "weapon_"))
		return NadeType::Flash;
	if (has(b, "smokegrenade") && !has(b, "weapon_"))
		return NadeType::Smoke;
	if ((has(b, "molotov") || has(b, "incgrenade")) && !has(b, "weapon_"))
		return NadeType::Molly;
	if (has(b, "decoy") && !has(b, "weapon_"))
		return NadeType::Decoy;

	return NadeType::Unknown;
}

NadeType ClassifyWeapon(const char* cls, const char* designer) {
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";
	if (std::strstr(a, "SmokeGrenade") || std::strstr(b, "smokegrenade"))
		return NadeType::Smoke;
	if (std::strstr(a, "Molotov") || std::strstr(a, "Incendiary")
		|| std::strstr(b, "molotov") || std::strstr(b, "incgrenade") || std::strstr(b, "incendiary"))
		return NadeType::Molly;
	if (std::strstr(a, "HEGrenade") || std::strstr(b, "hegrenade"))
		return NadeType::HE;
	if (std::strstr(a, "Flashbang") || std::strstr(b, "flashbang"))
		return NadeType::Flash;
	if (std::strstr(a, "Decoy") || std::strstr(b, "decoy"))
		return NadeType::Decoy;
	return NadeType::Unknown;
}

const char* SehDumpClassName(CEntityInstance* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	// vfunc 46 only (IDA-confirmed GetSchemaClassInfo). Do not try 44 — wrong slot.
	SchemaClassInfoData_t* cls = nullptr;
	__try {
		ent->dump_class_info(&cls);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		cls = nullptr;
	}
	if (cls && cls->szName && Mem::IsReadable(cls->szName, 2) && cls->szName[0])
		return cls->szName;
	cls = nullptr;
	__try {
		GetSchemaClassInfo(reinterpret_cast<uintptr_t>(ent), &cls);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 2) || !cls->szName[0])
		return nullptr;
	return cls->szName;
}

const char* GetClassName(CEntityInstance* ent) {
	return SehDumpClassName(ent);
}

// Direct identity-list walk (same layout as GetBaseEntity) — fallback if hook ptr missing
CEntityInstance* GetEntityByIndexRaw(int index) {
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::ValidEntityIndex(index))
		return nullptr;
	auto* es = I::GameEntity->Instance;
	if ((unsigned)index > 0x7FFE || (unsigned)(index >> 9) > 0x3F)
		return nullptr;
	void* chunk = nullptr;
	if (!Mem::ReadField(es, 0x10 + 8ull * (index >> 9), chunk) || !chunk)
		return nullptr;
	const uintptr_t slot = reinterpret_cast<uintptr_t>(chunk) + 0x70ull * (index & 0x1FF);
	if (!Mem::IsReadable(reinterpret_cast<void*>(slot), 0x18))
		return nullptr;
	uint32_t handleBits = 0;
	if (!Mem::ReadField(reinterpret_cast<void*>(slot), 0x10, handleBits))
		return nullptr;
	if ((handleBits & 0x7FFF) != static_cast<uint32_t>(index))
		return nullptr;
	void* ent = nullptr;
	if (!Mem::ReadField(reinterpret_cast<void*>(slot), 0, ent) || !Mem::ValidEntity(ent))
		return nullptr;
	return reinterpret_cast<CEntityInstance*>(ent);
}

// Skip handle-serial match — some projectile slots fail the strict check mid-predict
CEntityInstance* GetEntityByIndexLoose(int index) {
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
	void* ent = nullptr;
	if (!Mem::ReadField(reinterpret_cast<void*>(slot), 0, ent) || !Mem::ValidEntity(ent))
		return nullptr;
	return reinterpret_cast<CEntityInstance*>(ent);
}

// Designer from identity slot even when entity Get fails
const char* GetDesignerAtIndex(int index) {
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

// CEntityInstance::m_pEntity @+0x10, CEntityIdentity::m_designerName @+0x20
const char* GetDesignerName(CEntityInstance* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	CEntityIdentity* id = nullptr;
	if (!Mem::ReadField(ent, 0x10, id) || !id || !Mem::Valid(id, 0x28)) {
		__try {
			id = ent->m_pEntityIdentity();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	if (!id || !Mem::Valid(id, 0x28))
		return nullptr;
	const char* p = nullptr;
	if (!Mem::ReadField(id, 0x20, p) || !p) {
		__try {
			p = id->m_designerName();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	if (!p || !Mem::IsReadable(p, 2) || !p[0])
		return nullptr;
	return p;
}

bool ReadGrenadeWeaponState(C_CSWeaponBase* wep, bool& pinPulled, bool& held, float& throwStrength) {
	pinPulled = false;
	held = true;
	throwStrength = 1.f;
	if (!wep || !Mem::ValidEntity(wep))
		return false;
	const uint32_t offPin = SchemaOr("C_BaseCSGrenade->m_bPinPulled", kOffPinPulled);
	const uint32_t offHeld = SchemaOr("C_BaseCSGrenade->m_bIsHeldByPlayer", kOffHeldByPlayer);
	const uint32_t offStr = SchemaOr("C_BaseCSGrenade->m_flThrowStrength", kOffThrowStrength);
	pinPulled = ReadBoolOff(wep, offPin);
	held = ReadBoolOff(wep, offHeld);
	float v = ReadFloatOff(wep, offStr);
	if (Mem::Finite(v)) {
		if (v < 0.f) v = 0.f;
		if (v > 1.f) v = 1.f;
		throwStrength = v;
	}
	return true;
}

float ReadThrowVelocity(C_CSWeaponBase* wep, NadeType type) {
	const float fallback = (type == NadeType::Molly) ? 700.f : 750.f;
	if (!wep || !Mem::ValidEntity(wep))
		return fallback;
	CCSWeaponBaseVData* data = wep->Data();
	if (!data || !Mem::Valid(data, 8))
		return fallback;
	float v = 0.f;
	__try { v = data->m_flThrowVelocity(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
	if (!Mem::Finite(v) || v < 50.f || v > 2000.f)
		return fallback;
	return v;
}

Vector_t ApproxNormal(const Vector_t& dir) {
	const float ax = std::fabs(dir.x);
	const float ay = std::fabs(dir.y);
	const float az = std::fabs(dir.z);
	if (az >= ax && az >= ay)
		return dir.z <= 0.f ? Vector_t{ 0.f, 0.f, 1.f } : Vector_t{ 0.f, 0.f, -1.f };
	if (ax >= ay)
		return dir.x >= 0.f ? Vector_t{ -1.f, 0.f, 0.f } : Vector_t{ 1.f, 0.f, 0.f };
	return dir.y >= 0.f ? Vector_t{ 0.f, -1.f, 0.f } : Vector_t{ 0.f, 1.f, 0.f };
}

Vector_t ClipVelocity(const Vector_t& vel, const Vector_t& n, float overbounce) {
	const float backoff = Dot(vel, n) * overbounce;
	Vector_t out{
		vel.x - n.x * backoff,
		vel.y - n.y * backoff,
		vel.z - n.z * backoff
	};
	if (std::fabs(out.x) < 0.1f) out.x = 0.f;
	if (std::fabs(out.y) < 0.1f) out.y = 0.f;
	if (std::fabs(out.z) < 0.1f) out.z = 0.f;
	return out;
}

struct HitResult {
	float fraction = 1.f;
	Vector_t endPos{};
	Vector_t normal{};
	bool hit = false;
};

bool TraceGrenadeLine(const Vector_t& start, const Vector_t& end, void* skip, HitResult& out) {
	out = HitResult{ 1.f, end, Vector_t{}, false };
	if (!Trace::Ready())
		return false;

	Trace::CGameTrace tr{};
	// Andromeda: mask 0x2009, layer 4, a5 7
	if (!Trace::TraceLine(start, end, skip, tr, Trace::kMaskGrenade, 4, 7))
		return false;

	float frac = std::clamp(tr.fraction(), 0.f, 1.f);
	Vector_t n = tr.normal();
	const float nLen = n.Length();
	if (nLen > 1e-3f && FiniteVec(n))
		n = n * (1.f / nLen);
	else {
		const Vector_t seg = end - start;
		if (LenSq(seg) > 1e-6f)
			n = ApproxNormal(seg);
		else
			n = Vector_t{ 0.f, 0.f, 1.f };
	}

	Vector_t endPos = tr.endpos();
	if (!FiniteVec(endPos) || frac >= 0.999f) {
		// Reconstruct from fraction if engine endpos bad
		endPos = start + (end - start) * frac;
	}

	out = HitResult{ frac, endPos, n, frac < 0.97f };
	return true;
}

HitResult TraceGrenadeHull(const Vector_t& start, const Vector_t& end, void* skip) {
	HitResult center{};
	TraceGrenadeLine(start, end, skip, center);

	const Vector_t seg = end - start;
	const float len = seg.Length();
	if (len <= 1e-3f)
		return center;

	float bestFrac = center.fraction;
	Vector_t bestN = center.normal;
	bool bestHit = center.hit;

	const Vector_t dir = seg * (1.f / len);
	const Vector_t helper = (std::fabs(dir.z) < 0.9f)
		? Vector_t{ 0.f, 0.f, 1.f }
		: Vector_t{ 1.f, 0.f, 0.f };
	Vector_t p1 = Cross(dir, helper);
	const float p1l = p1.Length();
	if (p1l <= 1e-3f)
		return center;
	p1 = p1 * (1.f / p1l);
	const Vector_t p2 = Cross(dir, p1);

	const Vector_t offsets[4] = {
		p1 * kGrenadeHull,
		p1 * -kGrenadeHull,
		p2 * kGrenadeHull,
		p2 * -kGrenadeHull
	};

	for (int i = 0; i < 4; ++i) {
		HitResult tr{};
		if (TraceGrenadeLine(start + offsets[i], end + offsets[i], skip, tr) && tr.fraction < bestFrac) {
			bestFrac = tr.fraction;
			bestN = tr.normal;
			bestHit = tr.hit;
		}
	}

	HitResult out{ bestFrac, start + seg * bestFrac, bestN, bestHit };
	// Nudge off surface so the next tick doesn't startsolid inside geometry
	if (out.hit && FiniteVec(out.normal))
		out.endPos = out.endPos + out.normal * 0.125f;
	return out;
}

// fuseRemainSec: HE/flash remaining fuse from a live projectile (>=0).
// <0 → fresh throw: Valve (tick - 8) * dt > GRENADE_TIMER.
bool ShouldDetonate(NadeType type, const Vector_t& vel, int tick, float fuseRemainSec = -1.f) {
	const float t = static_cast<float>(tick) * kSimDt;
	switch (type) {
	case NadeType::Smoke:
		return Len2D(vel) < 0.1f && (tick % 12) == 0;
	case NadeType::Decoy:
		return Len2D(vel) < 0.2f && (tick % 12) == 0;
	case NadeType::Molly:
		// Fail-safe — real stop is impact in ResolveCollision
		return t > 2.f;
	case NadeType::Flash:
	case NadeType::HE: {
		if (fuseRemainSec >= 0.f && fuseRemainSec <= 3.f)
			return t >= fuseRemainSec; // live remaining (delay already spent)
		// Fresh throw: Think_Detonate (tick - 8) * dt > 1.5
		return tick > kFuseDelayTicks
			&& static_cast<float>(tick - kFuseDelayTicks) * kSimDt >= kGrenadeTimer;
	}
	default:
		return false;
	}
}

// Returns true if molly should impact-detonate (stop sim)
bool ResolveCollision(const HitResult& tr, Vector_t& pos, Vector_t& vel, NadeType type, void* skip) {
	if (type == NadeType::Molly && (tr.normal.z >= kMollyMaxSlopeZ || LenSq(vel) < kStopSpeedSq)) {
		vel = Vector_t{};
		return true;
	}

	Vector_t newVel = ClipVelocity(vel, tr.normal, 2.f) * kElasticity;

	if (tr.normal.z > 0.7f) {
		const float speedSq = LenSq(newVel);
		if (speedSq > 96000.f) {
			const float speed = std::sqrt(speedSq);
			const float l = Dot(newVel, tr.normal) / speed;
			if (l > 0.5f)
				newVel = newVel * (1.5f - l);
		}
		if (LenSq(newVel) < kStopSpeedSq) {
			vel = Vector_t{};
			return false;
		}
	}

	vel = newVel;
	const float remaining = 1.f - tr.fraction;
	if (remaining > 0.f) {
		const Vector_t postEnd = pos + newVel * (remaining * kSimDt);
		HitResult post{};
		if (TraceGrenadeLine(pos, postEnd, skip, post))
			pos = post.endPos;
		else
			pos = postEnd;
	}
	return false;
}

// Returns true if impact detonation (molly)
bool StepSim(Vector_t& pos, Vector_t& vel, NadeType type, void* skip, bool& hit) {
	// Trapezoidal integration on Z (Andromeda)
	const float newVz = vel.z - kGravity * kSimDt;
	const Vector_t move{
		vel.x * kSimDt,
		vel.y * kSimDt,
		(vel.z + newVz) * 0.5f * kSimDt
	};
	vel.z = newVz;

	const Vector_t end = pos + move;
	const HitResult tr = TraceGrenadeHull(pos, end, skip);
	pos = tr.endPos;
	hit = tr.hit;

	if (tr.hit)
		return ResolveCollision(tr, pos, vel, type, skip);
	return false;
}

// fuseRemainSec: HE/flash remaining fuse from live projectile (>=0).
// <0 → fresh throw with Valve 8-tick delay + GRENADE_TIMER.
// HE/flash stop at fuse boom point — never walk past detonation to a fake "land".
bool SimulatePath(Vector_t pos, Vector_t vel, void* skip, NadeType type, Path& out,
	float fuseRemainSec = -1.f) {
	out.points.clear();
	out.hits.clear();
	out.points.reserve(128);
	out.hits.reserve(16);
	out.type = type;
	out.radius = TypeRadius(type);
	out.land = pos;
	out.detonated = false;

	if (!FiniteVec(pos) || !FiniteVec(vel))
		return false;

	const bool timedBoom = (type == NadeType::HE || type == NadeType::Flash);
	const bool liveFuse = timedBoom && fuseRemainSec >= 0.f && fuseRemainSec <= 3.f;
	const float fuseLimit = liveFuse ? fuseRemainSec : (timedBoom ? kGrenadeTimer : -1.f);
	const int fuseDelay = liveFuse ? 0 : (timedBoom ? kFuseDelayTicks : 0);
	// Cap sim ticks by fuse (+ Valve delay on fresh throws)
	const int maxTicks = (fuseLimit >= 0.f)
		? (std::min(kMaxTicks, static_cast<int>(fuseLimit / kSimDt) + fuseDelay + 2))
		: kMaxTicks;

	auto fuseBoom = [&](int tick) -> bool {
		if (fuseLimit < 0.f)
			return false;
		if (liveFuse)
			return static_cast<float>(tick) * kSimDt >= fuseLimit;
		return tick > fuseDelay
			&& static_cast<float>(tick - fuseDelay) * kSimDt >= fuseLimit;
	};

	if (!Trace::Ready()) {
		// Free-flight fallback with correct gravity + fuse
		out.points.push_back(pos);
		for (int i = 0; i < maxTicks && i < 256; ++i) {
			if (fuseBoom(i)) {
				out.land = pos;
				out.detonated = true;
				break;
			}
			const float newVz = vel.z - kGravity * kSimDt;
			pos.x += vel.x * kSimDt;
			pos.y += vel.y * kSimDt;
			pos.z += (vel.z + newVz) * 0.5f * kSimDt;
			vel.z = newVz;
			if (!FiniteVec(pos))
				break;
			out.points.push_back(pos);
			if (pos.z < -2000.f)
				break;
		}
		if (!out.points.empty() && !out.detonated)
			out.land = out.points.back();
		return out.points.size() >= 2;
	}

	int bounce = 0;
	int timer = 0;
	bool valid = false;

	for (int tick = 0; tick < maxTicks; ++tick) {
		// Fuse boom before step — position is where it is when timer hits
		if (fuseBoom(tick)) {
			out.land = pos;
			out.detonated = true;
			valid = true;
			break;
		}

		if (timer == 0)
			out.points.push_back(pos);

		bool hit = false;
		const bool impact = StepSim(pos, vel, type, skip, hit);
		if (hit) {
			++bounce;
			if (out.hits.size() < 24u && FiniteVec(pos))
				out.hits.push_back(pos);
		}

		const bool stopped =
			std::fabs(vel.x) < 45.f &&
			std::fabs(vel.y) < 45.f &&
			LenSq(vel) < kStopSpeedSq;

		// HE/flash: rest on ground still waits for fuse (don't mark land early)
		if (timedBoom && stopped) {
			vel = Vector_t{};
			if (hit || ++timer >= kTicksPerPoint)
				timer = 0;
			continue;
		}

		if (impact || ShouldDetonate(type, vel, tick, fuseRemainSec)
			|| bounce > kMaxBounces
			|| (!timedBoom && stopped)) {
			out.land = pos;
			// impact molly / fuse detonate / smoke settle
			out.detonated = impact
				|| (type == NadeType::HE || type == NadeType::Flash)
				|| (type == NadeType::Molly);
			if (type == NadeType::Smoke || type == NadeType::Decoy)
				out.detonated = false; // ground rest / pop-on-stop
			valid = true;
			break;
		}

		if (hit || ++timer >= kTicksPerPoint)
			timer = 0;
	}

	if (valid && !out.points.empty()) {
		const Vector_t last = out.points.back();
		const Vector_t diff = last - out.land;
		if (LenSq(diff) > 1.f)
			out.points.push_back(out.land);
	}

	// Timed out: still usable path to last sample
	if (!valid && out.points.size() >= 2) {
		out.land = out.points.back();
		if (timedBoom)
			out.detonated = true;
		valid = true;
	}

	return valid && out.points.size() >= 2;
}

// SEH isolate — no C++ objects with dtors
static void* SehReadPtr(uintptr_t absAddr) {
	void* v = nullptr;
	__try { v = *reinterpret_cast<void**>(absAddr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return v;
}

// Prefer engine m_flDetonateTime - curtime; else wall-clock remaining.
// Distrust detonate that always reads ~full fuse while wall clock is nearly done
// (stale field on recycled shells after land → skip_fuse lag / wrong keep-alive).
float ReadHeFlashFuseRemain(void* ent, float wallLeft) {
	float engineLeft = -1.f;
	if (ent && Mem::ValidEntity(ent)) {
		const float det = ReadFloatOff(ent, kOffDetonateTime);
		// Absolute game time in a sensible range
		if (det > 1.f && det < 1.0e7f && std::isfinite(det)) {
			static uintptr_t s_gvAbs = 0;
			static bool s_tried = false;
			if (!s_tried) {
				s_tried = true;
				uintptr_t insn = M::patternScan("client", "48 8B 05 ? ? ? ? F3 0F 10 40 30 C3");
				if (insn)
					s_gvAbs = M::getAbsoluteAddress(insn, 3, 0);
			}
			if (s_gvAbs) {
				void* gv = SehReadPtr(s_gvAbs);
				if (gv) {
					const float ct = ReadFloatOff(gv, 0x30);
					if (ct > 1.f && det > ct && (det - ct) <= 3.f)
						engineLeft = det - ct;
				}
			}
		}
	}
	if (engineLeft >= 0.f) {
		// Stale full fuse on recycled shell while wall already spent → wall wins
		if (wallLeft >= 0.f && wallLeft <= 0.35f && engineLeft > 1.0f)
			return wallLeft;
		// Late entity discovery: wall still ~full but engine knows real remaining
		if (wallLeft > 1.0f && engineLeft < wallLeft - 0.25f)
			return engineLeft;
		return engineLeft;
	}
	if (wallLeft >= 0.f && wallLeft <= 3.f)
		return wallLeft;
	return kGrenadeTimer;
}

// Valve cstrike15 weapon_basecsgrenade::ThrowGrenade
// pitch boost, clamp(base*0.9,15,750), Lerp(strength, 0.3, 1.0)
Vector_t ComputeThrowVelocity(const QAngle_t& angles, float baseVel, float throwStrength) {
	float strength = std::clamp(throwStrength, 0.f, 1.f);
	// Mid-strength snap (M1+M2) — matches common CS2 cook behavior
	if (std::fabs(strength - 0.5f) < 0.1f)
		strength = 0.5f;

	QAngle_t adj = angles;
	if (adj.x > 90.f) adj.x -= 360.f;
	else if (adj.x < -90.f) adj.x += 360.f;
	// +10° upward when looking horizontal, fade at pitch extremes
	adj.x -= 10.f * (90.f - std::fabs(adj.x)) / 90.f;

	const float throwVel = std::clamp(baseVel * 0.9f, 15.f, 750.f);
	// Lerp(strength, GRENADE_SECONDARY_DAMPENING=0.3, 1.0)
	const float throwSpeed = throwVel * (0.3f + strength * 0.7f);

	Vector_t forward{};
	AngleVectors(adj, forward);
	return forward * throwSpeed;
}

int ReadProjectileBounces(C_BaseEntity* ent) {
	if (!ent)
		return 0;
	const uint32_t off = SchemaOr("C_BaseCSGrenadeProjectile->m_nBounces", kOffBounces);
	const int n = ReadIntOff(ent, off);
	return (n >= 0 && n < 64) ? n : 0;
}

bool IsLocalOwnedProjectile(C_BaseEntity* ent, C_CSPlayerPawn* local) {
	if (!ent || !local || !I::GameEntity || !I::GameEntity->Instance)
		return false;
	const uint32_t off = SchemaOr("C_BaseGrenade->m_hThrower", kOffThrower);
	if (!off || !Mem::IsReadable(reinterpret_cast<uint8_t*>(ent) + off, sizeof(CBaseHandle)))
		return false;
	auto* ph = reinterpret_cast<CBaseHandle*>(reinterpret_cast<uint8_t*>(ent) + off);
	if (!ph->valid())
		return false;
	auto* thrower = SehGetEntity(ph->index());
	return thrower && thrower == reinterpret_cast<CEntityInstance*>(local);
}

// CUtlAutoList — IDA: Smoke node @+0x1260 / next @+0x1270, Planted node @+0x1188 / next @+0x1198
// Heads resolved by pattern (hardcoded RVA breaks on patch). Fallback RVAs from this IDB.
constexpr uintptr_t kRvaSmokeAutoHeadFallback = 0x2312B80;
constexpr uintptr_t kOffSmokeAutoNext = 0x1270;
constexpr uintptr_t kRvaPlantedAutoHeadFallback = 0x236D678;
constexpr uintptr_t kOffPlantedAutoNext = 0x1198;

static HMODULE ClientMod() {
	static HMODULE s_client = nullptr;
	if (!s_client)
		s_client = GetModuleHandleW(L"client.dll");
	return s_client;
}

static void** ClientRvaPtr(uintptr_t rva) {
	HMODULE m = ClientMod();
	if (!m)
		return nullptr;
	return reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(m) + rva);
}

// IDA unique: lea rcx,[rbx+0x1260]; test rcx,rcx  then cmp [head],rdi
static void** ResolveSmokeAutoHead() {
	static void** s_head = nullptr;
	static bool s_tried = false;
	if (s_tried)
		return s_head;
	s_tried = true;
	uintptr_t lea = M::patternScan("client", "48 8D 8B 60 12 00 00 48 85 C9");
	if (lea) {
		// Within ctor: cmp qword ptr [rip+disp], rdi  (48 39 3D disp32)
		for (int i = 0; i < 0x40; ++i) {
			auto* p = reinterpret_cast<uint8_t*>(lea + i);
			if (!Mem::IsReadable(p, 7))
				break;
			if (p[0] == 0x48 && p[1] == 0x39 && p[2] == 0x3D) {
				s_head = reinterpret_cast<void**>(M::getAbsoluteAddress(lea + i, 3, 0));
				break;
			}
		}
	}
	if (!s_head)
		s_head = ClientRvaPtr(kRvaSmokeAutoHeadFallback);
	return s_head;
}

// IDA: mov [r14+0x1188],rax ; cmp [head],rbp
static void** ResolvePlantedAutoHead() {
	static void** s_head = nullptr;
	static bool s_tried = false;
	if (s_tried)
		return s_head;
	s_tried = true;
	uintptr_t mov = M::patternScan("client", "49 89 86 88 11 00 00 48 39 2D");
	if (mov) {
		// 49 89 86 88 11 00 00 | 48 39 2D disp32
		s_head = reinterpret_cast<void**>(M::getAbsoluteAddress(mov + 7, 3, 0));
	}
	if (!s_head)
		s_head = ClientRvaPtr(kRvaPlantedAutoHeadFallback);
	return s_head;
}

// Walk CUtlAutoList of entity bases linked via nextOff (head stores entity*, next @ nextOff)
static int WalkAutoListPtr(void** pHead, uintptr_t nextOff, void** out, int maxOut) {
	int n = 0;
	if (!pHead || !Mem::IsReadable(pHead, sizeof(void*)))
		return 0;
	void* e = nullptr;
	if (!Mem::Read(pHead, e) || !e)
		return 0;
	for (int guard = 0; e && n < maxOut && guard < 64; ++guard) {
		if (!Mem::ValidEntity(e))
			break;
		out[n++] = e;
		void* next = nullptr;
		if (!Mem::ReadField(e, nextOff, next))
			break;
		if (next == e)
			break;
		e = next;
	}
	return n;
}

// Shared: push one projectile/inferno into g_paths. Returns true if added.
static bool TryAddWorldProjectile(
	CEntityInstance* Entity, int indexHint,
	C_CSPlayerPawn* local, void* skip,
	bool wantWarn, bool wantPath,
	std::unordered_set<uintptr_t>& seen,
	int& count)
{
	if (!Entity || !Mem::ValidEntity(Entity) || count >= 64)
		return false;
	const uintptr_t key = reinterpret_cast<uintptr_t>(Entity);
	if (!seen.insert(key).second)
		return false;

	const char* designer = GetDesignerName(Entity);
	const char* cls = GetClassName(Entity);
	bool isInferno = false;
	const NadeType type = ClassifyProjectile(cls, designer, &isInferno);
	// Planted bomb is not a nade warn target here (visuals owns bomb ESP)
	if (type == NadeType::Unknown && !isInferno)
		return false;

	auto* base = reinterpret_cast<C_BaseEntity*>(Entity);
	Vector_t pos = ReadAbsOrigin(base);
	// First frames after throw: abs origin can be 0 — fall back to m_vInitialPosition
	if (!FiniteVec(pos) || (std::fabs(pos.x) < 0.01f && std::fabs(pos.y) < 0.01f && std::fabs(pos.z) < 0.01f)) {
		const Vector_t ip = ReadInitPosition(base);
		if (FiniteVec(ip) && !(std::fabs(ip.x) < 0.01f && std::fabs(ip.y) < 0.01f && std::fabs(ip.z) < 0.01f))
			pos = ip;
	}
	if (!FiniteVec(pos))
		return false;
	if (std::fabs(pos.x) < 0.01f && std::fabs(pos.y) < 0.01f && std::fabs(pos.z) < 0.01f)
		return false;

	const bool own = IsLocalOwnedProjectile(base, local);
	const uint32_t hkey = HandleKey(Entity, indexHint > 0 ? indexHint : 0);
	const Vector_t initPos = ReadInitPosition(base);
	const uint32_t throwSig = ThrowSigFromPos(FiniteVec(initPos) ? initPos : pos);
	const NadeType trackType = isInferno ? NadeType::Molly : type;
	const uint32_t fxKey = FxKey(hkey, throwSig, trackType);

	if (isInferno) {
		const int fc = ReadIntOff(base, NadeOff("C_Inferno->m_fireCount", kOffFireCount));
		if (fc <= 0)
			return false;
		const float life = InfernoLifeSeconds(base);
		const float left = EffectTimeLeft(fxKey, NadeType::Molly, life, throwSig);
		if (left <= 0.05f) {
			ClearFxTrack(fxKey);
			return false;
		}
		// age = life - left; used so lit-flag lag doesn't wipe first frames
		const float age = life - left;
		const float rad = InfernoRadius(base, pos, age);
		if (rad < 1.f)
			return false;
		Path path{};
		path.local_preview = false;
		path.own_projectile = own;
		path.origin = pos;
		path.land = pos;
		path.type = NadeType::Molly;
		path.radius = rad;
		path.time_left = left;
		path.effect_active = true;
		path.show_warn = wantWarn;
		path.ent_index = indexHint;
		path.points.push_back(pos);
		g_paths.push_back(std::move(path));
		++count;
		return true;
	}

	const bool didSmoke = (type == NadeType::Smoke) && ReadBoolOff(base,
		NadeOff("C_SmokeGrenadeProjectile->m_bDidSmokeEffect", kOffDidSmoke));
	if (didSmoke) {
		const float left = EffectTimeLeft(fxKey, NadeType::Smoke, kDurSmoke, throwSig);
		if (left <= 0.05f) {
			ClearFxTrack(fxKey);
			return false;
		}
		Path path{};
		path.local_preview = false;
		path.own_projectile = own;
		path.origin = pos;
		path.land = pos;
		path.type = NadeType::Smoke;
		path.radius = kRadiusSmoke;
		path.time_left = left;
		path.effect_active = true;
		path.show_warn = wantWarn;
		path.ent_index = indexHint;
		path.points.push_back(pos);
		g_paths.push_back(std::move(path));
		++count;
		return true;
	}

	// Abs velocity only for "flying". Never fall back to m_vInitialVelocity after
	// bounce/stop — that field stays at throw speed and falsely keeps fuse off forever.
	Vector_t vel = ReadEntityVelocity(base);
	const int bounces = ReadProjectileBounces(base);
	if (vel.Length() < 5.f && bounces <= 0) {
		const Vector_t iv = ReadInitVelocity(base);
		if (FiniteVec(iv) && iv.Length() > 5.f) {
			const float dx = pos.x - initPos.x, dy = pos.y - initPos.y, dz = pos.z - initPos.z;
			if (!FiniteVec(initPos) || (dx * dx + dy * dy + dz * dz) < (120.f * 120.f))
				vel = iv;
		}
	}
	const bool flying = vel.Length() >= 5.f;

	// HE/flash: remaining fuse drives path end (mid-air boom ≠ ground land).
	float fuseLeft = -1.f;
	float simFuse = -1.f;
	if (type == NadeType::HE || type == NadeType::Flash) {
		if (ProjectileExplodedSettled(base, flying)) {
			ClearFxTrack(fxKey);
			return false;
		}
		const float wallLeft = EffectTimeLeft(fxKey, type, kDurHeFlashFuse, throwSig);
		simFuse = ReadHeFlashFuseRemain(base, wallLeft);
		// Engine detonate (throw→boom) beats late wall discovery (was "too fast" badges)
		if (simFuse >= 0.f && simFuse <= 3.f)
			fuseLeft = simFuse;
		else
			fuseLeft = wallLeft;
		if (fuseLeft <= 0.05f) {
			ClearFxTrack(fxKey);
			return false;
		}
	} else if (type == NadeType::Molly) {
		// Air only — landed shell is dead; C_Inferno owns the burn warn
		if (!flying) {
			ClearFxTrack(fxKey);
			return false;
		}
		fuseLeft = EffectTimeLeft(fxKey, type, kDurMollyProjectile, throwSig);
		if (fuseLeft <= 0.05f) {
			ClearFxTrack(fxKey);
			return false;
		}
	}

	float decoyLeft = -1.f;
	if (type == NadeType::Decoy && !flying) {
		decoyLeft = EffectTimeLeft(fxKey, NadeType::Decoy, kDurDecoy, throwSig);
		if (decoyLeft <= 0.05f) {
			ClearFxTrack(fxKey);
			return false;
		}
	}


	Path path{};
	path.local_preview = false;
	path.own_projectile = own;
	path.origin = pos;
	path.land = pos;
	path.type = type;
	path.radius = TypeRadius(type);
	path.ent_index = indexHint;
	path.show_warn = wantWarn;
	if (type == NadeType::Decoy && !flying)
		path.time_left = decoyLeft;
	else if (fuseLeft >= 0.f)
		path.time_left = fuseLeft;
	else
		path.time_left = -1.f;
	if (type == NadeType::Decoy && !flying)
		path.effect_active = true;

	bool simOk = false;
	// Always sim HE/flash while fuse remains (air or rolling) so boom ≠ fake land
	const bool needSim = wantPath && (flying
		|| type == NadeType::HE || type == NadeType::Flash);
	if (needSim) {
		const float fuseForSim = (type == NadeType::HE || type == NadeType::Flash)
			? simFuse : -1.f;
		simOk = SimulatePath(pos, vel, skip ? skip : Entity, type, path, fuseForSim);
	}
	if (!simOk) {
		path.points.clear();
		path.points.push_back(pos);
		path.land = pos;
		path.origin = pos;
		// Settled HE/flash with fuse left still boom in place
		if (type == NadeType::HE || type == NadeType::Flash)
			path.detonated = true;
	} else {
		path.origin = pos;
	}
	g_paths.push_back(std::move(path));
	++count;
	return true;
}

void CollectProjectiles() {
	if (!Config::nade_pred_projectiles && !Config::nade_warn && !Config::nade_pred_damage)
		return;
	if (!I::GameEntity || !I::GameEntity->Instance) {
		ResetFxTracks();
		return;
	}
	if (!Mem::Valid(I::GameEntity->Instance, 0x2100)) {
		ResetFxTracks();
		return;
	}

	int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	// Heuristic round/map recycle if game events miss (entity list collapses).
	{
		if (nMax > s_collectPeak)
			s_collectPeak = nMax;
		if (s_collectPeak > 200 && nMax > 0 && nMax < s_collectPeak / 3) {
			if (++s_collectLowFrames >= 3) {
				char reason[64]{};
				snprintf(reason, sizeof(reason), "nMax_collapse %d->%d", s_collectPeak, nMax);
				RoundBoundaryReset(reason);
				s_collectPeak = nMax;
				s_collectLowFrames = 0;
			}
		} else {
			s_collectLowFrames = 0;
			if (nMax > 80)
				s_collectPeak = nMax;
		}
	}
	if (nMax <= 0) {
		// Entity storage is empty during round/map teardown. Keep one-frame
		// glitches harmless, but reset before the next round can reuse handles.
		// Without this, old FxTrack entries can suppress new warnings when the
		// client-side round event is missed.
		if (++s_collectEmptyFrames >= 2) {
			RoundBoundaryReset("entity_list_empty", false);
			s_collectEmptyFrames = 0;
			s_collectPeak = 0;
		}
		return;
	}
	s_collectEmptyFrames = 0;
	// Pad high: projectiles / planted C4 often sit above highest briefly
	if (nMax < 8192 - 512)
		nMax += 512;

	C_CSPlayerPawn* local = nullptr;
	void* skip = nullptr;
	if (H::oGetLocalPlayer) {
		local = H::oGetLocalPlayer(0);
		if (local && Mem::ValidEntity(local))
			skip = local;
	}

	const bool wantWarn = Config::nade_warn;
	const bool wantPath = Config::nade_pred && Config::nade_pred_projectiles;
	const bool wantDmg = Config::nade_pred && Config::nade_pred_damage;
	if (!wantWarn && !wantPath && !wantDmg)
		return;

	int count = 0;
	int validN = 0;
	int namedN = 0;
	int autoSmokeN = 0;
	int autoPlantN = 0;
	std::unordered_set<uintptr_t> seen;
	// Map/menu transition: clear so we re-inventory
	if (nMax + 32 < s_collectPrevMax) {
		s_collectSeenNames.clear();
		s_collectPrevMax = 0;
	}
	const bool nMaxGrew = nMax > s_collectPrevMax;
	const int grewFrom = s_collectPrevMax;
	if (nMaxGrew)
		s_collectPrevMax = nMax;

	for (int i = 1; i <= nMax && count < 64; ++i) {
		CEntityInstance* Entity = SehGetEntity(i);
		const char* designerIdx = GetDesignerAtIndex(i);
		if (!Mem::ValidEntity(Entity) && !designerIdx)
			continue;
		if (!Mem::ValidEntity(Entity))
			continue; // need entity for origin; designer-only skip
		++validN;

		const char* designer = GetDesignerName(Entity);
		if (!designer || !designer[0])
			designer = designerIdx;
		const char* cls = GetClassName(Entity);
		if (!cls && !designer)
			continue;
		++namedN;

		const char* c = cls ? cls : "";
		const char* d = designer ? designer : "";
		if (c[0] && s_collectSeenNames.insert(std::string("c:") + c).second)
		if (d[0] && s_collectSeenNames.insert(std::string("d:") + d).second)
		if (nMaxGrew && i > grewFrom)
		// Always log nade/bomb-related names even when index reused (not only nMax grow)
		if ((std::strstr(c, "Projectile") || std::strstr(c, "Inferno") || std::strstr(c, "Planted")
			|| std::strstr(d, "projectile") || std::strstr(d, "inferno") || std::strstr(d, "planted_c4")
			|| std::strstr(d, "grenade"))
			&& s_collectSeenNames.insert(std::string("nade:") + std::to_string(i) + ":" + c + ":" + d).second)

		TryAddWorldProjectile(Entity, i, local, skip, wantWarn, wantPath, seen, count);
	}

	// Secondary source: CUtlAutoList (smoke registers here; HE/molly have no list)
	{
		void* smokes[32]{};
		autoSmokeN = WalkAutoListPtr(ResolveSmokeAutoHead(), kOffSmokeAutoNext, smokes, 32);
		for (int i = 0; i < autoSmokeN; ++i) {
			auto* e = reinterpret_cast<CEntityInstance*>(smokes[i]);
			const char* cls = GetClassName(e);
			const char* des = GetDesignerName(e);
			TryAddWorldProjectile(e, 0, local, skip, wantWarn, wantPath, seen, count);
		}
		void* plants[8]{};
		autoPlantN = WalkAutoListPtr(ResolvePlantedAutoHead(), kOffPlantedAutoNext, plants, 8);
		for (int i = 0; i < autoPlantN; ++i) {
			auto* e = reinterpret_cast<CEntityInstance*>(plants[i]);
			const char* cls = GetClassName(e);
			const char* des = GetDesignerName(e);
		}
	}

	PurgeFxTracks();
	// Only log when count changes or every ~2s (was flooding every frame)
	static int s_lastFound = -1;
	static DWORD s_lastCollectLog = 0;
	const DWORD now = GetTickCount();
	if (count != s_lastFound || (now - s_lastCollectLog) > 2000) {
		s_lastFound = count;
		s_lastCollectLog = now;
	}
}

// Shared: compute throw start+vel from local grenade weapon
// Valve: eye + underhand Z, hull-trace 22u forward, pull back 6 → default 16 free-space
bool BuildLocalThrow(C_CSPlayerPawn* local, C_CSWeaponBase* wep, NadeType type,
	float throwStrength, Vector_t& outStart, Vector_t& outVel)
{
	QAngle_t angles{};
	if (!GetLocalViewAngles(angles))
		return false;
	if (angles.x < -90.f) angles.x += 360.f;
	else if (angles.x > 90.f) angles.x -= 360.f;

	const float strength = std::clamp(throwStrength, 0.f, 1.f);

	// Pitch-adjusted forward for spawn (same boost as throw vel)
	QAngle_t spawnAng = angles;
	spawnAng.x -= 10.f * (90.f - std::fabs(spawnAng.x)) / 90.f;
	Vector_t forward{};
	AngleVectors(spawnAng, forward);

	Vector_t eye = Bones::GetEyePos(local);
	if (!FiniteVec(eye))
		eye = local->getEyePosition();
	// Lerp(strength, -GRENADE_SECONDARY_LOWER(-12), 0)
	Vector_t src = eye + Vector_t{ 0.f, 0.f, strength * 12.f - 12.f };

	// Hull trace so thin walls don't spawn the nade inside geometry
	outStart = src + forward * 16.f;
	if (Trace::Ready()) {
		HitResult tr{};
		const Vector_t hullEnd = src + forward * 22.f;
		if (TraceGrenadeLine(src, hullEnd, local, tr) && tr.hit) {
			outStart = tr.endPos - forward * 6.f;
			// Keep spawn in front of eye if wall is very close
			const Vector_t back = outStart - src;
			if (Dot(back, forward) < 1.f)
				outStart = src + forward * 1.f;
		} else {
			outStart = hullEnd - forward * 6.f; // 16u free space
		}
	}

	outVel = ComputeThrowVelocity(angles, ReadThrowVelocity(wep, type), strength);
	Vector_t playerVel = local->m_vecAbsVelocity();
	if (!FiniteVec(playerVel))
		playerVel = Vector_t{};
	outVel = outVel + playerVel * 1.25f; // Valve player velocity scale
	return FiniteVec(outStart) && FiniteVec(outVel);
}

void CollectLocalPreview() {
	// Cook aim line: only while M1/pin pulled. Clears instantly if you switch weapon.
	// Post-throw path/warn is separate (ghost only after real throw confirm).
	if (!Config::nade_pred || !Config::nade_pred_local)
		return;
	if (!H::oGetLocalPlayer)
		return;

	C_CSPlayerPawn* local = H::oGetLocalPlayer(0);
	if (!local || !Mem::ValidEntity(local))
		return;
	const int hp = local->m_iHealth();
	if (hp <= 0 || hp > 200)
		return;

	C_CSWeaponBase* wep = local->GetActiveWeapon();
	if (!wep || !Mem::ValidEntity(wep))
		return;

	const char* cls = GetClassName(reinterpret_cast<CEntityInstance*>(wep));
	const char* designer = GetDesignerName(reinterpret_cast<CEntityInstance*>(wep));
	NadeType type = ClassifyWeapon(cls, designer);
	if (type == NadeType::Unknown)
		return;

	bool pinPulled = false;
	bool held = true;
	float throwStrength = 1.f;
	ReadGrenadeWeaponState(wep, pinPulled, held, throwStrength);

	// Always require pin/M1 — never show just for holding a nade in hand
	if (!held || !pinPulled)
		return;

	Vector_t start{}, vel{};
	if (!BuildLocalThrow(local, wep, type, throwStrength, start, vel))
		return;

	Path path{};
	path.local_preview = true;
	path.origin = start;
	// Fresh throw: pass <0 so sim applies Valve 8-tick fuse delay
	const float fuse = -1.f;
	if (SimulatePath(start, vel, local, type, path, fuse)) {
		path.origin = start;
		g_paths.push_back(std::move(path));
	}
}

// After real throw only: path/warn until entity appears, then smoke/molly full duration.
// Pin-pull + weapon switch must NOT show anything (user cancelled / never threw).
struct LocalThrowGhost {
	bool active = false;
	bool cooking = false;      // pin currently/was pulled on a nade
	bool pendingThrow = false; // M1 released while still on that nade (actual throw start)
	bool lastPin = false;
	NadeType type = NadeType::Unknown;
	float startTime = 0.f;
	float flightSec = 1.6f;
	float effectSec = 0.f;
	float throwStrength = 1.f;
	Vector_t start{};
	Vector_t vel{};
	std::vector<Vector_t> points;
	Vector_t land{};
};
LocalThrowGhost g_localGhost;

float GhostEffectDuration(NadeType t) {
	switch (t) {
	// Full effect life when entity scan misses (found=0) — short bridge made
	// smoke/molly warn vanish ~2s while volume still live.
	// Cleared early when real entity of same type appears.
	case NadeType::Smoke: return kDurSmoke;
	case NadeType::Molly: return kDurMolly;
	case NadeType::Decoy: return kDurDecoy;
	// HE/flash: flight only — entity fuse owns detonate clear
	case NadeType::HE:
	case NadeType::Flash: return 0.f;
	default: return 0.f;
	}
}

void ActivateLocalGhost(C_CSPlayerPawn* local) {
	if (g_localGhost.type == NadeType::Unknown || !local)
		return;
	Path sim{};
	sim.type = g_localGhost.type;
	// Fresh throw: <0 → Valve fuse delay inside SimulatePath
	if (!SimulatePath(g_localGhost.start, g_localGhost.vel, local, g_localGhost.type, sim, -1.f)
		|| sim.points.size() < 2) {
		g_localGhost.cooking = false;
		g_localGhost.pendingThrow = false;
		return;
	}
	g_localGhost.points = std::move(sim.points);
	g_localGhost.land = sim.land;
	g_localGhost.startTime = NowSeconds();
	// Sparse points every kTicksPerPoint — convert to flight time
	const float est = static_cast<float>(g_localGhost.points.size())
		* static_cast<float>(kTicksPerPoint) * kSimDt;
	g_localGhost.flightSec = std::clamp(est, 0.35f, 2.8f);
	g_localGhost.effectSec = GhostEffectDuration(g_localGhost.type);
	g_localGhost.active = true;
	g_localGhost.cooking = false;
	g_localGhost.pendingThrow = false;
}

void CollectLocalThrowGhost() {
	// Throw-only path for own util (no pin-hold / no weapon-switch false throw)
	if (!Config::nade_warn && !Config::nade_pred)
		return;
	if (!H::oGetLocalPlayer)
		return;

	// Only same-type entity-backed path kills ghost. Any-entity clear was wrong:
	// leftover HE/smoke/fire from round 1 wiped molly ghost / left wrong badge.
	if (g_localGhost.active || g_localGhost.cooking || g_localGhost.pendingThrow) {
		for (const auto& p : g_paths) {
			if (p.local_preview || p.ent_index <= 0)
				continue;
			if (p.type != g_localGhost.type)
				continue;
			// Prefer own util; same-type world also covers (entity owns warn)
			if (p.own_projectile || p.effect_active || p.type == g_localGhost.type) {
				g_localGhost = {};
				return;
			}
		}
	}

	C_CSPlayerPawn* local = H::oGetLocalPlayer(0);
	if (!local || !Mem::ValidEntity(local))
		return;
	const int hp = local->m_iHealth();
	if (hp <= 0 || hp > 200) {
		g_localGhost = {};
		return;
	}

	// Track cook/throw even while a ghost is active (replace on new throw).
	// Old gate `if (!active)` blocked second nade for full smoke/molly ghost life.
	{
		C_CSWeaponBase* wep = local->GetActiveWeapon();
		NadeType curType = NadeType::Unknown;
		bool pinPulled = false;
		bool held = false;
		float throwStrength = 1.f;
		if (wep && Mem::ValidEntity(wep)) {
			const char* cls = GetClassName(reinterpret_cast<CEntityInstance*>(wep));
			const char* designer = GetDesignerName(reinterpret_cast<CEntityInstance*>(wep));
			curType = ClassifyWeapon(cls, designer);
			if (curType != NadeType::Unknown)
				ReadGrenadeWeaponState(wep, pinPulled, held, throwStrength);
		}

		const bool lastPin = g_localGhost.lastPin;
		const NadeType lockedType = g_localGhost.active ? g_localGhost.type : NadeType::Unknown;

		// Cooking snapshot — don't clobber an active ghost's type/path until throw confirms
		if (curType != NadeType::Unknown && held && pinPulled) {
			if (!g_localGhost.active) {
				g_localGhost.cooking = true;
				g_localGhost.pendingThrow = false;
				g_localGhost.type = curType;
				g_localGhost.throwStrength = throwStrength;
				Vector_t s{}, v{};
				if (BuildLocalThrow(local, wep, curType, throwStrength, s, v)) {
					g_localGhost.start = s;
					g_localGhost.vel = v;
				}
			} else if (curType != lockedType) {
				// Second nade while first ghost still up — stage separately via pending
				g_localGhost.cooking = true;
			}
		}

		// Throw release on same nade type (or any nade if no active ghost)
		const NadeType throwType = g_localGhost.active ? lockedType : g_localGhost.type;
		if (g_localGhost.cooking
			&& curType != NadeType::Unknown
			&& (!g_localGhost.active || curType == throwType || curType != lockedType)
			&& held && lastPin && !pinPulled) {
			g_localGhost.pendingThrow = true;
			g_localGhost.type = curType;
			g_localGhost.throwStrength = throwStrength;
			Vector_t s{}, v{};
			if (wep && Mem::ValidEntity(wep)
				&& BuildLocalThrow(local, wep, curType, throwStrength, s, v)) {
				g_localGhost.start = s;
				g_localGhost.vel = v;
			}
		}

		if (g_localGhost.pendingThrow) {
			if (!held || curType != g_localGhost.type || curType == NadeType::Unknown)
				ActivateLocalGhost(local);
		}

		if (g_localGhost.cooking && !g_localGhost.pendingThrow && !g_localGhost.active) {
			if (curType != g_localGhost.type || curType == NadeType::Unknown) {
				g_localGhost.cooking = false;
				g_localGhost.pendingThrow = false;
				g_localGhost.type = NadeType::Unknown;
			}
		}

		g_localGhost.lastPin = pinPulled && held && curType != NadeType::Unknown;
	}

	if (!g_localGhost.active)
		return;

	const float age = NowSeconds() - g_localGhost.startTime;
	const float totalLife = g_localGhost.flightSec + g_localGhost.effectSec;
	if (age >= totalLife - 0.02f) {
		g_localGhost.active = false;
		return;
	}

	const bool inFlight = age < g_localGhost.flightSec;
	Vector_t pos{};
	if (inFlight) {
		const float u = (g_localGhost.flightSec > 0.01f)
			? std::clamp(age / g_localGhost.flightSec, 0.f, 1.f) : 1.f;
		const int n = static_cast<int>(g_localGhost.points.size());
		const float fi = u * static_cast<float>(n - 1);
		const int i0 = static_cast<int>(fi);
		const int i1 = (i0 + 1 < n) ? i0 + 1 : i0;
		const float frac = fi - static_cast<float>(i0);
		const Vector_t& a = g_localGhost.points[i0];
		const Vector_t& b = g_localGhost.points[i1];
		pos = Vector_t{
			a.x + (b.x - a.x) * frac,
			a.y + (b.y - a.y) * frac,
			a.z + (b.z - a.z) * frac
		};
	} else {
		pos = FiniteVec(g_localGhost.land) ? g_localGhost.land : g_localGhost.points.back();
	}

	const float left = totalLife - age;
	Path path{};
	path.local_preview = false;
	path.own_projectile = true;
	path.show_warn = Config::nade_warn;
	path.type = g_localGhost.type;
	path.origin = pos;
	path.land = FiniteVec(g_localGhost.land) ? g_localGhost.land : pos;
	path.radius = TypeRadius(g_localGhost.type);
	path.time_left = left;
	path.effect_active = !inFlight
		&& (g_localGhost.type == NadeType::Smoke
			|| g_localGhost.type == NadeType::Molly
			|| g_localGhost.type == NadeType::Decoy);
	if (inFlight && Config::nade_pred)
		path.points = g_localGhost.points;
	else
		path.points.push_back(pos);
	g_paths.push_back(std::move(path));
}

// Near-plane distance in clip space. Below this a vertex is at/behind the
// camera; segments crossing it are clipped instead of dropped so rings and
// paths stay continuous when the local player stands on/near an effect.
constexpr float kClipNear = 0.1f;

struct ClipVert {
	float numX = 0.f, numY = 0.f, w = 0.f;
	bool ok = false;
};

// Project a homogeneous clip vertex (assumed already w >= kClipNear) to screen.
ImVec2 ClipToScreen(const ClipVert& v, float cx, float cy) {
	const float invW = 1.f / v.w;
	return ImVec2(cx + v.numX * invW * cx, cy - v.numY * invW * cy);
}

// Draw one world-space segment with near-plane clipping. Returns nothing;
// silently skips fully-behind segments.
void DrawClippedSegment(ImDrawList* dl, const ClipVert& a, const ClipVert& b,
                        float cx, float cy, ImU32 col, float thick) {
	if (!a.ok || !b.ok)
		return;
	const bool aFront = a.w >= kClipNear;
	const bool bFront = b.w >= kClipNear;
	if (!aFront && !bFront)
		return;

	ClipVert ca = a, cb = b;
	if (aFront != bFront) {
		// Interpolate the crossing point to the near plane so the visible
		// portion of the segment still renders (no vanished arcs).
		const float t = (kClipNear - a.w) / (b.w - a.w);
		ClipVert mid;
		mid.numX = a.numX + (b.numX - a.numX) * t;
		mid.numY = a.numY + (b.numY - a.numY) * t;
		mid.w = kClipNear;
		mid.ok = true;
		if (aFront)
			cb = mid;
		else
			ca = mid;
	}

	const ImVec2 pa = ClipToScreen(ca, cx, cy);
	const ImVec2 pb = ClipToScreen(cb, cx, cy);
	dl->AddLine(pa, pb, col, thick);
}

void DrawCircle3D(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
                  float radius, ImU32 col, int segs = 32) {
	if (radius <= 1.f || !dl || !vm.viewMatrix)
		return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x <= 1.f || ds.y <= 1.f)
		return;
	const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;

	ClipVert prev{};
	for (int i = 0; i <= segs; ++i) {
		const float a = (static_cast<float>(i) / segs) * 6.2831853f;
		const Vector_t w{
			center.x + std::cos(a) * radius,
			center.y + std::sin(a) * radius,
			center.z + 2.f
		};
		ClipVert cur{};
		cur.ok = vm.WorldToClip(w, cur.numX, cur.numY, cur.w);
		if (i > 0)
			DrawClippedSegment(dl, prev, cur, cx, cy, col, 1.5f);
		prev = cur;
	}
}

// Live-effect rings (smoke/fire) and warn badges are drawn exclusively by
// Visuals::drawWorld (via MirrorNadePathsToWorld) to avoid stacked/overlapping
// draws. NadePred::Draw owns only the prediction path, land marker, and the
// in-flight predicted radius.

// CS2 HE: base ~99, radius 350, falloff 0.5 (Valve radius damage curve)
constexpr float kHeBaseDmg = 99.f;
constexpr float kHeRadius = 350.f;
constexpr float kHeFalloff = 0.5f;
// HE armor penetration (CSGO/CS2 style)
constexpr float kHeArmorRatio = 0.5f;
constexpr float kHeArmorBonus = 0.5f;

float HeDamageRaw(float dist) {
	if (dist >= kHeRadius || dist < 0.f)
		return 0.f;
	// flDmg * exp( -d^2 / (2 * r^2 * falloff) )
	return kHeBaseDmg * std::exp(-dist * dist / (2.f * kHeRadius * kHeRadius * kHeFalloff));
}

int ApplyArmorToDamage(float dmg, int armor) {
	if (dmg <= 0.f)
		return 0;
	if (armor <= 0)
		return static_cast<int>(std::ceil(dmg));

	float flDamage = dmg;
	float flNew = flDamage * kHeArmorRatio;
	float flArmor = (flDamage - flNew) * kHeArmorBonus;
	if (flArmor > static_cast<float>(armor)) {
		flArmor = static_cast<float>(armor) * (1.f / kHeArmorBonus);
		flNew = flDamage - flArmor;
	}
	if (flNew < 0.f)
		flNew = 0.f;
	return static_cast<int>(std::ceil(flNew));
}

struct DmgLabel {
	Vector_t world{};
	int dmg = 0;
	int hpLeft = 0;
	bool lethal = false;
	bool isLocal = false;
};

// HE blast blocked by solid world — require LOS from land to body samples
bool HeBlastClear(const Vector_t& land, const Vector_t& feet, float eyeZ, void* targetPawn) {
	if (!FiniteVec(land) || !FiniteVec(feet))
		return false;
	if (!Trace::Ready())
		return true; // no tracer: keep distance-only

	Vector_t src = land;
	src.z += 4.f; // sit above floor so startsolid doesn't false-block

	const float midZ = (eyeZ > 1.f) ? (eyeZ * 0.5f) : 36.f;
	const float headZ = (eyeZ > 1.f) ? eyeZ : 64.f;
	const Vector_t ends[3] = {
		Vector_t{ feet.x, feet.y, feet.z + 8.f },
		Vector_t{ feet.x, feet.y, feet.z + midZ },
		Vector_t{ feet.x, feet.y, feet.z + headZ }
	};

	// Any body sample with clear path / hit == pawn counts (CS blast samples torso)
	return Trace::IsAnyVisible(src, ends, 3, nullptr, targetPawn, Trace::kMaskVis);
}

void* ResolveCachedPawn(const CBaseHandle& h) {
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	auto* ent = I::GameEntity->Instance->Get(h.index());
	if (!Mem::ValidEntity(ent))
		return nullptr;
	return ent;
}

void CollectHeDamage(const Vector_t& land, std::vector<DmgLabel>& out) {
	out.clear();
	if (!FiniteVec(land))
		return;

	const int filterTeam = cached_local.team != 0 ? cached_local.team : cached_local.lastTeam;
	const bool wantTeamCheck = GameMode::WantTeamCheck(Config::teamCheck);

	// Enemies (and FFA everyone) from ESP cache
	for (const auto& p : cached_players) {
		if (p.health <= 0)
			continue;
		if (wantTeamCheck && filterTeam != 0 && p.team_num == filterTeam)
			continue;
		if (!FiniteVec(p.position))
			continue;

		const float eyeZ = FiniteVec(p.viewOffset) ? p.viewOffset.z : 64.f;
		const Vector_t mid{
			p.position.x,
			p.position.y,
			p.position.z + eyeZ * 0.5f
		};
		const float dist = (mid - land).Length();
		const float raw = HeDamageRaw(dist);
		if (raw < 1.f)
			continue;

		void* pawn = ResolveCachedPawn(p.handle);
		if (!HeBlastClear(land, p.position, eyeZ, pawn))
			continue; // wall / cover — no HE damage

		const int dmg = ApplyArmorToDamage(raw, p.armor);
		if (dmg <= 0)
			continue;

		DmgLabel lab{};
		lab.world = Vector_t{ p.position.x, p.position.y, p.position.z + 78.f };
		lab.dmg = dmg;
		lab.hpLeft = (p.health - dmg) > 0 ? (p.health - dmg) : 0;
		lab.lethal = dmg >= p.health;
		lab.isLocal = false;
		out.push_back(lab);
	}

	// Self damage (enemy HE or bad self-nade)
	if (cached_local.active && cached_local.alive && cached_local.health > 0 && FiniteVec(cached_local.position)) {
		const Vector_t mid{
			cached_local.position.x,
			cached_local.position.y,
			cached_local.position.z + 36.f
		};
		const float dist = (mid - land).Length();
		const float raw = HeDamageRaw(dist);
		if (raw >= 1.f) {
			C_CSPlayerPawn* lp = nullptr;
			int armor = 0;
			if (H::oGetLocalPlayer) {
				lp = H::oGetLocalPlayer(0);
				if (lp && Mem::ValidEntity(lp))
					armor = lp->m_ArmorValue();
			}
			if (HeBlastClear(land, cached_local.position, 64.f, lp)) {
				const int dmg = ApplyArmorToDamage(raw, armor);
				if (dmg > 0) {
					DmgLabel lab{};
					lab.world = Vector_t{
						cached_local.position.x,
						cached_local.position.y,
						cached_local.position.z + 78.f
					};
					lab.dmg = dmg;
					lab.hpLeft = (cached_local.health - dmg) > 0 ? (cached_local.health - dmg) : 0;
					lab.lethal = dmg >= cached_local.health;
					lab.isLocal = true;
					out.push_back(lab);
				}
			}
		}
	}
}

void DrawDamageChip(ImDrawList* dl, float x, float y, int dmg, bool lethal, bool isLocal) {
	char buf[32];
	if (lethal)
		std::snprintf(buf, sizeof(buf), isLocal ? "YOU -%d KILL" : "-%d KILL", dmg);
	else
		std::snprintf(buf, sizeof(buf), isLocal ? "YOU -%d" : "-%d", dmg);

	const ImVec2 ts = ImGui::CalcTextSize(buf);
	const float padX = 6.f, padY = 3.f;
	const ImVec2 min{ x - ts.x * 0.5f - padX, y - padY };
	const ImVec2 max{ x + ts.x * 0.5f + padX, y + ts.y + padY };

	const ImU32 fill = lethal
		? ImGui::ColorConvertFloat4ToU32(Config::nade_pred_damage_lethal_color)
		: ImGui::ColorConvertFloat4ToU32(Config::nade_pred_damage_color);
	const ImVec4 fc = ImGui::ColorConvertU32ToFloat4(fill);

	// Soft glow
	dl->AddRectFilled(ImVec2(min.x - 2.f, min.y - 2.f), ImVec2(max.x + 2.f, max.y + 2.f),
		IM_COL32((int)(fc.x * 255), (int)(fc.y * 255), (int)(fc.z * 255), 40), 8.f);
	// Body
	dl->AddRectFilled(min, max, IM_COL32(10, 10, 14, 210), 6.f);
	dl->AddRect(min, max, fill, 6.f, 0, 1.6f);
	// Accent bar
	dl->AddRectFilled(ImVec2(min.x, min.y), ImVec2(min.x + 3.f, max.y), fill, 6.f);

	const float tx = x - ts.x * 0.5f;
	dl->AddText(ImVec2(tx + 1.f, y + 1.f), IM_COL32(0, 0, 0, 200), buf);
	dl->AddText(ImVec2(tx, y), IM_COL32(255, 255, 255, 255), buf);
}

void DrawDamageIndicators(const ViewMatrix& vm) {
	if (!Config::nade_pred || !Config::nade_pred_damage)
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	std::vector<DmgLabel> hits;
	hits.reserve(16);

	for (const auto& path : g_paths) {
		if (path.type != NadeType::HE)
			continue;
		// Land from sim, or settled projectile origin (1-point paths still count)
		const Vector_t land = FiniteVec(path.land)
			? path.land
			: (FiniteVec(path.origin) ? path.origin
				: (!path.points.empty() ? path.points.back() : Vector_t{}));
		if (!FiniteVec(land))
			continue;

		CollectHeDamage(land, hits);
		if (hits.empty())
			continue;

		// Sort highest damage first
		std::sort(hits.begin(), hits.end(), [](const DmgLabel& a, const DmgLabel& b) {
			return a.dmg > b.dmg;
		});

		// Per-player floating labels
		for (const auto& h : hits) {
			Vector_t s{};
			if (!vm.WorldToScreen(h.world, s))
				continue;
			DrawDamageChip(dl, s.x, s.y - 12.f, h.dmg, h.lethal, h.isLocal);
		}

		// Summary at land: "HE 47 / 98" max hit
		Vector_t landS{};
		if (vm.WorldToScreen(land, landS)) {
			int maxDmg = hits.front().dmg;
			int kills = 0;
			for (const auto& h : hits)
				if (h.lethal) ++kills;

			char sum[48];
			if (kills > 0)
				std::snprintf(sum, sizeof(sum), "HE max %d  ·  %d KILL", maxDmg, kills);
			else
				std::snprintf(sum, sizeof(sum), "HE max %d dmg", maxDmg);

			const ImVec2 ts = ImGui::CalcTextSize(sum);
			const float x = landS.x;
			const float y = landS.y + 16.f;
			const ImVec2 min{ x - ts.x * 0.5f - 7.f, y - 2.f };
			const ImVec2 max{ x + ts.x * 0.5f + 7.f, y + ts.y + 2.f };
			const ImU32 accent = ImGui::ColorConvertFloat4ToU32(Config::nade_pred_damage_color);
			dl->AddRectFilled(min, max, IM_COL32(8, 8, 12, 200), 6.f);
			dl->AddRect(min, max, accent, 6.f, 0, 1.3f);
			dl->AddText(ImVec2(x - ts.x * 0.5f + 1.f, y + 1.f), IM_COL32(0, 0, 0, 200), sum);
			dl->AddText(ImVec2(x - ts.x * 0.5f, y), IM_COL32(255, 240, 200, 255), sum);
		}
	}
}

} // namespace

// Do NOT gate on EngineClient::valid() — CS2 vfunc indices for
// connected/in_game are fragile and were wiping nades every other round.
bool EntitySystemReady() {
	return I::GameEntity && I::GameEntity->Instance
		&& Mem::Valid(I::GameEntity->Instance, 0x2100);
}

void Update() {
	g_paths.clear();
	if (!Config::nade_pred && !Config::nade_warn) {
		ResetFxTracks();
		return;
	}
	if (!EntitySystemReady()) {
		ResetFxTracks();
		return;
	}

	g_paths.reserve(16);
	if (Config::nade_pred)
		CollectLocalPreview();
	if (Config::nade_pred || Config::nade_warn)
		CollectProjectiles();
	// Throw-only local ghost (path + warn + smoke/molly timer)
	if (Config::nade_warn || Config::nade_pred)
		CollectLocalThrowGhost();
}

void Draw(const ViewMatrix& vm) {
	// Draw if master pred on, or warn alone with collected paths
	if ((!Config::nade_pred && !Config::nade_warn) || g_paths.empty())
		return;
	if (!vm.viewMatrix)
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const float thick = std::clamp(Config::nade_pred_thickness, 1.f, 5.f);
	const float t = static_cast<float>(ImGui::GetTime());
	const float pulse = 0.5f + 0.5f * std::sin(t * 4.2f);

	// Path lines when nade_pred master on
	const bool drawPaths = Config::nade_pred;

	for (const auto& path : g_paths) {
		if (!drawPaths)
			break;
		// Local preview + own throws always draw; others need projectiles toggle
		if (!Config::nade_pred_projectiles && !path.local_preview && !path.own_projectile)
			continue;
		if (path.points.size() < 2)
			continue;

		const bool isLocal = path.local_preview || path.own_projectile;
		const ImVec4 col4 = TypeColor(path.type, isLocal);
		const ImU32 outline = IM_COL32(0, 0, 0, 170);
		const int nPts = static_cast<int>(path.points.size());
		const float flightEst = static_cast<float>(nPts) * static_cast<float>(kTicksPerPoint) * kSimDt;

		// Gradient arc: bright near nade → soft toward land.
		// Near-plane clip each segment so the trajectory line doesn't vanish
		// where it dips behind the camera (e.g. cooking a nade in first person).
		const ImVec2 ds = ImGui::GetIO().DisplaySize;
		if (ds.x <= 1.f || ds.y <= 1.f)
			continue;
		const float scx = ds.x * 0.5f, scy = ds.y * 0.5f;

		ClipVert prevC{};
		int drawn = 0;
		for (int i = 0; i < nPts; ++i) {
			ClipVert cur{};
			cur.ok = vm.WorldToClip(path.points[i], cur.numX, cur.numY, cur.w);
			if (i > 0 && prevC.ok && cur.ok && (prevC.w >= kClipNear || cur.w >= kClipNear)) {
				const float u = (nPts > 1) ? (static_cast<float>(i) / static_cast<float>(nPts - 1)) : 0.f;
				const float aMul = 0.92f - u * 0.40f;
				const int a = static_cast<int>(std::clamp(col4.w * aMul * 255.f, 50.f, 255.f));
				const ImU32 seg = IM_COL32(
					(int)(col4.x * 255.f), (int)(col4.y * 255.f), (int)(col4.z * 255.f), a);
				const float w = thick + (1.f - u) * 0.5f;
				DrawClippedSegment(dl, prevC, cur, scx, scy, outline, w + 1.35f);
				DrawClippedSegment(dl, prevC, cur, scx, scy, seg, w);
				++drawn;
			}
			prevC = cur;
		}
		if (drawn <= 0)
			continue;

		// Bounce ticks — small diamonds
		for (const auto& h : path.hits) {
			Vector_t hs{};
			if (!vm.WorldToScreen(h, hs))
				continue;
			const ImU32 bc = IM_COL32((int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255), 230);
			const ImVec2 c(hs.x, hs.y);
			dl->AddQuadFilled(
				ImVec2(c.x, c.y - 3.2f), ImVec2(c.x + 3.2f, c.y),
				ImVec2(c.x, c.y + 3.2f), ImVec2(c.x - 3.2f, c.y), outline);
			dl->AddQuadFilled(
				ImVec2(c.x, c.y - 2.2f), ImVec2(c.x + 2.2f, c.y),
				ImVec2(c.x, c.y + 2.2f), ImVec2(c.x - 2.2f, c.y), bc);
		}

		Vector_t landS{};
		if (vm.WorldToScreen(path.land, landS)) {
			const ImU32 landCol = ImGui::ColorConvertFloat4ToU32(Config::nade_pred_land_color);
			const ImU32 typeCol = ImGui::ColorConvertFloat4ToU32(col4);
			const float pr = 5.5f + pulse * 1.2f;
			const ImVec2 c(landS.x, landS.y);
			// Soft ring
			dl->AddCircle(c, pr + 4.f,
				IM_COL32((int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255),
					static_cast<int>(40 + pulse * 30)), 20, 1.6f);
			// Diamond land marker
			dl->AddQuadFilled(
				ImVec2(c.x, c.y - 6.f), ImVec2(c.x + 6.f, c.y),
				ImVec2(c.x, c.y + 6.f), ImVec2(c.x - 6.f, c.y), outline);
			dl->AddQuadFilled(
				ImVec2(c.x, c.y - 4.5f), ImVec2(c.x + 4.5f, c.y),
				ImVec2(c.x, c.y + 4.5f), ImVec2(c.x - 4.5f, c.y), landCol);
			dl->AddCircleFilled(c, 1.6f, IM_COL32(255, 255, 255, 220), 8);

			const char* tag = nullptr;
			if (path.type == NadeType::HE)
				tag = path.detonated ? "BOOM" : "LAND";
			else if (path.type == NadeType::Flash)
				tag = path.detonated ? "POP" : "LAND";
			else if (path.type == NadeType::Molly)
				tag = path.detonated ? "FIRE" : "LAND";
			else if (isLocal)
				tag = "LAND";

			char label[24]{};
			if (tag && flightEst >= 0.15f)
				std::snprintf(label, sizeof(label), "%s  %.1fs", tag, flightEst);
			else if (tag)
				std::snprintf(label, sizeof(label), "%s", tag);

			if (label[0]) {
				const ImVec2 ts = ImGui::CalcTextSize(label);
				const float lx = c.x - ts.x * 0.5f;
				const float ly = c.y + 10.f;
				dl->AddRectFilled(ImVec2(lx - 4.f, ly - 1.f), ImVec2(lx + ts.x + 4.f, ly + ts.y + 1.f),
					IM_COL32(6, 8, 12, 195), 4.f);
				dl->AddText(ImVec2(lx + 1.f, ly + 1.f), outline, label);
				dl->AddText(ImVec2(lx, ly), typeCol, label);
			}
		}

		// Predicted radius ring: skip flash, keep HE/smoke/molly.
		// Only for the in-flight *prediction* — once the effect is live the ring
		// is owned by Visuals::drawWorld, so skip effect_active here to avoid a
		// second overlapping ring at the same land position.
		if (Config::nade_pred_radius && path.radius > 1.f
			&& path.type != NadeType::Flash && !path.effect_active) {
			const ImU32 colDim = IM_COL32(
				(int)(col4.x * 220.f), (int)(col4.y * 220.f), (int)(col4.z * 220.f),
				static_cast<int>(75 + pulse * 35));
			if (path.type == NadeType::Smoke) {
				DrawCircle3D(dl, vm, path.land, kRadiusSmoke, colDim, 48);
				const ImU32 colInner = IM_COL32(
					(int)(col4.x * 255.f), (int)(col4.y * 255.f), (int)(col4.z * 255.f),
					static_cast<int>(55 + pulse * 30));
				DrawCircle3D(dl, vm, path.land, kSmokeRingRadius, colInner, 36);
			} else {
				DrawCircle3D(dl, vm, path.land, path.radius, colDim, 40);
			}
		}
	}

	// Live-effect rings + warn badges owned by Visuals::drawWorld.
	// HE damage on enemies / self at predicted land
	DrawDamageIndicators(vm);
}

const std::vector<Path>& Paths() {
	return g_paths;
}

// Debounce: round_start + round_officially_started + nMax heuristic can fire back-to-back
static DWORD s_lastRoundResetTick = 0;

void RoundBoundaryReset(const char* reason, bool force) {
	const DWORD now = GetTickCount();
	if (!force && s_lastRoundResetTick && (now - s_lastRoundResetTick) < 400u)
		return;
	s_lastRoundResetTick = now;
	g_paths.clear();
	++g_roundEpoch;
	ResetFxTracks();
	g_localGhost = {};
	s_collectSeenNames.clear();
	s_collectPrevMax = 0;
	s_collectPeak = 0;
	s_collectLowFrames = 0;
	s_collectEmptyFrames = 0;
	Esp::ResetWorldFxTimers();
}

void Reset() {
	// LevelInit / explicit wipe — always run (ignore debounce)
	RoundBoundaryReset("Reset", true);
}

// IDA: IGameEvent::GetName = vtable[1] (*(_QWORD*)evt + 8)
// (CBufferString getter on the event object is wrong and silently misses round_start)
static const char* EventGetName(void* gameEvent) {
	if (!gameEvent || !Mem::Valid(gameEvent, sizeof(void*)))
		return nullptr;
	void** vt = nullptr;
	if (!Mem::ReadField(gameEvent, 0, vt) || !vt || !Mem::Valid(vt, 16))
		return nullptr;
	using FnGetName = const char* (__fastcall*)(void*);
	FnGetName fn = nullptr;
	if (!Mem::ReadField(vt, 8, fn) || !fn)
		return nullptr;
	const char* name = nullptr;
	__try { name = fn(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!name || !Mem::IsReadable(name, 1) || !name[0])
		return nullptr;
	return name;
}

void OnGameEvent(void* gameEvent) {
	if (!gameEvent)
		return;
	// Caller (hkFireEventClientSide) wraps us in SEH
	const char* name = EventGetName(gameEvent);
	if (!name)
		return;
	// Wipe timers on round boundaries — LevelInit does not fire each round
	// Do NOT wipe on round_freeze_end (live round, nades/bomb already active)
	if (std::strcmp(name, "round_start") == 0
		|| std::strcmp(name, "round_officially_started") == 0
		|| std::strcmp(name, "round_end") == 0
		|| std::strcmp(name, "game_newmap") == 0
		|| std::strcmp(name, "cs_match_end_restart") == 0
		|| std::strcmp(name, "announce_phase_end") == 0
		|| std::strcmp(name, "begin_new_match") == 0
		|| std::strcmp(name, "round_prestart") == 0) {
		RoundBoundaryReset(name, false);
	}
}

} // namespace NadePred
