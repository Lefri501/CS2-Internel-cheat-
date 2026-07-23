#include "config.h"
#include "../keybinds/keybinds.h"
#include "../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstring>
#include <cstdio>

// CBA to make proper atm, it's 03:42 right now.
// For now just stores config values don't mind it too much
//
// (FYI THIS IS A HORRID SOLUTION BUT FUNCTIONS) 

namespace Config {
	std::atomic<bool> loading{ false };
	bool esp = false;
	bool glow = false;
	bool glow_team = true;
	bool glow_enemy = true;
	bool glow_only_visible = false;
	// Distinct from chams defaults (red) — glow ESP owns these colours alone
	ImVec4 glow_color = ImVec4(0.25f, 0.85f, 1.f, 1.f);
	ImVec4 glow_color_invis = ImVec4(1.f, 0.35f, 0.85f, 1.f);
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
	bool flag_money = false;
	bool flag_kit = false;
	bool flag_helmet = false;
	bool flag_nades = false;
	bool esp_rank = false;
	bool esp_3d_box = false;
	bool esp_oof = false;
	float esp_oof_radius = 280.f;
	float esp_oof_size = 14.f;
	ImVec4 esp_oof_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
	ImVec4 esp_3d_box_color = ImVec4(1.f, 0.45f, 0.2f, 0.95f);
	ImVec4 esp_rank_color = ImVec4(0.85f, 0.9f, 1.f, 1.f);

	bool float_damage = false;
	float float_damage_duration = 1.1f;
	float float_damage_speed = 55.f;
	ImVec4 float_damage_color = ImVec4(1.f, 1.f, 1.f, 1.f);
	ImVec4 float_damage_head_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
	ImVec4 float_damage_kill_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);

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


	bool custom_fog = false;
	ImVec4 custom_fog_color = ImVec4(0.58f, 0.62f, 0.85f, 1.0f);
	float custom_fog_start = 100.f;
	float custom_fog_end = 3000.f;
	float custom_fog_falloff = 1.f;

	bool weather = false;
	int weather_mode = 1; // Snow default when enabled (1..4)
	float weather_intensity = 0.55f;

	bool tracers = false;
	int tracers_style = 0; // Beam
	float tracers_duration = 2.0f;
	float tracers_thickness = 2.2f;
	ImVec4 tracers_color = ImVec4(0.35f, 0.85f, 1.f, 0.95f);

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

	bool localChams = false;
	int localChamsMaterial = 0;
	ImVec4 colLocalChams = ImVec4(0.2f, 0.85f, 1.0f, 0.85f);
	bool ragdollChams = false;
	int ragdollChamsMaterial = 0;
	ImVec4 colRagdollChams = ImVec4(0.55f, 0.55f, 0.6f, 0.75f);

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
	bool aspect_ratio_enabled = false;
	float aspect_ratio = 1.777778f; // 16:9
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
	bool smoke_color = false;
	ImVec4 smoke_color_value = ImVec4(0.55f, 0.70f, 0.95f, 1.f);
	bool fire_color = false;
	ImVec4 fire_color_value = ImVec4(1.f, 0.35f, 0.05f, 1.f);
	bool explosion_color = false;
	ImVec4 explosion_color_value = ImVec4(0.f, 1.f, 1.f, 1.f);
	bool remove_crosshair = false;
	bool force_crosshair = false;
	bool remove_hud = false;
	bool remove_postprocess = false;
	bool remove_recoil = false;

	bool knifebot = false;
	bool knifebot_prefer_stab = true;
	int knifebot_key = 0;
	int knifebot_key_mode = 0; // Always when enabled
	bool auto_pistol = false;
	float auto_pistol_delay_ms = 0.f;
	bool enemy_spectate = false;
	bool enemy_spectate_thirdperson = false;
	bool auto_defuse = false;

	bool scope_custom_lines = false;
	float scope_line_size = 1.f;
	float scope_line_gap = 4.f;
	float scope_line_thickness = 0.5f;
	ImVec4 scope_line_color = ImVec4(0.f, 0.f, 0.f, 1.f);
	bool scope_zoom_fov = false;
	float scope_fov_1 = 40.f;
	float scope_fov_2 = 15.f;
	bool scope_hide_viewmodel = false;

	bool aimbot = false;
	float aimbot_fov = 5.f;
	float aimbot_smooth = 5.f; // 0 = instant, higher = smoother
	float aimbot_humanize = 0.f; // 0 = off (recommended)
	int aimbot_smooth_mode = SMOOTH_LINEAR;

	bool autofire = false;
	bool autofire_silent = false;
	float autofire_fov = 5.f;
	float autofire_hitchance = 70.f; // default on — 0 = off
	int autofire_mode = AF_MODE_HITCHANCE;
	bool autofire_autostop = false;
	bool autofire_autoscope = false;
	bool autofire_scoped_only = false;
	bool autofire_autowall = false;
	bool autowall = true; // keybind host — Always = on whenever AF/TR AW checkbox on
	int autowall_key = 0;
	int autowall_key_mode = 0; // Always
	float autofire_mindamage = 1.f;    // visible
	float autofire_mindamage_aw = 1.f; // through wall
	int autofire_key = 0x06; // VK_XBUTTON2
	int autofire_key_mode = 1; // Hold
	int autofire_target_select = AF_TARGET_CROSSHAIR;
	bool autofire_vis_check = true;
	bool autofire_flash_check = true;
	bool autofire_smoke_check = false;
	bool autofire_focus_target = true;
	bool autofire_multipoint_dynamic = true;
	bool autofire_body_if_lethal = false;
	bool autofire_prefer_body = false;
	bool autofire_hitboxes[HB_COUNT] = {
		true,  // head
		true,  // neck
		true,  // chest
		false, // stomach
		false, // pelvis
		false, // arms
		false, // legs
		false  // feet
	};
	// Multipoint enable — Head/Chest/Stomach/Pelvis only (neck/limbs unused)
	bool autofire_multipoint[HB_COUNT] = {
		true,  // head
		false, // neck (not in MP list)
		true,  // chest
		false, // stomach
		false, // pelvis
		false, // arms
		false, // legs
		false  // feet
	};
	float autofire_multipoint_scale[HB_COUNT] = {
		0.55f, // head
		0.50f, // neck
		0.60f, // chest
		0.55f, // stomach
		0.50f, // pelvis
		0.45f, // arms
		0.45f, // legs
		0.40f  // feet
	};

	bool team_check = true;
	bool team_check_auto = true;
	bool aim_vis_check = true;
	bool aim_smoke_check = false;
	bool aim_flash_check = false;
	bool aim_scoped_only = false;

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
	bool fov_circle_autofire = false;
	bool fov_circle_magnet = false;
	ImVec4 fovCircleColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 fovCircleColorAf = ImVec4(1.0f, 0.45f, 0.15f, 0.90f);
	ImVec4 fovCircleColorMagnet = ImVec4(0.35f, 0.85f, 1.0f, 0.90f);

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
	float trigger_mindamage = 1.f;
	float trigger_mindamage_aw = 1.f;
	bool trigger_scoped_only = false;
	bool trigger_flash_check = true;
	bool trigger_smoke_check = false;
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
	bool trigger_magnet = false;
	float trigger_magnet_smooth = 12.f;
	float trigger_magnet_fov = 4.f;
	bool trigger_magnet_silent = false;
	bool trigger_magnet_lead = false;
	bool trigger_magnet_head_prio = true;
	bool trigger_magnet_only_ready = false;
	float trigger_magnet_deadzone = 0.12f;
	bool trigger_magnet_hitboxes[HB_COUNT] = {};

	static AimWeaponProfile MakeDefaultProfile() {
		AimWeaponProfile p{};
		p.aimbot_fov = 5.f;
		p.aimbot_smooth = 5.f;
		p.aimbot_humanize = 0.f;
		p.aimbot_smooth_mode = SMOOTH_LINEAR;
		p.aim_vis_check = true;
		p.aim_smoke_check = false;
		p.aim_flash_check = false;
		p.aim_scoped_only = false;
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
		p.autofire_fov = 5.f;
		p.autofire_hitchance = 70.f;
		p.autofire_mode = AF_MODE_HITCHANCE;
		p.autofire_autostop = false;
		p.autofire_autoscope = false;
		p.autofire_scoped_only = false;
		p.autofire_autowall = false;
		p.autofire_mindamage = 1.f;
		p.autofire_mindamage_aw = 1.f;
		p.autofire_target_select = AF_TARGET_CROSSHAIR;
		p.autofire_vis_check = true;
		p.autofire_flash_check = true;
		p.autofire_smoke_check = false;
		p.autofire_focus_target = true;
		p.autofire_multipoint_dynamic = true;
		p.autofire_body_if_lethal = false;
		p.autofire_prefer_body = false;
		for (int i = 0; i < HB_COUNT; ++i) {
			p.autofire_hitboxes[i] = false;
			p.autofire_multipoint[i] = false;
			p.autofire_multipoint_scale[i] = 0.50f;
		}
		p.autofire_hitboxes[HB_HEAD] = true;
		p.autofire_hitboxes[HB_NECK] = true;
		p.autofire_hitboxes[HB_CHEST] = true;
		p.autofire_multipoint[HB_HEAD] = true;
		p.autofire_multipoint[HB_CHEST] = true;
		p.autofire_multipoint_scale[HB_HEAD] = 0.55f;
		p.autofire_multipoint_scale[HB_NECK] = 0.50f;
		p.autofire_multipoint_scale[HB_CHEST] = 0.60f;
		p.autofire_multipoint_scale[HB_STOMACH] = 0.55f;
		p.autofire_multipoint_scale[HB_PELVIS] = 0.50f;
		p.autofire_multipoint_scale[HB_ARMS] = 0.45f;
		p.autofire_multipoint_scale[HB_LEGS] = 0.45f;
		p.autofire_multipoint_scale[HB_FEET] = 0.40f;
		p.trigger_delay_ms = 0.f;
		p.trigger_hitchance = 0.f;
		p.trigger_autowall = false;
		p.trigger_mindamage = 1.f;
		p.trigger_mindamage_aw = 1.f;
		p.trigger_scoped_only = false;
		p.trigger_flash_check = true;
		p.trigger_smoke_check = false;
		for (int i = 0; i < HB_COUNT; ++i)
			p.trigger_hitboxes[i] = false;
		p.trigger_hitboxes[HB_HEAD] = true;
		p.trigger_hitboxes[HB_NECK] = true;
		p.trigger_hitboxes[HB_CHEST] = true;
		p.trigger_autostop = false;
		p.trigger_mode = TR_MODE_HITCHANCE;
		p.trigger_magnet = false;
		p.trigger_magnet_smooth = 12.f;
		p.trigger_magnet_fov = 4.f;
		p.trigger_magnet_silent = false;
		p.trigger_magnet_lead = false;
		p.trigger_magnet_head_prio = true;
		p.trigger_magnet_only_ready = false;
		p.trigger_magnet_deadzone = 0.12f;
		for (int i = 0; i < HB_COUNT; ++i)
			p.trigger_magnet_hitboxes[i] = false;
		return p;
	}

	void InitWeaponProfilesDefaults() {
		const AimWeaponProfile base = MakeDefaultProfile();
		for (int g = 0; g < WG_COUNT; ++g)
			weapon_profiles[g] = base;

		// Snipers — tighter FOV, higher hitchance (autostop off unless user enables)
		weapon_profiles[WG_SNIPER].aimbot_fov = 3.f;
		weapon_profiles[WG_SNIPER].autofire_fov = 3.f;
		weapon_profiles[WG_SNIPER].aimbot_smooth = 3.f;
		weapon_profiles[WG_SNIPER].autofire_hitchance = 80.f;
		weapon_profiles[WG_SNIPER].autofire_autostop = false;
		weapon_profiles[WG_SNIPER].autofire_mindamage = 70.f;
		weapon_profiles[WG_SNIPER].autofire_mindamage_aw = 90.f;
		weapon_profiles[WG_SNIPER].trigger_mindamage = 70.f;
		weapon_profiles[WG_SNIPER].trigger_mindamage_aw = 90.f;
		weapon_profiles[WG_SNIPER].trigger_scoped_only = false;
		weapon_profiles[WG_SNIPER].trigger_delay_ms = 0.f;
		weapon_profiles[WG_SNIPER].trigger_hitchance = 0.f;
		for (int i = 0; i < HB_COUNT; ++i)
			weapon_profiles[WG_SNIPER].trigger_hitboxes[i] = false;
		weapon_profiles[WG_SNIPER].trigger_hitboxes[HB_HEAD] = true;

		// Pistols — wider FOV, less RCS
		weapon_profiles[WG_PISTOL].aimbot_fov = 8.f;
		weapon_profiles[WG_PISTOL].autofire_fov = 8.f;
		weapon_profiles[WG_PISTOL].autofire_hitchance = 60.f;
		weapon_profiles[WG_PISTOL].rcs = false;

		// Rifles — default RCS-friendly
		weapon_profiles[WG_RIFLE].rcs = true;
		weapon_profiles[WG_RIFLE].rcs_scale_x = 0.55f;
		weapon_profiles[WG_RIFLE].rcs_scale_y = 0.55f;

		// SMG — slightly wider
		weapon_profiles[WG_SMG].aimbot_fov = 7.f;
		weapon_profiles[WG_SMG].autofire_fov = 7.f;
		weapon_profiles[WG_SMG].autofire_hitchance = 65.f;

		// Shotgun — close FOV, low hitchance
		weapon_profiles[WG_SHOTGUN].aimbot_fov = 10.f;
		weapon_profiles[WG_SHOTGUN].autofire_fov = 10.f;
		weapon_profiles[WG_SHOTGUN].autofire_hitchance = 40.f;
		weapon_profiles[WG_SHOTGUN].autofire_mindamage = 20.f;

		// LMG — wider spray, RCS
		weapon_profiles[WG_LMG].aimbot_fov = 6.f;
		weapon_profiles[WG_LMG].autofire_fov = 6.f;
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
		p.aimbot_humanize = aimbot_humanize;
		p.aimbot_smooth_mode = aimbot_smooth_mode;
		p.aim_vis_check = aim_vis_check;
		p.aim_smoke_check = aim_smoke_check;
		p.aim_flash_check = aim_flash_check;
		p.aim_scoped_only = aim_scoped_only;
		p.autofire_scoped_only = aim_scoped_only;
		p.trigger_scoped_only = aim_scoped_only;
		std::memcpy(p.aim_hitboxes, aim_hitboxes, sizeof(aim_hitboxes));
		p.aim_reaction_delay_ms = aim_reaction_delay_ms;
		p.aim_target_switch_delay_ms = aim_target_switch_delay_ms;
		p.aim_first_shot_delay_ms = aim_first_shot_delay_ms;
		p.rcs = rcs;
		p.rcs_standalone = rcs_standalone;
		p.rcs_scale_x = rcs_scale_x;
		p.rcs_scale_y = rcs_scale_y;
		p.autofire_fov = autofire_fov;
		p.autofire_hitchance = autofire_hitchance;
		p.autofire_mode = autofire_mode;
		p.autofire_autostop = autofire_autostop;
		p.autofire_autoscope = autofire_autoscope;
		p.autofire_autowall = autofire_autowall;
		p.autofire_mindamage = autofire_mindamage;
		p.autofire_mindamage_aw = autofire_mindamage_aw;
		p.autofire_target_select = autofire_target_select;
		p.autofire_vis_check = autofire_vis_check;
		p.autofire_flash_check = autofire_flash_check;
		p.autofire_smoke_check = autofire_smoke_check;
		p.autofire_focus_target = autofire_focus_target;
		p.autofire_multipoint_dynamic = autofire_multipoint_dynamic;
		p.autofire_body_if_lethal = autofire_body_if_lethal;
		p.autofire_prefer_body = autofire_prefer_body;
		std::memcpy(p.autofire_hitboxes, autofire_hitboxes, sizeof(autofire_hitboxes));
		std::memcpy(p.autofire_multipoint, autofire_multipoint, sizeof(autofire_multipoint));
		std::memcpy(p.autofire_multipoint_scale, autofire_multipoint_scale, sizeof(autofire_multipoint_scale));
		p.trigger_delay_ms = trigger_delay_ms;
		p.trigger_hitchance = trigger_hitchance;
		p.trigger_autowall = trigger_autowall;
		p.trigger_mindamage = trigger_mindamage;
		p.trigger_mindamage_aw = trigger_mindamage_aw;
		// scope: single source aim_scoped_only (live already unified)
		p.aim_scoped_only = aim_scoped_only;
		p.autofire_scoped_only = aim_scoped_only;
		p.trigger_scoped_only = aim_scoped_only;
		p.trigger_flash_check = trigger_flash_check;
		p.trigger_smoke_check = trigger_smoke_check;
		std::memcpy(p.trigger_hitboxes, trigger_hitboxes, sizeof(trigger_hitboxes));
		p.trigger_autostop = trigger_autostop;
		p.trigger_mode = trigger_mode;
		p.trigger_magnet = trigger_magnet;
		p.trigger_magnet_smooth = trigger_magnet_smooth;
		p.trigger_magnet_fov = trigger_magnet_fov;
		p.trigger_magnet_silent = trigger_magnet_silent;
		p.trigger_magnet_lead = trigger_magnet_lead;
		p.trigger_magnet_head_prio = trigger_magnet_head_prio;
		p.trigger_magnet_only_ready = trigger_magnet_only_ready;
		p.trigger_magnet_deadzone = trigger_magnet_deadzone;
		std::memcpy(p.trigger_magnet_hitboxes, trigger_magnet_hitboxes, sizeof(trigger_magnet_hitboxes));
	}

	void ApplyProfileToLive(int group) {
		if (group < 0 || group >= WG_COUNT)
			group = WG_GENERAL;
		const AimWeaponProfile& p = weapon_profiles[group];
		aimbot_fov = p.aimbot_fov;
		aimbot_smooth = p.aimbot_smooth;
		aimbot_humanize = p.aimbot_humanize;
		aimbot_smooth_mode = p.aimbot_smooth_mode;
		if (aimbot_smooth_mode < 0 || aimbot_smooth_mode >= SMOOTH_MODE_COUNT)
			aimbot_smooth_mode = SMOOTH_LINEAR;
		aim_vis_check = p.aim_vis_check;
		aim_smoke_check = p.aim_smoke_check;
		aim_flash_check = p.aim_flash_check;
		// One Scope Check drives aim + AF + trigger
		aim_scoped_only = p.aim_scoped_only;
		autofire_scoped_only = p.aim_scoped_only;
		trigger_scoped_only = p.aim_scoped_only;
		std::memcpy(aim_hitboxes, p.aim_hitboxes, sizeof(aim_hitboxes));
		aim_reaction_delay_ms = p.aim_reaction_delay_ms;
		aim_target_switch_delay_ms = p.aim_target_switch_delay_ms;
		aim_first_shot_delay_ms = p.aim_first_shot_delay_ms;
		rcs = p.rcs;
		rcs_standalone = p.rcs_standalone;
		rcs_scale_x = p.rcs_scale_x;
		rcs_scale_y = p.rcs_scale_y;
		// autofire_silent is global — never overwrite from weapon profile
		autofire_fov = p.autofire_fov;
		autofire_hitchance = p.autofire_hitchance;
		autofire_mode = p.autofire_mode;
		if (autofire_mode < 0 || autofire_mode >= AF_MODE_COUNT)
			autofire_mode = AF_MODE_HITCHANCE;
		autofire_autostop = p.autofire_autostop;
		autofire_autoscope = p.autofire_autoscope;
		autofire_autowall = p.autofire_autowall;
		autofire_mindamage = p.autofire_mindamage;
		autofire_mindamage_aw = p.autofire_mindamage_aw;
		autofire_target_select = p.autofire_target_select;
		if (autofire_target_select < 0 || autofire_target_select >= AF_TARGET_COUNT)
			autofire_target_select = AF_TARGET_CROSSHAIR;
		autofire_vis_check = p.autofire_vis_check;
		autofire_flash_check = p.autofire_flash_check;
		autofire_smoke_check = p.autofire_smoke_check;
		autofire_focus_target = p.autofire_focus_target;
		autofire_multipoint_dynamic = p.autofire_multipoint_dynamic;
		autofire_body_if_lethal = p.autofire_body_if_lethal;
		autofire_prefer_body = p.autofire_prefer_body;
		std::memcpy(autofire_hitboxes, p.autofire_hitboxes, sizeof(autofire_hitboxes));
		std::memcpy(autofire_multipoint, p.autofire_multipoint, sizeof(autofire_multipoint));
		std::memcpy(autofire_multipoint_scale, p.autofire_multipoint_scale, sizeof(autofire_multipoint_scale));
		trigger_delay_ms = p.trigger_delay_ms;
		trigger_hitchance = p.trigger_hitchance;
		trigger_autowall = p.trigger_autowall;
		trigger_mindamage = p.trigger_mindamage;
		trigger_mindamage_aw = p.trigger_mindamage_aw;
		trigger_flash_check = p.trigger_flash_check;
		trigger_smoke_check = p.trigger_smoke_check;
		std::memcpy(trigger_hitboxes, p.trigger_hitboxes, sizeof(trigger_hitboxes));
		trigger_autostop = p.trigger_autostop;
		trigger_mode = p.trigger_mode;
		if (trigger_mode < 0 || trigger_mode >= TR_MODE_COUNT)
			trigger_mode = TR_MODE_HITCHANCE;
		trigger_magnet = p.trigger_magnet;
		trigger_magnet_smooth = p.trigger_magnet_smooth;
		trigger_magnet_fov = p.trigger_magnet_fov;
		if (trigger_magnet_fov < 0.5f) trigger_magnet_fov = 0.5f;
		if (trigger_magnet_fov > 30.f) trigger_magnet_fov = 30.f;
		trigger_magnet_silent = p.trigger_magnet_silent;
		trigger_magnet_lead = p.trigger_magnet_lead;
		trigger_magnet_head_prio = p.trigger_magnet_head_prio;
		trigger_magnet_only_ready = p.trigger_magnet_only_ready;
		trigger_magnet_deadzone = p.trigger_magnet_deadzone;
		if (trigger_magnet_deadzone < 0.f) trigger_magnet_deadzone = 0.f;
		if (trigger_magnet_deadzone > 1.f) trigger_magnet_deadzone = 1.f;
		std::memcpy(trigger_magnet_hitboxes, p.trigger_magnet_hitboxes, sizeof(trigger_magnet_hitboxes));
	}

	void ApplyWeaponGroup(C_CSWeaponBase* weapon) {
		const int g = ClassifyWeaponGroup(weapon);
		weapon_group_active = g;
		ApplyProfileToLive(g);
	}

	bool bhop = false;
	bool autostrafe = false;
	int autostrafe_mode = 0; // 0 mouse (legit), 1 vectorial (silent)
	bool jumpbug = false;
	int jumpbug_key = 0;
	int jumpbug_key_mode = 0; // Always (checkbox alone)
	bool edgejump = false;
	int edgejump_key = 0;
	int edgejump_key_mode = 0;
	bool edgebug = false;
	int edgebug_key = 0;
	int edgebug_key_mode = 0;
	bool backtrack = false;
	float backtrack_ms = 200.f;
	bool backtrack_skeleton = false;
	ImVec4 backtrack_color = ImVec4(0.45f, 0.85f, 1.f, 0.75f);
	// Legacy aliases — kept in sync with master in menu / load / reset
	bool autofire_backtrack = false;
	float autofire_backtrack_ms = 200.f;
	bool trigger_backtrack = false;
	float trigger_backtrack_ms = 200.f;
	bool backtrack_aim = false;
	bool hitlog = false;
	bool hitlog_console = false;
	float hitlog_duration = 4.f;
	ImVec2 hitlog_pos = ImVec2(-1.f, -1.f);
	float hitlog_width = 268.f;
	int hitlog_max_rows = 8;
	bool hitlog_show_hp = true;
	bool hitlog_show_stats = true;
	ImVec4 hitlog_color = ImVec4(0.92f, 0.92f, 0.95f, 0.95f);
	ImVec4 hitlog_head_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);
	ImVec4 hitlog_kill_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
	bool auto_accept = false;

	bool sound_esp = false;
	float sound_esp_duration = 1.4f;
	float sound_esp_ring_size = 1.f;
	ImVec4 sound_esp_color = ImVec4(0.35f, 0.85f, 1.f, 0.95f);

	bool vote_reveal = false;
	bool vote_auto = false;
	int vote_auto_choice = 0; // Yes
	float vote_auto_delay_ms = 250.f;

	bool hitmarker = true;
	bool hitmarker_screen = true;
	bool hitmarker_world = true;
	bool hitmarker_show_damage = false;
	float hitmarker_size = 14.f;
	float hitmarker_gap = 4.f;
	float hitmarker_thickness = 2.2f;
	float hitmarker_world_size = 11.f;
	float hitmarker_duration = 1.f;
	ImVec4 hitmarker_color = ImVec4(1.f, 1.f, 1.f, 0.95f);
	ImVec4 hitmarker_head_color = ImVec4(1.f, 0.85f, 0.15f, 1.f);
	ImVec4 hitmarker_kill_color = ImVec4(1.f, 0.22f, 0.22f, 1.f);

	bool hitsound = true;
	char hitsound_file[160] = "";
	char hitsound_head[160] = "";
	char hitsound_kill[160] = "";

	bool watermark = true;

	bool hud_keybind_strip = true;

	bool widget_keybinds = true;
	bool widget_bomb = true;
	bool widget_spectators = true;
	ImVec2 widget_keybinds_pos = ImVec2(-1.f, -1.f);
	ImVec2 widget_bomb_pos = ImVec2(-1.f, -1.f);
	ImVec2 widget_spectators_pos = ImVec2(-1.f, -1.f);
	bool widget_keybinds_only_when_active = false;
	bool widget_keybinds_show_all = true;
	// Dense matte shell
	// Steel defaults: neutral shell + blue accent (matches ApplyPreset 1)
	ImVec4 menu_accent = ImVec4(0.42f, 0.68f, 0.92f, 1.00f);
	ImVec4 menu_bg = ImVec4(0.068f, 0.070f, 0.078f, 0.98f);
	ImVec4 menu_child_bg = ImVec4(0.102f, 0.105f, 0.118f, 1.00f);
	ImVec4 menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
	ImVec4 menu_border = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
	ImVec4 menu_text = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
	ImVec4 menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
	float menu_rounding = 4.0f;
	float menu_opacity = 0.98f;
	bool menu_compact = true;
	int menu_dpi_scale = 100; // 100% default

	ImVec4 widget_keybinds_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
	ImVec4 widget_bomb_accent = ImVec4(0.78f, 0.58f, 0.42f, 0.95f);
	ImVec4 widget_bomb_urgent = ImVec4(0.82f, 0.38f, 0.38f, 0.95f);
	bool widget_bomb_show_damage = true;
	bool widget_bomb_show_defuse = true;
	ImVec4 widget_spectators_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
	bool widget_spectators_show_avatars = true;
	int widget_spectators_max = 8;
	bool widget_radar = false;
	ImVec2 widget_radar_pos = ImVec2(-1.f, -1.f);
	float widget_radar_size = 160.f;
	ImVec4 widget_radar_accent = ImVec4(0.52f, 0.72f, 0.80f, 0.95f);
	int widget_radar_shape = 0; // 0 circle, 1 square

	bool custom_paint_color = false;
	// Neutral white — seed from paint kit on enable (CustomPaint::SeedFromPaintKit)
	ImVec4 custom_color0 = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 custom_color1 = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 custom_color2 = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 custom_color3 = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

	bool knife_changer = false;
	int  knife_index = 0;
	int  knife_paint_kit = 0;
	int  knife_paint_kit_id = 0;
	float knife_wear = 0.0001f;
	int  knife_seed = 0;
	char knife_custom_name[161] = {};
	bool knife_custom_color = false;
	float knife_colors[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
	bool knife_colors_active = false;
	bool knife_colors_edited = false;

	bool glove_changer = false;
	int  glove_index = 0;
	int  glove_paint_kit = 0;
	int  glove_paint_kit_id = 0;
	float glove_wear = 0.0001f;
	int  glove_seed = 0;
	bool glove_custom_color = false;
	float glove_colors[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
	bool glove_colors_active = false;
	bool glove_colors_edited = false;

	bool weapon_skins = false;
	int  weapon_selected = 7; // AK-47
	WeaponSkinSlot weapon_skin[71] = {};

	bool agent_changer = false;
	int  agent_ct_def = 0;
	int  agent_t_def = 0;
	bool custom_model = false;
	char custom_model_path[260] = {};
	bool custom_knife = false;
	char custom_knife_path[260] = {};

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

	bool nade_lineup = false;
	bool nade_lineup_only_held = false; // show all kinds nearby; enable to filter by held nade
	float nade_lineup_stand_dist = 450.f;
	float nade_lineup_aim_dist = 20.f;
	float nade_lineup_select_dist = 550.f;
	ImVec4 nade_lineup_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	ImVec4 nade_lineup_aim_color = ImVec4(0.15f, 1.0f, 0.35f, 1.0f);
	bool nade_lineup_capture = true; // keybind host (always on; press captures)
	int nade_lineup_capture_key = VK_F6;
	int nade_lineup_capture_key_mode = 1; // Hold
	int nade_lineup_capture_throw = 0;
	int nade_lineup_capture_kind = 0;
	char nade_lineup_capture_name[64] = "Lineup";

	void ResetToDefaults() {
		esp = false;
		glow = false;
		glow_team = true;
		glow_enemy = true;
		glow_only_visible = false;
		glow_color = ImVec4(0.25f, 0.85f, 1.f, 1.f);
		glow_color_invis = ImVec4(1.f, 0.35f, 0.85f, 1.f);
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
		flag_money = false;
		flag_kit = false;
		flag_helmet = false;
		flag_nades = false;
		esp_rank = false;
		esp_3d_box = false;
		esp_oof = false;
		esp_oof_radius = 280.f;
		esp_oof_size = 14.f;
		esp_oof_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
		esp_3d_box_color = ImVec4(1.f, 0.45f, 0.2f, 0.95f);
		esp_rank_color = ImVec4(0.85f, 0.9f, 1.f, 1.f);
		float_damage = false;
		float_damage_duration = 1.1f;
		float_damage_speed = 55.f;
		float_damage_color = ImVec4(1.f, 1.f, 1.f, 1.f);
		float_damage_head_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
		float_damage_kill_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);
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
		custom_fog = false;
		custom_fog_color = ImVec4(0.58f, 0.62f, 0.85f, 1.0f);
		custom_fog_start = 100.f;
		custom_fog_end = 3000.f;
		custom_fog_falloff = 1.f;
		map_color_value = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);
		weather = false;
		weather_mode = 1;
		weather_intensity = 0.55f;
		tracers = false;
		tracers_style = 0;
		tracers_duration = 2.0f;
		tracers_thickness = 2.2f;
		tracers_color = ImVec4(0.35f, 0.85f, 1.f, 0.95f);
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
		localChams = false;
		localChamsMaterial = 0;
		colLocalChams = ImVec4(0.2f, 0.85f, 1.0f, 0.85f);
		ragdollChams = false;
		ragdollChamsMaterial = 0;
		colRagdollChams = ImVec4(0.55f, 0.55f, 0.6f, 0.75f);
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
		aspect_ratio_enabled = false;
		aspect_ratio = 1.777778f;
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
		smoke_color = false;
		smoke_color_value = ImVec4(0.55f, 0.70f, 0.95f, 1.f);
		fire_color = false;
		fire_color_value = ImVec4(1.f, 0.35f, 0.05f, 1.f);
		explosion_color = false;
		explosion_color_value = ImVec4(0.f, 1.f, 1.f, 1.f);
		remove_crosshair = false;
		force_crosshair = false;
		remove_hud = false;
		remove_postprocess = false;
		remove_recoil = false;
		knifebot = false;
		knifebot_prefer_stab = true;
		knifebot_key = 0;
		knifebot_key_mode = 0;
		auto_pistol = false;
		auto_pistol_delay_ms = 0.f;
		enemy_spectate = false;
		enemy_spectate_thirdperson = false;
		auto_defuse = false;
		scope_custom_lines = false;
		scope_line_size = 1.f;
		scope_line_gap = 4.f;
		scope_line_thickness = 0.5f;
		scope_line_color = ImVec4(0.f, 0.f, 0.f, 1.f);
		scope_zoom_fov = false;
		scope_fov_1 = 40.f;
		scope_fov_2 = 15.f;
		scope_hide_viewmodel = false;
		aimbot = false;
		aimbot_fov = 5.f;
		aimbot_smooth = 5.f;
		aimbot_humanize = 0.f;
		aimbot_smooth_mode = SMOOTH_LINEAR;
		autofire = false;
		autofire_silent = false;
		autofire_fov = 5.f;
		autofire_hitchance = 70.f;
		autofire_mode = AF_MODE_HITCHANCE;
		autofire_autostop = false;
		autofire_autoscope = false;
		autofire_scoped_only = false;
		autofire_autowall = false;
		autowall = true;
		autowall_key = 0;
		autowall_key_mode = 0;
		autofire_mindamage = 1.f;
		autofire_mindamage_aw = 1.f;
		autofire_key = 0x06;
		autofire_key_mode = 1;
		autofire_target_select = AF_TARGET_CROSSHAIR;
		autofire_vis_check = true;
		autofire_flash_check = true;
		autofire_smoke_check = false;
		autofire_focus_target = true;
		autofire_multipoint_dynamic = true;
		autofire_body_if_lethal = false;
		autofire_prefer_body = false;
		for (int i = 0; i < HB_COUNT; ++i) {
			autofire_hitboxes[i] = false;
			autofire_multipoint[i] = false;
			autofire_multipoint_scale[i] = 0.50f;
		}
		autofire_hitboxes[HB_HEAD] = true;
		autofire_hitboxes[HB_NECK] = true;
		autofire_hitboxes[HB_CHEST] = true;
		autofire_multipoint[HB_HEAD] = true;
		autofire_multipoint[HB_CHEST] = true;
		autofire_multipoint_scale[HB_HEAD] = 0.55f;
		autofire_multipoint_scale[HB_NECK] = 0.50f;
		autofire_multipoint_scale[HB_CHEST] = 0.60f;
		autofire_multipoint_scale[HB_STOMACH] = 0.55f;
		autofire_multipoint_scale[HB_PELVIS] = 0.50f;
		autofire_multipoint_scale[HB_ARMS] = 0.45f;
		autofire_multipoint_scale[HB_LEGS] = 0.45f;
		autofire_multipoint_scale[HB_FEET] = 0.40f;
		triggerbot = false;
		triggerbot_key = 0x12;
		triggerbot_key_mode = 1;
		trigger_delay_ms = 0.f;
		trigger_hitchance = 0.f;
		trigger_autowall = false;
		trigger_mindamage = 1.f;
		trigger_mindamage_aw = 1.f;
		trigger_scoped_only = false;
		trigger_flash_check = true;
		trigger_smoke_check = false;
		for (int i = 0; i < HB_COUNT; ++i)
			trigger_hitboxes[i] = false;
		trigger_hitboxes[HB_HEAD] = true;
		trigger_hitboxes[HB_NECK] = true;
		trigger_hitboxes[HB_CHEST] = true;
		trigger_autostop = false;
		trigger_mode = TR_MODE_HITCHANCE;
		trigger_magnet = false;
		trigger_magnet_smooth = 12.f;
		trigger_magnet_fov = 4.f;
		trigger_magnet_silent = false;
		trigger_magnet_lead = false;
		trigger_magnet_head_prio = true;
		trigger_magnet_only_ready = false;
		trigger_magnet_deadzone = 0.12f;
		for (int i = 0; i < HB_COUNT; ++i)
			trigger_magnet_hitboxes[i] = false;
		team_check = true;
		team_check_auto = true;
		aim_vis_check = true;
		aim_smoke_check = false;
		aim_flash_check = false;
		aim_scoped_only = false;
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
		fov_circle_autofire = false;
		fov_circle_magnet = false;
		fovCircleColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		fovCircleColorAf = ImVec4(1.0f, 0.45f, 0.15f, 0.90f);
		fovCircleColorMagnet = ImVec4(0.35f, 0.85f, 1.0f, 0.90f);
		weapon_group_active = WG_GENERAL;
		aimbot_key = 0x05;
		aimbot_key_mode = 1;
		bhop = false;
		autostrafe = false;
		autostrafe_mode = 0;
		jumpbug = false;
		jumpbug_key = 0;
		jumpbug_key_mode = 0;
		edgejump = false;
		edgejump_key = 0;
		edgejump_key_mode = 0;
		edgebug = false;
		edgebug_key = 0;
		edgebug_key_mode = 0;
		backtrack = false;
		backtrack_ms = 200.f;
		backtrack_skeleton = false;
		backtrack_color = ImVec4(0.45f, 0.85f, 1.f, 0.75f);
		autofire_backtrack = false;
		autofire_backtrack_ms = 200.f;
		trigger_backtrack = false;
		trigger_backtrack_ms = 200.f;
		backtrack_aim = false;
		hitlog = false;
		hitlog_console = false;
		hitlog_duration = 4.f;
		hitlog_pos = ImVec2(-1.f, -1.f);
		hitlog_width = 268.f;
		hitlog_max_rows = 8;
		hitlog_show_hp = true;
		hitlog_show_stats = true;
		hitlog_color = ImVec4(0.92f, 0.92f, 0.95f, 0.95f);
		hitlog_head_color = ImVec4(1.f, 0.85f, 0.2f, 1.f);
		hitlog_kill_color = ImVec4(1.f, 0.35f, 0.35f, 1.f);
		auto_accept = false;
		sound_esp = false;
		sound_esp_duration = 1.4f;
		sound_esp_ring_size = 1.f;
		sound_esp_color = ImVec4(0.35f, 0.85f, 1.f, 0.95f);
		vote_reveal = false;
		vote_auto = false;
		vote_auto_choice = 0;
		vote_auto_delay_ms = 250.f;
		hitmarker = true;
		hitmarker_screen = true;
		hitmarker_world = true;
		hitmarker_show_damage = false;
		hitmarker_size = 14.f;
		hitmarker_gap = 4.f;
		hitmarker_thickness = 2.2f;
		hitmarker_world_size = 11.f;
		hitmarker_duration = 1.f;
		hitmarker_color = ImVec4(1.f, 1.f, 1.f, 0.95f);
		hitmarker_head_color = ImVec4(1.f, 0.85f, 0.15f, 1.f);
		hitmarker_kill_color = ImVec4(1.f, 0.22f, 0.22f, 1.f);
		hitsound = true;
		hitsound_file[0] = 0;
		hitsound_head[0] = 0;
		hitsound_kill[0] = 0;
		watermark = true;
		hud_keybind_strip = true;
		widget_keybinds = true;
		widget_bomb = true;
		widget_spectators = true;
		widget_keybinds_pos = ImVec2(-1.f, -1.f);
		widget_bomb_pos = ImVec2(-1.f, -1.f);
		widget_spectators_pos = ImVec2(-1.f, -1.f);
		widget_keybinds_only_when_active = false;
		widget_keybinds_show_all = true;
		menu_accent = ImVec4(0.42f, 0.68f, 0.92f, 1.00f);
		menu_bg = ImVec4(0.068f, 0.070f, 0.078f, 0.98f);
		menu_child_bg = ImVec4(0.102f, 0.105f, 0.118f, 1.00f);
		menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
		menu_border = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
		menu_text = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
		menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
		menu_rounding = 4.0f;
		menu_opacity = 0.98f;
		menu_compact = true;
		menu_dpi_scale = 100;

		widget_keybinds_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
		widget_bomb_accent = ImVec4(0.78f, 0.58f, 0.42f, 0.95f);
		widget_bomb_urgent = ImVec4(0.82f, 0.38f, 0.38f, 0.95f);
		widget_bomb_show_damage = true;
		widget_bomb_show_defuse = true;
		widget_spectators_accent = ImVec4(0.55f, 0.68f, 0.82f, 0.95f);
		widget_spectators_show_avatars = true;
		widget_spectators_max = 8;
		widget_radar = false;
		widget_radar_pos = ImVec2(-1.f, -1.f);
		widget_radar_size = 160.f;
		widget_radar_accent = ImVec4(0.52f, 0.72f, 0.80f, 0.95f);
		widget_radar_shape = 0;
		custom_paint_color = false;
		custom_color0 = ImVec4(1.0f, 0.0f, 1.0f, 1.0f);
		custom_color1 = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
		custom_color2 = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
		custom_color3 = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
		knife_changer = false;
		knife_index = 0;
		knife_paint_kit = 0;
		knife_paint_kit_id = 0;
		knife_wear = 0.0001f;
		knife_seed = 0;
		knife_custom_name[0] = '\0';
		knife_custom_color = false;
		knife_colors_active = false;
		knife_colors_edited = false;
		{ const float d[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1}; memcpy(knife_colors, d, sizeof(knife_colors)); }
		glove_changer = false;
		glove_index = 0;
		glove_paint_kit = 0;
		glove_paint_kit_id = 0;
		glove_wear = 0.0001f;
		glove_seed = 0;
		glove_custom_color = false;
		glove_colors_active = false;
		glove_colors_edited = false;
		{ const float d[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1}; memcpy(glove_colors, d, sizeof(glove_colors)); }
		weapon_skins = false;
		weapon_selected = 7;
		for (int i = 0; i < 71; ++i)
			weapon_skin[i] = {};
		agent_changer = false;
		agent_ct_def = 0;
		agent_t_def = 0;
		custom_model = false;
		custom_model_path[0] = '\0';
		custom_knife = false;
		custom_knife_path[0] = '\0';
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
		nade_lineup = false;
		nade_lineup_only_held = false;
		nade_lineup_stand_dist = 450.f;
		nade_lineup_aim_dist = 20.f;
		nade_lineup_select_dist = 550.f;
		nade_lineup_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		nade_lineup_aim_color = ImVec4(0.15f, 1.0f, 0.35f, 1.0f);
		nade_lineup_capture = true;
		nade_lineup_capture_key = VK_F6;
		nade_lineup_capture_key_mode = 1;
		nade_lineup_capture_throw = 0;
		nade_lineup_capture_kind = 0;
		std::snprintf(nade_lineup_capture_name, sizeof(nade_lineup_capture_name), "Lineup");
		weapon_group_ui = WG_RIFLE;
		InitWeaponProfilesDefaults();

		// Keep live keybind objects in sync with Config::* defaults
		keybind.resetToDefaults();
		aimbot_key = keybind.getKey(aimbot);
		aimbot_key_mode = keybind.getMode(aimbot);
		autofire_key = keybind.getKey(autofire);
		autofire_key_mode = keybind.getMode(autofire);
		autowall_key = keybind.getKey(autowall);
		autowall_key_mode = keybind.getMode(autowall);
		knifebot_key = keybind.getKey(knifebot);
		knifebot_key_mode = keybind.getMode(knifebot);
		triggerbot_key = keybind.getKey(triggerbot);
		triggerbot_key_mode = keybind.getMode(triggerbot);
		thirdperson_key = keybind.getKey(thirdperson);
		thirdperson_key_mode = keybind.getMode(thirdperson);
		nade_lineup_capture_key = keybind.getKey(nade_lineup_capture);
		nade_lineup_capture_key_mode = keybind.getMode(nade_lineup_capture);
	}
}

namespace {
	struct WeaponProfileBoot {
		WeaponProfileBoot() { Config::InitWeaponProfilesDefaults(); }
	};
	static WeaponProfileBoot g_weaponProfileBoot;
}
