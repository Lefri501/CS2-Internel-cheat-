#define NOMINMAX
#include "enemy_spec.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/console/console.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/handle.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace EnemySpec {
namespace {

// ObserverMode_t (dump)
constexpr std::uint8_t kObsInEye = 2;
constexpr std::uint8_t kObsChase = 3;

// Sticky by entity entry index (stable; serial can change)
int g_targetIdx = -1;
int g_slot = 0;
bool g_wasDead = false;

// One latch per direction — any key in the group
bool g_edgeNext = false;
bool g_edgePrev = false;

bool AnyDown(const int* vks, int n)
{
	for (int i = 0; i < n; ++i) {
		if ((GetAsyncKeyState(vks[i]) & 0x8000) != 0)
			return true;
	}
	return false;
}

bool GroupEdge(const int* vks, int n, bool& latch)
{
	const bool down = AnyDown(vks, n);
	if (down && !latch) {
		latch = true;
		return true;
	}
	if (!down)
		latch = false;
	return false;
}

CPlayer_ObserverServices* ObsOf(C_CSPlayerPawn* pawn)
{
	if (!pawn || !Mem::ValidEntity(pawn))
		return nullptr;
	CPlayer_ObserverServices* obs = nullptr;
	__try { obs = pawn->m_pObserverServices(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!obs || !Mem::Valid(obs, 0x60))
		return nullptr;
	return obs;
}

bool LocalAlive(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return false;
	__try {
		if (pawn->m_iHealth() <= 0)
			return false;
		if (pawn->m_lifeState() != 0)
			return false;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

C_CSPlayerPawn* ResolveSpecPawn()
{
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (!local)
		return nullptr;
	if (LocalAlive(local))
		return nullptr;

	const CBaseHandle hCtrl = local->m_hController();
	if (hCtrl.valid() && I::GameEntity && I::GameEntity->Instance) {
		auto* ctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl);
		if (ctrl && Mem::ValidEntity(ctrl)) {
			CBaseHandle hObs = ctrl->m_hObserverPawn();
			if (hObs.valid()) {
				auto* op = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hObs);
				if (op && Mem::ValidEntity(op) && ObsOf(op))
					return op;
			}
			CBaseHandle hPawn = ctrl->m_hPawn();
			if (hPawn.valid()) {
				auto* pp = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
				if (pp && Mem::ValidEntity(pp) && ObsOf(pp))
					return pp;
			}
		}
	}
	if (ObsOf(local))
		return local;
	return nullptr;
}

struct EnemySlot {
	CBaseHandle handle{};
	int entIndex = 0; // ENT_ENTRY_MASK — stable sort key
	C_CSPlayerPawn* pawn = nullptr;
};

// All alive players except local (enemies + teammates). Sorted by ent index.
int CollectAlivePlayers(EnemySlot* out, int maxOut)
{
	if (!out || maxOut <= 0 || !I::GameEntity || !I::GameEntity->Instance)
		return 0;
	if (!Mem::Valid(I::GameEntity->Instance, 0x2100))
		return 0;

	const int nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
	const int nMax = (nMaxRaw > 128) ? 128 : nMaxRaw;
	int count = 0;

	for (int i = 1; i <= nMax; ++i) {
		auto* ent = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(ent))
			continue;

		// Designer first — dump_class_info on every slot was heavy on RENDER_START
		bool isCtrl = false;
		__try {
			CEntityIdentity* id = nullptr;
			if (!Mem::ReadField(ent, 0x10, id) || !id || !Mem::Valid(id, 0x28))
				id = ent->m_pEntityIdentity();
			if (id && Mem::Valid(id, 0x28)) {
				const char* designer = nullptr;
				if (!Mem::ReadField(id, 0x20, designer) || !designer)
					designer = id->m_designerName();
				if (designer && Mem::IsReadable(designer, 2) && designer[0]) {
					if (std::strcmp(designer, "cs_player_controller") == 0
						|| std::strstr(designer, "player_controller") != nullptr)
						isCtrl = true;
					else
						continue;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!isCtrl) {
			SchemaClassInfoData_t* cls = nullptr;
			__try { ent->dump_class_info(&cls); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
				continue;
			if (HASH(cls->szName) != HASH("CCSPlayerController"))
				continue;
		}

		auto* ctrl = reinterpret_cast<CCSPlayerController*>(ent);
		bool isLocal = false;
		__try { isLocal = ctrl->IsLocalPlayer(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (isLocal)
			continue;

		bool alive = false;
		__try { alive = ctrl->m_bPawnIsAlive(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!alive)
			continue;

		CBaseHandle hPawn = ctrl->m_hPlayerPawn();
		if (!hPawn.valid())
			hPawn = ctrl->m_hPawn();
		if (!hPawn.valid())
			continue;

		auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
		if (!pawn || !Mem::ValidEntity(pawn))
			continue;

		int hp = 0;
		__try { hp = pawn->m_iHealth(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (hp < 1)
			continue;

		if (count >= maxOut)
			break;

		CBaseHandle hSelf = pawn->handle();
		out[count].handle = hSelf.valid() ? hSelf : hPawn;
		out[count].entIndex = out[count].handle.valid()
			? out[count].handle.index()
			: i;
		out[count].pawn = pawn;
		++count;
	}

	// Stable sort by ent index so slot N is always same player order
	for (int a = 0; a < count; ++a) {
		for (int b = a + 1; b < count; ++b) {
			if (out[b].entIndex < out[a].entIndex) {
				const EnemySlot tmp = out[a];
				out[a] = out[b];
				out[b] = tmp;
			}
		}
	}
	return count;
}

void WriteTarget(CPlayer_ObserverServices* obs, const CBaseHandle& h, std::uint8_t mode)
{
	if (!obs || !h.valid())
		return;
	__try {
		obs->m_iObserverMode() = mode;
		obs->m_hObserverTarget() = h;
		obs->m_bForcedObserverMode() = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void ApplyToPawn(C_CSPlayerPawn* pawn, const CBaseHandle& h, std::uint8_t mode)
{
	if (CPlayer_ObserverServices* obs = ObsOf(pawn))
		WriteTarget(obs, h, mode);
}

int FindByIndex(const EnemySlot* list, int n, int entIdx)
{
	if (entIdx < 0)
		return -1;
	for (int i = 0; i < n; ++i) {
		if (list[i].entIndex == entIdx)
			return i;
	}
	return -1;
}

} // namespace

bool Init()
{
	g_targetIdx = -1;
	g_slot = 0;
	g_wasDead = false;
	g_edgeNext = false;
	g_edgePrev = false;
	Con::Ok("EnemySpec ready (LMB/RMB cycle, all alive players)");
	return true;
}

void OnFrame()
{
	if (!Config::enemy_spectate)
		return;
	if (g_bMenuOpen)
		return;

	C_CSPlayerPawn* live = H::SafeLocalPlayer();
	if (!live)
		return;

	if (LocalAlive(live)) {
		g_wasDead = false;
		g_targetIdx = -1;
		g_slot = 0;
		return;
	}

	C_CSPlayerPawn* specPawn = ResolveSpecPawn();
	if (!specPawn)
		return;

	EnemySlot enemies[64]{};
	const int n = CollectAlivePlayers(enemies, 64);
	if (n <= 0)
		return;

	// Fresh death → first enemy
	if (!g_wasDead) {
		g_slot = 0;
		g_targetIdx = enemies[0].entIndex;
		g_wasDead = true;
		g_edgeNext = false;
		g_edgePrev = false;
	}

	// IDA SpectatorInput 0x1808148F0 (cs_observer_observerservices.cpp):
	//   +attack  → "spec_next"   +attack2 → "spec_prev"
	// Mirror with LMB/RMB while dead + arrows / brackets / side buttons.
	static const int kNextKeys[] = {
		VK_LBUTTON, VK_RIGHT, VK_OEM_6 /*]*/, VK_XBUTTON2, VK_NEXT
	};
	static const int kPrevKeys[] = {
		VK_RBUTTON, VK_LEFT, VK_OEM_4 /*[*/, VK_XBUTTON1, VK_PRIOR
	};
	if (GroupEdge(kNextKeys, 5, g_edgeNext)) {
		g_slot = (g_slot + 1) % n;
		g_targetIdx = enemies[g_slot].entIndex;
	}
	if (GroupEdge(kPrevKeys, 5, g_edgePrev)) {
		g_slot = (g_slot - 1 + n) % n;
		g_targetIdx = enemies[g_slot].entIndex;
	}

	// Resolve sticky by entity index (not full handle serial)
	int pick = FindByIndex(enemies, n, g_targetIdx);
	if (pick < 0) {
		// Target died — keep slot in range
		if (g_slot < 0 || g_slot >= n)
			g_slot = 0;
		pick = g_slot;
		g_targetIdx = enemies[pick].entIndex;
	} else {
		g_slot = pick;
	}

	const CBaseHandle& h = enemies[pick].handle;
	if (!h.valid())
		return;

	const std::uint8_t mode = Config::enemy_spectate_thirdperson ? kObsChase : kObsInEye;

	// Force every frame — engine may try to snap back to teammate
	ApplyToPawn(specPawn, h, mode);
	if (live != specPawn)
		ApplyToPawn(live, h, mode);

	const CBaseHandle hCtrl = live->m_hController();
	if (hCtrl.valid() && I::GameEntity && I::GameEntity->Instance) {
		auto* ctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl);
		if (ctrl && Mem::ValidEntity(ctrl)) {
			CBaseHandle hObs = ctrl->m_hObserverPawn();
			if (hObs.valid()) {
				auto* op = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hObs);
				if (op && op != specPawn && op != live)
					ApplyToPawn(op, h, mode);
			}
		}
	}
}

} // namespace EnemySpec
