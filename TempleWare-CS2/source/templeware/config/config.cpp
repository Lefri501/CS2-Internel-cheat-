#include "config.h"
#include "../keybinds/keybinds.h"
#include "../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstring>

// CBA to make proper atm, it's 03:42 right now.
// For now just stores config values don't mind it too much
//
// (FYI THIS IS A HORRID SOLUTION BUT FUNCTIONS) 

namespace Config {
	bool esp = false;
	bool glow = false;
	bool glow_team = true;
	bool glow_enemy = true;
	bool glow_only_visible = false;
	ImVec4 glow_color = ImVec4(0.f, 1.f, 0.f, 1.f);
	ImVec4 glow_color_invis = ImVec4(1.f, 0.3f, 0.3f, 1.f);
	bool glow_world_weapons = false;
	bool glow_world_bomb = false;
	bool glow_world_grenades = false;
	bool showHealth = false;
	bool showArmor = false;
	bool showDistance = false;
	bool showWeapon = false;
	bool showWeaponIcon = false;
	bool teamCheck = false;
	bool espFill = false;
	bool showNameTags = false;
	bool esp_name_avatar = false;
	bool esp_skeleton = false;
	float esp_skeleton_thickness = 1.5f;
	ImVec4 esp_skeleton_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 esp_skeleton_color_invisible = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
	bool esp_vis_check = true;

	bool flag_flashed = false;
	bool flag_bomb = false;
	bool flag_scoped = false;
	bool flag_reloading = false;
	bool flag_defusing = false;

	bool world_esp_weapons = false;
	bool world_esp_bomb = true; // planted C4 ESP on by default
	bool world_esp_smoke = false;
	bool world_esp_molotov = false;
	bool world_esp_he = false;
	bool world_esp_flash = false;
	bool world_esp_decoy = false;
	bool world_esp_distance = false;
	ImVec4 world_esp_weapon_color = ImVec4(0.95f, 0.90f, 0.55f, 1.0f);
	ImVec4 world_esp_bomb_color = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
	ImVec4 world_esp_smoke_color = ImVec4(0.75f, 0.80f, 0.90f, 1.0f);
	ImVec4 world_esp_molotov_color = ImVec4(1.0f, 0.50f, 0.15f, 1.0f);
	ImVec4 world_esp_he_color = ImVec4(1.0f, 0.70f, 0.25f, 1.0f);
	ImVec4 world_esp_flash_color = ImVec4(0.95f, 0.95f, 0.55f, 1.0f);
	ImVec4 world_esp_decoy_color = ImVec4(0.65f, 0.85f, 0.55f, 1.0f);

	bool Night = false;
	float night_exposure = 0.45f; // darkness 0..1 (0 = none, 1 = darkest)

	bool skybox = false;
	ImVec4 skybox_color = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);

	bool lighting = false;
	ImVec4 lighting_color = ImVec4(1.0f, 0.92f, 0.75f, 1.0f);

	bool map_color = false;
	ImVec4 map_color_value = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);

	bool weather = false;
	int weather_mode = 1; // Rain default when enabled
	float weather_intensity = 0.55f;

	bool enemyChamsInvisible = false;
	bool enemyChams = false;
	bool teamChams = false;
	bool teamChamsInvisible = false;
	int chamsMaterial = 0;
	int chamsMaterialXQZ = 0;
	int armChamsMaterial = 0;
	int viewmodelChamsMaterial = 0;

	bool armChams = false;
	bool viewmodelChams = false;
	ImVec4 colViewmodelChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	ImVec4 colArmChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);

	ImVec4 colVisualChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	ImVec4 colVisualChamsIgnoreZ = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	ImVec4 teamcolVisualChamsIgnoreZ = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	ImVec4 teamcolVisualChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);

	float espThickness = 1.0f;
	float espFillOpacity = 0.5f;
	ImVec4 espColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	ImVec4 espColorInvisible = ImVec4(0.45f, 0.55f, 1.0f, 1.0f);

	int esp_box_style = ESP_BOX_FULL;
	float esp_box_width = 0.42f;
	float esp_bar_width = 3.0f;
	bool esp_health_auto = true;
	ImVec4 esp_health_color = ImVec4(0.20f, 0.90f, 0.25f, 1.0f);
	ImVec4 esp_armor_color = ImVec4(0.27f, 0.63f, 1.0f, 1.0f);
	ImVec4 esp_name_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 esp_weapon_color = ImVec4(1.0f, 0.86f, 0.47f, 1.0f);
	ImVec4 esp_distance_color = ImVec4(0.71f, 0.86f, 1.0f, 1.0f);

	bool fovEnabled = false;
	float fov = 90.0f;

	bool viewmodel_changer = false;
	float viewmodel_fov = 68.f;
	float viewmodel_x = 0.f;
	float viewmodel_y = 0.f;
	float viewmodel_z = 0.f;

	bool thirdperson = false;
	float thirdperson_distance = 150.f;
	int thirdperson_key = 0x04; // VK_MBUTTON
	int thirdperson_key_mode = 2; // Toggle

	bool antiflash = false;
	float antiflash_amount = 0.f;

	bool remove_legs = false;
	bool remove_smoke = false;
	bool remove_decals = false;
	bool remove_particles = false;

	bool scope_no_overlay = false;
	bool scope_remove_blur = false;
	bool scope_remove_bars = false;
	bool scope_remove_texture = false;
	bool scope_custom_look = false;
	float scope_size_scale = 1.f;
	float scope_blur_amount = 0.f;

	bool aimbot = false;
	float aimbot_fov = 5.f;
	float aimbot_smooth = 5.f; // 0 = instant, higher = smoother

	bool autofire = false;
	bool autofire_silent = false;
	float autofire_hitchance = 70.f; // default on — 0 = off
	bool autofire_autostop = false;
	bool autofire_autowall = false;
	float autofire_mindamage = 1.f;    // visible
	float autofire_mindamage_aw = 1.f; // through wall
	int autofire_key = 0x06; // VK_XBUTTON2
	int autofire_key_mode = 1; // Hold
	int autofire_target_select = AF_TARGET_CROSSHAIR;
	bool autofire_multipoint_dynamic = true;
	bool autofire_hitboxes[AF_MP_COUNT] = {
		true,  // head
		true,  // chest
		false, // stomach
		false  // pelvis
	};
	float autofire_multipoint_scale[AF_MP_COUNT] = {
		0.55f, // head
		0.60f, // chest
		0.55f, // stomach
		0.50f  // pelvis
	};

	bool team_check = true;
	bool team_check_auto = true;
	bool aim_vis_check = true;

	float aim_reaction_delay_ms = 0.f;
	float aim_target_switch_delay_ms = 0.f;
	float aim_first_shot_delay_ms = 0.f;
	// default: head + neck + chest
	bool aim_hitboxes[HB_COUNT] = {
		true,  // head
		true,  // neck
		true,  // chest
		false, // stomach
		false, // pelvis
		false, // arms
		false, // legs
		false  // feet
	};
	bool rcs = false;
	bool rcs_standalone = false;
	float rcs_scale_x = 0.5f;
	float rcs_scale_y = 0.5f;
	bool fov_circle = false;
	ImVec4 fovCircleColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

	AimWeaponProfile weapon_profiles[WG_COUNT]{};
	int weapon_group_ui = WG_RIFLE;
	int weapon_group_active = WG_GENERAL;

	int aimbot_key = 0x05; // VK_XBUTTON1
	int aimbot_key_mode = 1; // Hold

	bool triggerbot = false;
	int triggerbot_key = 0x12; // VK_MENU (Alt) — avoids autofire mouse5 clash
	int triggerbot_key_mode = 1; // Hold
	float trigger_delay_ms = 0.f;   // 0 = instant (flicks); raise for humanize
	float trigger_hitchance = 0.f;  // 0 = off; HC slows flick response
	bool trigger_autowall = false;
	bool trigger_scoped_only = false;
	bool trigger_hitboxes[HB_COUNT] = {
		true,  // HEAD
		true,  // NECK
		true,  // CHEST
		false, // STOMACH
		false, // PELVIS
		false, // ARMS
		false, // LEGS
		false  // FEET
	};
	bool trigger_autostop = false;
	int trigger_mode = TR_MODE_HITCHANCE;

	static AimWeaponProfile MakeDefaultProfile() {
		AimWeaponProfile p{};
		p.aimbot_fov = 5.f;
		p.aimbot_smooth = 5.f;
		p.aim_vis_check = true;
		for (int i = 0; i < HB_COUNT; ++i)
			p.aim_hitboxes[i] = false;
		p.aim_hitboxes[HB_HEAD] = true;
		p.aim_hitboxes[HB_NECK] = true;
		p.aim_hitboxes[HB_CHEST] = true;
		p.aim_reaction_delay_ms = 0.f;
		p.aim_target_switch_delay_ms = 0.f;
		p.aim_first_shot_delay_ms = 0.f;
		p.rcs = false;
		p.rcs_standalone = false;
		p.rcs_scale_x = 0.5f;
		p.rcs_scale_y = 0.5f;
		p.autofire_silent = false;
		p.autofire_hitchance = 70.f;
		p.autofire_autostop = false;
		p.autofire_autowall = false;
		p.autofire_mindamage = 1.f;
		p.autofire_mindamage_aw = 1.f;
		p.autofire_target_select = AF_TARGET_CROSSHAIR;
		p.autofire_multipoint_dynamic = true;
		for (int i = 0; i < AF_MP_COUNT; ++i) {
			p.autofire_hitboxes[i] = false;
			p.autofire_multipoint_scale[i] = 0.55f;
		}
		p.autofire_hitboxes[AF_MP_HEAD] = true;
		p.autofire_hitboxes[AF_MP_CHEST] = true;
		p.autofire_multipoint_scale[AF_MP_HEAD] = 0.55f;
		p.autofire_multipoint_scale[AF_MP_CHEST] = 0.60f;
		p.autofire_multipoint_scale[AF_MP_STOMACH] = 0.55f;
		p.autofire_multipoint_scale[AF_MP_PELVIS] = 0.50f;
		p.trigger_delay_ms = 0.f;
		p.trigger_hitchance = 0.f;
		p.trigger_autowall = false;
		p.trigger_scoped_only = false;
		for (int i = 0; i < HB_COUNT; ++i)
			p.trigger_hitboxes[i] = false;
		p.trigger_hitboxes[HB_HEAD] = true;
		p.trigger_hitboxes[HB_NECK] = true;
		p.trigger_hitboxes[HB_CHEST] = true;
		p.trigger_autostop = false;
		p.trigger_mode = TR_MODE_HITCHANCE;
		return p;
	}

	void InitWeaponProfilesDefaults() {
		const AimWeaponProfile base = MakeDefaultProfile();
		for (int g = 0; g < WG_COUNT; ++g)
			weapon_profiles[g] = base;

		// Snipers — tighter FOV, higher hitchance, autostop on
		weapon_profiles[WG_SNIPER].aimbot_fov = 3.f;
		weapon_profiles[WG_SNIPER].aimbot_smooth = 3.f;
		weapon_profiles[WG_SNIPER].autofire_hitchance = 80.f;
		weapon_profiles[WG_SNIPER].autofire_autostop = true;
		weapon_profiles[WG_SNIPER].autofire_mindamage = 70.f;
		weapon_profiles[WG_SNIPER].autofire_mindamage_aw = 90.f;
		weapon_profiles[WG_SNIPER].trigger_scoped_only = true;
		weapon_profiles[WG_SNIPER].trigger_delay_ms = 0.f;
		weapon_profiles[WG_SNIPER].trigger_hitchance = 0.f;
		for (int i = 0; i < HB_COUNT; ++i)
			weapon_profiles[WG_SNIPER].trigger_hitboxes[i] = false;
		weapon_profiles[WG_SNIPER].trigger_hitboxes[HB_HEAD] = true;

		// Pistols — wider FOV, less RCS
		weapon_profiles[WG_PISTOL].aimbot_fov = 8.f;
		weapon_profiles[WG_PISTOL].autofire_hitchance = 60.f;
		weapon_profiles[WG_PISTOL].rcs = false;

		// Rifles — default RCS-friendly
		weapon_profiles[WG_RIFLE].rcs = true;
		weapon_profiles[WG_RIFLE].rcs_scale_x = 0.55f;
		weapon_profiles[WG_RIFLE].rcs_scale_y = 0.55f;

		// SMG — slightly wider
		weapon_profiles[WG_SMG].aimbot_fov = 7.f;
		weapon_profiles[WG_SMG].autofire_hitchance = 65.f;

		// Shotgun — close FOV, low hitchance
		weapon_profiles[WG_SHOTGUN].aimbot_fov = 10.f;
		weapon_profiles[WG_SHOTGUN].autofire_hitchance = 40.f;
		weapon_profiles[WG_SHOTGUN].autofire_mindamage = 20.f;

		// LMG — wider spray, RCS
		weapon_profiles[WG_LMG].aimbot_fov = 6.f;
		weapon_profiles[WG_LMG].rcs = true;
		weapon_profiles[WG_LMG].autofire_hitchance = 55.f;

		weapon_group_ui = WG_RIFLE;
		weapon_group_active = WG_GENERAL;
		ApplyWeaponGroup(nullptr);
	}

	const char* WeaponGroupName(int group) {
		switch (group) {
		case WG_GENERAL: return "General";
		case WG_PISTOL:  return "Pistols";
		case WG_SMG:     return "SMGs";
		case WG_RIFLE:   return "Rifles";
		case WG_SHOTGUN: return "Shotguns";
		case WG_SNIPER:  return "Snipers";
		case WG_LMG:     return "LMGs";
		default:         return "General";
		}
	}

	int ClassifyWeaponGroup(C_CSWeaponBase* weapon) {
		if (!weapon)
			return WG_GENERAL;
		__try {
			if (weapon->IsNonGunWeapon())
				return WG_GENERAL;
			auto* vdata = weapon->Data();
			if (vdata) {
				const int t = vdata->m_WeaponType();
				// CCSWeaponType: pistol=1 smg=2 rifle=3 shotgun=4 sniper=5 machinegun=6
				if (t >= 1 && t <= 6)
					return t; // maps 1:1 onto WG_PISTOL..WG_LMG
			}
			// Defindex fallback
			const std::uint16_t def = weapon->m_iItemDefinitionIndex();
			switch (def) {
			case 1: case 2: case 3: case 4: case 30: case 32: case 36:
			case 61: case 63: case 64:
				return WG_PISTOL;
			case 17: case 19: case 23: case 24: case 26: case 33: case 34:
				return WG_SMG;
			case 7: case 8: case 10: case 13: case 16: case 39: case 60:
				return WG_RIFLE;
			case 25: case 27: case 29: case 35:
				return WG_SHOTGUN;
			case 9: case 11: case 38: case 40:
				return WG_SNIPER;
			case 14: case 28:
				return WG_LMG;
			default:
				return WG_GENERAL;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return WG_GENERAL;
		}
	}

	AimWeaponProfile& MenuAimProfile() {
		if (weapon_group_ui < 0 || weapon_group_ui >= WG_COUNT)
			weapon_group_ui = WG_GENERAL;
		return weapon_profiles[weapon_group_ui];
	}

	void PullLiveIntoProfile(int group) {
		if (group < 0 || group >= WG_COUNT)
			return;
		AimWeaponProfile& p = weapon_profiles[group];
		p.aimbot_fov = aimbot_fov;
		p.aimbot_smooth = aimbot_smooth;
		p.aim_vis_check = aim_vis_check;
		std::memcpy(p.aim_hitboxes, aim_hitboxes, sizeof(aim_hitboxes));
		p.aim_reaction_delay_ms = aim_reaction_delay_ms;
		p.aim_target_switch_delay_ms = aim_target_switch_delay_ms;
		p.aim_first_shot_delay_ms = aim_first_shot_delay_ms;
		p.rcs = rcs;
		p.rcs_standalone = rcs_standalone;
		p.rcs_scale_x = rcs_scale_x;
		p.rcs_scale_y = rcs_scale_y;
		p.autofire_silent = autofire_silent;
		p.autofire_hitchance = autofire_hitchance;
		p.autofire_autostop = autofire_autostop;
		p.autofire_autowall = autofire_autowall;
		p.autofire_mindamage = autofire_mindamage;
		p.autofire_mindamage_aw = autofire_mindamage_aw;
		p.autofire_target_select = autofire_target_select;
		p.autofire_multipoint_dynamic = autofire_multipoint_dynamic;
		std::memcpy(p.autofire_hitboxes, autofire_hitboxes, sizeof(autofire_hitboxes));
		std::memcpy(p.autofire_multipoint_scale, autofire_multipoint_scale, sizeof(autofire_multipoint_scale));
		p.trigger_delay_ms = trigger_delay_ms;
		p.trigger_hitchance = trigger_hitchance;
		p.trigger_autowall = trigger_autowall;
		p.trigger_scoped_only = trigger_scoped_only;
		std::memcpy(p.trigger_hitboxes, trigger_hitboxes, sizeof(trigger_hitboxes));
		p.trigger_autostop = trigger_autostop;
		p.trigger_mode = trigger_mode;
	}

	void ApplyWeaponGroup(C_CSWeaponBase* weapon) {
		const int g = ClassifyWeaponGroup(weapon);
		weapon_group_active = g;
		const AimWeaponProfile& p = weapon_profiles[g];
		aimbot_fov = p.aimbot_fov;
		aimbot_smooth = p.aimbot_smooth;
		aim_vis_check = p.aim_vis_check;
		std::memcpy(aim_hitboxes, p.aim_hitboxes, sizeof(aim_hitboxes));
		aim_reaction_delay_ms = p.aim_reaction_delay_ms;
		aim_target_switch_delay_ms = p.aim_target_switch_delay_ms;
		aim_first_shot_delay_ms = p.aim_first_shot_delay_ms;
		rcs = p.rcs;
		rcs_standalone = p.rcs_standalone;
		rcs_scale_x = p.rcs_scale_x;
		rcs_scale_y = p.rcs_scale_y;
		autofire_silent = p.autofire_silent;
		autofire_hitchance = p.autofire_hitchance;
		autofire_autostop = p.autofire_autostop;
		autofire_autowall = p.autofire_autowall;
		autofire_mindamage = p.autofire_mindamage;
		autofire_mindamage_aw = p.autofire_mindamage_aw;
		autofire_target_select = p.autofire_target_select;
		if (autofire_target_select < 0 || autofire_target_select >= AF_TARGET_COUNT)
			autofire_target_select = AF_TARGET_CROSSHAIR;
		autofire_multipoint_dynamic = p.autofire_multipoint_dynamic;
		std::memcpy(autofire_hitboxes, p.autofire_hitboxes, sizeof(autofire_hitboxes));
		std::memcpy(autofire_multipoint_scale, p.autofire_multipoint_scale, sizeof(autofire_multipoint_scale));
		trigger_delay_ms = p.trigger_delay_ms;
		trigger_hitchance = p.trigger_hitchance;
		trigger_autowall = p.trigger_autowall;
		trigger_scoped_only = p.trigger_scoped_only;
		std::memcpy(trigger_hitboxes, p.trigger_hitboxes, sizeof(trigger_hitboxes));
		trigger_autostop = p.trigger_autostop;
		trigger_mode = p.trigger_mode;
		if (trigger_mode < 0 || trigger_mode >= TR_MODE_COUNT)
			trigger_mode = TR_MODE_HITCHANCE;
	}

	bool bhop = false;
	bool autostrafe = false;
	int autostrafe_mode = 0; // 0 mouse (legit), 1 vectorial (silent)

	bool widget_keybinds = true;
	bool widget_bomb = true;
	bool widget_spectators = true;
	ImVec2 widget_keybinds_pos = ImVec2(-1.f, -1.f);
	ImVec2 widget_bomb_pos = ImVec2(-1.f, -1.f);
	ImVec2 widget_spectators_pos = ImVec2(-1.f, -1.f);
	bool widget_keybinds_only_when_active = false;
	bool widget_keybinds_show_all = true;
	ImVec4 widget_keybinds_accent = ImVec4(0.51f, 0.43f, 1.00f, 0.90f);
	ImVec4 widget_bomb_accent = ImVec4(1.00f, 0.55f, 0.27f, 0.90f);
	ImVec4 widget_bomb_urgent = ImVec4(1.00f, 0.27f, 0.27f, 0.94f);
	bool widget_bomb_show_damage = true;
	bool widget_bomb_show_defuse = true;
	ImVec4 widget_spectators_accent = ImVec4(0.51f, 0.43f, 1.00f, 0.90f);
	bool widget_spectators_show_avatars = true;
	int widget_spectators_max = 8;

	bool knife_changer = false;
	int  knife_index = 0;
	int  knife_paint_kit = 0;
	int  knife_paint_kit_id = 0;
	float knife_wear = 0.0001f;
	int  knife_seed = 0;
	char knife_custom_name[161] = {};

	bool glove_changer = false;
	int  glove_index = 0;
	int  glove_paint_kit = 0;
	int  glove_paint_kit_id = 0;
	float glove_wear = 0.0001f;
	int  glove_seed = 0;

	bool weapon_skins = false;
	int  weapon_selected = 7; // AK-47
	WeaponSkinSlot weapon_skin[71] = {};

	bool agent_changer = false;
	int  agent_ct_def = 0;
	int  agent_t_def = 0;

	bool nade_pred = false;
	bool nade_pred_local = true;           // cook aim path while pin/M1 held
	bool nade_pred_local_only_pin = true;  // always pin-only (enforced in code too)
	bool nade_pred_projectiles = true;
	bool nade_pred_radius = true;
	float nade_pred_thickness = 1.8f;
	ImVec4 nade_pred_color = ImVec4(0.35f, 0.85f, 1.0f, 1.0f);
	ImVec4 nade_pred_local_color = ImVec4(0.30f, 1.0f, 0.55f, 1.0f);
	ImVec4 nade_pred_land_color = ImVec4(1.0f, 0.85f, 0.20f, 1.0f);

	bool nade_warn = true;
	bool nade_warn_only_near = false;
	float nade_warn_range = 35.f;
	float nade_warn_icon_size = 28.f;
	ImVec4 nade_warn_color = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);

	bool nade_pred_damage = true;
	ImVec4 nade_pred_damage_color = ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
	ImVec4 nade_pred_damage_lethal_color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);

	void ResetToDefaults() {
		esp = false;
		glow = false;
		glow_team = true;
		glow_enemy = true;
		glow_only_visible = false;
		glow_color = ImVec4(0.f, 1.f, 0.f, 1.f);
		glow_color_invis = ImVec4(1.f, 0.3f, 0.3f, 1.f);
		glow_world_weapons = false;
		glow_world_bomb = false;
		glow_world_grenades = false;
		showHealth = false;
		showArmor = false;
		showDistance = false;
		showWeapon = false;
		showWeaponIcon = false;
		teamCheck = false;
		espFill = false;
		showNameTags = false;
		esp_name_avatar = false;
		esp_skeleton = false;
		esp_skeleton_thickness = 1.5f;
		esp_skeleton_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		esp_skeleton_color_invisible = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
		esp_vis_check = true;
		flag_flashed = false;
		flag_bomb = false;
		flag_scoped = false;
		flag_reloading = false;
		flag_defusing = false;
		world_esp_weapons = false;
		world_esp_bomb = true;
		world_esp_smoke = false;
		world_esp_molotov = false;
		world_esp_he = false;
		world_esp_flash = false;
		world_esp_decoy = false;
		world_esp_distance = false;
		world_esp_weapon_color = ImVec4(0.95f, 0.90f, 0.55f, 1.0f);
		world_esp_bomb_color = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
		world_esp_smoke_color = ImVec4(0.75f, 0.80f, 0.90f, 1.0f);
		world_esp_molotov_color = ImVec4(1.0f, 0.50f, 0.15f, 1.0f);
		world_esp_he_color = ImVec4(1.0f, 0.70f, 0.25f, 1.0f);
		world_esp_flash_color = ImVec4(0.95f, 0.95f, 0.55f, 1.0f);
		world_esp_decoy_color = ImVec4(0.65f, 0.85f, 0.55f, 1.0f);
		Night = false;
		night_exposure = 0.45f;
		skybox = false;
		skybox_color = ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
		lighting = false;
		lighting_color = ImVec4(1.0f, 0.92f, 0.75f, 1.0f);
		map_color = false;
		map_color_value = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);
		weather = false;
		weather_mode = 1;
		weather_intensity = 0.55f;
		enemyChamsInvisible = false;
		enemyChams = false;
		teamChams = false;
		teamChamsInvisible = false;
		chamsMaterial = 0;
		chamsMaterialXQZ = 0;
		armChamsMaterial = 0;
		viewmodelChamsMaterial = 0;
		armChams = false;
		viewmodelChams = false;
		colViewmodelChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		colArmChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		colVisualChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		colVisualChamsIgnoreZ = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		teamcolVisualChamsIgnoreZ = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		teamcolVisualChams = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		espThickness = 1.0f;
		espFillOpacity = 0.5f;
		espColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		espColorInvisible = ImVec4(0.45f, 0.55f, 1.0f, 1.0f);
		esp_box_style = ESP_BOX_FULL;
		esp_box_width = 0.42f;
		esp_bar_width = 3.0f;
		esp_health_auto = true;
		esp_health_color = ImVec4(0.20f, 0.90f, 0.25f, 1.0f);
		esp_armor_color = ImVec4(0.27f, 0.63f, 1.0f, 1.0f);
		esp_name_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		esp_weapon_color = ImVec4(1.0f, 0.86f, 0.47f, 1.0f);
		esp_distance_color = ImVec4(0.71f, 0.86f, 1.0f, 1.0f);
		fovEnabled = false;
		fov = 90.0f;
		viewmodel_changer = false;
		viewmodel_fov = 68.f;
		viewmodel_x = 0.f;
		viewmodel_y = 0.f;
		viewmodel_z = 0.f;
		thirdperson = false;
		thirdperson_distance = 150.f;
		thirdperson_key = 0x04;
		thirdperson_key_mode = 2;
		antiflash = false;
		antiflash_amount = 0.f;
		remove_legs = false;
		remove_smoke = false;
		remove_decals = false;
		remove_particles = false;
		scope_no_overlay = false;
		scope_remove_blur = false;
		scope_remove_bars = false;
		scope_remove_texture = false;
		scope_custom_look = false;
		scope_size_scale = 1.f;
		scope_blur_amount = 0.f;
		aimbot = false;
		aimbot_fov = 5.f;
		aimbot_smooth = 5.f;
		autofire = false;
		autofire_silent = false;
		autofire_hitchance = 70.f;
		autofire_autostop = false;
		autofire_autowall = false;
		autofire_mindamage = 1.f;
		autofire_mindamage_aw = 1.f;
		autofire_key = 0x06;
		autofire_key_mode = 1;
		autofire_target_select = AF_TARGET_CROSSHAIR;
		autofire_multipoint_dynamic = true;
		autofire_hitboxes[AF_MP_HEAD] = true;
		autofire_hitboxes[AF_MP_CHEST] = true;
		autofire_hitboxes[AF_MP_STOMACH] = false;
		autofire_hitboxes[AF_MP_PELVIS] = false;
		autofire_multipoint_scale[AF_MP_HEAD] = 0.55f;
		autofire_multipoint_scale[AF_MP_CHEST] = 0.60f;
		autofire_multipoint_scale[AF_MP_STOMACH] = 0.55f;
		autofire_multipoint_scale[AF_MP_PELVIS] = 0.50f;
		triggerbot = false;
		triggerbot_key = 0x12;
		triggerbot_key_mode = 1;
		trigger_delay_ms = 0.f;
		trigger_hitchance = 0.f;
		trigger_autowall = false;
		trigger_scoped_only = false;
		for (int i = 0; i < HB_COUNT; ++i)
			trigger_hitboxes[i] = false;
		trigger_hitboxes[HB_HEAD] = true;
		trigger_hitboxes[HB_NECK] = true;
		trigger_hitboxes[HB_CHEST] = true;
		trigger_autostop = false;
		trigger_mode = TR_MODE_HITCHANCE;
		team_check = true;
		team_check_auto = true;
		aim_vis_check = true;
		aim_reaction_delay_ms = 0.f;
		aim_target_switch_delay_ms = 0.f;
		aim_first_shot_delay_ms = 0.f;
		for (int i = 0; i < HB_COUNT; ++i)
			aim_hitboxes[i] = false;
		aim_hitboxes[HB_HEAD] = true;
		aim_hitboxes[HB_NECK] = true;
		aim_hitboxes[HB_CHEST] = true;
		rcs = false;
		rcs_standalone = false;
		rcs_scale_x = 0.5f;
		rcs_scale_y = 0.5f;
		fov_circle = false;
		fovCircleColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		aimbot_key = 0x05;
		aimbot_key_mode = 1;
		bhop = false;
		autostrafe = false;
		autostrafe_mode = 0;
		widget_keybinds = true;
		widget_bomb = true;
		widget_spectators = true;
		widget_keybinds_pos = ImVec2(-1.f, -1.f);
		widget_bomb_pos = ImVec2(-1.f, -1.f);
		widget_spectators_pos = ImVec2(-1.f, -1.f);
		widget_keybinds_only_when_active = false;
		widget_keybinds_show_all = true;
		widget_keybinds_accent = ImVec4(0.51f, 0.43f, 1.00f, 0.90f);
		widget_bomb_accent = ImVec4(1.00f, 0.55f, 0.27f, 0.90f);
		widget_bomb_urgent = ImVec4(1.00f, 0.27f, 0.27f, 0.94f);
		widget_bomb_show_damage = true;
		widget_bomb_show_defuse = true;
		widget_spectators_accent = ImVec4(0.51f, 0.43f, 1.00f, 0.90f);
		widget_spectators_show_avatars = true;
		widget_spectators_max = 8;
		knife_changer = false;
		knife_index = 0;
		knife_paint_kit = 0;
		knife_paint_kit_id = 0;
		knife_wear = 0.0001f;
		knife_seed = 0;
		knife_custom_name[0] = '\0';
		glove_changer = false;
		glove_index = 0;
		glove_paint_kit = 0;
		glove_paint_kit_id = 0;
		glove_wear = 0.0001f;
		glove_seed = 0;
		weapon_skins = false;
		weapon_selected = 7;
		for (int i = 0; i < 71; ++i)
			weapon_skin[i] = {};
		agent_changer = false;
		agent_ct_def = 0;
		agent_t_def = 0;
		nade_pred = false;
		nade_pred_local = true;
		nade_pred_local_only_pin = true;
		nade_pred_projectiles = true;
		nade_pred_radius = true;
		nade_pred_thickness = 1.8f;
		nade_pred_color = ImVec4(0.35f, 0.85f, 1.0f, 1.0f);
		nade_pred_local_color = ImVec4(0.30f, 1.0f, 0.55f, 1.0f);
		nade_pred_land_color = ImVec4(1.0f, 0.85f, 0.20f, 1.0f);
		nade_warn = true;
		nade_warn_only_near = false;
		nade_warn_range = 35.f;
		nade_warn_icon_size = 28.f;
		nade_warn_color = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
		nade_pred_damage = true;
		nade_pred_damage_color = ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
		nade_pred_damage_lethal_color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
		InitWeaponProfilesDefaults();

		// Keep live keybind objects in sync with Config::* defaults
		keybind.resetToDefaults();
		aimbot_key = keybind.getKey(aimbot);
		aimbot_key_mode = keybind.getMode(aimbot);
		autofire_key = keybind.getKey(autofire);
		autofire_key_mode = keybind.getMode(autofire);
		triggerbot_key = keybind.getKey(triggerbot);
		triggerbot_key_mode = keybind.getMode(triggerbot);
		thirdperson_key = keybind.getKey(thirdperson);
		thirdperson_key_mode = keybind.getMode(thirdperson);
	}
}

namespace {
	struct WeaponProfileBoot {
		WeaponProfileBoot() { Config::InitWeaponProfilesDefaults(); }
	};
	static WeaponProfileBoot g_weaponProfileBoot;
}
