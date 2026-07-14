#include "hitchance.h"

#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../bones/bones.h"
#include "../../config/config.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace HitChance {
namespace {

// IDA client.dll verified 2026-07-14 (patterns.hpp + IDB):
//   GetInaccuracy  @ 0x1807CE850  pattern::client::GetInaccuracy / C_CSWeaponBaseGun_GetInaccuracy
//     UpdateAccuracyPenalty (vt+2944); fminf(1, penalty+move+air+turn+…)
//   GetSpread      @ 0x1807CFA50  pattern::client::GetSpread / C_CSWeaponBaseGun_GetSpread
//     mode @ weapon+6104; VData @ +904; spread @ VData+1880[mode]
//   CalcSpread     @ 0x180CB7390  pattern::client::CalcSpread
//     (itemDef, nBullets, mode, seed, inac, spread, recoilIdx, outX*, outY*)
//     RandomSeed(seed); 4× RandomFloat; R8/Negev reshape; cos/sin mix
//   SPREADSEEDGEN  @ 0x180CB6A70  pattern::client::SpreadSeedGen / NoSpread1 / ComputeRandomSeed
//     SHA1(quantized pitch, quantized yaw, attackTick) → seed dword (roll ignored)
//   FX_FireBullets @ 0x180CB6B20: local client CalcSpread(seed + 1)
//
// Seed path: SPREADSEEDGEN(ang, tick) → seed; CalcSpread(..., seed+1, ...)

using FnGetInaccuracy = float(__fastcall*)(void* weapon, float* out1, float* out2);
using FnGetSpread = float(__fastcall*)(void* weapon);
// IDA: __int64 __fastcall SPREADSEEDGEN(__int64 unused, QAngle_t* ang, int tick)
using FnComputeRandomSeed = std::uint32_t(__fastcall*)(void* unused, const QAngle_t* angles, int attackTick);
// IDA: void CalcSpread(u16 def, int nBullets, int mode, u32 seed, float inac, float spr,
//                      float recoil, float* outX, float* outY)
using FnCalcSpread = void(__fastcall*)(
	std::uint16_t itemDef, int nBullets, int mode, std::uint32_t seed,
	float inac, float spread, float recoilIdx, float* outX, float* outY);

FnGetInaccuracy g_getInaccuracy = nullptr;
FnGetSpread g_getSpread = nullptr;
FnComputeRandomSeed g_computeSeed = nullptr;
FnCalcSpread g_calcSpread = nullptr;
bool g_ready = false;

// Exact patterns from patterns.hpp (IDA-hit confirmed)
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
constexpr const char* kPatCalcSpreadLoose =
	"48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 41 54 41 55 41 56 41 57 48 81 EC";

constexpr float kTwoPi = 6.28318530717958647692f;
// Full capsule for HC (was 0.55 — rejected spray body clips)
constexpr float kHcCapsuleScale = 1.0f;
constexpr float kHcCenterBias = 1.0f;
// Exact seed: use full capsule + slight bias to accommodate bloom variation
// between our seed validation and the game's CalcSpread call. The seed
// determines the DIRECTION within the cone (angle of random inacA/sprA).
// Bloom scaling (inac/spr) is applied later by CalcSpread, so the same seed
// produces different sx/sy magnitudes if bloom changes. A tight capsule here
// rejects valid seeds when bloom shifts. Full-scale catches them.
constexpr float kExactCapsuleScale = 1.0f;
constexpr float kExactCenterBias = 1.0f;

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
	__try {
		return g_getInaccuracy(weapon, nullptr, nullptr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1.f;
	}
}

float SehGetSpread(C_CSWeaponBase* weapon) {
	__try {
		return g_getSpread(weapon);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1.f;
	}
}

// Local CalcSpread — IDA 0x180CB7390 (R8 secondary + Negev early-spray reshape)
void CalcSpreadValve(
	std::uint32_t seed,
	float inac,
	float spr,
	float recoilIdx,
	std::uint16_t itemDef,
	int mode,
	float* outX,
	float* outY)
{
	ValveRng rng;
	rng.Seed(static_cast<int>(seed));

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

	*outX = std::cos(inacA) * inacR + std::cos(sprA) * sprR;
	*outY = std::sin(inacA) * inacR + std::sin(sprA) * sprR;
}

float EstimateInaccuracy(C_CSWeaponBase* weapon, C_CSPlayerPawn* local) {
	float inac = weapon ? weapon->m_fAccuracyPenalty() : 0.015f;
	if (!std::isfinite(inac) || inac < 0.f)
		inac = 0.01f;
	if (local) {
		const uint32_t flags = local->m_fFlags();
		const Vector_t vel = local->m_vecAbsVelocity();
		const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);
		if (!(flags & (1u << 0)))
			inac += 0.16f;
		else if (speed > 135.f)
			inac += 0.06f + (speed - 135.f) * 0.00035f;
		// turning inac field (GetInaccuracy also adds this)
		if (weapon) {
			const float turn = weapon->m_flTurningInaccuracy();
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

bool ReadWeaponBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local, float& inac, float& spr) {
	inac = g_getInaccuracy ? SehGetInaccuracy(weapon) : -1.f;
	spr = g_getSpread ? SehGetSpread(weapon) : -1.f;
	if (!std::isfinite(inac) || inac < 0.f)
		inac = EstimateInaccuracy(weapon, local);
	if (!std::isfinite(spr) || spr < 0.f) {
		// GetSpread is mode-indexed VData; fallback small base spread
		spr = 0.004f;
		if (weapon) {
			const float p = weapon->m_fAccuracyPenalty();
			if (std::isfinite(p) && p > 0.f)
				spr = (std::min)(0.08f, p * 0.15f + 0.002f);
		}
	}
	// Game clamps inac to 1.0; keep full spray range (do NOT clamp spr to 0.15)
	inac = std::clamp(inac, 0.f, 1.f);
	spr = std::clamp(spr, 0.f, 1.f);
	return std::isfinite(inac) && std::isfinite(spr);
}

std::uint32_t SehComputeSeed(const QAngle_t& angles, int attackTick) {
	if (!g_computeSeed)
		return 0;
	QAngle_t a = angles;
	a.z = 0.f;
	__try {
		return g_computeSeed(nullptr, &a, attackTick);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// Multi-bullet weapons write n pairs; HC uses first pellet only.
int WeaponBulletCount(C_CSWeaponBase* /*weapon*/) {
	return 1;
}

bool SehCalcSpreadGame(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	float* outX,
	float* outY)
{
	if (!g_calcSpread || !weapon || !outX || !outY)
		return false;
	__try {
		const std::uint16_t def = weapon->m_iItemDefinitionIndex();
		const int mode = weapon->m_weaponMode();
		const float recoil = weapon->m_flRecoilIndex();
		const int nBullets = WeaponBulletCount(weapon);
		// out arrays: CalcSpread writes nBullets floats each
		float xs[16]{};
		float ys[16]{};
		g_calcSpread(def, nBullets, mode, seed, inac, spr, recoil, xs, ys);
		*outX = xs[0];
		*outY = ys[0];
		return std::isfinite(*outX) && std::isfinite(*outY);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Prefer local Valve ran1 for MC (fast, matches IDA). Game fn for seed-exact paths.
void SpreadXY_Mc(
	C_CSWeaponBase* weapon,
	std::uint32_t seed,
	float inac,
	float spr,
	float* outX,
	float* outY)
{
	const std::uint16_t def = weapon ? weapon->m_iItemDefinitionIndex() : 0;
	const int mode = weapon ? weapon->m_weaponMode() : 0;
	const float recoil = weapon ? weapon->m_flRecoilIndex() : 0.f;
	CalcSpreadValve(seed, inac, spr, recoil, def, mode, outX, outY);
}

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
	SpreadXY_Mc(weapon, seed, inac, spr, outX, outY);
	return 1;
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
	if (!target || hitbox < 0)
		return false;
	float t = 0.f;
	Vector_t pt{};
	return Bones::RayHitsConfiguredHitbox(
		target, hitbox, eye, dir, scale, t, pt, bias);
}

bool BuildBulletDir(const QAngle_t& fire, float sx, float sy, Vector_t& outDir) {
	Vector_t forward{}, right{}, up{};
	fire.ToDirections(&forward, &right, &up);
	// IDA FireBullet: dir = fwd + right*sx + up*sy
	outDir = {
		forward.x + right.x * sx + up.x * sy,
		forward.y + right.y * sx + up.y * sy,
		forward.z + right.z * sx + up.z * sy
	};
	return NormalizeDir(outDir);
}

// Sample count scales with required % and bloom (spray needs more for stable %)
int McSampleCount(float requiredPct, float bloom) {
	int n = 160;
	if (requiredPct >= 60.f || bloom > 0.06f)
		n = 224;
	if (requiredPct >= 75.f || bloom > 0.12f)
		n = 288;
	if (requiredPct >= 90.f || bloom > 0.20f)
		n = 320;
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

	g_getInaccuracy = reinterpret_cast<FnGetInaccuracy>(pInac);
	g_getSpread = reinterpret_cast<FnGetSpread>(pSpr);
	g_computeSeed = reinterpret_cast<FnComputeRandomSeed>(pSeed);
	g_calcSpread = reinterpret_cast<FnCalcSpread>(pCalc);
	g_ready = true;

	Con::Info("HitChance: inac=%p spr=%p seed=%p calc=%p",
		(void*)g_getInaccuracy, (void*)g_getSpread,
		(void*)g_computeSeed, (void*)g_calcSpread);

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
	return true;
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
	if (!weapon || !target)
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
	const float recoil = weapon->m_flRecoilIndex();
	const bool spraying = recoil > 1.5f || inac > 0.06f;

	// Distance / aim validity
	Vector_t center = aimPoint;
	if (!Bones::GetHitboxPoint(target, hitbox, center) || !Bones::IsValidPos(center))
		center = aimPoint;
	const float dist = eye.Distance(center);
	if (dist < 1.f)
		return true;

	// Live capsule radius (full hitbox, not shrunk multipoint)
	float radius = Bones::MultipointRadius(target, hitbox);
	if (radius < 1.f)
		radius = 4.f;

	// Fast geometric pass: low bloom first-shot only (not mid-spray)
	if (!spraying && bloom > 1e-6f) {
		// Mean seed radius ~0.5 → expected lateral offset
		const float meanLat = bloom * dist * 0.5f;
		// Area-ish: if mean well inside capsule, high % without MC
		const float passR = radius * (1.15f - requiredPct * 0.003f);
		if (meanLat <= passR && meanLat * 2.2f <= radius * 1.05f)
			return true;
	}

	// Only reject absurd cases (not spray). Old radius*3.5 killed all sprays.
	{
		const float maxLat = bloom * dist;
		if (maxLat > radius * 80.f && requiredPct >= 50.f)
			return false;
	}

	Vector_t forward{}, right{}, up{};
	fireAngles.ToDirections(&forward, &right, &up);
	if (!std::isfinite(forward.x) || !std::isfinite(right.x) || !std::isfinite(up.x))
		return false;

	const int nSamples = McSampleCount(requiredPct, bloom);
	const int needed = (std::max)(1, static_cast<int>(
		std::ceil(requiredPct * static_cast<float>(nSamples) / 100.f + 1e-4f)));

	// Mid-spray: slightly prefer body volume — test aim hitbox first, then chest
	// as secondary if aim was head and required is moderate (spray transfer).
	const int hbPrimary = hitbox;
	int hbAlt = -1;
	if (spraying && requiredPct <= 70.f) {
		if (hitbox == Config::HB_HEAD || hitbox == Config::HB_NECK)
			hbAlt = Config::HB_CHEST;
	}

	int hits = 0;
	for (int i = 0; i < nSamples; ++i) {
		// Even cover of 0..255 seed space (game uses seed int into RandomSeed)
		const std::uint32_t seed = static_cast<std::uint32_t>((i * 256) / nSamples);

		float sx = 0.f, sy = 0.f;
		// Valve ran1 + recoilIdx/item reshape (IDA CalcSpread) — spray-accurate
		SpreadXY_Mc(weapon, seed, inac, spr, &sx, &sy);
		if (!std::isfinite(sx) || !std::isfinite(sy))
			continue;

		Vector_t dir{
			forward.x + right.x * sx + up.x * sy,
			forward.y + right.y * sx + up.y * sy,
			forward.z + right.z * sx + up.z * sy
		};
		if (!NormalizeDir(dir))
			continue;

		bool hit = CapsuleHit(eye, dir, target, hbPrimary, kHcCapsuleScale, kHcCenterBias);
		if (!hit && hbAlt >= 0)
			hit = CapsuleHit(eye, dir, target, hbAlt, kHcCapsuleScale, kHcCenterBias);

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

bool GetBulletDirection(
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	Vector_t& outDir,
	float* outSpreadX,
	float* outSpreadY)
{
	outDir.x = outDir.y = outDir.z = 0.f;
	if (!weapon || !fireAngles.IsValid() || seedTick <= 0)
		return false;
	if (!g_ready)
		Init();
	if (!g_computeSeed)
		return false;

	float inac = 0.f, spr = 0.f;
	if (!ReadWeaponBloom(weapon, local, inac, spr))
		return false;

	// IDA SPREADSEEDGEN 0x180CB6A70:
	//   SHA1(floor(AngleNormalize(pitch)*2)*0.5,
	//        floor(AngleNormalize(yaw)*2)*0.5, tick) — roll ignored.
	// IDA FX_FireBullets 0x180CB6B20 (local client): CalcSpread(seed + 1).
	// IDA FireBullet 0x1808472C0: dir = fwd - right_cs2*sx + up*sy;
	//   CS2 AngleVectors right is negated vs classic Source, so with our
	//   classic ToDirections: dir = fwd + right*sx + up*sy.
	QAngle_t ang = fireAngles;
	ang.z = 0.f;
	ang.Normalize();

	const std::uint32_t seed = SehComputeSeed(ang, seedTick);

	float sx = 0.f, sy = 0.f;
	// Prefer game CalcSpread for seed-exact path
	SpreadXY(weapon, seed + 1u, inac, spr, &sx, &sy);
	if (!std::isfinite(sx) || !std::isfinite(sy))
		return false;
	if (outSpreadX)
		*outSpreadX = sx;
	if (outSpreadY)
		*outSpreadY = sy;

	return BuildBulletDir(ang, sx, sy, outDir);
}

bool ExactShotHits(
	const Vector_t& eye,
	const QAngle_t& fireAngles,
	int seedTick,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	int hitbox,
	Vector_t* outPoint)
{
	if (outPoint)
		*outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!target || hitbox < 0)
		return false;
	if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
		return false;

	Vector_t dir{};
	if (!GetBulletDirection(fireAngles, seedTick, weapon, local, dir))
		return false;

	float t = 0.f;
	Vector_t pt{};
	if (!Bones::RayHitsConfiguredHitbox(
			target, hitbox, eye, dir, kExactCapsuleScale, t, pt, kExactCenterBias))
		return false;
	if (outPoint)
		*outPoint = pt;
	return true;
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
	Vector_t* outPoint)
{
	if (outHitbox)
		*outHitbox = -1;
	if (outPoint)
		*outPoint = Vector_t{ 0.f, 0.f, 0.f };
	if (!target)
		return false;
	if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
		return false;

	Vector_t dir{};
	if (!GetBulletDirection(fireAngles, seedTick, weapon, local, dir))
		return false;

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
		float t = 0.f;
		Vector_t pt{};
		if (!Bones::RayHitsConfiguredHitbox(
				target, hb, eye, dir, kExactCapsuleScale, t, pt, kExactCenterBias))
			continue;
		if (outHitbox)
			*outHitbox = hb;
		if (outPoint)
			*outPoint = pt;
		return true;
	}
	return false;
}

// API compat: no angle rewrite. Exact seedTick only (no ±1).
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
	if (!GetBulletDirection(wish, seedTick, weapon, local, dir))
		return false;
	outAngles = wish;
	if (outSeedTick)
		*outSeedTick = seedTick;
	return true;
}

uint32_t ComputeSeed(const QAngle_t& angles, int attackTick) {
	if (!g_ready) Init();
	return SehComputeSeed(angles, attackTick);
}

bool ReadCurrentBloom(C_CSWeaponBase* weapon, C_CSPlayerPawn* local,
                      float& outInac, float& outSpr) {
	if (!g_ready) Init();
	return ReadWeaponBloom(weapon, local, outInac, outSpr);
}

void* GetInaccuracyFn() { return reinterpret_cast<void*>(g_getInaccuracy); }
void* GetSpreadFn() { return reinterpret_cast<void*>(g_getSpread); }
void* GetCalcSpreadFn() { return reinterpret_cast<void*>(g_calcSpread); }
void* GetSpreadSeedGenFn() { return reinterpret_cast<void*>(g_computeSeed); }

} // namespace HitChance
