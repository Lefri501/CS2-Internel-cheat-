#include "hitchance.h"

#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../bones/bones.h"
#include "../../config/config.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace HitChance {
namespace {

// IDA client.dll verified 2026-07-16 (session 97e68540 + patterns.hpp):
//   GetInaccuracy  @ 0x1807CE850
//   GetSpread      @ 0x1807CFBA0  mode@+0x17D8; VData@+0x388
//   CalcSpread     @ 0x180CB77A0 live (pattern; dump RVA 0xCB8D70 is wrong fn)
//     sig: void(u16 def, int nBullets, int mode, u32 seed, float inac, float spr,
//               float recoil, float* outX, float* outY) — IDA RandomSeed(seed) then ran1
//   SPREADSEEDGEN  @ 0x180CB6E80  SHA1(floor(p*2)*0.5, floor(y*2)*0.5, tick) — roll ignored
//   dump aliases NoSpread1/ComputeRandomSeed/SpreadSeedGen → same fn (was wrong rva 0xCB8450)
//   FX_FireBullets @ 0x180CB6F30  local: seed=a7+1; nBullets @ VData+0x738 (1848)
//   FireBullet     @ 0x1808474E0  dir = fwd - right*sx + up*sy
//   ComputeAimPunchFire @ 0x180812CC0  services punch @ fire GameTime
//   GetRemovedAimPunch  @ 0x18088BBB0  services@pawn+0x14B8
//   UpdateTurningInAccuracy @ 0x1807EA5B0
//   GetWeaponInAccuracyRecoveryTime @ 0x1807CF600
// Seed path: SPREADSEEDGEN(punched, tick) → CalcSpread(..., seed+1, ...)

using FnGetInaccuracy = float(__fastcall*)(void* weapon, float* out1, float* out2);
using FnGetSpread = float(__fastcall*)(void* weapon);
// IDA: __int64 __fastcall SPREADSEEDGEN(__int64 unused, QAngle_t* ang, int tick)
using FnComputeRandomSeed = std::uint32_t(__fastcall*)(void* unused, const QAngle_t* angles, int attackTick);
// IDA: void CalcSpread(u16 def, int nBullets, int mode, u32 seed, float inac, float spr,
//                      float recoil, float* outX, float* outY)
using FnCalcSpread = void(__fastcall*)(
	std::uint16_t itemDef, int nBullets, int mode, std::uint32_t seed,
	float inac, float spread, float recoilIdx, float* outX, float* outY);
using FnUpdateTurningInAccuracy = void(__fastcall*)(void* weapon);
using FnGetRecoveryTime = float(__fastcall*)(void* weapon);

FnGetInaccuracy g_getInaccuracy = nullptr;
FnGetSpread g_getSpread = nullptr;
FnComputeRandomSeed g_computeSeed = nullptr;
FnCalcSpread g_calcSpread = nullptr;
FnUpdateTurningInAccuracy g_updateTurning = nullptr;
FnGetRecoveryTime g_getRecoveryTime = nullptr;
// IDA GetRemovedAimPunch(pawn, out) — rcx rewritten to services@+0x14B8; rdx/r8 pass through to 812D90
using FnGetRemovedAimPunch = void*(__fastcall*)(void* pawn, QAngle_t* out, char flag);
FnGetRemovedAimPunch g_getRemovedAimPunch = nullptr;
// IDA sub_180812CC0(services, out, fireGameTime, flag) — CSBaseGunFire punch (new aimpunch)
// Same dual-track composer as GetRemovedAimPunch/812D90, but timebase = fire tick.
using FnComputeAimPunchFire = float*(__fastcall*)(void* services, QAngle_t* out, void* fireGameTime, int flag);
FnComputeAimPunchFire g_computeAimPunchFire = nullptr;
std::uint32_t g_aimPunchServicesOff = 0x14B8;
void* g_fxFireBullets = nullptr;
bool g_ready = false;

// Exact patterns from patterns.hpp (IDA-hit confirmed, session 3f497758)
constexpr const char* kPatGetInaccuracy =
	"48 89 5C 24 10 55 56 57 48 81 EC B0 00 00 00 44";
constexpr const char* kPatGetInaccuracyLoose =
	"48 89 5C 24 ? 55 56 57 48 81 EC ? ? ? ? 44 0F 29";
constexpr const char* kPatGetSpread =
	"48 83 EC 38 48 63 91 D8 17 00 00 48 8B 81 88 03";
constexpr const char* kPatGetSpreadLoose =
	"48 83 EC ? 48 63 91 ? ? ? ? 48 8B 81";
constexpr const char* kPatSpreadSeedGen =
	"48 89 5C 24 08 57 48 81 EC F0 00 00 00 F3 0F 10 0A 48 8D 8C 24 10 01 00 00 41 8B D8 48 8B FA E8";
constexpr const char* kPatSpreadSeedGenLoose =
	"48 89 5C 24 08 57 48 81 EC F0 00 00 00 F3 0F 10";
constexpr const char* kPatCalcSpread =
	"48 8B C4 48 89 58 08 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 81 EC E0";
// Keep EC E0 — without stack size this prologue matches 70+ unrelated fns.
constexpr const char* kPatCalcSpreadLoose =
	"48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 41 54 41 55 41 56 41 57 48 81 EC E0";
// UpdateTurningInAccuracy @ 0x1807EA5B0 — unique via angle-history lea [rsi+17E0h]
// (short prologue also matches unrelated 0x1807E7330)
constexpr const char* kPatUpdateTurning =
	"40 56 48 81 EC 80 00 00 00 48 8B F1 BA FF FF FF FF 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 85 C0 75 ? 48 8B 05 ? ? ? ? 48 8B 40 ? 80 38 00 0F 84 ? ? ? ? 48 89 BC 24 ? ? ? ? 48 8D BE E0 17 00 00";
// dump: GetWeaponInAccuracyRecoveryTime
constexpr const char* kPatRecoveryTime =
	"48 89 5C 24 ? 57 48 83 EC 30 48 8B D9 E8 ? ? ? ? 48 8B F8 48 85 C0 75";
// dump patterns.hpp::FX_FireBullets
constexpr const char* kPatFxFireBullets =
	"48 8B C4 4C 89 48 20 48 89 50 10 55 53 57 41 54 41 55 48 8D A8 58 FB FF";
// dump patterns.hpp::GetRemovedAimPunch_E8 — IDA 0x18088BBB0
constexpr const char* kPatGetRemovedAimPunch =
	"40 53 48 83 EC 20 48 8B 89 B8 14 00 00 48 8B DA";
constexpr const char* kPatGetRemovedAimPunchLoose =
	"40 53 48 83 EC 20 48 8B 89 ? ? ? ? 48 8B DA E8";
// IDA sub_180812CC0 — fire-data punch. Tight only: loose (no 48 8B D9) also
// hits 0x18156BE20 (GCS GetMessage) — never fall back to that.
constexpr const char* kPatComputeAimPunchFire =
	"4C 8B DC 49 89 5B ? 49 89 6B ? 49 89 73 ? 57 48 83 EC 70 48 8B D9";
// IDA FillGunFireData / CSBaseGunFire fill @ 0x1807CFDD0 (unique, session 796bbc15)
constexpr const char* kPatFillGunFireData =
	"48 8B C4 55 53 56 57 41 55 48 8D 68 98 48 81 EC 40 01 00 00";
// Weapon caches last fire punch at +6940 (12 bytes) — fill writes these; we restore.
constexpr std::uint32_t kWeaponFirePunchCacheOff = 6940;

// IDA 7CFDD0 out blob (unsigned int* a2) — verified 2026-07-17.
#pragma pack(push, 1)
struct GunFireDataRaw {
	int tick;            // +0
	float frac;          // +4
	std::uint32_t unk8;  // +8
	int t12;             // +12
	float t16;           // +16
	float eyeX, eyeY;    // +20
	float eyeZ;          // +28
	float angX, angY;    // +32  (view + punch after fill)
	float angZ;          // +40
	float punchX, punchY;// +44
	float punchZ;        // +52
	std::uint8_t hasHist;// +56
	std::uint8_t flag57; // +57
	std::uint8_t pad58[2];
	int sourceMode;      // +60
	std::uint8_t tail[64];
};
#pragma pack(pop)
static_assert(sizeof(GunFireDataRaw) >= 64, "GunFireDataRaw");

using FnFillGunFireData = void*(__fastcall*)(void* weapon, void* outFireData, int mode);
FnFillGunFireData g_fillGunFireData = nullptr;

constexpr float kTwoPi = 6.28318530717958647692f;
// Full capsule for HC (was 0.55 — rejected spray body clips)
constexpr float kHcCapsuleScale = 1.0f;
constexpr float kHcCenterBias = 1.0f;
// Exact seed verify (legacy callers). NoSpread::TryView no longer re-calls Exact.
constexpr float kExactCapsuleScale = 1.0f;
constexpr float kExactCenterBias = 0.96f;

// tier0 CUniformRandomStream (ran1) — IDA RandomSeed/RandomFloat in CalcSpread
struct ValveRng {
	int state = 0;
	int index = 0;
	int table[32]{};
	bool seeded = false;

	static int Lcg(int v) {
		return 16807 * (v % 127773) - 2836 * (v / 127773);
	}

	void Seed(int s) {
		state = -std::abs(s);
		if (state == 0)
			state = -1;
		index = 0;
		seeded = false;
	}

	int Generate() {
		if (!seeded) {
			int v = -state;
			if (v < 1)
				v = 1;
			for (int j = 39; j >= 0; --j) {
				v = Lcg(v);
				if (v < 0)
					v += 2147483647;
				if (j < 32)
					table[j] = v;
			}
			state = v;
			index = table[0];
			seeded = true;
		}
		state = Lcg(state);
		if (state < 0)
			state += 2147483647;
		const int idx = index / 0x4000000;
		index = table[idx];
		table[idx] = state;
		return index;
	}

	float RandomFloat(float lo, float hi) {
		const float norm = (std::min)(0.99999988f, static_cast<float>(Generate()) * 4.6566129e-10f);
		return lo + norm * (hi - lo);
	}
};

float SehGetInaccuracy(C_CSWeaponBase* weapon) {
	if (!g_getInaccuracy || !weapon || !Mem::ValidEntity(weapon))
		return -1.f;
	// IDA 0x1807CE850: null-checks a2/a3; still pass locals (vtable UpdateAccuracyPenalty).
	float outMove = 0.f, outAir = 0.f;
	__try {
		return g_getInaccuracy(weapon, &outMove, &outAir);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1.f;
	}
}

float SehGetSpread(C_CSWeaponBase* weapon) {
	if (!g_getSpread || !weapon || !Mem::ValidEntity(weapon))
		return -1.f;
	__try {
		return g_getSpread(weapon);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1.f;
	}
}

// Local CalcSpread — IDA 0x180CB77A0 (R8 secondary + Negev early-spray reshape).
// Does NOT model weapon_accuracy_* force convars (IDA v14/v16 force 1.0).
// Search path only — final ExactShotHits always uses game CalcSpread when available.
// Seeds once, then emits nBullets (sx,sy) pairs matching game multi-pellet loop.
void CalcSpreadValve(
	std::uint32_t seed,
	float inac,
	float spr,
	float recoilIdx,
	std::uint16_t itemDef,
	int mode,
	int nBullets,
	float* outX,
	float* outY)
{
	if (!outX || !outY)
		return;
	if (nBullets < 1)
		nBullets = 1;
	if (nBullets > 16)
		nBullets = 16;

	ValveRng rng;
	rng.Seed(static_cast<int>(seed));

	for (int b = 0; b < nBullets; ++b) {
		float inacR = rng.RandomFloat(0.f, 1.f);
		const float inacA = rng.RandomFloat(0.f, kTwoPi);

		// itemDef 64 = R8, mode 1 = secondary; 28 = Negev
		if (itemDef == 64 && mode == 1)
			inacR = 1.f - (inacR * inacR);
		else if (itemDef == 28 && recoilIdx < 3.f) {
			float v = inacR;
			int c = 3;
			do {
				--c;
				v *= v;
			} while (static_cast<float>(c) > recoilIdx);
			inacR = 1.f - v;
		}
		inacR *= inac;

		float sprR = rng.RandomFloat(0.f, 1.f);
		const float sprA = rng.RandomFloat(0.f, kTwoPi);
		if (itemDef == 64 && mode == 1)
			sprR = 1.f - (sprR * sprR);
		else if (itemDef == 28 && recoilIdx < 3.f) {
			float v = sprR;
			int c = 3;
			do {
				--c;
				v *= v;
			} while (static_cast<float>(c) > recoilIdx);
			sprR = 1.f - v;
		}
		sprR *= spr;

		outX[b] = std::cos(inacA) * inacR + std::cos(sprA) * sprR;
		outY[b] = std::sin(inacA) * inacR + std::sin(sprA) * sprR;
	}
}

float EstimateInaccuracy(C_CSWeaponBase* weapon, C_CSPlayerPawn* local) {
	float inac = 0.015f;
	if (weapon && Mem::ValidEntity(weapon)) {
		__try {
			inac = weapon->m_fAccuracyPenalty();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			inac = 0.015f;
		}
	}
	if (!std::isfinite(inac) || inac < 0.f)
		inac = 0.01f;
	if (local) {
		// Live pawn — do not use Pred:: (HC may run outside Pred window)
		std::uint32_t flags = 0;
		Vector_t vel{};
		__try {
			flags = local->m_fFlags();
			vel = local->m_vecAbsVelocity();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			flags = 0;
			vel = Vector_t{ 0.f, 0.f, 0.f };
		}
		const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);
		if (!(flags & FL_ONGROUND))
			inac += 0.16f;
		else if (speed > 135.f)
			inac += 0.06f + (speed - 135.f) * 0.00035f;
		if (weapon && Mem::ValidEntity(weapon)) {
			float turn = 0.f;
			__try {
				turn = weapon->m_flTurningInaccuracy();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				turn = 0.f;
			}
			if (std::isfinite(turn) && turn > 0.f)
				inac += turn;
		}
	}
	return std::clamp(inac, 0.f, 1.f);
}

std::uint8_t* FindClient(const char* pat) {
	auto* p = M::FindPattern("client.dll", pat);
	if (!p)
		p = M::FindPattern("client", pat);
	return p;
}

void SehUpdateTurning(C_CSWeaponBase* weapon) {
	if (!g_updateTurning || !weapon)
		return;
	__try {
		g_updateTurning(weapon);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

float SehGetRecoveryTime(C_CSWeaponBase* weapon) {
	if (!g_getRecoveryTime || !weapon)
		return -1.f;
	__try {
		return g_getRecoveryTime(weapon);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1.f;
	}
}

bool ReadWeaponBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local, float& inac, float& spr) {
	inac = -1.f;
	spr = -1.f;
	if (!weapon || !Mem::ValidEntity(weapon))
		return false;
	// Refresh turning bloom field before GetInaccuracy (game also does this each tick)
	SehUpdateTurning(weapon);
	inac = SehGetInaccuracy(weapon);
	spr = SehGetSpread(weapon);
	if (!std::isfinite(inac) || inac < 0.f)
		inac = EstimateInaccuracy(weapon, local);
	if (!std::isfinite(spr) || spr < 0.f) {
		// GetSpread is mode-indexed VData; fallback small base spread
		spr = 0.004f;
		float p = 0.f;
		__try {
			p = weapon->m_fAccuracyPenalty();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			p = 0.f;
		}
		if (std::isfinite(p) && p > 0.f)
			spr = (std::min)(0.08f, p * 0.15f + 0.002f);
	}
	// Game clamps inac to 1.0; keep full spray range (do NOT clamp spr to 0.15)
	inac = std::clamp(inac, 0.f, 1.f);
	spr = std::clamp(spr, 0.f, 1.f);
	return std::isfinite(inac) && std::isfinite(spr);
}

// Returns false only on SEH / missing fn. seed==0 is a valid SHA1 result.
bool SehComputeSeed(const QAngle_t& angles, int attackTick, std::uint32_t& outSeed) {
	outSeed = 0;
	if (!g_computeSeed)
		return false;
	QAngle_t a = angles;
	a.z = 0.f;
	__try {
		outSeed = g_computeSeed(nullptr, &a, attackTick);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

std::uint32_t SehComputeSeedOrZero(const QAngle_t& angles, int attackTick) {
	std::uint32_t s = 0;
	SehComputeSeed(angles, attackTick, s);
	return s;
}

// IDA/schema: CCSWeaponBaseVData::m_nNumBullets @ +0x738 (shotguns 8–10).
int WeaponBulletCount(C_CSWeaponBase* weapon) {
	if (!weapon)
		return 1;
	CCSWeaponBaseVData* vd = weapon->Data();
	if (!vd)
		return 1;
	int n = 1;
	__try {
		n = vd->m_nNumBullets();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 1;
	}
	if (n < 1)
		return 1;
	if (n > 16)
		return 16;
	return n;
}

bool SehCalcSpreadGameN(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	int nBullets,
	float* outX,
	float* outY)
{
	if (!g_calcSpread || !weapon || !outX || !outY)
		return false;
	if (nBullets < 1)
		nBullets = 1;
	if (nBullets > 16)
		nBullets = 16;
	__try {
		const std::uint16_t def = weapon->m_iItemDefinitionIndex();
		const int mode = weapon->m_weaponMode();
		const float recoil = weapon->m_flRecoilIndex();
		g_calcSpread(def, nBullets, mode, seed, inac, spr, recoil, outX, outY);
		for (int i = 0; i < nBullets; ++i) {
			if (!std::isfinite(outX[i]) || !std::isfinite(outY[i]))
				return false;
		}
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehCalcSpreadGame(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	float* outX,
	float* outY)
{
	float xs[16]{}, ys[16]{};
	if (!SehCalcSpreadGameN(weapon, seed, inac, spr, 1, xs, ys))
		return false;
	*outX = xs[0];
	*outY = ys[0];
	return true;
}

// Prefer game CalcSpread for exact seed path. MC must NOT call game CalcSpread —
// it stomps tier0 RandomSeed global and desyncs the real fire that follows.
void SpreadXY_Mc(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	int nBullets,
	float* outX,
	float* outY)
{
	std::uint16_t def = 0;
	int mode = 0;
	float recoil = 0.f;
	if (weapon && Mem::ValidEntity(weapon)) {
		__try {
			def = weapon->m_iItemDefinitionIndex();
			mode = weapon->m_weaponMode();
			recoil = weapon->m_flRecoilIndex();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			def = 0;
			mode = 0;
			recoil = 0.f;
		}
	}
	if (!std::isfinite(recoil))
		recoil = 0.f;
	CalcSpreadValve(seed, inac, spr, recoil, def, mode, nBullets, outX, outY);
}

// First pellet: game CalcSpread first (bit-exact vs server), local ValveRng fallback.
// Old order (local first) made Solve accept then Exact (game) reject → no shoot.
int SpreadXY(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	float* outX,
	float* outY)
{
	if (SehCalcSpreadGame(weapon, seed, inac, spr, outX, outY))
		return 0;
	float xs[16]{}, ys[16]{};
	SpreadXY_Mc(weapon, seed, inac, spr, 1, xs, ys);
	*outX = xs[0];
	*outY = ys[0];
	return 1;
}

// Local-only first pellet (search path — no tier0 RandomSeed stomp).
void SpreadXY_Local(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	float* outX,
	float* outY)
{
	float xs[16]{}, ys[16]{};
	SpreadXY_Mc(weapon, seed, inac, spr, 1, xs, ys);
	*outX = xs[0];
	*outY = ys[0];
}

// IDA CB1340: floor(ang*2)*0.5 — key for seed cache bins.
float QuantizeHalfDegHC(float a)
{
	while (a > 180.f) a -= 360.f;
	while (a < -180.f) a += 360.f;
	return std::floor(a * 2.f) * 0.5f;
}

// Frame-local reuse for FindBestShot multi-cand (same tick/bin/bloom).
struct SeedDirCacheSlot {
	int tick = 0;
	int qpx = 0; // (int)round(quant*2) — integer half-deg key
	int qpy = 0;
	unsigned seedAdd = 0;
	std::uint16_t def = 0;
	int mode = 0;
	float inac = 0.f;
	float spr = 0.f;
	float recoil = 0.f;
	bool local = false;
	std::uint32_t seed = 0;
	float sx = 0.f;
	float sy = 0.f;
	bool valid = false;
};

constexpr int kSeedDirCacheN = 48;
SeedDirCacheSlot g_seedDirCache[kSeedDirCacheN]{};
int g_seedDirCacheNext = 0;

static bool BloomNear(float a, float b)
{
	// Same-tick reuse — GetInaccuracy re-reads can jitter slightly.
	return std::fabs(a - b) <= 1e-4f;
}

bool SeedDirCacheLookup(
	int tick, int qpx, int qpy, unsigned seedAdd,
	std::uint16_t def, int mode, float inac, float spr, float recoil, bool local,
	std::uint32_t& outSeed, float& outSx, float& outSy)
{
	for (int i = 0; i < kSeedDirCacheN; ++i) {
		const SeedDirCacheSlot& s = g_seedDirCache[i];
		if (!s.valid)
			continue;
		if (s.tick != tick || s.qpx != qpx || s.qpy != qpy || s.seedAdd != seedAdd)
			continue;
		if (s.def != def || s.mode != mode || s.local != local)
			continue;
		if (!BloomNear(s.inac, inac) || !BloomNear(s.spr, spr) || !BloomNear(s.recoil, recoil))
			continue;
		outSeed = s.seed;
		outSx = s.sx;
		outSy = s.sy;
		return true;
	}
	return false;
}

void SeedDirCacheStore(
	int tick, int qpx, int qpy, unsigned seedAdd,
	std::uint16_t def, int mode, float inac, float spr, float recoil, bool local,
	std::uint32_t seed, float sx, float sy)
{
	SeedDirCacheSlot& s = g_seedDirCache[g_seedDirCacheNext % kSeedDirCacheN];
	g_seedDirCacheNext = (g_seedDirCacheNext + 1) % kSeedDirCacheN;
	s.tick = tick;
	s.qpx = qpx;
	s.qpy = qpy;
	s.seedAdd = seedAdd;
	s.def = def;
	s.mode = mode;
	s.inac = inac;
	s.spr = spr;
	s.recoil = recoil;
	s.local = local;
	s.seed = seed;
	s.sx = sx;
	s.sy = sy;
	s.valid = true;
}

bool NormalizeDir(Vector_t& dir) {
	const float lenSqr = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
	if (!std::isfinite(lenSqr) || lenSqr < 1e-12f)
		return false;
	const float inv = 1.f / std::sqrt(lenSqr);
	dir.x *= inv;
	dir.y *= inv;
	dir.z *= inv;
	return std::isfinite(dir.x) && std::isfinite(dir.y) && std::isfinite(dir.z);
}

bool CapsuleHit(
	const Vector_t& eye,
	const Vector_t& dir,
	C_CSPlayerPawn* target,
	int hitbox,
	float scale,
	float bias)
{
	if (!target || hitbox < 0 || !Mem::ValidEntity(target))
		return false;
	float t = 0.f;
	Vector_t pt{};
	bool hit = false;
	__try {
		hit = Bones::RayHitsConfiguredHitbox(
			target, hitbox, eye, dir, scale, t, pt, bias);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		hit = false;
	}
	return hit;
}

// Pure math vs prebuilt capsule — no bone/studio re-resolve (HC MC hot path).
bool CapsuleHitCached(
	const Vector_t& eye,
	const Vector_t& dir,
	const Bones::Capsule& cap,
	float scale,
	float bias)
{
	if (!cap.ok || cap.radius < 0.1f)
		return false;
	float t = 0.f;
	Vector_t pt{};
	return Bones::RayHitsCapsule(eye, dir, cap, scale, t, pt, bias);
}

bool GetHitboxCapsuleSeh(C_CSPlayerPawn* target, int hitbox, Bones::Capsule& out)
{
	out = {};
	if (!target || hitbox < 0 || !Mem::ValidEntity(target))
		return false;
	__try {
		if (!Bones::GetHitboxCapsule(target, hitbox, out) || !out.ok)
			return false;
		return std::isfinite(out.radius) && out.radius > 0.1f
			&& Bones::IsValidPos(out.center);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		out = {};
		return false;
	}
}

float ReadRecoilIndexSeh(C_CSWeaponBase* weapon)
{
	if (!weapon)
		return 0.f;
	float r = 0.f;
	__try {
		r = weapon->m_flRecoilIndex();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.f;
	}
	return std::isfinite(r) ? r : 0.f;
}

bool BuildBulletDir(const QAngle_t& fire, float sx, float sy, Vector_t& outDir) {
	Vector_t forward{}, right{}, up{};
	fire.ToDirections(&forward, &right, &up);
	// IDA FireBullet 0x1808474E0: dir = forward - right*sx + up*sy
	// (NOT +right — that sign error caused air/run seed misses.)
	outDir = {
		forward.x - right.x * sx + up.x * sy,
		forward.y - right.y * sx + up.y * sy,
		forward.z - right.z * sx + up.z * sy
	};
	return NormalizeDir(outDir);
}

// Sample count — keep low; early-exit already decides pass/fail.
// Old 160–320 × capsule tests caused CreateMove FPS spikes with trigger HC.
int McSampleCount(float requiredPct, float bloom) {
	int n = 64;
	if (requiredPct >= 60.f || bloom > 0.06f)
		n = 96;
	if (requiredPct >= 75.f || bloom > 0.12f)
		n = 112;
	if (requiredPct >= 90.f || bloom > 0.20f)
		n = 128;
	return n;
}

} // namespace

bool Init() {
	auto* pInac = FindClient(kPatGetInaccuracy);
	if (!pInac) pInac = FindClient(kPatGetInaccuracyLoose);
	auto* pSpr = FindClient(kPatGetSpread);
	if (!pSpr) pSpr = FindClient(kPatGetSpreadLoose);
	auto* pSeed = FindClient(kPatSpreadSeedGen);
	if (!pSeed) pSeed = FindClient(kPatSpreadSeedGenLoose);
	auto* pCalc = FindClient(kPatCalcSpread);
	if (!pCalc) pCalc = FindClient(kPatCalcSpreadLoose);
	auto* pTurn = FindClient(kPatUpdateTurning);
	auto* pRec = FindClient(kPatRecoveryTime);
	auto* pFx = FindClient(kPatFxFireBullets);
	auto* pPunch = FindClient(kPatGetRemovedAimPunch);
	if (!pPunch) pPunch = FindClient(kPatGetRemovedAimPunchLoose);
	auto* pPunchFire = FindClient(kPatComputeAimPunchFire);
	auto* pFillFire = FindClient(kPatFillGunFireData);

	g_getInaccuracy = reinterpret_cast<FnGetInaccuracy>(pInac);
	g_getSpread = reinterpret_cast<FnGetSpread>(pSpr);
	g_computeSeed = reinterpret_cast<FnComputeRandomSeed>(pSeed);
	g_calcSpread = reinterpret_cast<FnCalcSpread>(pCalc);
	g_updateTurning = reinterpret_cast<FnUpdateTurningInAccuracy>(pTurn);
	g_getRecoveryTime = reinterpret_cast<FnGetRecoveryTime>(pRec);
	g_getRemovedAimPunch = reinterpret_cast<FnGetRemovedAimPunch>(pPunch);
	g_computeAimPunchFire = reinterpret_cast<FnComputeAimPunchFire>(pPunchFire);
	g_fillGunFireData = reinterpret_cast<FnFillGunFireData>(pFillFire);
	g_fxFireBullets = pFx;
	// HC needs bloom. Seed nospread still gated by SpreadSeedReady() (g_computeSeed).
	// Old g_ready=true always made Ready() lie when patterns missed.
	g_ready = (g_getInaccuracy != nullptr && g_getSpread != nullptr);

	// Services offset: prefer instruction decode, then schema, then dump FB.
	if (pPunch) {
		const std::uint32_t fromInsn = *reinterpret_cast<std::uint32_t*>(
			reinterpret_cast<std::uint8_t*>(pPunch) + 9);
		// mov rcx,[rcx+disp32] — sane field range for pawn services
		if (fromInsn >= 0x100 && fromInsn < 0x20000)
			g_aimPunchServicesOff = fromInsn;
		// Fallback: 812CC0 = 812D90 - 0xD0 (call target of GetRemovedAimPunch)
		if (!g_computeAimPunchFire) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(pPunch);
			// pattern ends ... 48 8B DA E8 rel32
			for (int i = 12; i < 24; ++i) {
				if (bytes[i] != 0xE8)
					continue;
				const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(bytes + i + 1);
				auto* p812D90 = reinterpret_cast<std::uint8_t*>(pPunch) + i + 5 + rel;
				g_computeAimPunchFire = reinterpret_cast<FnComputeAimPunchFire>(p812D90 - 0xD0);
				break;
			}
		}
	}
	{
		// Schema / dump if pattern miss or decode garbage
		const std::uint32_t sch = SchemaFinder::Get(
			hash_32_fnv1a_const("C_CSPlayerPawn->m_pAimPunchServices"));
		if (sch >= 0x100 && sch < 0x20000) {
			if (!pPunch || g_aimPunchServicesOff < 0x100 || g_aimPunchServicesOff >= 0x20000)
				g_aimPunchServicesOff = sch;
		} else if (g_aimPunchServicesOff < 0x100 || g_aimPunchServicesOff >= 0x20000) {
			g_aimPunchServicesOff = 0x14B8; // last-resort dump
		}
	}

	Con::Info("HitChance: inac=%p spr=%p seed=%p calc=%p turn=%p rec=%p punch=%p punchFire=%p fx=%p svc=0x%X",
		(void*)g_getInaccuracy, (void*)g_getSpread,
		(void*)g_computeSeed, (void*)g_calcSpread,
		(void*)g_updateTurning, (void*)g_getRecoveryTime,
		(void*)g_getRemovedAimPunch, (void*)g_computeAimPunchFire, g_fxFireBullets,
		g_aimPunchServicesOff);

	if (g_getInaccuracy && g_getSpread)
		Con::Ok("HitChance: IDA GetInaccuracy/GetSpread ready");
	else {
		Con::Error("HitChance: GetInaccuracy/GetSpread miss — penalty estimate");
		if (!g_getInaccuracy) Con::OffsetMiss("HitChance::GetInaccuracy");
		if (!g_getSpread) Con::OffsetMiss("HitChance::GetSpread");
	}

	if (g_computeSeed)
		Con::Ok("HitChance: SPREADSEEDGEN ready (seed nospread)");
	else {
		Con::Error("HitChance: SPREADSEEDGEN miss — seed nospread disabled");
		Con::OffsetMiss("HitChance::SpreadSeedGen");
	}

	if (g_calcSpread)
		Con::Ok("HitChance: CalcSpread ready");
	else {
		Con::Error("HitChance: CalcSpread miss — Valve ran1 fallback");
		Con::OffsetMiss("HitChance::CalcSpread");
	}

	if (g_updateTurning)
		Con::Ok("HitChance: UpdateTurningInAccuracy ready");
	else
		Con::OffsetMiss("HitChance::UpdateTurningInAccuracy");

	if (g_getRecoveryTime)
		Con::Ok("HitChance: GetWeaponInAccuracyRecoveryTime ready");
	else
		Con::OffsetMiss("HitChance::GetWeaponInAccuracyRecoveryTime");

	if (g_fxFireBullets)
		Con::Ok("HitChance: FX_FireBullets resolved (seed+1 path validated in IDA)");
	else
		Con::OffsetMiss("HitChance::FX_FireBullets");

	if (g_getRemovedAimPunch)
		Con::Ok("HitChance: GetRemovedAimPunch ready (legacy / HC)");
	else
		Con::OffsetMiss("HitChance::GetRemovedAimPunch");

	if (g_computeAimPunchFire)
		Con::Ok("HitChance: ComputeAimPunchFire ready (CSBaseGunFire / seed)");
	else
		Con::OffsetMiss("HitChance::ComputeAimPunchFire");

	if (g_fillGunFireData)
		Con::Ok("HitChance: FillGunFireData ready (7CFDD0 — seed tick/eye/punch)");
	else
		Con::OffsetMiss("HitChance::FillGunFireData");

	return true;
}

bool FillGunFireData(C_CSWeaponBase* weapon, GunFireData& out, int mode)
{
	out = {};
	if (!weapon || !Mem::ValidEntity(weapon))
		return false;
	if (!g_ready)
		Init();
	if (!g_fillGunFireData)
		return false;

	// Save weapon fire-punch cache (fill writes a1+6940).
	alignas(16) std::uint8_t savedPunch[12]{};
	bool saved = false;
	__try {
		auto* base = reinterpret_cast<std::uint8_t*>(weapon);
		std::memcpy(savedPunch, base + kWeaponFirePunchCacheOff, 12);
		saved = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		saved = false;
	}

	alignas(16) GunFireDataRaw raw{};
	bool filled = false;
	__try {
		g_fillGunFireData(weapon, &raw, mode);
		filled = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		filled = false;
	}

	if (saved) {
		__try {
			auto* base = reinterpret_cast<std::uint8_t*>(weapon);
			std::memcpy(base + kWeaponFirePunchCacheOff, savedPunch, 12);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	if (!filled)
		return false;

	out.tick = raw.tick;
	out.frac = std::isfinite(raw.frac) ? std::clamp(raw.frac, 0.f, 0.9999f) : 0.f;
	out.eye = Vector_t{ raw.eyeX, raw.eyeY, raw.eyeZ };
	out.angles = QAngle_t{ raw.angX, raw.angY, raw.angZ };
	out.punch = QAngle_t{ raw.punchX, raw.punchY, raw.punchZ };
	out.sourceMode = raw.sourceMode;
	out.hasHistory = raw.hasHist != 0;
	out.ok = out.tick > 0
		&& out.angles.IsValid()
		&& std::isfinite(out.eye.x) && std::isfinite(out.eye.y) && std::isfinite(out.eye.z);
	return out.ok;
}

bool ReadSeedFirePunch(
	C_CSPlayerPawn* local,
	C_CSWeaponBase* weapon,
	int seedTick,
	float tickFrac,
	QAngle_t& out)
{
	out = {};
	// Option C: punch must match the tick we stamp — fill only when tick aligns.
	if (weapon && Mem::ValidEntity(weapon) && seedTick > 0) {
		GunFireData fd{};
		if (FillGunFireData(weapon, fd, 0) && fd.punch.IsValid() && fd.tick > 0) {
			const int dTick = std::abs(fd.tick - seedTick);
			// Same or ±1 tick = same fire-data blob CSBaseGunFire used.
			if (dTick <= 1) {
				out = fd.punch;
				out.z = 0.f;
				// Zero punch is valid (first shot) — still use fill timebase.
				return true;
			}
		}
	}
	// Exact fire tick via 812CC0 (hist frac for subtick punch).
	return ReadAimPunchForFire(local, seedTick, out, tickFrac);
}

bool BuildSeedFireContext(
	C_CSPlayerPawn* local,
	C_CSWeaponBase* weapon,
	const QAngle_t& viewAngles,
	int seedTickHint,
	float tickFracHint,
	const Vector_t& eyeHint,
	SeedFireContext& out)
{
	out = {};
	if (!viewAngles.IsValid())
		return false;

	out.tick = seedTickHint;
	out.frac = std::isfinite(tickFracHint)
		? std::clamp(tickFracHint, 0.f, 0.9999f) : 0.f;
	out.eye = eyeHint;

	int fillTick = 0;
	float fillFrac = out.frac;
	bool fillPunchOk = false;
	QAngle_t fillPunch{};

	// 1) Game fill — eye/punch from CSBaseGunFire fill (IDA 7CFDD0).
	// NEVER steal nPlayerTickCount when hist already has a tick.
	// SPREADSEEDGEN (0x180CB6E80) hashes hist nPlayerTickCount; solving on fill
	// tick then stamping fill into hist (or worse: solving fill, stamping hist)
	// desyncs seed → client hit / server miss. Only adopt fill tick if no hint.
	if (weapon && Mem::ValidEntity(weapon)) {
		GunFireData fd{};
		if (FillGunFireData(weapon, fd, 0)) {
			out.fromFill = true;
			fillTick = fd.tick;
			if (fd.tick > 0 && seedTickHint <= 0) {
				out.tick = fd.tick;
				out.frac = fd.frac;
				fillFrac = fd.frac;
			}
			if (std::isfinite(fd.eye.x) && Bones::IsValidPos(fd.eye))
				out.eye = fd.eye;
			if (fd.punch.IsValid()) {
				fillPunch = fd.punch;
				fillPunch.z = 0.f;
				fillPunchOk = true;
			}
		}
	}

	// 2) Punch @ chosen fire tick.
	// Fill punch only if still on fill's tick (±1). Else 812CC0(out.tick, out.frac).
	if (fillPunchOk && out.tick > 0 && fillTick > 0
		&& std::abs(fillTick - out.tick) <= 1) {
		out.punch = fillPunch;
		out.punchOk = true;
		// Prefer fill frac when tick matched fill.
		if (std::isfinite(fillFrac))
			out.frac = std::clamp(fillFrac, 0.f, 0.9999f);
	}
	if (!out.punchOk && local && out.tick > 0) {
		if (ReadAimPunchForFire(local, out.tick, out.punch, out.frac)) {
			out.punch.z = 0.f;
			out.punchOk = true;
		}
	}

	// 3) Punched view = hist/cam view + punch (IDA 7C18A0 before SPREADSEEDGEN).
	out.punchedView = viewAngles;
	out.punchedView.z = 0.f;
	if (out.punchOk) {
		out.punchedView.x += out.punch.x;
		out.punchedView.y += out.punch.y;
	}
	out.punchedView.Normalize();
	out.punchedView.x = std::clamp(out.punchedView.x, -89.f, 89.f);
	out.punchedView.z = 0.f;

	out.ok = out.tick > 0 && out.punchedView.IsValid();
	return out.ok;
}

bool Ready() {
	return g_ready;
}

bool SpreadSeedReady() {
	if (!g_ready)
		Init();
	return g_computeSeed != nullptr;
}

bool Passes(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	float requiredPct,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target)
{
	if (!std::isfinite(requiredPct))
		return false;
	if (requiredPct <= 0.f)
		return true;
	if (!weapon || !target || !Mem::ValidEntity(weapon) || !Mem::ValidEntity(target))
		return false;
	if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z)
		|| !std::isfinite(aimPoint.x) || !std::isfinite(aimPoint.y) || !std::isfinite(aimPoint.z)
		|| !fireAngles.IsValid())
		return false;
	requiredPct = std::clamp(requiredPct, 0.f, 100.f);
	if (!g_ready)
		Init();

	// Live bloom from game (includes spray penalty via UpdateAccuracyPenalty).
	// Do NOT hard-fail on speed/air — GetInaccuracy already bakes move/air.
	float inac = 0.f, spr = 0.f;
	if (!ReadWeaponBloom(weapon, local, inac, spr))
		return false;

	const float bloom = inac + spr;
	const float recoil = ReadRecoilIndexSeh(weapon);
	const bool spraying = recoil > 1.5f || inac > 0.06f;

	// Build capsules ONCE — old path re-ran ForceUpdateBones + studio reads
	// per MC sample (CreateMove AV + FPS death under trigger HC).
	Bones::Capsule capPrimary{};
	if (!GetHitboxCapsuleSeh(target, hitbox, capPrimary)) {
		// No studio capsule → fail closed (old GetHitboxPoint fallback AVd under spray)
		const float dist = eye.Distance(aimPoint);
		if (dist < 1.f)
			return true;
		return false;
	}

	Vector_t center = capPrimary.center;
	if (!Bones::IsValidPos(center))
		center = aimPoint;
	const float dist = eye.Distance(center);
	if (dist < 1.f)
		return true;

	float radius = capPrimary.radius;
	if (radius < 1.f)
		radius = 4.f;

	// Absurd cone only — no optimistic geometric early-accept (was over-firing).
	{
		const float maxLat = bloom * dist;
		if (maxLat > radius * 80.f && requiredPct >= 50.f)
			return false;
	}

	// CS2: bullet dir from view+GetRemovedAimPunch. Callers pass hist/view angles.
	QAngle_t punched = fireAngles;
	if (local) {
		QAngle_t punch{};
		if (ReadAimPunch(local, punch) && punch.IsValid()) {
			punched.x += punch.x;
			punched.y += punch.y;
			punched.z = 0.f;
			punched.Normalize();
			punched.x = std::clamp(punched.x, -89.f, 89.f);
		}
	}

	Vector_t forward{}, right{}, up{};
	punched.ToDirections(&forward, &right, &up);
	if (!std::isfinite(forward.x) || !std::isfinite(right.x) || !std::isfinite(up.x))
		return false;

	const int nSamples = McSampleCount(requiredPct, bloom);
	const int needed = (std::max)(1, static_cast<int>(
		std::ceil(requiredPct * static_cast<float>(nSamples) / 100.f + 1e-4f)));

	Bones::Capsule capAlt{};
	bool haveAlt = false;
	if (spraying && requiredPct <= 70.f) {
		if (hitbox == Config::HB_HEAD || hitbox == Config::HB_NECK)
			haveAlt = GetHitboxCapsuleSeh(target, Config::HB_CHEST, capAlt);
	}

	// Multi-pellet: hit if ANY pellet lands. Local ValveRng only (game CalcSpread
	// would stomp global RandomSeed across hundreds of MC samples).
	const int nBullets = WeaponBulletCount(weapon);
	float xs[16]{};
	float ys[16]{};

	int hits = 0;
	for (int i = 0; i < nSamples; ++i) {
		const std::uint32_t seed = static_cast<std::uint32_t>(i);
		SpreadXY_Mc(weapon, seed, inac, spr, nBullets, xs, ys);

		bool hit = false;
		for (int b = 0; b < nBullets && !hit; ++b) {
			const float sx = xs[b];
			const float sy = ys[b];
			if (!std::isfinite(sx) || !std::isfinite(sy))
				continue;

			Vector_t dir{
				forward.x - right.x * sx + up.x * sy,
				forward.y - right.y * sx + up.y * sy,
				forward.z - right.z * sx + up.z * sy
			};
			if (!NormalizeDir(dir))
				continue;

			hit = CapsuleHitCached(eye, dir, capPrimary, kHcCapsuleScale, kHcCenterBias);
			if (!hit && haveAlt)
				hit = CapsuleHitCached(eye, dir, capAlt, kHcCapsuleScale, kHcCenterBias);
		}

		if (hit)
			++hits;

		const int left = nSamples - i - 1;
		if (hits >= needed)
			return true;
		if (hits + left < needed)
			return false;
	}

	return hits >= needed;
}

bool PassesSafe(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	float requiredPct,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target)
{
	bool ok = false;
	__try {
		ok = Passes(eye, fireAngles, aimPoint, hitbox, weapon, requiredPct, local, target);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	return ok;
}

bool ReadAimPunch(C_CSPlayerPawn* local, QAngle_t& out)
{
	out = {};
	if (!local || !Mem::ValidEntity(local))
		return false;
	if (!g_ready)
		Init();
	if (!g_getRemovedAimPunch)
		return false;
	QAngle_t punch{};
	__try {
		g_getRemovedAimPunch(local, &punch, 1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!punch.IsValid())
		return false;
	out = punch;
	return true;
}

// IDA CSBaseGunFire fill (7CFDD0): punch = 812CC0(services, out, fireData, 1)
// fireData+0 = GameTime (tick used by SPREADSEEDGEN). UC used m_aimPunchAngle —
// that is outdated; this is the new dual-track aimpunch at fire tick.
bool ReadAimPunchForFire(
	C_CSPlayerPawn* local, int seedTick, QAngle_t& out, float tickFrac)
{
	out = {};
	if (!local || !Mem::ValidEntity(local) || seedTick <= 0)
		return false;
	if (!g_ready)
		Init();

	QAngle_t firePunch{};
	bool fireOk = false;
	if (g_computeAimPunchFire) {
		void* services = nullptr;
		__try {
			services = *reinterpret_cast<void**>(
				reinterpret_cast<std::uint8_t*>(local) + g_aimPunchServicesOff);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			services = nullptr;
		}
		if (services && Mem::Valid(services, 0x100)) {
			// Fire data starts with GameTime_t; 812CC0 passes it as timebase to 80E9E0.
			float frac = tickFrac;
			if (!std::isfinite(frac))
				frac = 0.f;
			frac = std::clamp(frac, 0.f, 0.9999f);
			alignas(8) struct {
				int tick;
				float frac;
			} fireTime{ seedTick, frac };
			__try {
				g_computeAimPunchFire(services, &firePunch, &fireTime, 1);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				firePunch = {};
			}
			fireOk = firePunch.IsValid();
		}
	}

	// AF deagle log: punch=(0,0) on air shots with large dAng → seed desync vs
	// server. Zero fire-tick punch is Valid() but wrong mid-spray — blend in
	// GetRemovedAimPunch when fire path is near-zero.
	QAngle_t curPunch{};
	const bool curOk = ReadAimPunch(local, curPunch) && curPunch.IsValid();
	const float fireMag = fireOk ? (std::fabs(firePunch.x) + std::fabs(firePunch.y)) : 0.f;
	const float curMag = curOk ? (std::fabs(curPunch.x) + std::fabs(curPunch.y)) : 0.f;
	if (fireOk && fireMag > 0.02f) {
		out = firePunch;
		return true;
	}
	if (curOk && curMag > fireMag) {
		out = curPunch;
		return true;
	}
	if (fireOk) {
		out = firePunch;
		return true;
	}
	return curOk;
}

bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX,
	float* outSpreadY,
	unsigned seedAdd,
	float tickFrac)
{
	outDir.x = outDir.y = outDir.z = 0.f;
	if (!weapon || !Mem::ValidEntity(weapon) || !fireAngles.IsValid() || seedTick <= 0)
		return false;
	if (!g_ready)
		Init();
	if (!g_computeSeed)
		return false;

	float inac = 0.f, spr = 0.f;
	if (!ReadWeaponBloom(weapon, local, inac, spr))
		return false;
	QAngle_t punch{};
	const QAngle_t* punchPtr = nullptr;
	// IDA 7CFDD0 → 812CC0 punch, then angles += punch (7C18A0) before SPREADSEEDGEN.
	// Prefer fill punch (same hist as CSBaseGunFire); else fire-tick 812CC0.
	if (local && ReadSeedFirePunch(local, weapon, seedTick, tickFrac, punch))
		punchPtr = &punch;
	return GetBulletDirectionCached(
		fireAngles, seedTick, weapon, inac, spr, outDir, outSpreadX, outSpreadY, seedAdd, punchPtr);
}

bool GetBulletDirectionCached(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	Vector_t& outDir,
	float* outSpreadX,
	float* outSpreadY,
	unsigned seedAdd,
	const QAngle_t* aimPunch,
	bool useLocalSpread)
{
	outDir.x = outDir.y = outDir.z = 0.f;
	if (!weapon || !fireAngles.IsValid() || seedTick <= 0)
		return false;
	if (!g_ready)
		Init();
	if (!g_computeSeed)
		return false;
	if (!std::isfinite(inac) || !std::isfinite(spr))
		return false;

	// IDA: hist view + ComputeAimPunchFire → SPREADSEEDGEN(punched, tick)
	// → CalcSpread(seed+1) → FireBullet(punched, sx, sy).
	// SPREADSEEDGEN (0x180CB6E80) hashes quant pitch+yaw ONLY — roll ignored for seed.
	// FireBullet AngleVectors (0x18160ED70) USES roll for right/up — keep z for cancel.
	QAngle_t ang = fireAngles;
	if (aimPunch && aimPunch->IsValid()) {
		ang.x += aimPunch->x;
		ang.y += aimPunch->y;
		// punch has no roll; leave ang.z
	}
	// Do NOT force z=0 — roll-trick nospread needs it for dir cancel.
	if (!std::isfinite(ang.z))
		ang.z = 0.f;
	ang.Normalize();
	if (!ang.IsValid())
		return false;

	// SHA1 only sees 0.5° bins — cache key = quantized punched pitch/yaw + tick + bloom.
	// Roll not in seed key (IDA CB6E80) but affects BuildBulletDir basis.
	const float qpxF = QuantizeHalfDegHC(ang.x);
	const float qpyF = QuantizeHalfDegHC(ang.y);
	const int qpx = static_cast<int>(std::lround(qpxF * 2.f));
	const int qpy = static_cast<int>(std::lround(qpyF * 2.f));

	std::uint16_t def = 0;
	int mode = 0;
	float recoil = 0.f;
	__try {
		def = weapon->m_iItemDefinitionIndex();
		mode = weapon->m_weaponMode();
		recoil = weapon->m_flRecoilIndex();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		def = 0;
		mode = 0;
		recoil = 0.f;
	}
	if (!std::isfinite(recoil))
		recoil = 0.f;

	std::uint32_t seed = 0;
	float sx = 0.f, sy = 0.f;
	// useLocalSpread=true: search path (NoSpread::Solve 20–80×/tick).
	// MUST NOT call game CalcSpread — stomps tier0 RandomSeed → FPS death / crash.
	// Local ValveRng matches IDA ran1; SPREADSEEDGEN still uses game seedgen once.
	const bool useLocal = useLocalSpread;
	if (SeedDirCacheLookup(
			seedTick, qpx, qpy, seedAdd, def, mode, inac, spr, recoil,
			useLocal, seed, sx, sy)) {
		if (outSpreadX) *outSpreadX = sx;
		if (outSpreadY) *outSpreadY = sy;
		return BuildBulletDir(ang, sx, sy, outDir);
	}

	// Seedgen still needs full ang (game quantizes inside); same bin → same seed.
	if (!SehComputeSeed(ang, seedTick, seed))
		return false;

	if (useLocal) {
		SpreadXY_Local(weapon, seed + seedAdd, inac, spr, &sx, &sy);
	} else {
		// Single-shot verify / rare path — game CalcSpread bit-exact
		if (!SehCalcSpreadGame(weapon, seed + seedAdd, inac, spr, &sx, &sy))
			SpreadXY_Local(weapon, seed + seedAdd, inac, spr, &sx, &sy);
	}
	if (!std::isfinite(sx) || !std::isfinite(sy))
		return false;

	SeedDirCacheStore(
		seedTick, qpx, qpy, seedAdd, def, mode, inac, spr, recoil,
		useLocal, seed, sx, sy);

	if (outSpreadX)
		*outSpreadX = sx;
	if (outSpreadY)
		*outSpreadY = sy;

	return BuildBulletDir(ang, sx, sy, outDir);
}

// Build first-pellet or full multi-pellet dirs for ExactShotHits.
// Shotguns: nBullets 8–10 — any pellet hit = server damage (IDA FX loops all).
static int BuildExactPelletDirs(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	float tickFrac,
	Vector_t* outDirs,
	int maxDirs)
{
	if (!outDirs || maxDirs < 1)
		return 0;
	if (!weapon || !Mem::ValidEntity(weapon) || !fireAngles.IsValid() || seedTick <= 0)
		return 0;
	if (!g_ready)
		Init();
	if (!g_computeSeed)
		return 0;

	float inac = 0.f, spr = 0.f;
	if (!ReadWeaponBloom(weapon, local, inac, spr))
		return 0;

	// Keep roll — IDA SPREADSEEDGEN ignores z; FireBullet AngleVectors uses it.
	// Zeroing z here killed roll-trick solutions (air AWP / run deagle overspray).
	QAngle_t ang = fireAngles;
	if (!std::isfinite(ang.z))
		ang.z = 0.f;
	QAngle_t punch{};
	if (local && ReadSeedFirePunch(local, weapon, seedTick, tickFrac, punch)) {
		ang.x += punch.x;
		ang.y += punch.y;
		// punch has no roll
	}
	ang.Normalize();
	if (!ang.IsValid())
		return 0;

	std::uint32_t seed = 0;
	// Seedgen only needs pitch/yaw; pass full ang (game quantizes inside)
	if (!SehComputeSeed(ang, seedTick, seed))
		return 0;

	const int nWant = (std::min)(maxDirs, WeaponBulletCount(weapon));
	float xs[16]{}, ys[16]{};
	// Game CalcSpread first — bit-exact match with server fire path.
	// ExactShotHits is the FIRE-time verify (called ~1× per fire attempt from
	// AF/Trigger, not per search cand), so the one tier0 RandomSeed stomp per
	// verify is acceptable. Search still uses local ValveRng via
	// GetBulletDirectionCached (called 20-80× per tick, would stomp too much).
	// Rare AWP miss fixed: local ran1 reimpl can drift 1 ULP from tier0 on
	// float-boundary bins; game path guarantees identical (sx,sy) as server.
	bool anyFinite = false;
	if (SehCalcSpreadGameN(weapon, seed + 1u, inac, spr, nWant, xs, ys)) {
		for (int b = 0; b < nWant; ++b) {
			if (std::isfinite(xs[b]) && std::isfinite(ys[b])) {
				anyFinite = true;
				break;
			}
		}
	}
	if (!anyFinite) {
		// Game path unavailable (sig miss / SEH) — fallback local ValveRng.
		SpreadXY_Mc(weapon, seed + 1u, inac, spr, nWant, xs, ys);
	}

	int n = 0;
	for (int b = 0; b < nWant; ++b) {
		if (!std::isfinite(xs[b]) || !std::isfinite(ys[b]))
			continue;
		if (!BuildBulletDir(ang, xs[b], ys[b], outDirs[n]))
			continue;
		++n;
	}
	return n;
}

static void ExactCapsuleScale(
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	const Vector_t& eye,
	C_CSPlayerPawn* target,
	float& scale,
	float& bias)
{
	scale = kExactCapsuleScale;
	bias = kExactCenterBias;
	std::uint16_t def = 0;
	__try {
		def = weapon->m_iItemDefinitionIndex();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		def = 0;
	}
	const bool sniper = (def == 9 || def == 11 || def == 38 || def == 40);
	const bool heavyPist = (def == 1 || def == 64);
	const bool shotgun = (def == 25 || def == 27 || def == 29 || def == 35);
	// Align with search accept (0.96/0.92). Tight Exact after loose search = silent miss.
	if (sniper) {
		int mode = 0, zl = 0;
		__try {
			mode = weapon->m_weaponMode();
			zl = weapon->m_zoomLevel();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			mode = 0; zl = 0;
		}
		const bool scoped = (mode == 1) || (zl >= 1 && zl <= 3);
		if (scoped) {
			scale = 0.97f;
			bias = 0.93f;
		} else {
			// Unscoped air AWP: large bloom — need full capsule, not tight rim reject
			scale = 0.98f;
			bias = 0.94f;
		}
	} else if (heavyPist || shotgun) {
		// Deagle: open so Exact matches Solve accept (jump/run bloom)
		scale = 1.02f;
		bias = 0.97f;
	}

	Vector_t origin{};
	if (Bones::GetOrigin(target, origin) && Bones::IsValidPos(origin)) {
		const float dist = eye.Distance(origin);
		if (dist > 1500.f) {
			scale = (std::min)(1.08f, scale + 0.02f);
			bias = (std::min)(0.99f, bias + 0.02f);
		}
	}

	// Local airborne: open more — jump inac already in GetInaccuracy; tight Exact
	// rejected deagle air seeds after Solve found a path.
	if (local) {
		std::uint32_t lf = 0;
		__try { lf = local->m_fFlags(); } __except (EXCEPTION_EXECUTE_HANDLER) { lf = 0; }
		if ((lf & FL_ONGROUND) == 0) {
			const float addS = heavyPist ? 0.08f : 0.04f;
			const float addB = heavyPist ? 0.04f : 0.04f;
			scale = (std::min)(1.12f, scale + addS);
			bias = (std::min)(1.0f, bias + addB);
		}
	}
}

bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint,
	float tickFrac)
{
	if (outPoint)
		*outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!target || hitbox < 0)
		return false;
	if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
		return false;

	Vector_t dirs[16]{};
	const int nDirs = BuildExactPelletDirs(
		fireAngles, seedTick, weapon, local, tickFrac, dirs, 16);
	if (nDirs <= 0)
		return false;

	float scale = kExactCapsuleScale;
	float bias = kExactCenterBias;
	ExactCapsuleScale(weapon, local, eye, target, scale, bias);

	// Any pellet that hits the hitbox counts (shotgun / multi-bullet VData).
	for (int b = 0; b < nDirs; ++b) {
		float t = 0.f;
		Vector_t pt{};
		if (!Bones::RayHitsConfiguredHitbox(
				target, hitbox, eye, dirs[b], scale, t, pt, bias))
			continue;
		if (outPoint)
			*outPoint = pt;
		return true;
	}
	return false;
}

bool ExactShotHitsAny(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	const bool* enabledHitboxes,
	int* outHitbox,
	Vector_t* outPoint,
	float tickFrac)
{
	if (outHitbox)
		*outHitbox = -1;
	if (outPoint)
		*outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!target || !Mem::ValidEntity(target))
		return false;
	if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
		return false;

	Vector_t dirs[16]{};
	const int nDirs = BuildExactPelletDirs(
		fireAngles, seedTick, weapon, local, tickFrac, dirs, 16);
	if (nDirs <= 0)
		return false;

	float scale = kExactCapsuleScale;
	float bias = kExactCenterBias;
	ExactCapsuleScale(weapon, local, eye, target, scale, bias);

	// Prefer core HBs — limbs inflate false accepts for seed wait.
	static constexpr int kOrder[] = {
		Config::HB_HEAD, Config::HB_NECK, Config::HB_CHEST,
		Config::HB_STOMACH, Config::HB_PELVIS,
		Config::HB_ARMS, Config::HB_LEGS, Config::HB_FEET
	};

	for (int hb : kOrder) {
		if (hb < 0 || hb >= Config::HB_COUNT)
			continue;
		if (enabledHitboxes && !enabledHitboxes[hb])
			continue;
		for (int b = 0; b < nDirs; ++b) {
			float t = 0.f;
			Vector_t pt{};
			if (!Bones::RayHitsConfiguredHitbox(
					target, hb, eye, dirs[b], scale, t, pt, bias))
				continue;
			if (outHitbox)
				*outHitbox = hb;
			if (outPoint)
				*outPoint = pt;
			return true;
		}
	}
	return false;
}

// Validates that SPREADSEEDGEN + CalcSpread produce a usable bullet dir for this
// tick/angles. Does not rewrite angles (no target → cannot ExactShotHits).
bool FindNoSpreadAngles(
	const QAngle_t& wishAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	QAngle_t& outAngles,
	int* outSeedTick)
{
	outAngles = wishAngles;
	if (outSeedTick)
		*outSeedTick = 0;
	if (!weapon || !wishAngles.IsValid() || seedTick <= 0)
		return false;
	if (!g_ready)
		Init();
	if (!g_computeSeed)
		return false;

	QAngle_t wish = wishAngles;
	wish.z = 0.f;
	wish.Normalize();

	Vector_t dir{};
	float sx = 0.f, sy = 0.f;
	if (!GetBulletDirection(wish, seedTick, weapon, local, dir, &sx, &sy, 1u))
		return false;
	if (!std::isfinite(dir.x) || !std::isfinite(dir.y) || !std::isfinite(dir.z))
		return false;
	outAngles = wish;
	if (outSeedTick)
		*outSeedTick = seedTick;
	return true;
}

uint32_t ComputeSeed(const QAngle_t& angles, int attackTick) {
	if (!g_ready) Init();
	return SehComputeSeedOrZero(angles, attackTick);
}

bool ReadCurrentBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local,
                      float& outInac, float& outSpr) {
	if (!g_ready) Init();
	return ReadWeaponBloom(weapon, local, outInac, outSpr);
}

float GetInaccuracyRecoveryTime(C_CSWeaponBase* weapon) {
	if (!g_ready) Init();
	return SehGetRecoveryTime(weapon);
}

void* GetInaccuracyFn() { return reinterpret_cast<void*>(g_getInaccuracy); }
void* GetSpreadFn() { return reinterpret_cast<void*>(g_getSpread); }
void* GetCalcSpreadFn() { return reinterpret_cast<void*>(g_calcSpread); }
void* GetSpreadSeedGenFn() { return reinterpret_cast<void*>(g_computeSeed); }
void* GetUpdateTurningFn() { return reinterpret_cast<void*>(g_updateTurning); }
void* GetRecoveryTimeFn() { return reinterpret_cast<void*>(g_getRecoveryTime); }
void* GetFxFireBulletsFn() { return g_fxFireBullets; }

} // namespace HitChance
