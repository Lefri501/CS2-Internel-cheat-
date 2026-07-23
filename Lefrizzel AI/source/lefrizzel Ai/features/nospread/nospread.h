#pragma once

// Seed nospread — IDA CSBaseGunFire path (client.dll, verified 2026-07-23)
//
// Patterns (patterns.hpp / patterns.json → live IDA):
//   GetInaccuracy     0x1807CE850  "48 89 5C 24 10 55 56 57 48 81 EC B0..."
//   GetSpread         0x1807CFBA0  "48 83 EC 38 48 63 91 D8 17 00 00..."
//                     weaponMode @ +0x17D8 → VData spread table +1880
//   SPREADSEEDGEN     0x180CB6E80  SHA1(quant pitch, quant yaw, nPlayerTickCount)
//   quant             0x180CB1340  floor(AngleNormalize(a)*2)*0.5
//   CalcSpread        0x180CB77A0  RandomSeed(seed) → sx/sy (seed+1 at fire)
//   FireBullet        0x1808474E0  dir = fwd - right*sx + up*sy
//   FillGunFireData   0x1807CFDD0  tick/eye/punch for seed timebase
//   ComputeAimPunchFire 0x180812CC0
//
// Flow: unpunched wish → +fire punch → SPREADSEEDGEN → CalcSpread(seed+1)
//       → solve view so pellet hits capsule → Exact verify → fire.

#include "../../utils/math/vector/vector.h"
#include <cstdint>

class C_CSWeaponBase;
class C_CSPlayerPawn;

namespace NoSpread {

bool Init();
bool Ready();

// ── One-shot solve (preferred API) ───────────────────────────────────
struct Shot {
	QAngle_t fireAngles{};
	Vector_t hitPoint{};
	int hitbox = -1;
	int seedTick = 0;
	float seedFrac = 0.f;
	float sx = 0.f;
	float sy = 0.f;
	bool ok = false;
};

// Compensate wishView so the deterministic pellet hits an enabled hitbox.
// wishView = unpunched camera / silent view. punch applied inside.
// enabledHitboxes = Config::autofire_hitboxes or trigger_hitboxes (null = all).
// Returns true only when ExactShotHits verifies the solution.
bool Solve(
	const Vector_t& eye,
	const QAngle_t& wishView,
	int seedTick,
	float tickFrac,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int preferHitbox,
	const bool* enabledHitboxes,
	Shot& out);

// Soft re-arm after fire + VData cycle (deagle ~full cycle) + bloom settle.
// Heavy pistol/sniper never early-drop on CanWeaponFire alone (overspray).
bool SeedCycleAllowsFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local);
void NoteSeedFired(C_CSWeaponBase* weapon, C_CSPlayerPawn* local = nullptr);

// Aim point = capsule center + short target-velocity lead.
Vector_t SeedAimPoint(C_CSPlayerPawn* target, int hitbox, const Vector_t& fallback);

// Hard mindmg on Solve pellet HB only (same-HB center retry). Never switches HB
// (old feet→head rewrite fired 20 dmg under mindmg 100).
// allowPen=true → pen + minDamageAw (hp-aware). Vis → minDamageVis.
bool SeedPassesDamage(
	const Vector_t& eye,
	Vector_t& inOutPoint,
	int& inOutHb,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamageVis,
	float minDamageAw);

// ── Thin wrappers (HitChance / legacy call sites) ────────────────────
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

std::uint32_t ComputeSeed(const QAngle_t& angles, int attackTick);

// ── Legacy types kept so AF/trigger compile if any residual refs ─────
struct SeedGate {
	float maxCompDelta = 6.f;
	float maxDeltaCap = 72.f;
	float maxRad = 0.50f;
	float maxLat = 900.f;
	float maxAimDist = 72.f;
	float maxBloom = 0.99f;
	float minScore = 1.05f;
	bool allowAnyHb = true;
	bool requireSameBin = false;
	bool exactNeedsAimNear = false;
	bool alwaysBloomGate = false;
	bool requireSettled = false;
};

inline float SeedMinAccept(const SeedGate&) { return 1.05f; }

SeedGate GetSeedGate(C_CSWeaponBase* weapon, bool sniperScoped);
float SeedAimDistLimit(const SeedGate& gate, float eyeToAimDist, bool sniperHot);
bool BloomAllowsSeedFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local, const SeedGate& gate);
float ComputeSeedMaxDelta(
	C_CSWeaponBase* weapon,
	const SeedGate& gate,
	float inac,
	float spr,
	bool moving);
void GetWildGateLimits(
	const SeedGate& gate,
	bool moving,
	float bloom,
	float& outRad,
	float& outLat,
	float& outAim,
	float& outAng);

// Overload used by older AF/TR early-gate calls
bool SeedCycleAllowsFire(C_CSWeaponBase* weapon, C_CSPlayerPawn* local, const SeedGate& gate);

struct BestShot {
	QAngle_t angles{};
	Vector_t point{};
	int hitbox = -1;
	int seedTick = 0;
	float score = -1.f;
	bool sameSeedBin = false;
	bool preferHit = false;
};

bool FindBestShot(
	const Vector_t& eye,
	const QAngle_t& wishAngles,
	const Vector_t& aimPoint,
	int preferHitbox,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	float maxDeltaDeg,
	BestShot& out,
	float tickFrac = 0.f);

bool FindNoSpreadAngles(
	const QAngle_t& wishAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	QAngle_t& outAngles,
	int* outSeedTick = nullptr);

// Debug diagnostics (Release = no-op body for heavy dump)
namespace SeedDbg {
struct Snap {
	const char* who = "seed";
	const char* event = "WAIT";
	const char* reason = "";
	const char* path = "";
	const char* angSrc = "";
	C_CSWeaponBase* weapon = nullptr;
	C_CSPlayerPawn* local = nullptr;
	bool sniperScoped = false;
	bool onGround = true;
	bool moving = false;
	bool punchOk = false;
	float speed2d = 0.f;
	float inac = 0.f;
	float spr = 0.f;
	float seedFrac = 0.f;
	float sx = 0.f;
	float sy = 0.f;
	float score = -1.f;
	float maxDelta = 0.f;
	float dAng = 0.f;
	float dAim = 0.f;
	float lat = 0.f;
	int seedTick = 0;
	int atkIdx = -1;
	int histCount = 0;
	int preferHb = -1;
	int hitHb = -1;
	int def = 0;
	QAngle_t wish{};
	QAngle_t fire{};
	QAngle_t punch{};
	Vector_t eye{};
	Vector_t aimPt{};
	Vector_t hitPt{};
	SeedGate gate{};
};
void Log(const Snap& s, unsigned intervalMs = 150u);
const char* HbName(int hb);
const char* WpnTag(int def);
} // namespace SeedDbg

} // namespace NoSpread
