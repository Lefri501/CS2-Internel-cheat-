#pragma once
// Priority A dump patterns — resolve + small APIs for features that still need them.
// Progress tracked in: cs2 sdk dump all you need/Patterns/lefrizzel_NEED_LIST.md

#include "../../utils/math/vector/vector.h"
#include <cstdint>

class C_CSWeaponBase;
class C_CSPlayerPawn;
class C_BaseEntity;

namespace SdkPrioA {

enum class Status : std::uint8_t {
	NotStarted = 0,
	Resolved,   // pattern found, API available
	Hooked,     // resolved + detour installed
	AlreadyHad, // was already implemented elsewhere
	Blocked,    // dump entry unusable / misnamed / duplicate
	Failed,     // scan miss
};

struct Entry {
	const char* name = "";
	Status status = Status::NotStarted;
	void* addr = nullptr;
	const char* note = "";
};

// Call once after modules + pattern scan ready (from Interfaces::init).
bool Init();
bool Ready();

// ---- status table (for logs / next AI) ----
const Entry* Entries(int& countOut);
const char* StatusName(Status s);

// Generation bumped on OnAdd/OnRemove/LevelShutdown — bones/ESP can invalidate caches.
std::uint32_t EntityGen();
std::uint32_t MapGen();

// Safety: Overwatch / demo / HLTV — aim/trigger/autofire should soft-disable.
bool IsOverwatch();
bool IsDemoOrHltv();
bool ShouldSoftDisableCombat(); // true if either

// Game rules global (may be null).
void* GameRules();

// Weapon VData via C_CSWeaponBase::GetEconWpnData (function path).
void* GetEconWpnData(C_CSWeaponBase* weapon);

// Econ item view helpers (may be null if not resolved).
void* EconItemView_GetStaticData(void* econItemView);
void* EconItemView_GetBasePlayerWeaponVData(void* econItemView);

// Engine abs origin (fn). Returns false → caller uses schema fallback.
bool GetAbsOrigin(void* entity, Vector_t& out);

// Surrounding AABB from engine ComputeHitboxSurroundingBox.
bool ComputeHitboxSurroundingBox(void* entity, Vector_t& minsOut, Vector_t& maxsOut);

// Hitgroup helper when trace hitbox object is valid (dump GetHitGroup).
int GetHitGroupFromHitbox(void* hitboxObj);

// Movement filter / bbox (resolved for later use; optional).
void* InitPlayerMovementTraceFilterFn();
void* TracePlayerBBoxFn();

// Glove apply (optional call after skin glove path).
using GloveApplyFn = void(__fastcall*)(void* /*ctx*/);
GloveApplyFn GloveApplyPerTick();

// Addresses for Hooks::init (null if scan miss).
void* OnAddAddr();
void* OnRemoveAddr();
void* LevelShutdownAddr();
void MarkHooked(const char* name, const char* note = nullptr);

// Hook bodies (H:: wrappers call these).
void OnLevelShutdown();
void OnEntityAdded(void* entitySystem, void* entity, int handle);
void OnEntityRemoved(void* entitySystem, void* entity, int handle);

// Optional: call InstallHooks after H:: hooks are placed (currently no-op).
void InstallHooks();

} // namespace SdkPrioA
