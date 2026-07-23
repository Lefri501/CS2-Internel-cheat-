#pragma once

#include "../../utils/math/vector/vector.h"
#include <cstdint>

class C_CSWeaponBase;
class C_CSPlayerPawn;

namespace HitChance {

// IDA client (verified 2026-07-17, session 796bbc15):
//   GetInaccuracy 0x1807CE850, GetSpread 0x1807CFBA0
//   SPREADSEEDGEN 0x180CB6E80 → CB1340 floor(*2)*0.5 SHA1
//   CalcSpread 0x180CB77A0, FX_FireBullets 0x180CB6F30 local seed+1
//   FireBullet 0x1808474E0 dir=fwd-right*sx+up*sy
//   FillGunFireData 0x1807CFDD0 (CSBaseGunFire fill via 7C77E0)
//   ComputeAimPunchFire 0x180812CC0(services@pawn+0x14B8, out, fireData, 1)
//     then angles += punch (7C18A0) — SPREADSEEDGEN sees punched angles
//   GetRemovedAimPunch 0x18088BBB0
//   UpdateTurningInAccuracy 0x1807EA5B0, RecoveryTime 0x1807CF600

bool Init();
bool Ready();
bool SpreadSeedReady();

// IDA 7CFDD0 fire-data blob used by CSBaseGunFire before SPREADSEEDGEN.
// Layout (unsigned int* a2): tick@0, frac@4, eye@20, ang@32 (punched), punch@44.
struct GunFireData {
	int tick = 0;
	float frac = 0.f;
	Vector_t eye{};
	QAngle_t angles{};   // already view+punch after fill
	QAngle_t punch{};
	int sourceMode = -1; // 0 exact hist … 5 calculated
	bool hasHistory = false;
	bool ok = false;
};

// Calls game FillGunFireData(weapon, mode). Restores weapon punch cache fields.
// Prefer for seed tick/eye/punch — matches CSBaseGunFire history selection.
bool FillGunFireData(C_CSWeaponBase* weapon, GunFireData& out, int mode = 0);

// Seed fire context: unpunched view → punch@fireTick → punched for SPREADSEEDGEN.
struct SeedFireContext {
	int tick = 0;
	float frac = 0.f;
	Vector_t eye{};
	QAngle_t punch{};
	QAngle_t punchedView{}; // view + punch (what SPREADSEEDGEN hashes)
	bool punchOk = false;
	bool fromFill = false;
	bool ok = false;
};

// Prefer FillGunFireData for tick/eye/punch; else 812CC0(tick,frac).
// viewAngles = hist/cam/silent view (UNPUNCHED). punchedView = view + fire punch.
bool BuildSeedFireContext(
	C_CSPlayerPawn* local,
	C_CSWeaponBase* weapon,
	const QAngle_t& viewAngles,
	int seedTickHint,
	float tickFracHint,
	const Vector_t& eyeHint,
	SeedFireContext& out);

// GetInaccuracy + GetSpread + local ValveRng Monte Carlo vs studio capsule.
// (MC does not call game CalcSpread — would stomp global RandomSeed.)
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

// SEH shell — bone/studio AV in Passes must not take down the process.
bool PassesSafe(
	const Vector_t& eye,
	const QAngle_t& aimAngle,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	float minChancePercent,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target);

// IDA CSBaseGunFire: fire angles = hist view + ComputeAimPunchFire(services, fireTick),
// THEN SPREADSEEDGEN(punched, tick) + FireBullet(punched). UC m_aimPunchAngle is outdated.
// Pass history/view angles (no punch). We add fire-tick punch when local!=null.
// seedAdd=1 (local FX). dir = fwd - right*sx + up*sy (IDA FireBullet).
bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX = nullptr,
	float* outSpreadY = nullptr,
	unsigned seedAdd = 1u,
	float tickFrac = 0.f);

// Pre-read bloom. aimPunch = fire-tick punch (nullptr = none).
// Always local ValveRng (seed+1) — matches FindBestShot + ExactShotHits.
// useLocalSpread kept for API; game CalcSpread stomped tier0 RandomSeed.
// Frame cache: same (quant pitch/yaw, tick, bloom, weapon) reuses seed+sx/sy.
bool GetBulletDirectionCached(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	float inac,
	float spr,
	Vector_t& outDir,
	float* outSpreadX = nullptr,
	float* outSpreadY = nullptr,
	unsigned seedAdd = 1u,
	const QAngle_t* aimPunch = nullptr,
	bool useLocalSpread = false);

// GetRemovedAimPunch (current-time). Prefer ReadAimPunchForFire for seed paths.
bool ReadAimPunch(C_CSPlayerPawn* local, QAngle_t& out);

// IDA sub_180812CC0 — new aimpunch at fire GameTime (tick + frac).
// Pass hist flPlayerTickFraction when available (was always 0).
bool ReadAimPunchForFire(
	C_CSPlayerPawn* local, int seedTick, QAngle_t& out, float tickFrac = 0.f);

// Prefer punch from FillGunFireData when weapon set; else ReadAimPunchForFire.
bool ReadSeedFirePunch(
	C_CSPlayerPawn* local,
	C_CSWeaponBase* weapon,
	int seedTick,
	float tickFrac,
	QAngle_t& out);

// Exact seed ray vs hitbox capsule. outPoint = hit on axis for wall/dmg.
bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint = nullptr,
	float tickFrac = 0.f);

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
	Vector_t* outPoint = nullptr,
	float tickFrac = 0.f);

// Validates seed path produces a finite bullet dir (no angle rewrite; no target).
bool FindNoSpreadAngles(
	const QAngle_t& wishAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	QAngle_t& outAngles,
	int* outSeedTick = nullptr);

// Game SPREADSEEDGEN(angles, nPlayerTickCount).
std::uint32_t ComputeSeed(const QAngle_t& angles, int attackTick);

// Live weapon bloom via GetInaccuracy/GetSpread (+ UpdateTurningInAccuracy).
bool ReadCurrentBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local,
                      float& outInac, float& outSpr);

// VData recovery time after shot (autostop / spray timing). -1 if unresolved.
float GetInaccuracyRecoveryTime(C_CSWeaponBase* weapon);

// Resolved function pointers (debug)
void* GetInaccuracyFn();
void* GetSpreadFn();
void* GetCalcSpreadFn();
void* GetSpreadSeedGenFn();
void* GetUpdateTurningFn();
void* GetRecoveryTimeFn();
void* GetFxFireBulletsFn();

} // namespace HitChance
