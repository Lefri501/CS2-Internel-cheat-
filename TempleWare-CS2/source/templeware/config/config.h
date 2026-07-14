#pragma once
#include "../../../external/imgui/imgui.h"

class C_CSWeaponBase;

// CBA to make proper atm, it's 03:42 right now.
// For now just stores config values don't mind it too much
//
// (FYI THIS IS A HORRID SOLUTION BUT FUNCTIONS) 

namespace Config {
	extern bool esp;
	extern bool showHealth;
	extern bool showArmor;
	extern bool showDistance;
	extern bool showWeapon;
	extern bool showWeaponIcon; // UC CS2 icon font under player / world
	extern bool teamCheck;
	extern bool espFill;
	extern float espThickness;
	extern float espFillOpacity;
	extern ImVec4 espColor;
	extern ImVec4 espColorInvisible; // box when occluded (vis check on)
	extern bool showNameTags;
	extern bool esp_name_avatar; // Steam avatar beside name ESP
	extern bool esp_skeleton;
	extern float esp_skeleton_thickness;
	extern ImVec4 esp_skeleton_color;
	extern ImVec4 esp_skeleton_color_invisible;
	extern bool esp_vis_check;
	// Glow ESP (Andromeda DrawGlow path)
	extern bool glow;
	extern bool glow_team;
	extern bool glow_enemy;
	extern bool glow_only_visible;
	extern ImVec4 glow_color;       // visible
	extern ImVec4 glow_color_invis; // behind wall
	extern bool glow_world_weapons;
	extern bool glow_world_bomb;
	extern bool glow_world_grenades;

	// ESP style (right-click settings)
	enum EspBoxStyle : int { ESP_BOX_FULL = 0, ESP_BOX_CORNER = 1 };
	extern int esp_box_style;        // EspBoxStyle
	extern float esp_box_width;      // width = height * this (0.28–0.70)
	extern float esp_bar_width;      // health/armor bar px
	extern bool esp_health_auto;     // true = green→red by HP
	extern ImVec4 esp_health_color;
	extern ImVec4 esp_armor_color;
	extern ImVec4 esp_name_color;
	extern ImVec4 esp_weapon_color;
	extern ImVec4 esp_distance_color;

	// Flags ESP
	extern bool flag_flashed;
	extern bool flag_bomb;
	extern bool flag_scoped;
	extern bool flag_reloading;
	extern bool flag_defusing;

	// World ESP (dropped items / bomb / projectiles)
	extern bool world_esp_weapons;
	extern bool world_esp_bomb;
	extern bool world_esp_smoke;
	extern bool world_esp_molotov;
	extern bool world_esp_he;
	extern bool world_esp_flash;
	extern bool world_esp_decoy;
	extern bool world_esp_distance;
	extern ImVec4 world_esp_weapon_color;
	extern ImVec4 world_esp_bomb_color;
	extern ImVec4 world_esp_smoke_color;
	extern ImVec4 world_esp_molotov_color;
	extern ImVec4 world_esp_he_color;
	extern ImVec4 world_esp_flash_color;
	extern ImVec4 world_esp_decoy_color;

	extern bool enemyChamsInvisible;
	extern bool enemyChams;
	extern bool teamChams;
	extern bool teamChamsInvisible;
	extern int chamsMaterial;          // Flat / Illuminate / Glow / Ghost / Latex
	extern int chamsMaterialXQZ;       // enemy behind walls (ignore-Z)
	extern int armChamsMaterial;       // hands
	extern int viewmodelChamsMaterial; // weapon chams (held viewmodel)

	extern ImVec4 colVisualChams;
	extern ImVec4 colVisualChamsIgnoreZ;
	extern ImVec4 teamcolVisualChamsIgnoreZ;
	extern ImVec4 teamcolVisualChams;

	extern bool armChams;
	extern bool viewmodelChams;
	extern ImVec4 colViewmodelChams;
	extern ImVec4 colArmChams;

	extern bool fovEnabled;
	extern float fov;

	// Viewmodel (CalcViewModel / GetViewModelOffsets hook)
	extern bool viewmodel_changer;
	extern float viewmodel_fov;
	extern float viewmodel_x;
	extern float viewmodel_y;
	extern float viewmodel_z;

	// Third person (OverrideView hook)
	extern bool thirdperson;
	extern float thirdperson_distance;
	extern int thirdperson_key;
	extern int thirdperson_key_mode; // 0 Always, 1 Hold, 2 Toggle

	extern bool antiflash;           // legacy (derived from amount when saving)
	extern float antiflash_amount;   // 0 = full flash, 100 = fully removed

	// Visuals → Removals (early-return hooks)
	extern bool remove_legs;       // DRAWLEGS — firstperson legs
	extern bool remove_smoke;      // DRAWSMOKEVERTEX
	extern bool remove_decals;     // RENDERDECALS
	extern bool remove_particles;  // CREATEPARTICLEEFFECT

	// Scope HUD (DrawScopeOverlay hook)
	extern bool scope_no_overlay;       // force draw=false (everything off)
	extern bool scope_remove_blur;      // zero blur strength (+8)
	extern bool scope_remove_bars;      // enlarge lens to kill letterbox bars
	extern bool scope_remove_texture;   // zero blend / lens texture
	extern bool scope_custom_look;      // apply size/blur sliders
	extern float scope_size_scale;      // multiplies +12/+16 (1 = default)
	extern float scope_blur_amount;     // 0..1 overrides +8 when custom look

	extern bool Night;
	extern float night_exposure; // darkness 0..1 (0 = none, 1 = darkest)

	// Skybox tint via C_EnvSky::m_vTintColor (+ lighting-only)
	extern bool skybox;
	extern ImVec4 skybox_color;

	// Global light (CGlobalLightBase::m_LightColor) via GLOBALLIGHTUPDATESTATE
	extern bool lighting;
	extern ImVec4 lighting_color;

	// World/map mesh tint via DrawArray (non-player meshes)
	extern bool map_color;
	extern ImVec4 map_color_value;

	// Weather particles via GameParticleManager (rain/snow/ash)
	extern bool weather;
	extern int weather_mode; // 0 Off, 1 Rain, 2 Snow, 3 Ash
	extern float weather_intensity; // 0..1

	extern bool aimbot;
	extern float aimbot_fov;
	extern float aimbot_smooth; // 0 = snap, 50 = slowest

	// Autofire — own keybind; shares Aimbot FOV/smooth (one FOV circle)
	extern bool autofire;
	extern bool autofire_silent; // true silent = no crosshair move
	extern float autofire_hitchance; // 0 = off, 1..100 = Monte Carlo %
	extern bool autofire_autostop; // counter-strafe before shot for accuracy
	extern bool autofire_autowall; // penetrate walls when checking damage
	extern float autofire_mindamage; // min dmg for visible targets (0 = any hit)
	extern float autofire_mindamage_aw; // min dmg through walls (0 = any hit)
	extern int autofire_key;
	extern int autofire_key_mode; // 0 Always, 1 Hold, 2 Toggle

	// Autofire target priority (Gamesense-style SortTargets)
	enum AfTargetSelect : int {
		AF_TARGET_CROSSHAIR = 0, // lowest FOV to aim point
		AF_TARGET_DISTANCE = 1,  // closest player origin
		AF_TARGET_DAMAGE = 2,    // highest estimated damage (+ lethal prefer)
		AF_TARGET_COUNT
	};
	extern int autofire_target_select;
	extern bool autofire_multipoint_dynamic; // shrink scale by bloom×distance

	// Autofire multipoint hitboxes (subset — not full aimbot list)
	enum AfMpHitbox : int {
		AF_MP_HEAD = 0,
		AF_MP_CHEST,
		AF_MP_STOMACH,
		AF_MP_PELVIS,
		AF_MP_COUNT
	};
	extern bool autofire_hitboxes[AF_MP_COUNT];      // multi-select combo
	extern float autofire_multipoint_scale[AF_MP_COUNT]; // 0 = center only, 1 = full edge

	extern bool team_check;
	// Auto team-check from game mode (DM/FFA → off). Applies to aim + ESP.
	extern bool team_check_auto;
	extern bool aim_vis_check;

	// Humanization delays (ms, 0 = off / instant)
	extern float aim_reaction_delay_ms;       // detection → start aiming
	extern float aim_target_switch_delay_ms;  // old target → new target
	extern float aim_first_shot_delay_ms;     // lock → allow first attack

	// Multi-select hitboxes (bitmask AimHitbox)
	enum AimHitbox : int {
		HB_HEAD = 0,
		HB_NECK,
		HB_CHEST,
		HB_STOMACH,
		HB_PELVIS,
		HB_ARMS,
		HB_LEGS,
		HB_FEET,
		HB_COUNT
	};
	extern bool aim_hitboxes[HB_COUNT]; // multi-select

	extern bool rcs;              // aimbot RCS (compensate when locking)
	extern bool rcs_standalone;   // independent RCS while shooting
	extern float rcs_scale_x;
	extern float rcs_scale_y;
	extern bool fov_circle;
	extern ImVec4 fovCircleColor;

	// Trigger accuracy mode (per weapon group)
	enum TriggerMode : int {
		TR_MODE_HITCHANCE = 0,     // Monte Carlo % + movement gates
		TR_MODE_SEED_NOSPREAD = 1, // exact SPREADSEEDGEN ray wait (no angle rewrite)
		TR_MODE_COUNT
	};

	// ---- Per weapon-group aim/autofire profiles ----
	// CS2 CCSWeaponType: knife=0 pistol=1 smg=2 rifle=3 shotgun=4 sniper=5 lmg=6
	enum WeaponGroup : int {
		WG_GENERAL = 0, // knife / unknown / fallback
		WG_PISTOL,
		WG_SMG,
		WG_RIFLE,
		WG_SHOTGUN,
		WG_SNIPER,
		WG_LMG,
		WG_COUNT
	};

	struct AimWeaponProfile {
		float aimbot_fov = 5.f;
		float aimbot_smooth = 5.f;
		bool aim_vis_check = true;
		bool aim_hitboxes[HB_COUNT]{};
		float aim_reaction_delay_ms = 0.f;
		float aim_target_switch_delay_ms = 0.f;
		float aim_first_shot_delay_ms = 0.f;
		bool rcs = false;
		bool rcs_standalone = false;
		float rcs_scale_x = 0.5f;
		float rcs_scale_y = 0.5f;
		bool autofire_silent = false;
		float autofire_hitchance = 70.f;
		bool autofire_autostop = false;
		bool autofire_autowall = false;
		float autofire_mindamage = 1.f;
		float autofire_mindamage_aw = 1.f;
		int autofire_target_select = AF_TARGET_CROSSHAIR;
		bool autofire_multipoint_dynamic = true;
		bool autofire_hitboxes[AF_MP_COUNT]{};
		float autofire_multipoint_scale[AF_MP_COUNT]{};

		// Triggerbot (per group) — fires only when crosshair already on enemy
		float trigger_delay_ms = 0.f;      // 0 = instant; flicks ignore delay anyway
		float trigger_hitchance = 0.f;     // 0 = off; flicks skip HC even if set
		bool trigger_autowall = false;
		bool trigger_scoped_only = false;  // useful for AWPs
		bool trigger_hitboxes[HB_COUNT]{}; // own multi-select list
		bool trigger_autostop = false;
		int trigger_mode = TR_MODE_HITCHANCE; // Hitchance | Seed Nospread
	};

	extern AimWeaponProfile weapon_profiles[WG_COUNT];
	extern int weapon_group_ui;     // menu editor selection
	extern int weapon_group_active; // runtime group from held weapon

	AimWeaponProfile& MenuAimProfile();
	void InitWeaponProfilesDefaults();
	void ApplyWeaponGroup(::C_CSWeaponBase* weapon); // copies profile → live Config::*
	void PullLiveIntoProfile(int group); // live Config::* → profile (migrate / copy)
	const char* WeaponGroupName(int group);
	int ClassifyWeaponGroup(::C_CSWeaponBase* weapon);

	// Live mirrors (filled by ApplyWeaponGroup)
	extern float trigger_delay_ms;
	extern float trigger_hitchance;
	extern bool trigger_autowall;
	extern bool trigger_scoped_only;
	extern bool trigger_hitboxes[HB_COUNT];
	extern bool trigger_autostop;
	extern int trigger_mode; // TriggerMode

	// Triggerbot master + keybind (global)
	extern bool triggerbot;
	extern int triggerbot_key;
	extern int triggerbot_key_mode; // 0 Always, 1 Hold, 2 Toggle

	// keybind persist (aimbot)
	extern int aimbot_key;
	extern int aimbot_key_mode; // 0 Always, 1 Hold, 2 Toggle

	extern bool bhop;
	extern bool autostrafe;
	extern int autostrafe_mode; // 0 mouse (legit), 1 vectorial (silent)

	// Overlay widgets (Misc)
	extern bool widget_keybinds;
	extern bool widget_bomb;
	extern bool widget_spectators;
	extern ImVec2 widget_keybinds_pos; // <0 = auto
	extern ImVec2 widget_bomb_pos;
	extern ImVec2 widget_spectators_pos;
	extern bool widget_keybinds_only_when_active; // hide until a bind is active
	extern bool widget_keybinds_show_all;         // false = active binds only
	extern ImVec4 widget_keybinds_accent;
	extern ImVec4 widget_bomb_accent;
	extern ImVec4 widget_bomb_urgent;
	extern bool widget_bomb_show_damage;
	extern bool widget_bomb_show_defuse;
	extern ImVec4 widget_spectators_accent;
	extern bool widget_spectators_show_avatars;
	extern int widget_spectators_max; // 1..16

	// Skinchanger — knife / gloves / weapons
	extern bool knife_changer;
	extern int  knife_index; // index into SkinChanger::Knives()
	extern int  knife_paint_kit; // index into KnifePaintKits() for selected knife
	extern int  knife_paint_kit_id; // actual paint kit id (source of truth for apply)
	extern float knife_wear;
	extern int  knife_seed;
	extern char knife_custom_name[161];

	extern bool glove_changer;
	extern int  glove_index; // index into SkinChanger::Gloves()
	extern int  glove_paint_kit;
	extern int  glove_paint_kit_id;
	extern float glove_wear;
	extern int  glove_seed;

	extern bool weapon_skins;
	extern int  weapon_selected; // def index 1..70 for menu
	struct WeaponSkinSlot {
		int paint_kit = 0;
		int paint_kit_id = 0;
		float wear = 0.0001f;
		int seed = 0;
	};
	extern WeaponSkinSlot weapon_skin[71]; // indexed by def_index

	extern bool agent_changer;
	extern int  agent_ct_def; // 0 = default / off
	extern int  agent_t_def;

	// Grenade prediction
	extern bool nade_pred;
	extern bool nade_pred_local;          // local throw preview
	extern bool nade_pred_local_only_pin; // only when pin pulled
	extern bool nade_pred_projectiles;    // in-air nades
	extern bool nade_pred_radius;         // land effect ring
	extern float nade_pred_thickness;
	extern ImVec4 nade_pred_color;
	extern ImVec4 nade_pred_local_color;
	extern ImVec4 nade_pred_land_color;

	// Grenade warning (incoming projectile icons)
	extern bool nade_warn;
	extern bool nade_warn_only_near;      // only when land/pos near local
	extern float nade_warn_range;         // meters
	extern float nade_warn_icon_size;
	extern ImVec4 nade_warn_color;

	// HE damage indicator at predicted land
	extern bool nade_pred_damage;
	extern ImVec4 nade_pred_damage_color;
	extern ImVec4 nade_pred_damage_lethal_color;

	// Restore every field to config.cpp defaults (used before Load)
	void ResetToDefaults();
}
