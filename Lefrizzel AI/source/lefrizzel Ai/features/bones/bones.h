#pragma once

#include <cstddef>
#include <cstdint>
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../utils/math/vector/vector.h"
#include "../../config/config.h"

// CS2 CBoneData: pos(12) + scale(4) + quat(16) = 32
// IDA (client.dll): CalcBones @ 0x180A49A00, GetTransformsForHitboxList @ 0x180A59120
//   skel+0x1C0 bone array, +0x1D4 count, +0x458 done-mask (early-out)
// Combat: ForceUpdate local only — never clear enemy mask (online desync).
// Hitboxes: studio set via GetHitBoxSet; map studio→HB by bone name (+ hardcoded fallback).
namespace Bones {

constexpr size_t STRIDE = 32;
constexpr size_t MODELSTATE_FB = 0x140;
constexpr size_t BONE_PTR_OFF = 0x80;   // modelState → bone array
constexpr size_t BONE_COUNT_OFF = 0x94; // modelState → bone count
constexpr size_t SKEL_BONE_PTR = 0x1C0; // skeleton → bone array
constexpr size_t SKEL_BONE_CNT = 0x1D4; // skeleton → bone count

// Named joints used by aim + skeleton ESP
enum Slot : int {
	S_PELVIS = 0,
	S_SPINE0,
	S_SPINE1,
	S_SPINE2,
	S_SPINE3,
	S_NECK,
	S_HEAD,
	S_CLAV_L,
	S_ARM_U_L,
	S_ARM_L_L,
	S_HAND_L,
	S_CLAV_R,
	S_ARM_U_R,
	S_ARM_L_R,
	S_HAND_R,
	S_LEG_U_L,
	S_LEG_L_L,
	S_ANKLE_L,
	S_LEG_U_R,
	S_LEG_L_R,
	S_ANKLE_R,
	S_COUNT
};

struct BonePair {
	int from; // Slot
	int to;   // Slot
};

// Skeleton connections (slots). Clavicles when resolved, else neck→arm.
constexpr BonePair kSkeleton[] = {
	{ S_PELVIS, S_SPINE0 }, { S_SPINE0, S_SPINE1 }, { S_SPINE1, S_SPINE2 },
	{ S_SPINE2, S_SPINE3 }, { S_SPINE3, S_NECK },   { S_NECK, S_HEAD },
	{ S_NECK, S_CLAV_L }, { S_CLAV_L, S_ARM_U_L }, { S_ARM_U_L, S_ARM_L_L }, { S_ARM_L_L, S_HAND_L },
	{ S_NECK, S_CLAV_R }, { S_CLAV_R, S_ARM_U_R }, { S_ARM_U_R, S_ARM_L_R }, { S_ARM_L_R, S_HAND_R },
	{ S_PELVIS, S_LEG_U_L }, { S_LEG_U_L, S_LEG_L_L }, { S_LEG_L_L, S_ANKLE_L },
	{ S_PELVIS, S_LEG_U_R }, { S_LEG_U_R, S_LEG_L_R }, { S_LEG_L_R, S_ANKLE_R },
};
constexpr int kSkeletonCount = sizeof(kSkeleton) / sizeof(kSkeleton[0]);

// Per-pawn resolved bone array indices (-1 = missing)
struct Map {
	int idx[S_COUNT];
	int boneCount;
	bool ok;
};

struct EngineAddrs {
	void* lookupBone = nullptr;           // GETBONEPOSITIONBYNAME
	void* getBoneIdByName = nullptr;      // C_BASEENTITY_GETBONEIDBYNAME (E8 rip)
	void* getHitboxSet = nullptr;         // C_BASEENTITY_GETHITBOXSET
	void* calcWorldSpaceBones = nullptr;  // CALCULATEWORLDSPACEBONES
	void* getTransformsForHitboxList = nullptr;
	bool resolved = false;
};

bool Init();
const EngineAddrs& Engines();

// CreateMove generation — invalidates per-pawn hitbox/capsule micro-cache.
void BeginFrame(std::uint32_t generation);

// Force CSkeletonInstance::CalculateWorldSpaceBones (flags 0x100 = hitbox-ready).
// IDA GetTransformsForHitboxList @ 0x180A59120: CalcBones(skel, 0x100) then bone ptr @ skel+0x1C0.
// IDA CalcBones early-out: if (skel+0x458 & flags) == flags return. Clear mask first.
bool ForceUpdateBones(const C_CSPlayerPawn* pawn, unsigned int flags = 0x100u);

bool IsValidPos(const Vector_t& p);
bool GetOrigin(const C_CSPlayerPawn* pawn, Vector_t& out);
// Local: NetClientInfo LocalData.ShootPosition (engine fire origin).
// Else: m_vecAbsOrigin + m_vecViewOffset (never m_vOldOrigin).
Vector_t GetEyePos(const C_CSPlayerPawn* pawn);
// Alias - same as GetEyePos (preferred name for aim/trigger traces).
Vector_t GetShootPos(const C_CSPlayerPawn* pawn);

void* GetSkeleton(const C_CSPlayerPawn* pawn);
// Combat read: ForceUpdate local only; enemies = engine pose (no skel mask clear).
// Old always-force on enemies → flicker + aim miss under trigger/AF.
bool GetBoneArray(const C_CSPlayerPawn* pawn, uintptr_t& boneArray, Vector_t& origin, float& height);
// Read-only: no CalcBones. ESP / Present / glow / skeleton draw - avoids pose flicker.
// outBoneCount: live skeleton count when readable (0 if unknown).
bool GetBoneArrayReadonly(const C_CSPlayerPawn* pawn, uintptr_t& boneArray,
	Vector_t& origin, float& height, int* outBoneCount = nullptr);
bool GetBonePos(uintptr_t boneArray, int boneIndex, Vector_t& out);

// Slot lookup — map.idx[slot] to a bone index in boneArray. Returns false if
// slot is out of range, unresolved (idx<0), or the bone position is invalid.
bool GetSlotPos(uintptr_t boneArray, const Map& map, int slot, Vector_t& out);

// Resolve name→index (LookupBone) + geometry fill for missing limbs
bool ResolveMap(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	const Vector_t& origin, float height, Map& out);

// Cached resolve (per pawn, invalidated on bone-array change)
bool ResolveMapCached(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	const Vector_t& origin, float height, Map& out);

// Fill all resolved slot positions; returns count written (<= S_COUNT).
// forceUpdate=false for Present/ESP/hitmarker; true for CreateMove/backtrack.
int CollectSkeletonPoints(const C_CSPlayerPawn* pawn, Vector_t* outSlots, bool* outValid,
		bool forceUpdate = true);

// Read single slot position from a resolved Map. Preferred over GetBonePos when
// the caller already has the ResolveMap output; handles geometry-fill fallbacks.
bool GetSlotPos(uintptr_t boneArray, const Map& map, int slot, Vector_t& out);

int CollectHitboxPoints(const C_CSPlayerPawn* pawn, int hb, Vector_t* out, int maxOut);
bool GetHitboxPoint(const C_CSPlayerPawn* pawn, int hb, Vector_t& out);

// Live studio capsule from C_BaseEntity::GetHitboxSet (stride 0x70).
// IDA ComputeHitboxSurroundingBox: set+0x10 count, set+0x18 array.
struct Capsule {
	Vector_t mins{};
	Vector_t maxs{};
	Vector_t center{};
	float radius = 0.f;
	int studioIndex = -1;
	bool ok = false;
};

bool GetHitboxCapsule(const C_CSPlayerPawn* pawn, int hb, Capsule& out);
bool GetStudioCapsule(const C_CSPlayerPawn* pawn, int studioIndex, Capsule& out);

// Priority A: engine C_BaseEntity::ComputeHitboxSurroundingBox (when resolved).
bool ComputeSurroundingBox(const C_CSPlayerPawn* pawn, Vector_t& mins, Vector_t& maxs);

// Map engine studio index / hitgroup → Config::HB_* (-1 = unknown)
int StudioIndexToHitbox(int studioIndex);
int HitgroupToHitbox(int hitgroup);

// Ray (eye + t*dir, t≥0, dir unit) vs live capsule. radiusScale multiplies shape radius.
// centerBias (0..1): require closest approach within radius*scale*centerBias (1=full capsule).
bool RayHitsCapsule(
	const Vector_t& eye,
	const Vector_t& dir,
	const Capsule& cap,
	float radiusScale,
	float& outT,
	Vector_t& outPoint,
	float centerBias = 1.f);

// Test all studio capsules for a Config::HB_* group; returns nearest hit along ray.
bool RayHitsConfiguredHitbox(
	const C_CSPlayerPawn* pawn,
	int hb,
	const Vector_t& eye,
	const Vector_t& dir,
	float radiusScale,
	float& outT,
	Vector_t& outPoint,
	float centerBias = 1.f);

// Fallback constants when hitbox set is unavailable.
float MultipointRadius(int hb);
float TriggerHitboxRadius(int hb);

// Live m_flShapeRadius (pawn + Config::HB_*). Falls back to constants.
float MultipointRadius(const C_CSPlayerPawn* pawn, int hb);
float TriggerHitboxRadius(const C_CSPlayerPawn* pawn, int hb);

// Capsule multipoint: center + edge points on live studio radius/axis.
//   scale 0 = center only, 1 = full radius.
//   shootPos + bloom (>=0) → dynamic scale: shrink by distance*bloom.
//   targetAirborne → clamp scale ~0.7 (in-air fix).
int CollectHitboxMultipoints(
	const C_CSPlayerPawn* pawn,
	int hb,
	float scale,
	Vector_t* out,
	int maxOut,
	const Vector_t* shootPos = nullptr,
	float bloom = -1.f,
	bool targetAirborne = false);

} // namespace Bones
