#define NOMINMAX
#include "knifebot.h"

#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../visuals/visuals.h"

#include <cmath>
#include <cstdint>
#include <Windows.h>

namespace KnifeBot {
namespace {

constexpr float kSlashRange = 64.f;
constexpr float kStabRange = 48.f;

bool IsKnifeWeapon(C_CSWeaponBase* wpn) {
	if (!wpn || !Mem::Valid(wpn, 0x40))
		return false;
	__try {
		auto* vd = wpn->Data();
		if (vd && Mem::Valid(vd, 0x20) && vd->m_WeaponType() == 0)
			return true;
		const std::uint16_t def = wpn->m_iItemDefinitionIndex();
		return def == 42 || def == 59 || (def >= 500 && def <= 526);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

float Dist(const Vector_t& a, const Vector_t& b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void SetAttack(CUserCmd* cmd, bool stab) {
	if (!cmd)
		return;
	const std::uint64_t mask = stab
		? static_cast<std::uint64_t>(IN_SECOND_ATTACK)
		: static_cast<std::uint64_t>(IN_ATTACK);
	cmd->nButtons.nValue |= mask;
	cmd->nButtons.nValueChanged |= mask;
	auto* base = cmd->csgoUserCmd.pBaseCmd;
	if (base && base->pInButtonState) {
		base->pInButtonState->nValue |= mask;
		base->pInButtonState->nValueChanged |= mask;
	}
}

} // namespace

bool Init() { return true; }

void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd) {
	if (!Config::knifebot || !local || !cmd)
		return;
	if (!keybind.isActive(Config::knifebot))
		return;

	C_CSWeaponBase* wpn = nullptr;
	__try { wpn = local->GetActiveWeapon(); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (!IsKnifeWeapon(wpn))
		return;

	Vector_t eye = cached_local.position;
	eye.z += 64.f;
	if (!std::isfinite(eye.x))
		return;

	float bestDist = 1e9f;
	bool bestStab = false;
	bool found = false;

	for (const auto& p : cached_players) {
		if (p.type != enemy || p.health < 1)
			continue;
		Vector_t chest = p.position;
		chest.z += 36.f;
		const float d = Dist(eye, chest);
		if (d > kSlashRange || d >= bestDist)
			continue;
		bestDist = d;
		bestStab = Config::knifebot_prefer_stab ? (d <= kStabRange) : (d <= kStabRange * 0.75f);
		found = true;
	}

	if (!found)
		return;
	SetAttack(cmd, bestStab);
}

} // namespace KnifeBot
