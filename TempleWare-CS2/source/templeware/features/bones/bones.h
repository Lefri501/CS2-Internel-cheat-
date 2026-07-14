#pragma once

#include <cstddef>
#include <cstdint>
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../utils/math/vector/vector.h"
#include "../../config/config.h"

// CS2 CBoneData: pos(12) + scale(4) + quat(16) = 32
// IDA a2bd4708 CSkeletonInstance_CalculateWorldSpaceBones @ 0x180A495B0:
//   +0x140 CModelState (schema)
//   +0x1C0 bone array ptr  (= modelState + 0x80)
//   +0x1D4 bone count      (= modelState + 0x94)
//   +0x1E0 m_hModel        (= modelState + 0xA0)
// Patterns (cs2_patterns_raw.txt):
//   GETBONEPOSITIONBYNAME / LOOKUPBONE
//   C_BASEENTITY_GETHITBOXSET / GETBONEIDBYNAME
//   CALCULATEWORLDSPACEBONES / GETTRANSFORMSFORHITBOXLIST
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

// Force CSkeletonInstance::CalculateWorldSpaceBones (flags 0x100 = hitbox-ready)
bool ForceUpdateBones(const C_CSPlayerPawn* pawn, unsigned int flags = 0x100u);

bool IsValidPos(const Vector_t& p);
bool GetOrigin(const C_CSPlayerPawn* pawn, Vector_t& out);
// Local: NetClientInfo LocalData.ShootPosition (engine fire origin).
// Else: m_vecAbsOrigin + m_vecViewOffset (never m_vOldOrigin).
Vector_t GetEyePos(const C_CSPlayerPawn* pawn);
// Alias — same as GetEyePos (preferred name for aim/trigger traces).
Vector_t GetShootPos(const C_CSPlayerPawn* pawn);

void* GetSkeleton(const C_CSPlayerPawn* pawn);
bool GetBoneArray(const C_CSPlayerPawn* pawn, uintptr_t& boneArray, Vector_t& origin, float& height);
bool GetBonePos(uintptr_t boneArray, int boneIndex, Vector_t& out);

// Resolve name→index (LookupBone) + geometry fill for missing limbs
bool ResolveMap(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	const Vector_t& origin, float height, Map& out);

// Cached resolve (per pawn, invalidated on bone-array change)
bool ResolveMapCached(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	const Vector_t& origin, float height, Map& out);

bool GetSlotPos(uintptr_t boneArray, const Map& map, int slot, Vector_t& out);

// Fill all resolved slot positions; returns count written (≤ S_COUNT)
int CollectSkeletonPoints(const C_CSPlayerPawn* pawn, Vector_t* outSlots, bool* outValid);

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
