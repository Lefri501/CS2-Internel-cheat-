#include "hooks.h"
#include <iostream>
#include <cstdio>
#include <cstring>

#include "../../lefrizzel Ai/utils/memory/Interface/Interface.h"
#include "../utils/memory/patternscan/patternscan.h"
#include "../utils/memory/gaa/gaa.h"
#include "../utils/console/console.h"
#include "../utils/crypto/xorstr.h"

#include "../players/hook/playerHook.h"
#include "../features/visuals/visuals.h"
#include "../features/chams/chams.h"

#include "../../cs2/datatypes/cutlbuffer/cutlbuffer.h"
#include "../../cs2/datatypes/keyvalues/keyvalues.h"
#include "../../cs2/entity/C_Material/C_Material.h"

#include "../config/config.h"
#include "../keybinds/keybinds.h"
#include "../interfaces/interfaces.h"
#include "../features/aim/aim.h"
#include "../features/aim/aim_common.h"
#include "../features/movement/movement.h"
#include "../features/movement/jumpbug.h"
#include "../features/world/world.h"
#include "../features/world/weather.h"
#include "../features/hitmarker/hitmarker.h"
#include "../features/hitsound/hitsound.h"
#include "../features/sound_esp/sound_esp.h"
#include "../features/vote/vote.h"
#include "../features/panorama/panorama.h"
#include "../features/glow/glow.h"
#include "../features/skinchanger/skinchanger.h"
#include "../features/gamemode/gamemode.h"
#include "../features/nade_pred/nade_pred.h"
#include "../features/nade_lineup/nade_lineup.h"
#include "../features/prediction/prediction.h"
#include "../features/engine2/engine2.h"
#include "../features/w2s/w2s.h"
#include "../features/bomb/bomb.h"
#include "../features/knifebot/knifebot.h"
#include "../features/auto_pistol/auto_pistol.h"
#include "../features/enemy_spec/enemy_spec.h"
#include "../features/sdk_prio_a/sdk_prio_a.h"
#include "../features/cl_bypass/cl_bypass.h"

#include "../features/input_inject/input_inject.h"
#include "../features/subtick_move/subtick_move.h"
#include "../features/backtrack/backtrack.h"
#include "../utils/security/vacdetect.h"
#include "../../cs2/datatypes/viewmatrix/viewmatrix.h"

C_CSPlayerPawn* H::SafeLocalPlayer() noexcept
{
	if (!oGetLocalPlayer)
		return nullptr;
	C_CSPlayerPawn* p = nullptr;
	__try {
		p = oGetLocalPlayer(0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!p)
		return nullptr;
	// range+vtable only — no VirtualQuery (hot path)
	const auto a = reinterpret_cast<std::uintptr_t>(p);
	if (a < 0x10000ull || a > 0x00007FFFFFFFFFFFull)
		return nullptr;
	void* vt = nullptr;
	__try {
		vt = *reinterpret_cast<void**>(p);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	const auto va = reinterpret_cast<std::uintptr_t>(vt);
	if (va < 0x10000ull || va > 0x00007FFFFFFFFFFFull)
		return nullptr;

	// TDM death/respawn: probe health read — free'd pawn fails SEH / garbage hp.
	// Dead pawns still returned (EnemySpec); combat uses SafeLocalAlive.
	int hp = 0;
	__try {
		hp = p->m_iHealth();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (hp < 0 || hp > 200)
		return nullptr;
	return p;
}

// Alive local only — CreateMove / combat / skin apply. Spec uses SafeLocalPlayer.
C_CSPlayerPawn* H::SafeLocalAlive() noexcept
{
	C_CSPlayerPawn* p = SafeLocalPlayer();
	if (!p)
		return nullptr;
	int hp = 0;
	std::uint8_t life = 1;
	__try {
		hp = p->m_iHealth();
		life = p->m_lifeState();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (hp <= 0 || hp > 200 || life != 0)
		return nullptr;
	return p;
}

// Menu input block hooks — do NOT ShowCursor here (refcount spam); Present owns cursor.
void __fastcall H::hkIsRelativeMouseMode(void* pInputSystem, bool active)
{
	if (pInputSystem)
		g_pInputSystem = pInputSystem;

	// Track preferred mode for restore-on-close:
	// - Menu closed: trust every game request
	// - Menu open: only latch true (ignore forced-false noise while UI is up)
	if (!g_bMenuOpen || active)
		g_wantRelativeMouse = active;

	auto orig = IsRelativeMouseMode.GetOriginal();
	if (!orig)
		return;

	// Menu open: force absolute mouse for ImGui cursor; closed: honor game
	orig(pInputSystem, g_bMenuOpen ? false : active);
}

bool __fastcall H::hkMouseInputEnabled(void* rcx)
{
	// Block game mouse look/click while menu is up (ImGui uses WndProc path)
	if (g_bMenuOpen)
		return false;

	auto orig = MouseInputEnabled.GetOriginal();
	if (orig)
		return orig(rcx);
	return true;
}

static void sehFrameStage(void* a1, int stage) {
	(void)a1;
	__try {
		const bool soft = VacDetect::IsSoftPaused();

		// Knife: ttapp uses stage 7 = FRAME_NET_UPDATE_END (CreateMove alone is overwritten)
		if (!soft && stage == FRAME_NET_UPDATE_END) {
			// Death edge even when local null/dead — player_death event often misses free'd pawn
			__try { SkinChanger::OnLifeTick(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			// Alive only — skins/bones on dying/respawn pawn AV (TDM)
			if (C_CSPlayerPawn* local = H::SafeLocalAlive()) {
				__try { SkinChanger::OnFrameStage(local, stage); }
				__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Skin.FSN_net", GetExceptionCode()); }
			}
			// celerity: warm custom weather particles outside render frame
			World::Weather::WarmTick();
		}

		if (!soft && stage == FRAME_RENDER_START) {
			// Dead-only: force observer target onto enemies (comp/MM block)
			__try { EnemySpec::OnFrame(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("EnemySpec", GetExceptionCode()); }
		}

		if (stage == FRAME_RENDER_END) {
			// Soft-pause: no world mutators / weather / cache work
			if (!soft) {
				// UC: custom SetModel on FRAME_RENDER_END
				if (C_CSPlayerPawn* local = H::SafeLocalAlive()) {
					__try { SkinChanger::OnFrameStage(local, stage); }
					__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Skin.FSN_re", GetExceptionCode()); }
				}
				__try { World::Update(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("World.Update", GetExceptionCode()); }
				__try { Esp::cache(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Esp.cache", GetExceptionCode()); }
				__try { World::Weather::Update(); }
				__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Weather.Update", GetExceptionCode()); }
			} else {
				// Weather::Update tears down when soft-paused
				__try { World::Weather::Update(); }
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("FrameStageNotify", GetExceptionCode());
	}
}

// Full input strip while menu is open (attack, move, look, weapon scroll, etc.)
static void strip_menu_input(CUserCmd* cmd)
{
	if (!cmd)
		return;

	// Zero every button path — wheel weapon-switch often lands in nValueScroll
	cmd->nButtons.nValue = 0;
	cmd->nButtons.nValueChanged = 0;
	cmd->nButtons.nValueScroll = 0;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (base) {
		base->flForwardMove = 0.f;
		base->flSideMove = 0.f;
		base->flUpMove = 0.f;
		base->nMousedX = 0;
		base->nMousedY = 0;
		base->nImpulse = 0;
		// Mouse wheel → weapon select; must clear or scroll still swaps guns
		base->nWeaponSelect = 0;

		// IDA: buttons_pb @ +0x38; skip if unset / obviously bad
		if (base->pInButtonState
			&& reinterpret_cast<uintptr_t>(base->pInButtonState) > 0x10000ull) {
			base->pInButtonState->nValue = 0;
			base->pInButtonState->nValueChanged = 0;
			base->pInButtonState->nValueScroll = 0;
		}

		auto& field = base->subtickMovesField;
		if (field.pRep && field.nCurrentSize > 0 && field.nCurrentSize <= 64
			&& field.pRep->nAllocatedSize > 0 && field.pRep->nAllocatedSize <= 128) {
			const int n = (field.nCurrentSize < field.pRep->nAllocatedSize)
				? field.nCurrentSize : field.pRep->nAllocatedSize;
			for (int i = 0; i < n; ++i) {
				CSubtickMoveStep* step = field.pRep->tElements[i];
				if (!step)
					continue;
				step->nButton = 0;
				step->bPressed = false;
			}
		}
	}

	cmd->csgoUserCmd.nAttack1StartHistoryIndex = -1;
	cmd->csgoUserCmd.nAttack2StartHistoryIndex = -1;
}

static void sehCreateMovePost(CUserCmd* user_cmd)
{
	__try {
		if (!user_cmd)
			return;

		if (!I::EngineClient && !Engine2::NetworkGameClient())
			return;

		const bool engOk = I::EngineClient && I::EngineClient->in_game() && I::EngineClient->connected();
		const bool e2Ok = Engine2::IsInGame() || Engine2::Ready();
		if (!engOk && !e2Ok)
			return;

		// VAC soft-pause: hooks stay, no aim/trigger/movement/skin writes
		if (VacDetect::IsSoftPaused())
			return;

		// Alive only — dead/respawn pawn has free'd weapon services (TDM crash)
		C_CSPlayerPawn* pLocalPawn = H::SafeLocalAlive();
		if (!pLocalPawn)
			return;

		// Andromeda CL_Bypass: clear subtick queue before feature mutations
		CL_Bypass::PreClientCreateMove(user_cmd);

		// Strip first so wheel/attack/move die; SkinChanger may then set nWeaponSelect
		if (g_bMenuOpen) {
			strip_menu_input(user_cmd);
			// Clear stale aim/trigger shoot flags (CreateMove used to skip Aimbot while open)
			Aimbot(user_cmd);
		}

		// Knife mid-round re-equip (nWeaponSelect) — runs even with menu open
		SkinChanger::OnCreateMove(pLocalPawn, user_cmd);

		if (g_bMenuOpen) {
			CL_Bypass::PostClientCreateMove(Input::pCSGOInput, user_cmd);
			return;
		}

		// Order: strafe → jumpbug claim → bhop (live flags) → Pred (aim only) → aim
		// Bhop does NOT use Pred re-sim — ProcessMovement re-run = land lag/desync.
		if (g_movement)
			g_movement->OnCreateMove(user_cmd);

		if (Config::jumpbug || Config::edgebug || Config::edgejump) {
			__try {
				JumpBug::OnCreateMove(user_cmd, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("JumpBug", GetExceptionCode());
			}
		}

		// Bhop BEFORE Pred — live FL_ONGROUND edge, cmd-only.
		if (Config::bhop) {
			__try {
				SubtickMove::RewriteBhop(user_cmd, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("SubtickBhop", GetExceptionCode());
			}
		}

		// Pred for aim/AF/TR only — not for bhop.
		const bool needPred =
			Config::autofire
			|| Config::triggerbot
			|| keybind.isActive(Config::aimbot)
			|| Config::backtrack
			|| (Config::autostrafe && Config::autostrafe_mode == 1)
			|| Config::jumpbug
			|| Config::edgebug
			|| Config::edgejump;
		const bool predOn = needPred ? Pred::Start(user_cmd) : false;

		AimCommon::CollectAimTargets(pLocalPawn);

		if (Backtrack::WantRecords()) {
			__try {
				Backtrack::OnCreateMove(user_cmd, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("Backtrack", GetExceptionCode());
			}
		}

		// Same-tick cmd angles for aim/AF/TR (SetViewAngles + silent StampCmdAngles)
		AimCommon::BindCmd(user_cmd);
		Aimbot(user_cmd);
		AimHumanize_OnCreateMove(user_cmd);
		AimCommon::UnbindCmd();
		KnifeBot::OnCreateMove(pLocalPawn, user_cmd);
		if (Config::auto_pistol)
			AutoPistol::OnCreateMove(pLocalPawn, user_cmd);

		if (Config::auto_defuse) {
			__try {
				if (void* c4 = Bomb::PlantedC4Entity())
					Bomb::TryStartDefuse(c4, pLocalPawn);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		if (predOn)
			Pred::End();

		// Sanitize: keep flWhen==1.0 (UC release); do not clamp to 0.999
		if (user_cmd->csgoUserCmd.pBaseCmd)
			InputInject::SanitizeSubticks(user_cmd->csgoUserCmd.pBaseCmd);

		// Buttons/subtick flush only — CRC rewrite stays disabled (serialize-safe)
		CL_Bypass::PostClientCreateMove(Input::pCSGOInput, user_cmd);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		AimCommon::UnbindCmd();
		Pred::End();
		Con::Seh("CreateMove post", GetExceptionCode());
	}
}

void __fastcall H::hkCreateMove(void* pInput, int slot, bool active)
{
	// Pre-CM: pack CCSGOInput + moveSvc for ProcessMovement (runs inside original).
	// LUT: only +0x258 = code 3. Held +0x250|+0x258 = code 1 (dead hop).
	if (pInput)
		Input::pCSGOInput = pInput;

	if (slot == 0 && Config::bhop && pInput) {
		__try {
			SubtickMove::PreCreateMoveBhop(pInput);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Con::Seh("PreBhop", GetExceptionCode());
		}
	}

	if (CreateMove.IsHooked()) {
		auto original = CreateMove.GetOriginal();
		if (original) {
			CL_Bypass::SetInOriginalCreateMove(true);
			original(pInput, slot, active);
			CL_Bypass::SetInOriginalCreateMove(false);
		}
	}

	// Only local player slot
	if (slot != 0)
		return;

	// After original CreateMove — resolve cmd via SetupCmd / Andromeda tick+array path
	CUserCmd* user_cmd = Input::get_user_cmd(0);
	if (!user_cmd) {
		static int s_nullCount = 0;
		if (s_nullCount < 5) {
			++s_nullCount;
			Con::Error(
				"get_user_cmd null (SetupCmd=%p GetCmd=%p Array=%p Tick=%p Table=%p)",
				(void*)Input::SetupCmd,
				(void*)Input::GetCUserCmdBySequenceNumber,
				(void*)Input::GetCUserCmdArray,
				(void*)Input::GetEntityCmdSlot,
				Input::ppUserCmdArrayTable);
		}
		return;
	}

	sehCreateMovePost(user_cmd);
}

void __fastcall H::hkFrameStageNotify(void* a1, int stage)
{
	// Null original = pattern miss / disable race — never call through nullptr
	if (FrameStageNotify.IsHooked()) {
		if (auto original = FrameStageNotify.GetOriginal())
			original(a1, stage);
	}
	sehFrameStage(a1, stage);
}

static void sehDisablePVS(void* pvs) {
	__try {
		M::vfunc<void*, 6U, void>(pvs, false);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("LevelInit PVS vfunc", GetExceptionCode());
	}
}

static bool pvs_init = false;
static void* g_pPVS = nullptr;

static void sehSkinMapLoad() {
	__try { SkinChanger::OnMapLoad(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* __fastcall H::hkLevelInit(__int64 a1, __int64 a2) {
	const auto original = H::LevelInit.GetOriginal();
	if (!original) return nullptr;

	// IDA: this pattern is NOT CHLClient::LevelInit (it's a cmd/tick path).
	// a2 is NOT a map name — treating it as one wiped GameMode map cache.
	(void)a2;
	static char s_lastMap[128]{};
	GameMode::EnsureMap();
	const char* map = GameMode::BaseMap();
	const bool mapChanged = map && map[0]
		&& (!s_lastMap[0] || _stricmp(s_lastMap, map) != 0);
	if (mapChanged) {
		std::snprintf(s_lastMap, sizeof(s_lastMap), "%s", map);
		// Same combat wipe as LevelShutdown (handles mid-session map without shutdown)
		Aimbot_Reset();
		Backtrack::Clear();
		Pred::Invalidate();
		Esp::InvalidateCaches();
		GameMode::OnLevelInit(map);
		NadePred::Reset();
		NadeLineup::OnLevelInit(map);
		Bomb::RefreshSites();
		World::InvalidateEnvCache();
		World::Weather::Shutdown();
		World::Weather::OnLevelChange(); // re-extract assets + re-arm particle warm
		World::Fog::Shutdown();
		// Clear marks only — re-Install so patterns stay live after map change
		Hitmarker::Shutdown();
		Hitmarker::Install();
		Hitsound::Shutdown();
		SoundEsp::Clear();
		sehSkinMapLoad();
	}

	if (!pvs_init) {
		uintptr_t pvs_addr = M::patternScan(XS("engine2"), XS("48 8D 0D ? ? ? ? 33 D2 FF 50"));
		if (pvs_addr)
			g_pPVS = (void*)M::getAbsoluteAddress(pvs_addr, 0x3);
		if (!g_pPVS)
			g_pPVS = Engine2::PvsManager();
		pvs_init = true;
	}

	if (g_pPVS)
		sehDisablePVS(g_pPVS);

	return original(a1, a2);
}

void H::Hooks::init() {

	uintptr_t weapon_data_addr = M::patternScan(XS("client"), XS("48 8B 81 ? ? ? ? 85 D2 78 ? 48 83 FA ? 73 ? F3 0F 10 84 90 ? ? ? ? C3 F3 0F 10 80 ? ? ? ? C3 CC CC CC CC"));
	if (weapon_data_addr)
		oGetWeaponData = *reinterpret_cast<int*>(weapon_data_addr + 0x3);
	else
		Con::OffsetMiss("oGetWeaponData pattern");

	uintptr_t base_entity_addr = M::patternScan(XS("client"), XS("4C 8D 49 ? 81 FA"));
	if (base_entity_addr)
		ogGetBaseEntity = reinterpret_cast<decltype(ogGetBaseEntity)>(base_entity_addr);
	else
		Con::OffsetMiss("ogGetBaseEntity pattern");

	uintptr_t local_player_addr = M::patternScan(XS("client"), XS("48 83 EC 28 83 F9 FF 75 ? 48 8B 0D ? ? ? ? 48 8D 54 24 30 48 8B 01 FF 90 ? ? ? ? 8B 08 48 63 C1 4C 8D 05"));
	if (local_player_addr)
		oGetLocalPlayer = reinterpret_cast<decltype(oGetLocalPlayer)>(local_player_addr);
	else
		Con::OffsetMiss("oGetLocalPlayer pattern");

	// Andromeda CL_Bypass — MessageLite::SerializePartialToArray (move_crc capture)
	CL_Bypass::Init();

	// UC / IDA CCSGOInput::CreateMove — builds usercmd + subticks (not the old CHLClient-style fn)
	// pattern: 85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40  (IDA 0x180B09520)
	uintptr_t create_move_addr = M::patternScan(XS("client"),
		XS("85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40"));
	if (!create_move_addr) {
		// fallback older dump pattern
		create_move_addr = M::patternScan(XS("client"),
			XS("48 8B C4 4C 89 40 18 48 89 48 08 55 53 41 54 41 55"));
	}
	if (create_move_addr)
	{
		if (!CreateMove.Add(reinterpret_cast<void*>(create_move_addr),
			reinterpret_cast<void*>(&hkCreateMove)))
			Con::Error("CreateMove hook.Add failed @ 0x%p", (void*)create_move_addr);
		else
			Con::Ok("CreateMove (CCSGOInput) @ 0x%p", (void*)create_move_addr);
	}
	else
		Con::OffsetMiss("CreateMove pattern");

	auto addHook = [](auto& hook, uintptr_t addr, auto detour, const char* name) {
		if (!addr) {
			Con::OffsetMiss(name);
			return;
		}
		if (!hook.Add(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(detour)))
			Con::Error("%s hook.Add failed @ 0x%p", name, (void*)addr);
		else
			Con::Ok("%s @ 0x%p", name, (void*)addr);
	};

	addHook(FrameStageNotify, M::patternScan(XS("client"), XS("48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED")), &hkFrameStageNotify, "FrameStageNotify");
	// CAnimatableSceneObjectDescRender — long unique first, short fallback (live RVA 0x539D0)
	{
		uintptr_t da = M::patternScan(XS("scenesystem"),
			XS("48 8B C4 53 57 41 54 48 81 EC D0 00 00 00 49 63"));
		if (!da)
			da = M::patternScan(XS("scenesystem"), XS("48 8B C4 53 57 41 54"));
		addHook(DrawArray, da, &chams::hook, "DrawArray");
	}
	// DRAWSKYBOXARRAY — scenesystem sky draw; tint floats live on scene object +0xE8..+0xF0
	addHook(DrawSkyboxArray,
		M::patternScan(XS("scenesystem"),
			XS("45 85 C9 0F 8E ? ? ? ? 4C 8B DC 55 41 56 49 8D AB 58 FC FF FF 48 81 EC 98 04 00 00")),
		&hkDrawSkyboxArray, "DrawSkyboxArray");
	// Andromeda-style: Aggregate lightData map tint + LightSceneObject / GlobalLightUpdate
	World::InstallMapColorHook();
	World::InstallLightingHook();
	World::Weather::Install();
	Hitmarker::Install();
	Hitsound::Install();
	SoundEsp::Install();
	Vote::Install();
	Panorama::Install();
	addHook(GetRenderFov, M::patternScan(XS("client"), XS("40 53 48 83 EC ? 48 8B D9 E8 ? ? ? ? 48 85 C0 74 ? 48 8B C8 48 83 C4")), &hkGetRenderFov, "GetRenderFov");
	// CALCVIEWMODEL — viewmodel XYZ + FOV (bypass engine clamps after original)
	addHook(GetViewModelOffsets,
		M::patternScan(XS("client"), XS("40 55 53 56 41 56 41 57 48 8B EC 48 83 EC 20 4D")),
		&hkGetViewModelOffsets, "GetViewModelOffsets");
	// GetScreenAspectRatio — engine2 IVEngineClient vtable slot 88
	// pattern dump: 48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8D 0D (rva 0x76050)
	addHook(GetScreenAspectRatio,
		M::patternScan(XS("engine2"),
			XS("48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8D 0D")),
		&hkGetScreenAspectRatio, "GetScreenAspectRatio");
	// OVERRIDEVIEW — CViewSetup origin/angles (third person)
	// dump: pattern::client::OverrideView primary; loose fallback
	{
		uintptr_t ov = M::patternScan(XS("client"),
			XS("40 57 48 83 EC 60 48 8B FA E8 ? ? ? ? BA FF"));
		if (!ov)
			ov = M::patternScan(XS("client"),
				XS("40 57 48 83 EC ? 48 8B FA E8 ? ? ? ? BA"));
		addHook(OverrideView, ov, &hkOverrideView, "OverrideView");
	}
	// DRAWSCOPEOVERLAY — sniper scope HUD out-struct (bars / blur / texture)
	addHook(DrawScopeOverlay,
		M::patternScan(XS("client"), XS("48 8B C4 53 57 48 83 EC ? 48 8B FA")),
		&hkDrawScopeOverlay, "DrawScopeOverlay");

	W2S::Init();
	// GetMatrixForView dump (RVA 0x1666C0) is a 3-arg FOV helper — NOT matrix
	// capture. Hooking it as 6-arg poisoned W2S live matrix / stack. Use
	// ScreenTransform + pViewMatrix instead (see w2s.cpp).
	// DrawCrosshair — reticle allow-gate (rifles / knives). Unique dump pattern.
	addHook(DrawCrosshair,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 85")),
		&hkDrawCrosshair, "DrawCrosshair");
	// WeaponHidesCrosshair — sniper zoom gate that clears HUD show-flag before DrawCrosshair
	addHook(WeaponHidesCrosshair,
		M::patternScan(XS("client"),
			XS("48 8D 81 70 E6 FF FF 48 85 C0 74 ? 83 B8 40 1A 00 00 01")),
		&hkWeaponHidesCrosshair, "WeaponHidesCrosshair");
	// IDA 0x180F5BBF0 ShouldShowHudElements — unique via dual ConVar probes + 80 38 00
	addHook(ShouldShowHudElements,
		M::patternScan(XS("client"),
			XS("48 83 EC 28 BA FF FF FF FF 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 85 C0 75 ? 48 8B 05 ? ? ? ? 48 8B 40 ? 80 38 00 74 ? BA FF FF FF FF")),
		&hkShouldShowHudElements, "ShouldShowHudElements");
	// UpdatePostProcessing dump pattern was tournament dem handler — NOT hooked.
	// remove_postprocess pokes cl_disable_postprocessing each World::Update tick.

	addHook(LevelInit, M::patternScan(XS("client"), XS("40 55 56 41 56 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B")), &hkLevelInit, "LevelInit");
	// FlashOverlay — dump FlashOverlay; IDA 0x18113C960 "FlashbangOverlay"
	addHook(RenderFlashBangOverlay,
		M::patternScan(XS("client"), XS("85 D2 0F 88 ? ? ? ? 48 89 4C 24 08 55 56")),
		&hkRenderFlashbangOverlay, "FlashOverlay");
	// DRAWLEGS — Firstperson Legs
	addHook(DrawLegs,
		M::patternScan(XS("client"), XS("40 55 53 56 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? F2 0F 10 42")),
		&hkDrawLegs, "DrawLegs");
	// DRAWSMOKEVERTEX — smoke volume
	addHook(DrawSmokeVertex,
		M::patternScan(XS("client"), XS("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 48 8B 9C 24 ? ? ? ? 4D 8B F8")),
		&hkDrawSmokeVertex, "DrawSmokeVertex");
	// DRAWSMOKEARRAY — live client.dll 0xCB4AD0 (frame C8 07); IDA was D8 07 — wildcard both
	// Unique: mov [rsp+10],rdx; push rbp; push r12; lea rbp; sub rsp; mov r12,rdx; lea rcx; mov edx,-1
	{
		uintptr_t smokeArr = M::patternScan(XS("client"),
			XS("48 89 54 24 10 55 41 54 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 4C 8B E2 48 8D 0D ? ? ? ? BA FF FF FF FF"));
		if (!smokeArr)
			smokeArr = M::patternScan(XS("client"),
				XS("48 89 54 24 10 55 41 54 48 8D AC 24 ? ? ? ? 48 81 EC ? 07 00 00 4C 8B E2"));
		addHook(DrawSmokeArray, smokeArr, &hkDrawSmokeArray, "DrawSmokeArray");
	}
	// RENDERDECALS — bullet / blood / explosion decals
	addHook(RenderDecals,
		M::patternScan(XS("client"), XS("44 88 4C 24 ? 48 89 54 24 ? 55 53 57")),
		&hkRenderDecals, "RenderDecals");
	// CacheParticleEffect — real particle spawn (IDA 0x18078EE10). Old CreateParticleEffect = SetCP.
	addHook(CacheParticleEffect,
		M::patternScan(XS("client"),
			XS("4C 8B DC 53 48 81 EC 90 00 00 00 F2 0F 10 05")),
		&hkCacheParticleEffect, "CacheParticleEffect");
	// ParticleDrawArray — particles.dll fire/molly tint (UC Particle Modulation, IDA 0x1802826D0)
	// Write RGB floats at a2+0x50 before draw. Name filter in detour (inferno_fx / groundfire).
	addHook(ParticleDrawArray,
		M::patternScan(XS("particles"),
			XS("48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55")),
		&hkParticleDrawArray, "ParticleDrawArray");

	addHook(MouseInputEnabled, M::patternScan(XS("client"), XS("40 53 48 83 EC 20 80 B9 ? ? ? ? ? 48 8B D9 75 78")), &hkMouseInputEnabled, "MouseInputEnabled");
	addHook(IsRelativeMouseMode, M::patternScan(XS("inputsystem"), XS("48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC ? 0F B6 F2")), &hkIsRelativeMouseMode, "IsRelativeMouseMode");
	addHook(DrawGlow, M::patternScan(XS("client"), XS("40 53 48 83 EC 20 48 8B 54")), &hkDrawGlow, "DrawGlow");
	// GetGlowColor — last-mile float4 before ManageGlow; isolates glow from chams mesh tint
	addHook(GetGlowColor,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 54")),
		&hkGetGlowColor, "GetGlowColor");
	// ManageGlowSceneObject — force pure glow colour + backface mult = 1.0f
	addHook(ManageGlowSceneObject,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 48 8B F2 45 33 F6")),
		&hkManageGlowSceneObject, "ManageGlowSceneObject");
	// ApplyGlowScene (B04B30) — pre-stamp + post force scene float4 (chams-proof)
	addHook(ApplyGlowScene,
		M::patternScan(XS("client"),
			XS("48 89 5C 24 ? 48 89 6C 24 ? 48 89 7C 24 ? 41 56 48 81 EC 80 00 00 00")),
		&hkApplyGlowScene, "ApplyGlowScene");
	addHook(FireEventClientSide,
		M::patternScan(XS("client"), XS("40 53 41 54 41 56 48 83 EC ? 4C 8B F2")),
		&hkFireEventClientSide, "FireEventClientSide");
	// yougey wetness/rain: CMapInfo env rain strength + wetness coverage
	// Dump SetupMapInfo — wildcard stack saves; hard E8 broke on some builds
	uintptr_t setupMap = M::patternScan(XS("client"),
		XS("48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 57 48 81 EC ? ? ? ? 0F 29 70 ? 48 8B EA 0F 29 78 ? 45 33 C0"));
	if (!setupMap)
		setupMap = M::patternScan(XS("client"),
			XS("48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 81 EC ? ? ? ? 0F 29 70 E8 48 8B EA"));
	addHook(SetupMapInfo, setupMap, &hkSetupMapInfo, "SetupMapInfo");

	// Priority A: entity add/remove (gen bump) + LevelShutdown map unload
	if (void* a = SdkPrioA::OnAddAddr()) {
		addHook(OnAddEntity, reinterpret_cast<uintptr_t>(a), &hkOnAddEntity, "OnAddEntity");
		if (OnAddEntity.IsHooked())
			SdkPrioA::MarkHooked("OnAddEntity", "gen bump only");
	}
	if (void* a = SdkPrioA::OnRemoveAddr()) {
		addHook(OnRemoveEntity, reinterpret_cast<uintptr_t>(a), &hkOnRemoveEntity, "OnRemoveEntity");
		if (OnRemoveEntity.IsHooked())
			SdkPrioA::MarkHooked("OnRemoveEntity", "gen bump only");
	}
	if (void* a = SdkPrioA::LevelShutdownAddr()) {
		addHook(LevelShutdown, reinterpret_cast<uintptr_t>(a), &hkLevelShutdown, "LevelShutdown");
		if (LevelShutdown.IsHooked())
			SdkPrioA::MarkHooked("LevelShutdown", "map gen + feature cleanup");
	}
}

void __fastcall H::hkOnAddEntity(void* entitySystem, void* entity, int handle) {
	__try { SdkPrioA::OnEntityAdded(entitySystem, entity, handle); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	if (OnAddEntity.IsHooked()) {
		auto original = OnAddEntity.GetOriginal();
		if (original)
			original(entitySystem, entity, handle);
	}
}

void __fastcall H::hkOnRemoveEntity(void* entitySystem, void* entity, int handle) {
	__try { SdkPrioA::OnEntityRemoved(entitySystem, entity, handle); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	if (OnRemoveEntity.IsHooked()) {
		auto original = OnRemoveEntity.GetOriginal();
		if (original)
			original(entitySystem, entity, handle);
	}
}

void* __fastcall H::hkLevelShutdown(void* a1) {
	__try { SdkPrioA::OnLevelShutdown(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	if (LevelShutdown.IsHooked()) {
		auto original = LevelShutdown.GetOriginal();
		if (original)
			return original(a1);
	}
	return nullptr;
}

std::uintptr_t __fastcall H::hkSetupMapInfo(std::uintptr_t mapInfo, void* unk) {
	// yougey writes rain/wetness before original.
	__try { World::Weather::ApplyMapInfo(mapInfo); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	if (SetupMapInfo.IsHooked()) {
		auto original = SetupMapInfo.GetOriginal();
		if (original)
			return original(mapInfo, unk);
	}
	return 0;
}

void* __fastcall H::hkDrawGlow(void* glowProp) {
	// Stamp colours before AND after original so mid-call systems can't keep a chams tint.
	__try {
		Glow::OnDrawGlow(reinterpret_cast<Glow::CGlowProperty*>(glowProp));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("hkDrawGlow", GetExceptionCode());
	}
	void* ret = nullptr;
	if (DrawGlow.IsHooked()) {
		auto original = DrawGlow.GetOriginal();
		if (original)
			ret = original(glowProp);
	}
	__try {
		Glow::OnDrawGlow(reinterpret_cast<Glow::CGlowProperty*>(glowProp));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	if (ret)
		return ret;
	auto* g = reinterpret_cast<Glow::CGlowProperty*>(glowProp);
	if (g && g->ok()) {
		return reinterpret_cast<void*>(
			static_cast<std::uintptr_t>(
				static_cast<unsigned int>(g->glow_type())));
	}
	return nullptr;
}

void __fastcall H::hkGetGlowColor(void* glowProp, float* outRgba) {
	// IDA B499F0: engine pre-fills outRgba from mesh/entity, then merges override
	// only if RGB≠0 or A≠255. For players we fully own the float4 — skip original
	// so chams vertex tint never seeds the buffer.
	__try {
		if (Glow::ForceSceneColor(reinterpret_cast<Glow::CGlowProperty*>(glowProp), outRgba))
			return;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("hkGetGlowColor force", GetExceptionCode());
	}
	if (GetGlowColor.IsHooked()) {
		auto original = GetGlowColor.GetOriginal();
		if (original) {
			__try {
				original(glowProp, outRgba);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
	}
}

void* __fastcall H::hkManageGlowSceneObject(void** glowSceneOut, void* a2, void* sceneNode,
	float* color4, int a5, int a6, int a7, int a8) {
	// a8 is m_flGlowBackfaceMult — force 1.0f (pure colour path, not mesh-linked).
	// color4: re-apply Config glow if GetGlowColor just pushed a force.
	(void)a8;
	__try {
		Glow::ForceManageColor(color4);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	constexpr int kBackfaceOne = 0x3F800000; // 1.0f
	if (ManageGlowSceneObject.IsHooked()) {
		auto original = ManageGlowSceneObject.GetOriginal();
		if (original)
			return original(glowSceneOut, a2, sceneNode, color4, a5, a6, a7, kBackfaceOne);
	}
	return nullptr;
}

std::int64_t __fastcall H::hkApplyGlowScene(void* glowProp, void* sceneNode) {
	// IDA B04B30: complete glow pipeline. Stamp Config colours before original
	// and force scene-object float4 after so chams cannot recolour the outline.
	using Fn = std::int64_t(__fastcall*)(Glow::CGlowProperty*, void*);
	Fn original = nullptr;
	if (ApplyGlowScene.IsHooked())
		original = reinterpret_cast<Fn>(ApplyGlowScene.GetOriginal());
	return Glow::OnApplyGlowScene(
		reinterpret_cast<Glow::CGlowProperty*>(glowProp), sceneNode, original);
}

bool __fastcall H::hkFireEventClientSide(void* eventManager, void* gameEvent) {
	// rewrite weapon string BEFORE original so killfeed sees spoofed knife
	__try { SkinChanger::OnFireEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	// Round boundary wipe — LevelInit only runs on map load, not each round
	__try { NadePred::OnGameEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	// Weather particles die each round — force rebuild without menu reselect
	__try { World::Weather::OnGameEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Hitmarker::OnGameEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { SoundEsp::OnGameEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Vote::OnGameEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	if (FireEventClientSide.IsHooked()) {
		auto original = FireEventClientSide.GetOriginal();
		if (original)
			return original(eventManager, gameEvent);
	}
	return false;
}
