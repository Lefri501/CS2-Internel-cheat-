#define NOMINMAX
#include "auto_pistol.h"

#include "../../config/config.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../autofire/autofire.h"
#include "../triggerbot/triggerbot.h"
#include "../aim/aim_common.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace AutoPistol {
namespace {

constexpr std::uint64_t kAttack = IN_ATTACK;

// IDA CCSGOInput — only +0x258 = IsButtonActive code 3
constexpr std::uintptr_t kInputBtn0 = 0x250;
constexpr std::uintptr_t kInputBtn1 = 0x258;
constexpr std::uintptr_t kInputBtn2 = 0x260;
constexpr std::uintptr_t kInputBtn3 = 0x268;

constexpr std::uint16_t kPistolDefs[] = {
	1, 2, 3, 4, 30, 32, 36, 61, 63, 64,
};

// Spam-click: PRESS one CM → RELEASE next CM → wait weapon ready → repeat.
// No subtick, no cycle% gate, no sticky armed tick (those caused pause/desync).
bool g_needRelease = false;
std::uint64_t g_lastPressMs = 0;
std::int32_t g_lastEdgeCmd = -1;

bool AttackHeld()
{
	return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

bool IsPistolDef(std::uint16_t def)
{
	for (std::uint16_t d : kPistolDefs) {
		if (d == def)
			return true;
	}
	return false;
}

bool IsSemiPistol(C_CSWeaponBase* wpn)
{
	if (!wpn || !Mem::ValidEntity(wpn))
		return false;

	__try {
		const std::uint16_t def = wpn->m_iItemDefinitionIndex();
		if (IsPistolDef(def))
			return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}

	__try {
		auto* ent = reinterpret_cast<CEntityInstance*>(wpn);
		SchemaClassInfoData_t* cls = nullptr;
		ent->dump_class_info(&cls);
		if (cls && cls->szName && Mem::IsReadable(cls->szName, 1)) {
			const char* n = cls->szName;
			if (std::strstr(n, "DEagle") || std::strstr(n, "Glock")
				|| std::strstr(n, "USP") || std::strstr(n, "P250")
				|| std::strstr(n, "FiveSeven") || std::strstr(n, "Tec9")
				|| std::strstr(n, "Elite") || std::strstr(n, "Revolver")
				|| std::strstr(n, "HKP2000") || std::strstr(n, "CZ75")
				|| std::strstr(n, "Pistol"))
				return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}

	__try {
		if (wpn->IsNonGunWeapon())
			return false;
		auto* vd = wpn->Data();
		if (vd && Mem::Valid(vd, 0x20) && vd->m_WeaponType() == 1) {
			bool fullAuto = false;
			__try { fullAuto = vd->m_bIsFullAuto(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { fullAuto = false; }
			return !fullAuto;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}

	return false;
}

bool ClipOk(C_CSWeaponBase* wep)
{
	if (!wep)
		return false;
	__try {
		if (wep->m_bInReload())
			return false;
		return wep->m_iClip1() != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void ClearAttackCmd(CUserCmd* cmd)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue &= ~kAttack;
	cmd->nButtons.nValueChanged &= ~kAttack;
	cmd->nButtons.nValueScroll &= ~kAttack;
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pInButtonState) {
			base->pInButtonState->nValue &= ~kAttack;
			base->pInButtonState->nValueChanged &= ~kAttack;
			base->pInButtonState->nValueScroll &= ~kAttack;
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			base->SetBits(BASE_BITS_BUTTONPB);
		}
	}
	cmd->csgoUserCmd.nAttack1StartHistoryIndex = -1;
}

void ClearAttackInput()
{
	void* pInput = Input::GetCSGOInput();
	if (!pInput)
		return;
	__try {
		auto* b = reinterpret_cast<std::uint8_t*>(pInput);
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn0) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn1) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn2) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn3) &= ~kAttack;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void InjectAttackInput()
{
	void* pInput = Input::GetCSGOInput();
	if (!pInput)
		return;
	__try {
		auto* b = reinterpret_cast<std::uint8_t*>(pInput);
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn0) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn2) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn3) &= ~kAttack;
		*reinterpret_cast<std::uint64_t*>(b + kInputBtn1) |= kAttack;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Single rising edge — same shape as aim PressAttack semi path. No subtick.
void EdgeAttack(CUserCmd* cmd)
{
	if (!cmd)
		return;

	cmd->nButtons.nValue |= kAttack;
	cmd->nButtons.nValueChanged |= kAttack;
	cmd->nButtons.nValueScroll &= ~kAttack;

	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pInButtonState) {
			base->pInButtonState->nValue |= kAttack;
			base->pInButtonState->nValueChanged |= kAttack;
			base->pInButtonState->nValueScroll &= ~kAttack;
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			base->SetBits(BASE_BITS_BUTTONPB);
		}
	}

	if (cmd->csgoUserCmd.inputHistoryField.nCurrentSize > 0)
		cmd->csgoUserCmd.SetAttack1StartHistoryIndex(0);

	InjectAttackInput();
}

} // namespace

bool Init()
{
	g_needRelease = false;
	g_lastPressMs = 0;
	g_lastEdgeCmd = -1;
	return true;
}

void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd)
{
	if (!Config::auto_pistol || !local || !cmd)
		return;

	if (Autofire::WantShoot() || Triggerbot::WantShoot())
		return;

	const std::int32_t cmdNum = cmd->nCommandNumber;
	// Same cmd re-entry: do not re-edge (desync). Leave cmd alone if we already edged it.
	if (cmdNum != 0 && cmdNum == g_lastEdgeCmd)
		return;

	if (!AttackHeld()) {
		g_needRelease = false;
		g_lastPressMs = 0;
		return;
	}

	C_CSWeaponBase* wpn = nullptr;
	__try { wpn = local->GetActiveWeapon(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }

	if (!IsSemiPistol(wpn))
		return;

	if (!ClipOk(wpn)) {
		ClearAttackCmd(cmd);
		ClearAttackInput();
		g_needRelease = false;
		return;
	}

	// RELEASE tick — strip held M1 so next press is a real spam click
	if (g_needRelease) {
		ClearAttackCmd(cmd);
		ClearAttackInput();
		g_needRelease = false;
		g_lastEdgeCmd = cmdNum;
		return;
	}

	// Weapon not ready — strip hold (semi needs edge later), wait
	if (!AimCommon::CanWeaponFire(wpn, local)) {
		ClearAttackCmd(cmd);
		ClearAttackInput();
		return;
	}

	// Optional user delay only. Floor 1 tick so double-CM same frame cannot double-edge.
	const float delayMs = std::clamp(Config::auto_pistol_delay_ms, 0.f, 500.f);
	const std::uint64_t minGap = (delayMs > 0.f)
		? static_cast<std::uint64_t>(delayMs)
		: 15ull;
	const std::uint64_t now = GetTickCount64();
	if (g_lastPressMs != 0 && (now - g_lastPressMs) < minGap) {
		ClearAttackCmd(cmd);
		ClearAttackInput();
		return;
	}

	// Clear player hold then inject our edge (spam click)
	ClearAttackCmd(cmd);
	ClearAttackInput();
	EdgeAttack(cmd);

	g_lastPressMs = now;
	g_needRelease = true;
	g_lastEdgeCmd = cmdNum;
}

} // namespace AutoPistol
