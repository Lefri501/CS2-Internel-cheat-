#include "hooks.h"
#include <iostream>

#include "../../templeware/utils/memory/Interface/Interface.h"
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
#include "../interfaces/interfaces.h"
#include "../features/aim/aim.h"
#include "../features/movement/movement.h"
#include "../features/world/world.h"
#include "../features/world/weather.h"
#include "../features/glow/glow.h"
#include "../features/skinchanger/skinchanger.h"
#include "../features/gamemode/gamemode.h"
#include "../features/nade_pred/nade_pred.h"
#include "../utils/security/vacdetect.h"

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
	__try {
		const bool soft = VacDetect::IsSoftPaused();

		// Knife: ttapp uses stage 7 = FRAME_NET_UPDATE_END (CreateMove alone is overwritten)
		if (!soft && stage == FRAME_NET_UPDATE_END) {
			if (H::oGetLocalPlayer) {
				if (C_CSPlayerPawn* local = H::oGetLocalPlayer(0)) {
					const int hp = local->m_iHealth();
					if (hp > 0 && hp <= 200)
						SkinChanger::OnFrameStage(local, stage);
				}
			}
		}

		if (stage == FRAME_RENDER_END) {
			// Soft-pause: no world mutators / weather / cache work
			if (!soft) {
				World::Update();
				Esp::cache();
				World::Weather::Update();
			} else {
				// Weather::Update tears down when soft-paused
				World::Weather::Update();
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

		if (base->pInButtonState) {
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
	cmd->csgoUserCmd.nAttack3StartHistoryIndex = -1;
}

static void sehCreateMovePost()
{
	__try {
		if (!I::EngineClient)
			return;

		if (!I::EngineClient->in_game() || !I::EngineClient->connected())
			return;

		// VAC soft-pause: hooks stay, no aim/trigger/movement/skin writes
		if (VacDetect::IsSoftPaused())
			return;

		if (!H::oGetLocalPlayer)
			return;

		C_CSPlayerPawn* pLocalPawn = H::oGetLocalPlayer(0);
		if (!pLocalPawn)
			return;

		const int hp = pLocalPawn->m_iHealth();
		if (hp <= 0 || hp > 200)
			return;

		CUserCmd* user_cmd = Input::get_user_cmd(0);
		if (!user_cmd) {
			static bool once = false;
			if (!once) {
				once = true;
				Con::Error("get_user_cmd returned null (menu block / bhop / features depend on this)");
			}
			return;
		}

		// Strip first so wheel/attack/move die; SkinChanger may then set nWeaponSelect
		if (g_bMenuOpen) {
			strip_menu_input(user_cmd);
			// Clear stale aim/trigger shoot flags (CreateMove used to skip Aimbot while open)
			Aimbot(user_cmd);
		}

		// Knife mid-round re-equip (nWeaponSelect) — runs even with menu open
		SkinChanger::OnCreateMove(pLocalPawn, user_cmd);

		if (g_bMenuOpen)
			return;

		// Aim + RCS on CreateMove (same tick as input); then first-shot gate
		Aimbot(user_cmd);
		AimHumanize_OnCreateMove(user_cmd);

		if (g_movement)
			g_movement->OnCreateMove(user_cmd);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("CreateMove post", GetExceptionCode());
	}
}

void __fastcall H::hkCreateMove(void* rcx, int slot, float flInputSampleTime, bool active)
{
	// Always run engine first
	if (CreateMove.IsHooked()) {
		auto original = CreateMove.GetOriginal();
		if (original)
			original(rcx, slot, flInputSampleTime, active);
	}

	sehCreateMovePost();
}

void __fastcall H::hkFrameStageNotify(void* a1, int stage)
{
	if (FrameStageNotify.IsHooked())
		FrameStageNotify.GetOriginal()(a1, stage);
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

void* __fastcall H::hkLevelInit(__int64 a1, __int64 a2) {
	const auto original = H::LevelInit.GetOriginal();
	if (!original) return nullptr;

	// a2 is often map name (const char*) — used for workshop/aim FFA team-check
	GameMode::OnLevelInit(reinterpret_cast<const char*>(a2));
	// Fresh effect timers each map load (Reset already clears world fx + bomb track)
	NadePred::Reset();
	World::Weather::Shutdown();

	if (!pvs_init) {
		uintptr_t pvs_addr = M::patternScan(XS("engine2"), XS("48 8D 0D ? ? ? ? 33 D2 FF 50"));
		if (pvs_addr)
			g_pPVS = (void*)M::getAbsoluteAddress(pvs_addr, 0x3);
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

	uintptr_t create_move_addr = M::patternScan(XS("client"), XS("48 8B C4 4C 89 40 18 48 89 48 08 55 53 41 54 41 55"));
	if (create_move_addr)
	{
		if (!CreateMove.Add(reinterpret_cast<void*>(create_move_addr),
			reinterpret_cast<void*>(&hkCreateMove)))
			Con::Error("CreateMove hook.Add failed @ 0x%p", (void*)create_move_addr);
		else
			Con::Ok("CreateMove @ 0x%p", (void*)create_move_addr);
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
	addHook(DrawArray, M::patternScan(XS("scenesystem"), XS("48 8B C4 53 57 41 54")), &chams::hook, "DrawArray");
	// DRAWSKYBOXARRAY — scenesystem sky draw; tint floats live on scene object +0xE8..+0xF0
	addHook(DrawSkyboxArray,
		M::patternScan(XS("scenesystem"),
			XS("45 85 C9 0F 8E ? ? ? ? 4C 8B DC 55 41 56 49 8D AB 58 FC FF FF 48 81 EC 98 04 00 00")),
		&hkDrawSkyboxArray, "DrawSkyboxArray");
	// Andromeda-style: Aggregate lightData map tint + LightSceneObject / GlobalLightUpdate
	World::InstallMapColorHook();
	World::InstallLightingHook();
	World::Weather::Install();
	addHook(GetRenderFov, M::patternScan(XS("client"), XS("40 53 48 83 EC ? 48 8B D9 E8 ? ? ? ? 48 85 C0 74 ? 48 8B C8 48 83 C4")), &hkGetRenderFov, "GetRenderFov");
	// CALCVIEWMODEL — viewmodel XYZ + FOV (bypass engine clamps after original)
	addHook(GetViewModelOffsets,
		M::patternScan(XS("client"), XS("40 55 53 56 41 56 41 57 48 8B EC 48 83 EC 20 4D")),
		&hkGetViewModelOffsets, "GetViewModelOffsets");
	// OVERRIDEVIEW — CViewSetup origin/angles/FOV (third person)
	addHook(OverrideView,
		M::patternScan(XS("client"), XS("40 57 48 83 EC ? 48 8B FA E8 ? ? ? ? BA")),
		&hkOverrideView, "OverrideView");
	// DRAWSCOPEOVERLAY — sniper scope HUD out-struct (bars / blur / texture)
	addHook(DrawScopeOverlay,
		M::patternScan(XS("client"), XS("48 8B C4 53 57 48 83 EC ? 48 8B FA")),
		&hkDrawScopeOverlay, "DrawScopeOverlay");
	addHook(LevelInit, M::patternScan(XS("client"), XS("40 55 56 41 56 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B")), &hkLevelInit, "LevelInit");
	addHook(RenderFlashBangOverlay, M::patternScan(XS("client"), XS("85 D2 0F 88 ? ? ? ? 48 89 4C 24 ? 55 56")), &hkRenderFlashbangOverlay, "RenderFlashBangOverlay");
	// DRAWLEGS — Firstperson Legs
	addHook(DrawLegs,
		M::patternScan(XS("client"), XS("40 55 53 56 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? F2 0F 10 42")),
		&hkDrawLegs, "DrawLegs");
	// DRAWSMOKEVERTEX — smoke volume
	addHook(DrawSmokeVertex,
		M::patternScan(XS("client"), XS("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 48 8B 9C 24 ? ? ? ? 4D 8B F8")),
		&hkDrawSmokeVertex, "DrawSmokeVertex");
	// RENDERDECALS — bullet / blood / explosion decals
	addHook(RenderDecals,
		M::patternScan(XS("client"), XS("44 88 4C 24 ? 48 89 54 24 ? 55 53 57")),
		&hkRenderDecals, "RenderDecals");
	// CREATEPARTICLEEFFECT — particle spawn (blanket when enabled)
	addHook(CreateParticleEffect,
		M::patternScan(XS("client"), XS("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? F3 0F 10 1D ? ? ? ? 41 8B F8 8B DA 4C 8D 05")),
		&hkCreateParticleEffect, "CreateParticleEffect");

	addHook(MouseInputEnabled, M::patternScan(XS("client"), XS("40 53 48 83 EC 20 80 B9 ? ? ? ? ? 48 8B D9 75 78")), &hkMouseInputEnabled, "MouseInputEnabled");
	addHook(IsRelativeMouseMode, M::patternScan(XS("inputsystem"), XS("48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC ? 0F B6 F2")), &hkIsRelativeMouseMode, "IsRelativeMouseMode");
	addHook(DrawGlow, M::patternScan(XS("client"), XS("40 53 48 83 EC 20 48 8B 54")), &hkDrawGlow, "DrawGlow");
	addHook(FireEventClientSide,
		M::patternScan(XS("client"), XS("40 53 41 54 41 56 48 83 EC ? 4C 8B F2")),
		&hkFireEventClientSide, "FireEventClientSide");
	// yougey wetness/rain: CMapInfo env rain strength + wetness coverage
	addHook(SetupMapInfo,
		M::patternScan(XS("client"), XS("48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 81 EC ? ? ? ? 0F 29 70 E8 48 8B EA")),
		&hkSetupMapInfo, "SetupMapInfo");
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
	return Glow::OnDrawGlow(reinterpret_cast<Glow::CGlowProperty*>(glowProp));
}

bool __fastcall H::hkFireEventClientSide(void* eventManager, void* gameEvent) {
	// rewrite weapon string BEFORE original so killfeed sees spoofed knife
	__try { SkinChanger::OnFireEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	// Round boundary wipe — LevelInit only runs on map load, not each round
	__try { NadePred::OnGameEvent(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	if (FireEventClientSide.IsHooked()) {
		auto original = FireEventClientSide.GetOriginal();
		if (original)
			return original(eventManager, gameEvent);
	}
	return false;
}
