#include "bones.h"

#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../interfaces/interfaces.h"
#include "../../hooks/hooks.h"
#include "../sdk_prio_a/sdk_prio_a.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <Windows.h>

namespace Bones {
namespace {

// Exact dump patterns (patterns.hpp) — IDA d31a587d unique hits:
//   LookupBone / GetBonePositionByName → 0x18090F4D0
//   C_BaseEntity::GetHitBoxSet         → 0x1808FD710  (wildcards matched 2 — use exact)
//   CalculateWorldSpaceBones           → 0x180A49A00
//   GetTransformsForHitboxList         → 0x180A59120 (calls CalcBones 0x100)
using LookupBone_t = int(__fastcall*)(void* entity, const char* name);
using CalcWorldBones_t = void(__fastcall*)(void* skeleton, unsigned int flags);
using GetHitboxSet_t = void*(__fastcall*)(void* entity, unsigned int set);
// skeleton, outTransforms (CUtlVector-like), hitboxList { count, entries* }
using GetTransforms_t = char(__fastcall*)(void* skeleton, void* outTransforms, void* hitboxList);

EngineAddrs g_eng{};
LookupBone_t g_Lookup = nullptr;
CalcWorldBones_t g_CalcBones = nullptr;
GetHitboxSet_t g_GetHitboxSet = nullptr;
GetTransforms_t g_GetTransforms = nullptr;

struct CacheEntry {
	const void* pawn = nullptr;
	uintptr_t boneArray = 0;
	Map map{};
};
constexpr int kCacheSlots = 48;
CacheEntry g_cache[kCacheSlots]{};
int g_cacheWrite = 0;

// Per-pawn frame: hitbox set + last capsules by Config::HB_*
struct FrameCapsuleCache {
	const void* pawn = nullptr;
	std::uint32_t gen = 0;
	uintptr_t hbArr = 0;
	int hbCount = 0;
	bool setOk = false;
	// studio index → Config::HB_* (-1 = unused). Built from bone names + hardcoded fallback.
	std::int8_t studioToHb[64]{};
	bool studioMapOk = false;
	Capsule byHb[Config::HB_COUNT]{};
	bool haveHb[Config::HB_COUNT]{};
};
constexpr int kFrameSlots = 24;
FrameCapsuleCache g_frameCache[kFrameSlots]{};
int g_frameWrite = 0;
std::uint32_t g_frameGen = 0;

FrameCapsuleCache* FindFrameCache(const void* pawn) {
	if (!pawn)
		return nullptr;
	for (int i = 0; i < kFrameSlots; ++i) {
		if (g_frameCache[i].pawn == pawn && g_frameCache[i].gen == g_frameGen)
			return &g_frameCache[i];
	}
	return nullptr;
}

FrameCapsuleCache& AcquireFrameCache(const void* pawn) {
	if (auto* hit = FindFrameCache(pawn))
		return *hit;
	FrameCapsuleCache& e = g_frameCache[g_frameWrite % kFrameSlots];
	g_frameWrite++;
	e = {};
	e.pawn = pawn;
	e.gen = g_frameGen;
	return e;
}

void CachePut(const void* pawn, uintptr_t boneArray, const Map& map) {
	CacheEntry& e = g_cache[g_cacheWrite % kCacheSlots];
	g_cacheWrite++;
	e.pawn = pawn;
	e.boneArray = boneArray;
	e.map = map;
}

bool CacheGet(const void* pawn, uintptr_t boneArray, Map& out) {
	for (int i = 0; i < kCacheSlots; ++i) {
		const CacheEntry& e = g_cache[i];
		if (e.pawn == pawn && e.boneArray == boneArray && e.map.ok) {
			out = e.map;
			return true;
		}
	}
	return false;
}

int CallLookup(void* entity, const char* name) {
	if (!entity || !name || !name[0] || !g_Lookup)
		return -1;
	int idx = -1;
	__try {
		idx = g_Lookup(entity, name);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		idx = -1;
	}
	if (idx < 0 || idx > 256)
		return -1;
	return idx;
}

// Try several name casings used across CS2 player / hostage / bip models
int LookupNames(void* entity, const char* const* names, int count) {
	for (int i = 0; i < count; ++i) {
		if (!names[i] || !names[i][0])
			continue;
		const int idx = CallLookup(entity, names[i]);
		if (idx >= 0)
			return idx;
	}
	return -1;
}

int LookupName(void* entity, const char* a, const char* b = nullptr,
	const char* c = nullptr, const char* d = nullptr, const char* e = nullptr)
{
	const char* names[] = { a, b, c, d, e };
	return LookupNames(entity, names, 5);
}

void HitboxSlots(int hb, const int*& slots, int& count) {
	static const int head[]    = { S_HEAD };
	static const int neck[]    = { S_NECK };
	static const int chest[]   = { S_SPINE3, S_SPINE2 };
	static const int stomach[] = { S_SPINE1, S_SPINE0 };
	static const int pelvis[]  = { S_PELVIS };
	static const int arms[]    = {
		S_CLAV_L, S_ARM_U_L, S_ARM_L_L, S_HAND_L,
		S_CLAV_R, S_ARM_U_R, S_ARM_L_R, S_HAND_R
	};
	static const int legs[]    = { S_LEG_U_L, S_LEG_L_L, S_LEG_U_R, S_LEG_L_R };
	static const int feet[]    = { S_ANKLE_L, S_ANKLE_R };

	switch (hb) {
	case Config::HB_HEAD:    slots = head;    count = 1; break;
	case Config::HB_NECK:    slots = neck;    count = 1; break;
	case Config::HB_CHEST:   slots = chest;   count = 2; break;
	case Config::HB_STOMACH: slots = stomach; count = 2; break;
	case Config::HB_PELVIS:  slots = pelvis;  count = 1; break;
	case Config::HB_ARMS:    slots = arms;    count = 8; break;
	case Config::HB_LEGS:    slots = legs;    count = 4; break;
	case Config::HB_FEET:    slots = feet;    count = 2; break;
	default:                 slots = head;    count = 1; break;
	}
}

void HitboxZBand(int hb, float& zMin, float& zMax) {
	switch (hb) {
	case Config::HB_HEAD:    zMin = 0.92f; zMax = 1.20f; break;
	case Config::HB_NECK:    zMin = 0.78f; zMax = 0.96f; break;
	case Config::HB_CHEST:   zMin = 0.55f; zMax = 0.88f; break;
	case Config::HB_STOMACH: zMin = 0.38f; zMax = 0.65f; break;
	case Config::HB_PELVIS:  zMin = 0.18f; zMax = 0.45f; break;
	case Config::HB_ARMS:    zMin = 0.40f; zMax = 0.95f; break;
	case Config::HB_LEGS:    zMin = 0.05f; zMax = 0.52f; break;
	case Config::HB_FEET:    zMin = 0.00f; zMax = 0.18f; break;
	default:                 zMin = 0.f;   zMax = 1.2f;  break;
	}
}

float HitboxFallbackFrac(int hb) {
	switch (hb) {
	case Config::HB_HEAD:    return 1.00f;
	case Config::HB_NECK:    return 0.90f;
	case Config::HB_CHEST:   return 0.72f;
	case Config::HB_STOMACH: return 0.52f;
	case Config::HB_PELVIS:  return 0.32f;
	case Config::HB_ARMS:    return 0.65f;
	case Config::HB_LEGS:    return 0.28f;
	case Config::HB_FEET:    return 0.05f;
	default:                 return 0.72f;
	}
}

bool InHitboxBand(const Vector_t& p, const Vector_t& origin, float height, int hb) {
	if (height < 1.f)
		return false;
	float zMin = 0.f, zMax = 1.f;
	HitboxZBand(hb, zMin, zMax);
	const float zf = (p.z - origin.z) / height;
	if (zf < zMin || zf > zMax)
		return false;

	const float horiz = std::sqrt(
		(p.x - origin.x) * (p.x - origin.x) +
		(p.y - origin.y) * (p.y - origin.y));
	const bool limb = (hb == Config::HB_ARMS || hb == Config::HB_LEGS || hb == Config::HB_FEET);
	if (horiz > (limb ? 48.f : 30.f))
		return false;

	const float dist = (p - origin).Length();
	return dist >= 0.5f && dist <= 130.f;
}

bool FallbackPoint(const C_CSPlayerPawn* pawn, int hb, Vector_t& out) {
	const Vector_t eye = GetEyePos(pawn);
	Vector_t origin{};
	if (!IsValidPos(eye) || !GetOrigin(pawn, origin))
		return false;
	const float h = eye.z - origin.z;
	if (h < 1.f)
		return false;

	// Head slightly above eye; body along eye→origin column (stable crouch/stand)
	if (hb == Config::HB_HEAD)
		out = { eye.x, eye.y, eye.z + 2.5f };
	else {
		const float frac = HitboxFallbackFrac(hb);
		out = { eye.x, eye.y, origin.z + h * frac };
	}
	return IsValidPos(out);
}

// Do NOT assume bone 0=pelvis / 6=head — agents/skins differ. Scan first N bones.
bool ValidateBoneArray(uintptr_t ptr, const Vector_t& origin, float height, int boneCount) {
	if (!ptr || ptr < 0x10000 || height < 1.f)
		return false;

	const int limit = (boneCount > 2)
		? (std::min)(boneCount, 96)
		: 32;
	if (limit < 4)
		return false;

	float bestPelvisD = 1e9f;
	float bestHeadZ = -1e9f;
	float bestPelvisZ = 0.f;
	int valid = 0;
	for (int i = 0; i < limit; ++i) {
		Vector_t p{};
		if (!GetBonePos(ptr, i, p))
			continue;
		++valid;
		const float d = (p - origin).Length();
		if (d < bestPelvisD && d < 90.f) {
			bestPelvisD = d;
			bestPelvisZ = p.z;
		}
		if (p.z > bestHeadZ)
			bestHeadZ = p.z;
	}
	if (valid < 4)
		return false;
	if (bestPelvisD > 90.f)
		return false;

	// Crouch / stand / jump: head band relative to eye-derived height
	const float dzHead = bestHeadZ - origin.z;
	if (dzHead < height * 0.50f || dzHead > height * 1.55f)
		return false;
	if (bestHeadZ < bestPelvisZ + height * 0.30f)
		return false;
	return true;
}

void GeomLimbChain(
	uintptr_t ba, const Vector_t& origin, float height,
	const Vector_t& right, bool leftSide,
	float zMin, float zMax, float minLat,
	int& outU, int& outL, int& outEnd)
{
	outU = outL = outEnd = -1;

	struct Cand { int i; float z; float lat; float dist; };
	Cand cands[64];
	int n = 0;

	for (int bi = 0; bi < 64 && n < 64; ++bi) {
		Vector_t p{};
		if (!GetBonePos(ba, bi, p))
			continue;
		const float zf = (p.z - origin.z) / height;
		if (zf < zMin || zf > zMax)
			continue;
		const Vector_t d = p - origin;
		const float lat = d.x * right.x + d.y * right.y;
		if (leftSide) {
			if (lat > -minLat)
				continue;
		} else {
			if (lat < minLat)
				continue;
		}
		const float horiz = std::sqrt(d.x * d.x + d.y * d.y);
		if (horiz < 2.f || horiz > 55.f)
			continue;
		if ((p - origin).Length() < 3.f)
			continue;
		cands[n++] = { bi, zf, lat, horiz };
	}
	if (n < 1)
		return;

	int iU = 0, iE = 0;
	for (int i = 1; i < n; ++i) {
		if (cands[i].z > cands[iU].z)
			iU = i;
		if (cands[i].z < cands[iE].z)
			iE = i;
	}
	outU = cands[iU].i;
	outEnd = cands[iE].i;

	if (n >= 3 && outU != outEnd) {
		const float midZ = 0.5f * (cands[iU].z + cands[iE].z);
		int iM = -1;
		float best = 1e9f;
		for (int i = 0; i < n; ++i) {
			if (cands[i].i == outU || cands[i].i == outEnd)
				continue;
			const float dz = std::fabs(cands[i].z - midZ);
			if (dz < best) {
				best = dz;
				iM = cands[i].i;
			}
		}
		outL = iM;
	}
	if (outL < 0)
		outL = outU;
}

void FillGeometry(uintptr_t ba, const Vector_t& origin, float height, Map& m) {
	if (m.idx[S_PELVIS] < 0) m.idx[S_PELVIS] = 0;
	if (m.idx[S_SPINE0] < 0) m.idx[S_SPINE0] = 1;
	if (m.idx[S_SPINE1] < 0) m.idx[S_SPINE1] = 2;
	if (m.idx[S_SPINE2] < 0) m.idx[S_SPINE2] = 3;
	if (m.idx[S_SPINE3] < 0) m.idx[S_SPINE3] = 4;
	if (m.idx[S_NECK]   < 0) m.idx[S_NECK]   = 5;
	if (m.idx[S_HEAD]   < 0) m.idx[S_HEAD]   = 6;

	// Keep spine→arm connected when clavicle names missing
	if (m.idx[S_CLAV_L] < 0) m.idx[S_CLAV_L] = m.idx[S_NECK];
	if (m.idx[S_CLAV_R] < 0) m.idx[S_CLAV_R] = m.idx[S_NECK];

	Vector_t pelvis{}, head{};
	if (!GetBonePos(ba, m.idx[S_PELVIS], pelvis) || !GetBonePos(ba, m.idx[S_HEAD], head))
		return;

	Vector_t fwd = head - pelvis;
	fwd.z = 0.f;
	float fl = fwd.Length();
	if (fl > 0.1f) {
		fwd.x /= fl;
		fwd.y /= fl;
	} else {
		fwd = { 1.f, 0.f, 0.f };
	}
	Vector_t right = { fwd.y, -fwd.x, 0.f };

	const bool needArm =
		m.idx[S_ARM_U_L] < 0 || m.idx[S_ARM_L_L] < 0 || m.idx[S_HAND_L] < 0 ||
		m.idx[S_ARM_U_R] < 0 || m.idx[S_ARM_L_R] < 0 || m.idx[S_HAND_R] < 0;
	const bool needLeg =
		m.idx[S_LEG_U_L] < 0 || m.idx[S_LEG_L_L] < 0 || m.idx[S_ANKLE_L] < 0 ||
		m.idx[S_LEG_U_R] < 0 || m.idx[S_LEG_L_R] < 0 || m.idx[S_ANKLE_R] < 0;

	if (needArm) {
		int u, l, e;
		if (m.idx[S_ARM_U_L] < 0 || m.idx[S_HAND_L] < 0) {
			GeomLimbChain(ba, origin, height, right, true, 0.42f, 0.96f, 6.f, u, l, e);
			if (m.idx[S_ARM_U_L] < 0) m.idx[S_ARM_U_L] = u;
			if (m.idx[S_ARM_L_L] < 0) m.idx[S_ARM_L_L] = l;
			if (m.idx[S_HAND_L]  < 0) m.idx[S_HAND_L]  = e;
		}
		if (m.idx[S_ARM_U_R] < 0 || m.idx[S_HAND_R] < 0) {
			GeomLimbChain(ba, origin, height, right, false, 0.42f, 0.96f, 6.f, u, l, e);
			if (m.idx[S_ARM_U_R] < 0) m.idx[S_ARM_U_R] = u;
			if (m.idx[S_ARM_L_R] < 0) m.idx[S_ARM_L_R] = l;
			if (m.idx[S_HAND_R]  < 0) m.idx[S_HAND_R]  = e;
		}
	}

	if (needLeg) {
		int u, l, e;
		if (m.idx[S_LEG_U_L] < 0 || m.idx[S_ANKLE_L] < 0) {
			GeomLimbChain(ba, origin, height, right, true, 0.0f, 0.48f, 3.f, u, l, e);
			if (m.idx[S_LEG_U_L] < 0) m.idx[S_LEG_U_L] = u;
			if (m.idx[S_LEG_L_L] < 0) m.idx[S_LEG_L_L] = l;
			if (m.idx[S_ANKLE_L] < 0) m.idx[S_ANKLE_L] = e;
		}
		if (m.idx[S_LEG_U_R] < 0 || m.idx[S_ANKLE_R] < 0) {
			GeomLimbChain(ba, origin, height, right, false, 0.0f, 0.48f, 3.f, u, l, e);
			if (m.idx[S_LEG_U_R] < 0) m.idx[S_LEG_U_R] = u;
			if (m.idx[S_LEG_L_R] < 0) m.idx[S_LEG_L_R] = l;
			if (m.idx[S_ANKLE_R] < 0) m.idx[S_ANKLE_R] = e;
		}
	}
}

void ResolveByName(void* entity, Map& m) {
	m.idx[S_PELVIS] = LookupName(entity, "pelvis", "Pelvis", "bip_pelvis", "hip_ROOT");
	m.idx[S_SPINE0] = LookupName(entity, "spine_0", "spine_1", "Spine", "bip_spine_0");
	m.idx[S_SPINE1] = LookupName(entity, "spine_1", "spine_2", "bip_spine_1");
	m.idx[S_SPINE2] = LookupName(entity, "spine_2", "spine_3", "bip_spine_2");
	m.idx[S_SPINE3] = LookupName(entity, "spine_3", "spine_4", "bip_spine_3");
	m.idx[S_NECK]   = LookupName(entity, "neck_0", "neck_1", "Neck", "bip_neck");
	m.idx[S_HEAD]   = LookupName(entity, "head_0", "head", "Head", "bip_head");

	m.idx[S_CLAV_L] = LookupName(entity, "clavicle_L", "clavicle_l", "clav_L", "bip_collar_L");
	m.idx[S_ARM_U_L] = LookupName(entity, "arm_upper_L", "arm_upper_l", "upperarm_L", "bip_upperArm_L");
	m.idx[S_ARM_L_L] = LookupName(entity, "arm_lower_L", "arm_lower_l", "forearm_L", "bip_lowerArm_L");
	m.idx[S_HAND_L]  = LookupName(entity, "hand_L", "hand_l", "Hand_L", "bip_hand_L");

	m.idx[S_CLAV_R] = LookupName(entity, "clavicle_R", "clavicle_r", "clav_R", "bip_collar_R");
	m.idx[S_ARM_U_R] = LookupName(entity, "arm_upper_R", "arm_upper_r", "upperarm_R", "bip_upperArm_R");
	m.idx[S_ARM_L_R] = LookupName(entity, "arm_lower_R", "arm_lower_r", "forearm_R", "bip_lowerArm_R");
	m.idx[S_HAND_R]  = LookupName(entity, "hand_R", "hand_r", "Hand_R", "bip_hand_R");

	m.idx[S_LEG_U_L] = LookupName(entity, "leg_upper_L", "leg_upper_l", "thigh_L", "bip_hip_L");
	m.idx[S_LEG_L_L] = LookupName(entity, "leg_lower_L", "leg_lower_l", "calf_L", "bip_knee_L");
	m.idx[S_ANKLE_L] = LookupName(entity, "ankle_L", "ankle_l", "foot_L", "bip_foot_L");

	m.idx[S_LEG_U_R] = LookupName(entity, "leg_upper_R", "leg_upper_r", "thigh_R", "bip_hip_R");
	m.idx[S_LEG_L_R] = LookupName(entity, "leg_lower_R", "leg_lower_r", "calf_R", "bip_knee_R");
	m.idx[S_ANKLE_R] = LookupName(entity, "ankle_R", "ankle_r", "foot_R", "bip_foot_R");
}

int ReadBoneCount(void* skeleton) {
	if (!skeleton)
		return 0;
	int count = 0;
	if (Mem::ReadField(skeleton, SKEL_BONE_CNT, count) && count > 0 && count <= 256)
		return count;

	uint32_t modelStateOff = SchemaFinder::Get(hash_32_fnv1a_const("CSkeletonInstance->m_modelState"));
	if (!modelStateOff)
		modelStateOff = static_cast<uint32_t>(MODELSTATE_FB);
	count = 0;
	if (Mem::ReadField(skeleton, modelStateOff + BONE_COUNT_OFF, count) && count > 0 && count <= 256)
		return count;
	return 0;
}

bool TryBonePtr(void* skeleton, size_t absOff, uintptr_t& outPtr) {
	outPtr = 0;
	if (!skeleton)
		return false;
	uintptr_t ptr = 0;
	if (!Mem::ReadField(skeleton, absOff, ptr))
		return false;
	if (!Mem::IsUserPtr(reinterpret_cast<void*>(ptr)))
		return false;
	if (!Mem::IsReadable(reinterpret_cast<void*>(ptr), STRIDE * 8))
		return false;
	outPtr = ptr;
	return true;
}

} // namespace

bool Init() {
	std::memset(&g_eng, 0, sizeof(g_eng));
	g_Lookup = nullptr;
	g_CalcBones = nullptr;
	g_GetHitboxSet = nullptr;
	g_GetTransforms = nullptr;
	std::memset(g_cache, 0, sizeof(g_cache));
	g_cacheWrite = 0;

	// Exact dump patterns first; loose fallbacks if build drifts
	g_eng.lookupBone = M::FindPattern("client",
		"40 53 48 83 EC 20 48 8B 89 30 03 00 00 48 8B DA 48 8B 01 FF 50 68 48 8B");
	if (!g_eng.lookupBone) {
		g_eng.lookupBone = M::FindPattern("client",
			"40 53 48 83 EC ? 48 8B 89 ? ? ? ? 48 8B DA 48 8B 01 FF 50 ? 48 8B C8");
	}
	g_Lookup = reinterpret_cast<LookupBone_t>(g_eng.lookupBone);

	// Exact GetHitBoxSet (IDA 0x1808FD710). Prefix alone hits buymenu_purchase —
	// keep trailing "48 8B F0 48" so match is unique.
	g_eng.getHitboxSet = M::FindPattern("client",
		"48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 40 01 00 00 8B DA 48 8B F9 E8 ? ? ? ? 48 8B F0 48");
	if (!g_eng.getHitboxSet) {
		g_eng.getHitboxSet = M::FindPattern("client",
			"48 89 5C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 8B DA 48 8B F9 E8 ? ? ? ? 48 8B F0 48");
	}
	g_GetHitboxSet = reinterpret_cast<GetHitboxSet_t>(g_eng.getHitboxSet);

	// IDA CSkeletonInstance_CalculateWorldSpaceBones @ 0x180A49A00
	g_eng.calcWorldSpaceBones = M::FindPattern("client",
		"48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 B8 88 42");
	if (!g_eng.calcWorldSpaceBones) {
		g_eng.calcWorldSpaceBones = M::FindPattern("client",
			"48 89 4C 24 ? 55 53 56 57 41 54 41 55 41 56 41 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8D 6C 24 ? 48 8B 81");
	}
	g_CalcBones = reinterpret_cast<CalcWorldBones_t>(g_eng.calcWorldSpaceBones);

	// IDA GetTransformsForHitboxList @ 0x180A59120 — calls CalcBones(0x100)
	g_eng.getTransformsForHitboxList = M::FindPattern("client",
		"48 89 5C 24 20 55 56 57 41 54 41 55 48 81 EC D0");
	if (!g_eng.getTransformsForHitboxList) {
		g_eng.getTransformsForHitboxList = M::FindPattern("client",
			"48 89 5C 24 20 55 56 57 41 54 41 55 48 81 EC ? ? ? ? 49 63 30 4D 8B E0 48 8B EA 48 8B D9 85 F6");
	}
	g_GetTransforms = reinterpret_cast<GetTransforms_t>(g_eng.getTransformsForHitboxList);

	// Same body as LookupBone on this build (GetBoneIdByName / GetBonePositionByName)
	if (g_eng.lookupBone)
		g_eng.getBoneIdByName = g_eng.lookupBone;

	// Local ForceUpdateBones needs CalcBones; Lookup alone is not enough
	g_eng.resolved = (g_Lookup != nullptr && g_CalcBones != nullptr);

	Con::Info("Bones Lookup=%p CalcBones=%p HitboxSet=%p Transforms=%p BoneId=%p",
		g_eng.lookupBone, g_eng.calcWorldSpaceBones, g_eng.getHitboxSet,
		g_eng.getTransformsForHitboxList, g_eng.getBoneIdByName);

	if (!g_eng.lookupBone)
		Con::OffsetMiss("Bones::LookupBone (GETBONEPOSITIONBYNAME)");
	if (!g_eng.calcWorldSpaceBones)
		Con::OffsetMiss("Bones::CalculateWorldSpaceBones");
	if (!g_eng.getHitboxSet)
		Con::OffsetMiss("Bones::GetHitboxSet");
	if (!g_eng.getTransformsForHitboxList)
		Con::OffsetMiss("Bones::GetTransformsForHitboxList");
	else
		Con::Ok("Bones: GetTransformsForHitboxList ready (CalcBones 0x100 path)");

	return g_eng.resolved;
}

const EngineAddrs& Engines() {
	return g_eng;
}

static bool IsLocalPawn(const C_CSPlayerPawn* pawn);

bool ForceUpdateBones(const C_CSPlayerPawn* pawn, unsigned int flags) {
	if (!Mem::ValidEntity(pawn))
		return false;
	// NEVER force enemies — clear skel+0x458 (IDA early-out) snaps mesh off
	// interp → flicker + aim/render desync under trigger seed spam.
	if (!IsLocalPawn(pawn))
		return false;
	void* skel = GetSkeleton(pawn);
	if (!skel)
		return false;
	if (!g_CalcBones)
		return false;

	// IDA CSkeletonInstance_CalculateWorldSpaceBones @ 0x180A49A00:
	//   flags &= 0xFFFFF
	//   if ((*(DWORD*)(skel+0x458) & flags) == flags) return;  // early-out
	// Local only: clear requested bits so post-move bones refresh.
	constexpr size_t kBoneMaskOff = 0x458;
	const unsigned int need = flags & 0xFFFFFu;
	if (need != 0 && Mem::Valid(skel, kBoneMaskOff + sizeof(std::uint32_t))) {
		std::uint32_t mask = 0;
		if (Mem::ReadField(skel, kBoneMaskOff, mask)) {
			const std::uint32_t cleared = mask & ~need;
			if (cleared != mask) {
				__try {
					*reinterpret_cast<std::uint32_t*>(
						reinterpret_cast<std::uint8_t*>(skel) + kBoneMaskOff) = cleared;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
		}
	}

	__try {
		g_CalcBones(skel, flags);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}


bool IsValidPos(const Vector_t& p) {
	return Mem::Finite(p.x) && Mem::Finite(p.y) && Mem::Finite(p.z)
		&& !(p.x == 0.f && p.y == 0.f && p.z == 0.f)
		&& std::fabs(p.x) < 16384.f && std::fabs(p.y) < 16384.f && std::fabs(p.z) < 16384.f;
}

void BeginFrame(std::uint32_t generation) {
	g_frameGen = generation;
}

bool ComputeSurroundingBox(const C_CSPlayerPawn* pawn, Vector_t& mins, Vector_t& maxs) {
	mins = Vector_t{ 0.f, 0.f, 0.f };
	maxs = Vector_t{ 0.f, 0.f, 0.f };
	if (!Mem::ValidEntity(pawn))
		return false;
	return SdkPrioA::ComputeHitboxSurroundingBox(
		const_cast<C_CSPlayerPawn*>(pawn), mins, maxs);
}

bool GetOrigin(const C_CSPlayerPawn* pawn, Vector_t& out) {
	out = Vector_t{ 0.f, 0.f, 0.f };
	if (!Mem::ValidEntity(pawn))
		return false;
	// Priority A: engine GetAbsOrigin when resolved
	if (SdkPrioA::GetAbsOrigin(const_cast<C_CSPlayerPawn*>(pawn), out) && IsValidPos(out))
		return true;
	static uint32_t s_sceneOff = 0;
	static uint32_t s_originOff = 0;
	if (!s_sceneOff) {
		s_sceneOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_pGameSceneNode"));
		s_originOff = SchemaFinder::Get(hash_32_fnv1a_const("CGameSceneNode->m_vecAbsOrigin"));
	}
	if (!s_sceneOff || !s_originOff)
		return false;
	void* node = nullptr;
	if (!Mem::ReadField(pawn, s_sceneOff, node) || !Mem::Valid(node, s_originOff + sizeof(Vector_t)))
		return false;
	if (!Mem::ReadField(node, s_originOff, out))
		return false;
	return IsValidPos(out);
}

static bool IsLocalPawn(const C_CSPlayerPawn* pawn) {
	if (!pawn)
		return false;
	// Prefer alive local (CreateMove combat); fall back to any local pawn.
	C_CSPlayerPawn* local = H::SafeLocalAlive();
	if (!local)
		local = H::SafeLocalPlayer();
	return local == pawn;
}

// abs + viewoffset — engine-style eye for any pawn (NOT m_vOldOrigin)
static Vector_t EyeFromAbsView(const C_CSPlayerPawn* pawn) {
	Vector_t origin{};
	if (!GetOrigin(pawn, origin))
		return {};
	static uint32_t s_viewOffset = 0;
	if (!s_viewOffset) {
		s_viewOffset = SchemaFinder::Get(
			hash_32_fnv1a_const("C_BaseModelEntity->m_vecViewOffset"));
	}
	if (!s_viewOffset)
		return {};
	Vector_t view{};
	if (!Mem::ReadField(pawn, s_viewOffset, view))
		return {};
	if (!Mem::Finite(view.x) || !Mem::Finite(view.y) || !Mem::Finite(view.z))
		return {};
	const Vector_t result = origin + view;
	return IsValidPos(result) ? result : Vector_t{};
}

// IDA client GetInterpolatedShootPosition (sub_1808C24C0 / sub_1808C29A0):
//   CCSPlayer_WeaponServices + 0xE8 : ShootPositionHistoryEntry_t[32] (20 bytes each)
//   +0x368 head index, +0x36C count
// Entry: tick(int) @0, fraction(float) @4, pos xyz @8/12/16
// This is the origin the engine uses for weapon-fire traces — not m_vOldOrigin.
static bool ShootPosFromWeaponHistory(const C_CSPlayerPawn* pawn, Vector_t& out) {
	out = Vector_t{ 0.f, 0.f, 0.f };
	if (!Mem::ValidEntity(pawn))
		return false;

	static uint32_t s_wpnSvcOff = 0;
	if (!s_wpnSvcOff) {
		s_wpnSvcOff = SchemaFinder::Get(
			hash_32_fnv1a_const("C_BasePlayerPawn->m_pWeaponServices"));
	}
	if (!s_wpnSvcOff)
		return false;
	const uint32_t wpnSvcOff = s_wpnSvcOff;

	void* svc = nullptr;
	if (!Mem::ReadField(pawn, wpnSvcOff, svc) || !Mem::Valid(svc, 0x380))
		return false;

	constexpr std::size_t kHistBase = 0xE8;
	constexpr std::size_t kHeadOff = 0x368;
	constexpr std::size_t kCountOff = 0x36C;
	constexpr int kSlots = 32;
	constexpr int kEntrySize = 20;

	int head = 0;
	int count = 0;
	if (!Mem::ReadField(svc, kHeadOff, head) || !Mem::ReadField(svc, kCountOff, count))
		return false;
	if (count <= 0 || count > kSlots)
		return false;

	// Newest slot is typically (head - 1) mod 32; also scan for max tick as fallback
	Vector_t best{};
	int bestTick = -1;
	const int start = ((head % kSlots) + kSlots - 1) % kSlots;
	for (int n = 0; n < count; ++n) {
		const int slot = (start - n + kSlots * 4) % kSlots;
		const auto* e = reinterpret_cast<const std::uint8_t*>(svc) + kHistBase
			+ static_cast<std::size_t>(slot) * kEntrySize;
		if (!Mem::IsReadable(e, kEntrySize))
			continue;
		int tick = 0;
		float fx = 0.f, fy = 0.f, fz = 0.f;
		std::memcpy(&tick, e + 0, 4);
		std::memcpy(&fx, e + 8, 4);
		std::memcpy(&fy, e + 12, 4);
		std::memcpy(&fz, e + 16, 4);
		if (tick < 0 || !std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(fz))
			continue;
		if (fx == 0.f && fy == 0.f && fz == 0.f)
			continue;
		if (std::fabs(fx) > 16384.f || std::fabs(fy) > 16384.f || std::fabs(fz) > 16384.f)
			continue;
		if (tick >= bestTick) {
			bestTick = tick;
			best = { fx, fy, fz };
		}
		// First (newest) valid is usually enough
		if (n == 0 && IsValidPos(best)) {
			out = best;
			return true;
		}
	}
	if (bestTick >= 0 && IsValidPos(best)) {
		out = best;
		return true;
	}
	return false;
}

Vector_t GetEyePos(const C_CSPlayerPawn* pawn) {
	if (!Mem::ValidEntity(pawn))
		return {};

	// Local: engine fire origin chain (UC thread — never m_vOldOrigin+view)
	// 1) WeaponServices shoot-position history (IDA GetInterpolatedShootPosition)
	// 2) NetClientInfo LocalData.ShootPosition
	// 3) abs origin + view offset
	if (IsLocalPawn(pawn)) {
		Vector_t shoot{};
		if (ShootPosFromWeaponHistory(pawn, shoot) && IsValidPos(shoot))
			return shoot;
		if (I::EngineClient) {
			bool ok = false;
			__try {
				ok = I::EngineClient->get_local_shoot_position(shoot);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				ok = false;
			}
			if (ok && IsValidPos(shoot))
				return shoot;
		}
	}

	return EyeFromAbsView(pawn);
}

Vector_t GetShootPos(const C_CSPlayerPawn* pawn) {
	return GetEyePos(pawn);
}

void* GetSkeleton(const C_CSPlayerPawn* pawn) {
	if (!Mem::ValidEntity(pawn))
		return nullptr;
	const uint32_t sceneOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_pGameSceneNode"));
	if (!sceneOff)
		return nullptr;
	void* skeleton = nullptr;
	if (!Mem::ReadField(pawn, sceneOff, skeleton) || !Mem::Valid(skeleton, 0x200))
		return nullptr;
	return skeleton;
}

namespace {
bool ResolveBoneArrayPtr(const C_CSPlayerPawn* pawn, bool forceUpdate,
	uintptr_t& boneArray, Vector_t& origin, float& height, int* outBoneCount)
{
	boneArray = 0;
	height = 0.f;
	origin = Vector_t{ 0.f, 0.f, 0.f };
	if (outBoneCount)
		*outBoneCount = 0;
	if (!Mem::ValidEntity(pawn))
		return false;
	if (!GetOrigin(pawn, origin))
		return false;
	const Vector_t eye = GetEyePos(pawn);
	if (!IsValidPos(eye))
		return false;
	height = eye.z - origin.z;
	// Crouch ~40, stand ~64, jump stretch; reject garbage only
	if (height < 6.f || height > 140.f)
		return false;

	void* skeleton = GetSkeleton(pawn);
	if (!skeleton)
		return false;

	// Never call CalcBones from DrawArray — re-enters skeleton mid-render → crash.
	if (forceUpdate)
		ForceUpdateBones(pawn, 0x100u);

	const int boneCount = ReadBoneCount(skeleton);
	if (outBoneCount)
		*outBoneCount = boneCount;

	// Prefer IDA-confirmed skeleton+0x1C0 (= modelState+0x80)
	uintptr_t ptr = 0;
	uintptr_t softPtr = 0; // readable but failed strict validate
	auto tryPtr = [&](uintptr_t cand) -> bool {
		if (!cand)
			return false;
		if (ValidateBoneArray(cand, origin, height, boneCount > 0 ? boneCount : 128)) {
			boneArray = cand;
			return true;
		}
		if (!softPtr)
			softPtr = cand;
		return false;
	};

	if (TryBonePtr(skeleton, SKEL_BONE_PTR, ptr) && tryPtr(ptr))
		return true;

	uint32_t modelStateOff = SchemaFinder::Get(hash_32_fnv1a_const("CSkeletonInstance->m_modelState"));
	if (!modelStateOff)
		modelStateOff = static_cast<uint32_t>(MODELSTATE_FB);

	// CModelState bone array is NOT schema-registered — fixed modelState+0x80
	static const size_t kOffs[] = { BONE_PTR_OFF, 0xD0, 0x128, 0x150, 0x160, 0x1A0 };
	for (size_t i = 0; i < sizeof(kOffs) / sizeof(kOffs[0]); ++i) {
		if (!TryBonePtr(skeleton, modelStateOff + kOffs[i], ptr))
			continue;
		if (tryPtr(ptr))
			return true;
	}
	// Soft accept: first readable bone array with ≥4 finite positions (crouch/agent edge)
	if (softPtr) {
		int ok = 0;
		for (int i = 0; i < 16 && ok < 4; ++i) {
			Vector_t p{};
			if (GetBonePos(softPtr, i, p))
				++ok;
		}
		if (ok >= 4) {
			boneArray = softPtr;
			return true;
		}
	}
	return false;
}
} // namespace

bool GetBoneArray(const C_CSPlayerPawn* pawn, uintptr_t& boneArray, Vector_t& origin, float& height) {
	// Local: force. Enemy: always readonly (ForceUpdateBones also refuses enemies).
	const bool force = IsLocalPawn(pawn);
	return ResolveBoneArrayPtr(pawn, force, boneArray, origin, height, nullptr);
}

bool GetBoneArrayReadonly(const C_CSPlayerPawn* pawn, uintptr_t& boneArray,
	Vector_t& origin, float& height, int* outBoneCount)
{
	return ResolveBoneArrayPtr(pawn, false, boneArray, origin, height, outBoneCount);
}

bool GetBonePos(uintptr_t boneArray, int boneIndex, Vector_t& out) {
	out = Vector_t{ 0.f, 0.f, 0.f };
	if (!Mem::IsUserPtr(reinterpret_cast<void*>(boneArray)) || !Mem::ValidBoneIndex(boneIndex))
		return false;
	const auto addr = boneArray + static_cast<size_t>(boneIndex) * STRIDE;
	if (!Mem::Read(reinterpret_cast<const void*>(addr), out))
		return false;
	return IsValidPos(out);
}

bool ResolveMap(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	const Vector_t& origin, float height, Map& out)
{
	std::memset(&out, 0, sizeof(out));
	for (int i = 0; i < S_COUNT; ++i)
		out.idx[i] = -1;
	out.ok = false;
	out.boneCount = 0;

	if (!pawn || !boneArray || height < 1.f)
		return false;

	if (void* skel = GetSkeleton(pawn))
		out.boneCount = ReadBoneCount(skel);

	ResolveByName(const_cast<C_CSPlayerPawn*>(pawn), out);
	FillGeometry(boneArray, origin, height, out);

	Vector_t pelvis{}, head{};
	if (!GetSlotPos(boneArray, out, S_PELVIS, pelvis) || !GetSlotPos(boneArray, out, S_HEAD, head))
		return false;
	if ((pelvis - origin).Length() > 96.f)
		return false;
	// Crouch / jump: head can sit lower relative to eye height
	if (head.z < origin.z + height * 0.55f)
		return false;
	if (head.z < pelvis.z + 8.f)
		return false;

	out.ok = true;
	return true;
}

bool ResolveMapCached(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	const Vector_t& origin, float height, Map& out)
{
	if (CacheGet(pawn, boneArray, out))
		return true;
	if (!ResolveMap(pawn, boneArray, origin, height, out))
		return false;
	CachePut(pawn, boneArray, out);
	return true;
}

bool GetSlotPos(uintptr_t boneArray, const Map& map, int slot, Vector_t& out) {
	if (slot < 0 || slot >= S_COUNT)
		return false;
	const int bi = map.idx[slot];
	return GetBonePos(boneArray, bi, out);
}

int CollectSkeletonPoints(const C_CSPlayerPawn* pawn, Vector_t* outSlots, bool* outValid,
	bool forceUpdate)
{
	if (!pawn || !outSlots || !outValid)
		return 0;
	for (int i = 0; i < S_COUNT; ++i) {
		outSlots[i] = Vector_t{ 0.f, 0.f, 0.f };
		outValid[i] = false;
	}

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	Map map{};
	const bool haveBones = forceUpdate
		? GetBoneArray(pawn, boneArray, origin, height)
		: GetBoneArrayReadonly(pawn, boneArray, origin, height);
	if (!haveBones || !ResolveMapCached(pawn, boneArray, origin, height, map))
		return 0;

	int n = 0;
	for (int s = 0; s < S_COUNT; ++s) {
		Vector_t p{};
		if (!GetSlotPos(boneArray, map, s, p))
			continue;
		outSlots[s] = p;
		outValid[s] = true;
		++n;
	}
	return n;
}

int CollectHitboxPoints(const C_CSPlayerPawn* pawn, int hb, Vector_t* out, int maxOut) {
	if (!pawn || !out || maxOut < 1)
		return 0;

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	Map map{};
	const bool have = GetBoneArray(pawn, boneArray, origin, height)
		&& ResolveMapCached(pawn, boneArray, origin, height, map);

	if (hb == Config::HB_HEAD) {
		if (have) {
			Vector_t head{}, neck{};
			if (GetSlotPos(boneArray, map, S_HEAD, head) && InHitboxBand(head, origin, height, Config::HB_HEAD)) {
				if (!GetSlotPos(boneArray, map, S_NECK, neck) || neck.z < head.z - 1.f) {
					out[0] = head;
					return 1;
				}
			}
		}
		Vector_t fb{};
		if (FallbackPoint(pawn, Config::HB_HEAD, fb)) {
			out[0] = fb;
			return 1;
		}
		return 0;
	}

	const int* list = nullptr;
	int nList = 0;
	HitboxSlots(hb, list, nList);

	int n = 0;
	if (have) {
		for (int i = 0; i < nList && n < maxOut; ++i) {
			Vector_t p{};
			if (!GetSlotPos(boneArray, map, list[i], p))
				continue;
			if (!InHitboxBand(p, origin, height, hb))
				continue;
			out[n++] = p;
		}

		const bool limb = (hb == Config::HB_ARMS || hb == Config::HB_LEGS || hb == Config::HB_FEET);
		if (n == 0 && limb) {
			const Vector_t eye = GetEyePos(pawn);
			Vector_t fwd = eye - origin;
			fwd.z = 0.f;
			float fl = fwd.Length();
			if (fl > 0.1f) { fwd.x /= fl; fwd.y /= fl; }
			else { fwd = { 1.f, 0.f, 0.f }; }
			const Vector_t right = { fwd.y, -fwd.x, 0.f };

			Vector_t bestL{}, bestR{};
			float bestLScore = 1e9f, bestRScore = -1e9f;
			bool haveL = false, haveR = false;

			const int scan = (map.boneCount > 0) ? (std::min)(map.boneCount, 96) : 64;
			for (int bi = 0; bi < scan; ++bi) {
				Vector_t p{};
				if (!GetBonePos(boneArray, bi, p) || !InHitboxBand(p, origin, height, hb))
					continue;
				const Vector_t d = p - origin;
				const float lat = d.x * right.x + d.y * right.y;
				if (lat < -3.f && (!haveL || lat < bestLScore)) {
					bestLScore = lat; bestL = p; haveL = true;
				}
				if (lat > 3.f && (!haveR || lat > bestRScore)) {
					bestRScore = lat; bestR = p; haveR = true;
				}
			}
			if (haveL && n < maxOut) out[n++] = bestL;
			if (haveR && n < maxOut) out[n++] = bestR;
		}
	}

	if (n == 0) {
		Vector_t fb{};
		if (FallbackPoint(pawn, hb, fb))
			out[n++] = fb;
	}
	return n;
}

bool GetHitboxPoint(const C_CSPlayerPawn* pawn, int hb, Vector_t& out) {
	Capsule cap{};
	if (GetHitboxCapsule(pawn, hb, cap) && IsValidPos(cap.center)) {
		out = cap.center;
		return true;
	}
	Vector_t pts[8];
	const int n = CollectHitboxPoints(pawn, hb, pts, 8);
	if (n <= 0)
		return false;
	out = pts[0];
	return true;
}

// Forward — defined later; used by capsule fallback before definition site
float MultipointRadius(int hb);

// Default studio indices for stock player models (fallback when name map fails)
enum StudioHb : int {
	ST_HEAD = 0, ST_NECK = 1, ST_PELVIS = 2, ST_STOMACH = 3,
	ST_CHEST = 4, ST_LOWER_CHEST = 5, ST_UPPER_CHEST = 6,
	ST_R_THIGH = 7, ST_L_THIGH = 8, ST_R_CALF = 9, ST_L_CALF = 10,
	ST_R_FOOT = 11, ST_L_FOOT = 12, ST_R_HAND = 13, ST_L_HAND = 14,
	ST_R_UPPER_ARM = 15, ST_R_FOREARM = 16, ST_L_UPPER_ARM = 17, ST_L_FOREARM = 18
};

constexpr size_t kHitboxStride = 0x70;
constexpr size_t kHbMins = 0x18;
constexpr size_t kHbMaxs = 0x24;
constexpr size_t kHbRadius = 0x30;
constexpr size_t kHbBoneName = 0x10;
// CHitBox: hitgroup often @ +0x38 (IDA invalid_hitbox init)
constexpr size_t kHbHitgroup = 0x38;
constexpr size_t kSetCount = 0x10;
constexpr size_t kSetArray = 0x18;

// Case-insensitive substring
bool NameHas(const char* s, const char* needle) {
	if (!s || !needle || !needle[0])
		return false;
	const size_t nlen = std::strlen(needle);
	for (const char* p = s; *p; ++p) {
		size_t i = 0;
		for (; i < nlen; ++i) {
			const char a = p[i];
			if (!a) return false;
			const char b = needle[i];
			const char al = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
			const char bl = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
			if (al != bl) break;
		}
		if (i == nlen) return true;
	}
	return false;
}

// Map bone / hitbox name → Config::HB_*
int ClassifyBoneName(const char* name) {
	if (!name || !name[0])
		return -1;
	// Order matters: more specific first
	if (NameHas(name, "head") || NameHas(name, "skull"))
		return Config::HB_HEAD;
	if (NameHas(name, "neck"))
		return Config::HB_NECK;
	if (NameHas(name, "clav") || NameHas(name, "collar")
		|| NameHas(name, "upperarm") || NameHas(name, "arm_upper")
		|| NameHas(name, "forearm") || NameHas(name, "arm_lower")
		|| NameHas(name, "hand") || NameHas(name, "wrist")
		|| NameHas(name, "elbow") || NameHas(name, "shoulder"))
		return Config::HB_ARMS;
	if (NameHas(name, "thigh") || NameHas(name, "leg_upper")
		|| NameHas(name, "calf") || NameHas(name, "leg_lower")
		|| NameHas(name, "knee") || NameHas(name, "shin"))
		return Config::HB_LEGS;
	if (NameHas(name, "ankle") || NameHas(name, "foot") || NameHas(name, "toe"))
		return Config::HB_FEET;
	if (NameHas(name, "pelvis") || NameHas(name, "hip") || NameHas(name, "groin"))
		return Config::HB_PELVIS;
	if (NameHas(name, "stomach") || NameHas(name, "abdomen")
		|| NameHas(name, "spine_0") || NameHas(name, "spine_1")
		|| NameHas(name, "spine0") || NameHas(name, "spine1"))
		return Config::HB_STOMACH;
	if (NameHas(name, "chest") || NameHas(name, "spine") || NameHas(name, "torso")
		|| NameHas(name, "body") || NameHas(name, "ribcage"))
		return Config::HB_CHEST;
	return -1;
}

int HitgroupToHbLocal(int hg) {
	switch (hg) {
	case 1: return Config::HB_HEAD;
	case 8: return Config::HB_NECK;
	case 2: return Config::HB_CHEST;
	case 3: return Config::HB_STOMACH;
	case 4: case 5: return Config::HB_ARMS;
	case 6: case 7: return Config::HB_LEGS;
	default: return -1;
	}
}

// Hardcoded stock map — used when names unreadable
void FillHardcodedStudioMap(std::int8_t* map, int count) {
	for (int i = 0; i < 64; ++i)
		map[i] = -1;
	auto set = [&](int si, int hb) {
		if (si >= 0 && si < count && si < 64)
			map[si] = static_cast<std::int8_t>(hb);
	};
	set(ST_HEAD, Config::HB_HEAD);
	set(ST_NECK, Config::HB_NECK);
	set(ST_PELVIS, Config::HB_PELVIS);
	set(ST_STOMACH, Config::HB_STOMACH);
	set(ST_CHEST, Config::HB_CHEST);
	set(ST_LOWER_CHEST, Config::HB_CHEST);
	set(ST_UPPER_CHEST, Config::HB_CHEST);
	set(ST_R_THIGH, Config::HB_LEGS);
	set(ST_L_THIGH, Config::HB_LEGS);
	set(ST_R_CALF, Config::HB_LEGS);
	set(ST_L_CALF, Config::HB_LEGS);
	set(ST_R_FOOT, Config::HB_FEET);
	set(ST_L_FOOT, Config::HB_FEET);
	set(ST_R_HAND, Config::HB_ARMS);
	set(ST_L_HAND, Config::HB_ARMS);
	set(ST_R_UPPER_ARM, Config::HB_ARMS);
	set(ST_R_FOREARM, Config::HB_ARMS);
	set(ST_L_UPPER_ARM, Config::HB_ARMS);
	set(ST_L_FOREARM, Config::HB_ARMS);
}

bool ReadStudioBoneName(uintptr_t hbArr, int studioIdx, char* out, int outSz) {
	if (!out || outSz < 2 || studioIdx < 0 || !hbArr)
		return false;
	out[0] = 0;
	const auto* hb = reinterpret_cast<void*>(hbArr + static_cast<size_t>(studioIdx) * kHitboxStride);
	const char* boneName = nullptr;
	if (!Mem::ReadField(hb, kHbBoneName, boneName)
		|| !boneName || !Mem::IsUserPtr(const_cast<char*>(boneName)))
		return false;
	__try {
		for (int i = 0; i < outSz - 1; ++i) {
			out[i] = boneName[i];
			if (!out[i]) {
				out[i] = 0;
				return out[0] != 0;
			}
		}
		out[outSz - 1] = 0;
		return out[0] != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out[0] = 0;
		return false;
	}
}

void BuildStudioMap(FrameCapsuleCache& fc) {
	if (fc.studioMapOk)
		return;
	FillHardcodedStudioMap(fc.studioToHb, fc.hbCount);
	if (!fc.setOk || fc.hbCount <= 0)
		return;

	int named = 0;
	for (int si = 0; si < fc.hbCount && si < 64; ++si) {
		char name[64]{};
		if (!ReadStudioBoneName(fc.hbArr, si, name, 64))
			continue;
		const int byName = ClassifyBoneName(name);
		if (byName >= 0) {
			fc.studioToHb[si] = static_cast<std::int8_t>(byName);
			++named;
			continue;
		}
		// Hitgroup field as secondary signal
		int hg = -1;
		const auto* hb = reinterpret_cast<void*>(
			fc.hbArr + static_cast<size_t>(si) * kHitboxStride);
		if (Mem::ReadField(hb, kHbHitgroup, hg)) {
			const int byHg = HitgroupToHbLocal(hg);
			if (byHg >= 0)
				fc.studioToHb[si] = static_cast<std::int8_t>(byHg);
		}
	}
	fc.studioMapOk = true;
	if (named > 0) {
		// name path worked for at least one box
	}
}

// Collect studio indices for a Config hitbox group (name map first)
int CollectStudioIndices(FrameCapsuleCache& fc, int hb, int* out, int maxOut) {
	if (!out || maxOut < 1 || hb < 0 || hb >= Config::HB_COUNT)
		return 0;
	BuildStudioMap(fc);
	int n = 0;
	for (int si = 0; si < fc.hbCount && si < 64 && n < maxOut; ++si) {
		if (fc.studioToHb[si] == static_cast<std::int8_t>(hb))
			out[n++] = si;
	}
	if (n > 0)
		return n;

	// Absolute fallback: hardcoded lists
	static const int head[] = { ST_HEAD };
	static const int neck[] = { ST_NECK };
	static const int chest[] = { ST_UPPER_CHEST, ST_CHEST, ST_LOWER_CHEST };
	static const int stomach[] = { ST_STOMACH };
	static const int pelvis[] = { ST_PELVIS };
	static const int arms[] = {
		ST_L_UPPER_ARM, ST_R_UPPER_ARM, ST_L_FOREARM, ST_R_FOREARM, ST_L_HAND, ST_R_HAND
	};
	static const int legs[] = { ST_L_THIGH, ST_R_THIGH, ST_L_CALF, ST_R_CALF };
	static const int feet[] = { ST_L_FOOT, ST_R_FOOT };
	const int* list = head;
	int count = 1;
	switch (hb) {
	case Config::HB_HEAD:    list = head;    count = 1; break;
	case Config::HB_NECK:    list = neck;    count = 1; break;
	case Config::HB_CHEST:   list = chest;   count = 3; break;
	case Config::HB_STOMACH: list = stomach; count = 1; break;
	case Config::HB_PELVIS:  list = pelvis;  count = 1; break;
	case Config::HB_ARMS:    list = arms;    count = 6; break;
	case Config::HB_LEGS:    list = legs;    count = 4; break;
	case Config::HB_FEET:    list = feet;    count = 2; break;
	default: break;
	}
	for (int i = 0; i < count && n < maxOut; ++i) {
		if (list[i] >= 0 && list[i] < fc.hbCount)
			out[n++] = list[i];
	}
	return n;
}

bool ReadHitboxSet(void* entity, uintptr_t& arrayOut, int& countOut) {
	arrayOut = 0;
	countOut = 0;
	if (!entity || !g_GetHitboxSet)
		return false;
	void* set = nullptr;
	__try {
		set = g_GetHitboxSet(entity, 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!set || !Mem::Valid(set, 0x20))
		return false;
	int count = 0;
	uintptr_t arr = 0;
	if (!Mem::ReadField(set, kSetCount, count) || count <= 0 || count > 64)
		return false;
	if (!Mem::ReadField(set, kSetArray, arr) || !Mem::IsUserPtr(reinterpret_cast<void*>(arr)))
		return false;
	if (!Mem::IsReadable(reinterpret_cast<void*>(arr),
			static_cast<size_t>(count) * kHitboxStride))
		return false;
	arrayOut = arr;
	countOut = count;
	return true;
}

Vector_t QuatRotate(float x, float y, float z, float w, const Vector_t& v) {
	const float tw = w * 0.f - x * v.x - y * v.y - z * v.z;
	const float tx = w * v.x + x * 0.f + y * v.z - z * v.y;
	const float ty = w * v.y - x * v.z + y * 0.f + z * v.x;
	const float tz = w * v.z + x * v.y - y * v.x + z * 0.f;
	const float cx = -x, cy = -y, cz = -z;
	return Vector_t{
		tw * cx + tx * w + ty * cz - tz * cy,
		tw * cy - tx * cz + ty * w + tz * cx,
		tw * cz + tx * cy - ty * cx + tz * w
	};
}

struct BoneXformRaw {
	float px, py, pz, scale, qx, qy, qz, qw;
};

bool ReadBoneXform(uintptr_t boneArray, int boneIndex, Vector_t& pos, float& scale,
	float& qx, float& qy, float& qz, float& qw)
{
	pos = Vector_t{ 0.f, 0.f, 0.f };
	scale = 1.f;
	qx = qy = qz = 0.f;
	qw = 1.f;
	if (!Mem::IsUserPtr(reinterpret_cast<void*>(boneArray)) || !Mem::ValidBoneIndex(boneIndex))
		return false;
	BoneXformRaw raw{};
	const auto* addr = reinterpret_cast<const void*>(boneArray + static_cast<size_t>(boneIndex) * STRIDE);
	if (!Mem::Read(addr, raw))
		return false;
	pos = { raw.px, raw.py, raw.pz };
	scale = raw.scale;
	qx = raw.qx; qy = raw.qy; qz = raw.qz; qw = raw.qw;
	if (!IsValidPos(pos))
		return false;
	if (!Mem::Finite(scale) || scale < 0.01f || scale > 10.f)
		scale = 1.f;
	if (!Mem::Finite(qx) || !Mem::Finite(qy) || !Mem::Finite(qz) || !Mem::Finite(qw)) {
		qx = qy = qz = 0.f;
		qw = 1.f;
	}
	return true;
}

bool BuildCapsuleFromStudio(const C_CSPlayerPawn* pawn, uintptr_t boneArray,
	uintptr_t hbArr, int studioIdx, Capsule& out)
{
	out = {};
	out.studioIndex = studioIdx;
	if (studioIdx < 0 || !hbArr)
		return false;

	const auto* hb = reinterpret_cast<void*>(hbArr + static_cast<size_t>(studioIdx) * kHitboxStride);
	Vector_t localMins{}, localMaxs{};
	float radius = 0.f;
	if (!Mem::ReadField(hb, kHbMins, localMins))
		return false;
	if (!Mem::ReadField(hb, kHbMaxs, localMaxs))
		return false;
	if (!Mem::ReadField(hb, kHbRadius, radius))
		return false;
	if (!Mem::Finite(radius) || radius < 0.f || radius > 64.f)
		return false;

	// Sphere-ish boxes: radius only, mins/maxs near zero
	const float localSpan = (localMaxs - localMins).Length();
	if (radius < 0.05f && localSpan < 0.05f)
		return false;
	if (radius < 0.05f)
		radius = (std::max)(localSpan * 0.15f, 0.5f);

	int boneIdx = -1;
	char nameBuf[64]{};
	if (ReadStudioBoneName(hbArr, studioIdx, nameBuf, 64))
		boneIdx = CallLookup(const_cast<C_CSPlayerPawn*>(pawn), nameBuf);

	// Name lookup failed → try common aliases for this studio slot
	if (boneIdx < 0 && pawn) {
		static const char* kHead[] = { "head_0", "head", "Head", nullptr };
		static const char* kNeck[] = { "neck_0", "neck_1", "Neck", nullptr };
		static const char* kPelvis[] = { "pelvis", "Pelvis", nullptr };
		const char* const* aliases = nullptr;
		if (studioIdx == ST_HEAD) aliases = kHead;
		else if (studioIdx == ST_NECK) aliases = kNeck;
		else if (studioIdx == ST_PELVIS) aliases = kPelvis;
		if (aliases) {
			for (int i = 0; aliases[i]; ++i) {
				boneIdx = CallLookup(const_cast<C_CSPlayerPawn*>(pawn), aliases[i]);
				if (boneIdx >= 0) break;
			}
		}
	}

	Vector_t mins = localMins;
	Vector_t maxs = localMaxs;
	bool xformed = false;
	if (boneIdx >= 0 && boneArray) {
		Vector_t pos{};
		float scale = 1.f, qx = 0.f, qy = 0.f, qz = 0.f, qw = 1.f;
		if (ReadBoneXform(boneArray, boneIdx, pos, scale, qx, qy, qz, qw)) {
			const Vector_t sm{ localMins.x * scale, localMins.y * scale, localMins.z * scale };
			const Vector_t sx{ localMaxs.x * scale, localMaxs.y * scale, localMaxs.z * scale };
			mins = QuatRotate(qx, qy, qz, qw, sm) + pos;
			maxs = QuatRotate(qx, qy, qz, qw, sx) + pos;
			// Sphere hitbox: mins==maxs after transform → expand along nothing, keep center
			if ((mins - maxs).Length() < 0.01f) {
				mins = pos;
				maxs = pos;
			}
			xformed = true;
		}
	}

	// No bone xform: cannot use local mins as world — fail closed (caller falls back)
	if (!xformed)
		return false;

	if (!IsValidPos(mins) || !IsValidPos(maxs))
		return false;

	out.mins = mins;
	out.maxs = maxs;
	out.center = Vector_t{
		(mins.x + maxs.x) * 0.5f,
		(mins.y + maxs.y) * 0.5f,
		(mins.z + maxs.z) * 0.5f
	};
	// Scale radius with bone scale when available (already applied to mins/maxs)
	out.radius = radius;
	out.ok = IsValidPos(out.center) && radius > 0.05f;
	return out.ok;
}

bool GetHitboxCapsule(const C_CSPlayerPawn* pawn, int hb, Capsule& out) {
	out = {};
	if (!Mem::ValidEntity(pawn) || hb < 0 || hb >= Config::HB_COUNT)
		return false;

	FrameCapsuleCache& fc = AcquireFrameCache(pawn);
	if (fc.haveHb[hb]) {
		out = fc.byHb[hb];
		return out.ok;
	}

	uintptr_t hbArr = 0;
	int hbCount = 0;
	if (fc.setOk) {
		hbArr = fc.hbArr;
		hbCount = fc.hbCount;
	} else if (ReadHitboxSet(const_cast<C_CSPlayerPawn*>(pawn), hbArr, hbCount)) {
		fc.hbArr = hbArr;
		fc.hbCount = hbCount;
		fc.setOk = true;
	} else {
		fc.haveHb[hb] = true;
		fc.byHb[hb] = {};
		return false;
	}

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	// Enemy: readonly pose. Local: force. GetBoneArray already branches.
	if (!GetBoneArray(pawn, boneArray, origin, height))
		GetBoneArrayReadonly(pawn, boneArray, origin, height);

	int indices[24]{};
	const int nList = CollectStudioIndices(fc, hb, indices, 24);

	Capsule best{};
	float bestScore = -1.f;
	for (int i = 0; i < nList; ++i) {
		const int si = indices[i];
		Capsule c{};
		if (!BuildCapsuleFromStudio(pawn, boneArray, hbArr, si, c))
			continue;
		// Prefer larger / more central body boxes for multi-index groups (chest)
		const float score = c.radius + (c.maxs - c.mins).Length() * 0.15f;
		if (score > bestScore) {
			bestScore = score;
			best = c;
		}
	}

	// Studio path failed → bone-slot center + constant radius (still usable for aim)
	if (!best.ok) {
		Vector_t pts[4]{};
		const int np = CollectHitboxPoints(pawn, hb, pts, 4);
		if (np > 0 && IsValidPos(pts[0])) {
			best.center = pts[0];
			best.mins = pts[0];
			best.maxs = pts[0];
			best.radius = MultipointRadius(hb);
			best.studioIndex = -1;
			best.ok = true;
		}
	}

	fc.haveHb[hb] = true;
	fc.byHb[hb] = best;
	if (!best.ok)
		return false;
	out = best;
	return true;
}

bool GetStudioCapsule(const C_CSPlayerPawn* pawn, int studioIndex, Capsule& out) {
	out = {};
	if (!Mem::ValidEntity(pawn) || studioIndex < 0)
		return false;

	FrameCapsuleCache& fc = AcquireFrameCache(pawn);
	uintptr_t hbArr = 0;
	int hbCount = 0;
	if (fc.setOk) {
		hbArr = fc.hbArr;
		hbCount = fc.hbCount;
	} else if (ReadHitboxSet(const_cast<C_CSPlayerPawn*>(pawn), hbArr, hbCount)) {
		fc.hbArr = hbArr;
		fc.hbCount = hbCount;
		fc.setOk = true;
	} else {
		return false;
	}
	if (studioIndex >= hbCount)
		return false;
	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	if (!GetBoneArray(pawn, boneArray, origin, height))
		GetBoneArrayReadonly(pawn, boneArray, origin, height);
	return BuildCapsuleFromStudio(pawn, boneArray, hbArr, studioIndex, out);
}

int StudioIndexToHitbox(int studioIndex) {
	// Stock default — callers without pawn context
	if (studioIndex < 0 || studioIndex >= 64)
		return -1;
	std::int8_t map[64]{};
	FillHardcodedStudioMap(map, 64);
	return map[studioIndex];
}

int HitgroupToHitbox(int hitgroup) {
	// HITGROUP_*: 1 head, 2 chest, 3 stomach, 4/5 arms, 6/7 legs, 8 neck
	switch (hitgroup) {
	case 1: return Config::HB_HEAD;
	case 8: return Config::HB_NECK;
	case 2: return Config::HB_CHEST;
	case 3: return Config::HB_STOMACH;
	case 4:
	case 5: return Config::HB_ARMS;
	case 6:
	case 7: return Config::HB_LEGS;
	default: return -1;
	}
}

static float Dot3(const Vector_t& a, const Vector_t& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool RayHitsCapsule(
	const Vector_t& eye,
	const Vector_t& dir,
	const Capsule& cap,
	float radiusScale,
	float& outT,
	Vector_t& outPoint,
	float centerBias)
{
	outT = 0.f;
	outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!cap.ok || !IsValidPos(eye))
		return false;
	// dir must be unit-ish; reject garbage
	const float dlen2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
	if (dlen2 < 0.5f || dlen2 > 1.5f || !Mem::Finite(dir.x))
		return false;

	float radius = cap.radius * std::clamp(radiusScale, 0.05f, 1.f);
	if (radius < 0.25f)
		radius = 0.25f;
	const float bias = std::clamp(centerBias, 0.15f, 1.f);
	const float maxLat = radius * bias;
	const float rSqr = maxLat * maxLat;

	Vector_t mins = cap.mins;
	Vector_t maxs = cap.maxs;
	// Sphere hitbox: mins==maxs — ray vs sphere
	Vector_t axis = maxs - mins;
	float axisLen = axis.Length();
	if (axisLen > 1.f) {
		axis.x /= axisLen; axis.y /= axisLen; axis.z /= axisLen;
		const float inset = (1.f - std::clamp(radiusScale, 0.05f, 1.f)) * cap.radius * 0.45f;
		const float maxInset = axisLen * 0.40f;
		const float pad = (std::min)(inset, maxInset);
		mins = {
			mins.x + axis.x * pad,
			mins.y + axis.y * pad,
			mins.z + axis.z * pad
		};
		maxs = {
			maxs.x - axis.x * pad,
			maxs.y - axis.y * pad,
			maxs.z - axis.z * pad
		};
		axis = maxs - mins;
		axisLen = axis.Length();
	} else {
		// Degenerate capsule → pure sphere at center
		mins = cap.center;
		maxs = cap.center;
		axis = Vector_t{ 0.f, 0.f, 0.f }; // not {} — Vector_t vs Vector2D_t op= ambiguous
		axisLen = 0.f;
	}

	// Analytic ray–capsule: closest approach of ray to segment
	// Parametric ray: eye + t*dir, t>=0
	const Vector_t w0 = eye - mins;
	float t = 0.f;
	Vector_t closest{};

	if (axisLen < 1e-4f) {
		// Sphere
		const float b = Dot3(w0, dir);
		const float c = Dot3(w0, w0) - rSqr;
		const float disc = b * b - c;
		if (disc < 0.f)
			return false;
		const float s = std::sqrt(disc);
		t = -b - s;
		if (t < 0.f)
			t = -b + s;
		if (t < 0.f)
			return false;
		closest = {
			eye.x + dir.x * t,
			eye.y + dir.y * t,
			eye.z + dir.z * t
		};
	} else {
		// Segment axis + radius (same as before, but t along unit dir not 0..8192 sc)
		const Vector_t u = dir;
		const Vector_t v = axis;
		const Vector_t w = w0;
		const float a = Dot3(u, u);
		const float b = Dot3(u, v);
		const float c = Dot3(v, v);
		const float d = Dot3(u, w);
		const float e = Dot3(v, w);
		const float denom = a * c - b * b;
		float sc, tc;
		if (denom < 1e-8f) {
			sc = 0.f;
			tc = (c > 1e-8f) ? (e / c) : 0.f;
		} else {
			sc = (b * e - c * d) / denom;
			tc = (a * e - b * d) / denom;
		}
		// sc is distance along unit dir (not clamped to segment param of ray length)
		tc = std::clamp(tc, 0.f, 1.f);
		if (sc < 0.f)
			sc = 0.f;
		const Vector_t pRay{
			eye.x + u.x * sc,
			eye.y + u.y * sc,
			eye.z + u.z * sc
		};
		const Vector_t pAxis{
			mins.x + v.x * tc,
			mins.y + v.y * tc,
			mins.z + v.z * tc
		};
		const float dx = pRay.x - pAxis.x;
		const float dy = pRay.y - pAxis.y;
		const float dz = pRay.z - pAxis.z;
		if (dx * dx + dy * dy + dz * dz > rSqr)
			return false;
		t = sc;
		closest = pRay;
	}

	if (t < 1.f || t > 8192.f)
		return false;
	if (!IsValidPos(closest))
		return false;
	outT = t;
	outPoint = closest;
	return true;
}

bool RayHitsConfiguredHitbox(
	const C_CSPlayerPawn* pawn,
	int hb,
	const Vector_t& eye,
	const Vector_t& dir,
	float radiusScale,
	float& outT,
	Vector_t& outPoint,
	float centerBias)
{
	outT = 0.f;
	outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!Mem::ValidEntity(pawn) || hb < 0 || hb >= Config::HB_COUNT)
		return false;

	FrameCapsuleCache& fc = AcquireFrameCache(pawn);
	uintptr_t hbArr = 0;
	int hbCount = 0;
	if (fc.setOk) {
		hbArr = fc.hbArr;
		hbCount = fc.hbCount;
	} else if (ReadHitboxSet(const_cast<C_CSPlayerPawn*>(pawn), hbArr, hbCount)) {
		fc.hbArr = hbArr;
		fc.hbCount = hbCount;
		fc.setOk = true;
	} else {
		Vector_t center{};
		if (!GetHitboxPoint(pawn, hb, center))
			return false;
		Capsule fake{};
		fake.mins = center;
		fake.maxs = center;
		fake.center = center;
		fake.radius = MultipointRadius(hb);
		fake.ok = true;
		return RayHitsCapsule(eye, dir, fake, radiusScale, outT, outPoint, centerBias);
	}

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	if (!GetBoneArray(pawn, boneArray, origin, height))
		GetBoneArrayReadonly(pawn, boneArray, origin, height);

	int indices[24]{};
	const int nList = CollectStudioIndices(fc, hb, indices, 24);

	float bestT = 1.0e12f;
	Vector_t bestPt{};
	bool found = false;
	for (int i = 0; i < nList; ++i) {
		const int si = indices[i];
		Capsule c{};
		if (!BuildCapsuleFromStudio(pawn, boneArray, hbArr, si, c))
			continue;
		float t = 0.f;
		Vector_t pt{};
		if (!RayHitsCapsule(eye, dir, c, radiusScale, t, pt, centerBias))
			continue;
		if (t >= bestT)
			continue;
		bestT = t;
		bestPt = pt;
		found = true;
	}
	// Cached group capsule as last resort
	if (!found) {
		Capsule group{};
		if (GetHitboxCapsule(pawn, hb, group) && group.ok)
			found = RayHitsCapsule(eye, dir, group, radiusScale, bestT, bestPt, centerBias);
	}
	if (!found)
		return false;
	outT = bestT;
	outPoint = bestPt;
	return true;
}

float MultipointRadius(int hb) {
	switch (hb) {
	case Config::HB_HEAD:    return 4.2f;
	case Config::HB_NECK:    return 3.4f;
	case Config::HB_CHEST:   return 8.0f;
	case Config::HB_STOMACH: return 7.2f;
	case Config::HB_PELVIS:  return 6.8f;
	case Config::HB_ARMS:    return 3.2f;
	case Config::HB_LEGS:    return 4.5f;
	case Config::HB_FEET:    return 3.6f;
	default:                 return 5.5f;
	}
}

float TriggerHitboxRadius(int hb) {
	switch (hb) {
	case Config::HB_HEAD:    return 2.55f;
	case Config::HB_NECK:    return 2.10f;
	case Config::HB_CHEST:   return 4.80f;
	case Config::HB_STOMACH: return 4.40f;
	case Config::HB_PELVIS:  return 4.20f;
	case Config::HB_ARMS:    return 2.40f;
	case Config::HB_LEGS:    return 3.00f;
	case Config::HB_FEET:    return 2.20f;
	default:                 return 3.00f;
	}
}

float MultipointRadius(const C_CSPlayerPawn* pawn, int hb) {
	Capsule c{};
	if (GetHitboxCapsule(pawn, hb, c) && c.radius > 0.5f)
		return c.radius;
	return MultipointRadius(hb);
}

float TriggerHitboxRadius(const C_CSPlayerPawn* pawn, int hb) {
	Capsule c{};
	if (GetHitboxCapsule(pawn, hb, c) && c.radius > 0.5f) {
		const float r = c.radius * 0.60f;
		return (std::max)(r, 1.5f);
	}
	return TriggerHitboxRadius(hb);
}

static void NormalizeSafe(Vector_t& v, const Vector_t& fallback) {
	const float len = v.Length();
	if (len > 1e-4f) {
		v.x /= len; v.y /= len; v.z /= len;
	} else {
		v = fallback;
	}
}

int CollectHitboxMultipoints(
	const C_CSPlayerPawn* pawn,
	int hb,
	float scale,
	Vector_t* out,
	int maxOut,
	const Vector_t* shootPos,
	float bloom,
	bool targetAirborne)
{
	if (!pawn || !out || maxOut < 1)
		return 0;

	Capsule cap{};
	Vector_t center{};
	float radius0 = MultipointRadius(hb);
	Vector_t axisDir{ 0.f, 0.f, 1.f };
	float halfLen = 0.f;

	if (GetHitboxCapsule(pawn, hb, cap) && cap.ok) {
		center = cap.center;
		radius0 = (std::max)(cap.radius, 0.5f);
		Vector_t axis = cap.maxs - cap.mins;
		halfLen = axis.Length() * 0.5f;
		if (halfLen > 0.1f) {
			axisDir = axis;
			NormalizeSafe(axisDir, Vector_t{ 0.f, 0.f, 1.f });
		}
	} else if (!GetHitboxPoint(pawn, hb, center)) {
		return 0;
	}

	out[0] = center;
	int n = 1;

	float s = std::clamp(scale, 0.f, 1.f);
	if (targetAirborne)
		s = (std::min)(s, 0.70f);

	if (shootPos && bloom >= 0.f && radius0 > 1e-4f) {
		const float dist = center.Distance(*shootPos);
		const float usable = (std::max)(radius0 - dist * bloom, 0.f);
		const float dyn = std::clamp(usable / radius0, 0.f, 1.f);
		s = (std::min)(s, dyn);
	}

	if (s < 0.02f || maxOut < 2)
		return n;

	const float r = radius0 * s;

	Vector_t toShooter{ 0.f, 0.f, 0.f };
	if (shootPos) {
		toShooter = { shootPos->x - center.x, shootPos->y - center.y, shootPos->z - center.z };
	} else {
		Vector_t origin{};
		if (GetOrigin(pawn, origin))
			toShooter = { origin.x - center.x, origin.y - center.y, 0.f };
	}
	NormalizeSafe(toShooter, Vector_t{ 1.f, 0.f, 0.f });

	Vector_t radial = toShooter - axisDir * (toShooter.x * axisDir.x + toShooter.y * axisDir.y + toShooter.z * axisDir.z);
	NormalizeSafe(radial, toShooter);

	Vector_t side{
		axisDir.y * radial.z - axisDir.z * radial.y,
		axisDir.z * radial.x - axisDir.x * radial.z,
		axisDir.x * radial.y - axisDir.y * radial.x
	};
	NormalizeSafe(side, Vector_t{ 0.f, 1.f, 0.f });

	auto push = [&](const Vector_t& p) {
		if (n >= maxOut)
			return;
		out[n++] = p;
	};

	push(center + radial * r);
	push(center + side * r);
	push(center + side * (-r));

	if (halfLen > 0.5f) {
		const Vector_t top = center + axisDir * halfLen;
		const Vector_t bot = center + axisDir * (-halfLen);
		push(top + radial * (r * 0.85f));
		push(bot + radial * (r * 0.85f));
		if (hb == Config::HB_HEAD) {
			push(top + axisDir * (r * 0.35f));
			push(top + side * (r * 0.7071f) + radial * (r * 0.35f));
			push(top + side * (-r * 0.7071f) + radial * (r * 0.35f));
		} else {
			push(center + axisDir * (halfLen * 0.55f) + radial * (r * 0.75f));
			push(center + axisDir * (-halfLen * 0.55f) + radial * (r * 0.75f));
		}
	} else if (hb == Config::HB_HEAD) {
		push(center + axisDir * r);
		push(center + radial * (-r * 0.65f) + axisDir * (r * 0.35f));
		push(center + side * (r * 0.7071f) + axisDir * (r * 0.7071f));
		push(center + side * (-r * 0.7071f) + axisDir * (r * 0.7071f));
	} else {
		push(center + axisDir * (r * 0.55f));
		push(center + axisDir * (-r * 0.45f));
		push(center + radial * (r * 0.75f) + side * (r * 0.55f));
		push(center + radial * (r * 0.75f) + side * (-r * 0.55f));
	}

	return n;
}

} // namespace Bones
