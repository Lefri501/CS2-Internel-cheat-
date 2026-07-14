#pragma once

#include "../../utils/math/vector/vector.h"
#include <cstdint>

class C_CSWeaponBase;
class C_CSPlayerPawn;

namespace HitChance {

// IDA client (verified):
//   GetInaccuracy 0x1807CE850, GetSpread 0x1807CFA50
//   SPREADSEEDGEN 0x180CB6A70, CalcSpread 0x180CB7390
//   FX_FireBullets uses CalcSpread(seed+1) for local client

bool Init();
bool Ready();
bool SpreadSeedReady();

// GetInaccuracy + GetSpread + CalcSpread Monte Carlo vs studio capsule.
// Spray-safe: no hard speed/bloom reject. minChancePercent 0 = off.
bool Passes(
	const Vector_t& eye,
	const QAngle_t& aimAngle,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	float minChancePercent,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target);

// seed = SPREADSEEDGEN(pitch/yaw quantized 0.5°, tick); CalcSpread(seed+1).
// Roll ignored in seed hash. dir = fwd + right*sx + up*sy (classic AngleVectors).
bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX = nullptr,
	float* outSpreadY = nullptr);

// Exact seed ray vs hitbox capsule. outPoint = hit on axis for wall/dmg.
bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint = nullptr);

// Any enabled hitbox (nullptr = all). Prefers head→chest order.
bool ExactShotHitsAny(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	const bool* enabledHitboxes,
	int* outHitbox = nullptr,
	Vector_t* outPoint = nullptr);

// API compat: no angle rewrite; validates seedTick only.
bool FindNoSpreadAngles(
	const QAngle_t& wishAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	QAngle_t& outAngles,
	int* outSeedTick = nullptr);

// Game SPREADSEEDGEN(angles, nPlayerTickCount).
std::uint32_t ComputeSeed(const QAngle_t& angles, int attackTick);

// Live weapon bloom via GetInaccuracy/GetSpread.
bool ReadCurrentBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local,
                      float& outInac, float& outSpr);

// Resolved function pointers (debug)
void* GetInaccuracyFn();
void* GetSpreadFn();
void* GetCalcSpreadFn();
void* GetSpreadSeedGenFn();

} // namespace HitChance
