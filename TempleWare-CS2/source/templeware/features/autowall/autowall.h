#pragma once

#include "../../utils/math/vector/vector.h"

class C_CSWeaponBase;
class C_CSPlayerPawn;

namespace AutoWall {

struct Result {
	float damage = 0.f;
	bool penetrated = false;
	bool hit = false;
};

bool Init();
bool Ready();

// Post-armor damage at aimPoint.
// allowPen=false → visible estimate (caller owns LOS).
// allowPen=true  → game CreateTrace+DamageToPoint, else TraceLine pen fallback.
Result Fire(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen);

// true if damage >= minDamage. minDamage<=0 → any hit.
// If target HP < minDamage, requires lethal (dmg >= HP).
bool PassesMinDamage(
	const Vector_t& eye,
	const Vector_t& aimPoint,
	int hitbox,
	C_CSWeaponBase* weapon,
	C_CSPlayerPawn* local,
	C_CSPlayerPawn* target,
	bool allowPen,
	float minDamage);

} // namespace AutoWall
