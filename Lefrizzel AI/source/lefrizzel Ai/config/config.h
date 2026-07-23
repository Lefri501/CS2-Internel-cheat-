#pragma once
#include "../../../external/imgui/imgui.h"
#include <atomic>

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
	extern bool flag_money;      // $account
	extern bool flag_kit;        // defuse kit
	extern bool flag_helmet;     // helmet
	extern bool flag_nades;      // H/F/S/M/D held
	extern bool esp_rank;        // competitive rank under name
	extern bool esp_3d_box;      // oriented collision AABB wireframe
	extern bool esp_oof;         // offscreen arrows
	extern float esp_oof_radius; // px from screen center
	extern float esp_oof_size;   // arrow size
	extern ImVec4 esp_oof_color;
	extern ImVec4 esp_3d_box_color;
	extern ImVec4 esp_rank_color;

	// Floating damage numbers (world, independent of hitmarker X)
	extern bool float_damage;
	extern float float_damage_duration;
	extern float float_damage_speed; // px/sec up
	extern ImVec4 float_damage_color;
	extern ImVec4 float_damage_head_color;
	extern ImVec4 float_damage_kill_color;

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
	extern int chamsMaterial;          // Flat..Ghost2 + Pulse/Rainbow/Holo/Energy
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

	// Local body (thirdperson) + corpse chams
	extern bool localChams;
	extern int localChamsMaterial;
	extern ImVec4 colLocalChams;
	extern bool ragdollChams;
	extern int ragdollChamsMaterial;
	extern ImVec4 colRagdollChams;

	extern bool fovEnabled;
	extern float fov;

	// Aspect ratio (GetScreenAspectRatio / engine2 hook)
	extern bool aspect_ratio_enabled;
	extern float aspect_ratio; // width/height; e.g. 1.777... = 16:9

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
	extern bool remove_smoke;      // DrawSmokeVertex + DrawSmokeArray
	extern bool remove_decals;     // RENDERDECALS
	extern bool remove_particles;  // CacheParticleEffect spawn gate
	extern bool smoke_color;       // tint C_SmokeGrenadeProjectile m_vSmokeColor
	extern ImVec4 smoke_color_value;
	extern bool fire_color;        // tint molotov/inferno via ParticleDrawArray (particles.dll)
	extern ImVec4 fire_color_value;
	extern bool explosion_color;   // tint HE / explosion particles via ParticleDrawArray
	extern ImVec4 explosion_color_value;
	extern bool remove_crosshair;  // DrawCrosshair — hide
	extern bool force_crosshair;   // DrawCrosshair — show on snipers unscoped; hide when scoped
	extern bool remove_hud;        // ShouldShowHudElements (IDA 0x180F5BBF0)
	extern bool remove_postprocess; // poke cl_disable_postprocessing ConVar
	// Visual recoil only — strip aim-punch from CViewSetup angles (OverrideView).
	// Does NOT zero GetRemovedAimPunch used by fire/seed/RCS (IDA 0x18088BBB0).
	extern bool remove_recoil;

	// Scope — custom lines (DrawScopeOverlay kill + ImGui redraw)
	extern bool scope_custom_lines;
	extern float scope_line_size;      // 0..1 arm length (1 = full screen)
	extern float scope_line_gap;       // center hole px
	extern float scope_line_thickness; // line width px (0.1 hairline .. 6)
	extern ImVec4 scope_line_color;

	// Scope zoom FOV (GetRenderFov + OverrideView) — per zoom stage
	extern bool scope_zoom_fov;
	extern float scope_fov_1; // first zoom (m_zoomLevel == 1)
	extern float scope_fov_2; // second zoom (m_zoomLevel >= 2)

	// Hide weapon + viewmodel arms while scoped
	extern bool scope_hide_viewmodel;

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

	// Custom gradient fog (env_gradient_fog entity — celerity fog_handler)
	extern bool custom_fog;
	extern ImVec4 custom_fog_color;
	extern float custom_fog_start;
	extern float custom_fog_end;
	extern float custom_fog_falloff;

	// Weather via engine particles + rain_fx
	extern bool weather;
	// 1 Snow, 2 Stars, 3 Ash, 4 Rain
	extern int weather_mode;
	extern float weather_intensity; // 0..1 density

	// Local bullet tracers (eye → bullet_impact)
	extern bool tracers;
	extern int tracers_style;       // 0 Beam, 1 Laser, 2 Glow, 3 Dashed, 4 Energy
	extern float tracers_duration;  // seconds on screen
	extern float tracers_thickness; // base line width px
	extern ImVec4 tracers_color;

	extern bool aimbot;
	extern float aimbot_fov;
	extern float aimbot_smooth; // 0 = snap, 50 = slowest
	extern float aimbot_humanize; // 0 = off (default), 100 = subtle speed variance + tiny bias
	// Visible-aim smooth curve (Iniuria-style)
	enum AimSmoothMode : int {
		SMOOTH_CONSTANT = 0, // fixed deg/sec every frame
		SMOOTH_LINEAR = 1,   // strong far → soft near (settle)
		SMOOTH_SINE = 2,     // sine distance + mild wave (organic)
		SMOOTH_MODE_COUNT
	};
	extern int aimbot_smooth_mode; // AimSmoothMode

	// Knife bot (CreateMove)
	extern bool knifebot;
	extern bool knifebot_prefer_stab;
	extern int knifebot_key;
	extern int knifebot_key_mode;

	// Auto-pistol (Misc) — re-edge IN_ATTACK on semi pistols while M1 held
	extern bool auto_pistol;
	extern float auto_pistol_delay_ms; // extra wait after fire ready (0 = ASAP)

	// Enemy spectate while dead (comp/MM — client force observer target)
	extern bool enemy_spectate;
	extern bool enemy_spectate_thirdperson; // chase vs in-eye

	// Bomb helpers
	extern bool auto_defuse; // StartDefuse when near planted C4

	// Autofire — own keybind + FOV (separate from Aimbot FOV)
	extern bool autofire;
	extern bool autofire_silent; // GLOBAL — not per weapon group
	extern float autofire_fov; // lock FOV degrees (own circle when AF key active)
	extern float autofire_hitchance; // 0 = off, 1..100 = Monte Carlo % (HC mode)
	extern bool autofire_autostop; // counter-strafe before shot for accuracy
	extern bool autofire_autoscope; // scope weapons: zoom when hittable target in AF FOV
	extern bool autofire_scoped_only; // snipers: only fire while scoped (off by default)
	extern bool autofire_autowall; // penetrate walls when checking damage
	// Global AW keybind host (Always/Hold/Toggle). AF/TR pen also need their checkbox.
	extern bool autowall;
	extern int autowall_key;
	extern int autowall_key_mode; // 0 Always, 1 Hold, 2 Toggle
	// Autofire shot-value gate (Aimbot ignores). Trigger has its own pair.
	// If target HP < mindmg, lethal still passes. 0 = any hit.
	extern float autofire_mindamage;    // visible / non-pen
	extern float autofire_mindamage_aw; // through walls (pen)
	extern int autofire_key;
	extern int autofire_key_mode; // 0 Always, 1 Hold, 2 Toggle

	// Autofire accuracy mode — separate from trigger; Hitchance vs Seed Nospread
	enum AutofireMode : int {
		AF_MODE_HITCHANCE = 0,
		AF_MODE_SEED_NOSPREAD = 1,
		AF_MODE_COUNT
	};
	extern int autofire_mode; // AutofireMode

	// Autofire target priority (Gamesense-style SortTargets)
	enum AfTargetSelect : int {
		AF_TARGET_CROSSHAIR = 0, // lowest FOV to aim point
		AF_TARGET_DISTANCE = 1,  // closest player origin
		AF_TARGET_DAMAGE = 2,    // highest estimated damage (+ lethal prefer)
		AF_TARGET_COUNT
	};
	extern int autofire_target_select; // who-priority: crosshair / distance / damage
	// Target-selection filters (who is valid)
	extern bool autofire_vis_check;    // LOS required (when AW off)
	extern bool autofire_flash_check;  // skip while local is flashed
	extern bool autofire_smoke_check;  // skip aims through active smoke volumes
	extern bool autofire_focus_target; // don't switch targets while already shooting
	extern bool autofire_multipoint_dynamic; // shrink MP scale by bloom×distance
	// Body aim policy (hitbox pick among enabled AF hitboxes)
	extern bool autofire_body_if_lethal; // oneshot body (chest/stomach/pelvis) > head
	extern bool autofire_prefer_body;    // soft body bias; head still allowed

	// Multi-select hitboxes (shared index for aim / autofire / trigger lists)
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

	// Autofire — two separate lists (same HB indices):
	//   hitboxes   = which bones AF may lock on
	//   multipoint = which of those also aim edge/extra points (else center only)
	extern bool autofire_hitboxes[HB_COUNT];
	extern bool autofire_multipoint[HB_COUNT];
	extern float autofire_multipoint_scale[HB_COUNT]; // 0 = center, 1 = full edge (MP on only)

	extern bool team_check;
	// Auto team-check from game mode (DM/FFA → off). Applies to aim + ESP.
	extern bool team_check_auto;
	extern bool aim_vis_check;
	extern bool aim_smoke_check;   // skip aims through smoke
	extern bool aim_flash_check;   // pause while flashed
	extern bool aim_scoped_only;   // snipers: only aim while scoped

	// Humanization delays (ms, 0 = off / instant)
	extern float aim_reaction_delay_ms;       // detection → start aiming
	extern float aim_target_switch_delay_ms;  // old target → new target
	extern float aim_first_shot_delay_ms;     // lock → allow first attack

	extern bool aim_hitboxes[HB_COUNT]; // aimbot only — center lock, closest to crosshair (no MP)

	extern bool rcs;              // aimbot RCS (compensate when locking)
	extern bool rcs_standalone;   // independent RCS while shooting
	extern float rcs_scale_x;
	extern float rcs_scale_y;
	extern bool fov_circle;            // aimbot FOV ring
	extern bool fov_circle_autofire;   // autofire FOV ring (independent)
	extern bool fov_circle_magnet;     // trigger magnet FOV ring
	extern ImVec4 fovCircleColor;
	extern ImVec4 fovCircleColorAf;
	extern ImVec4 fovCircleColorMagnet;

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
		float aimbot_humanize = 0.f; // 0..100 — subtle only (no shake)
		int aimbot_smooth_mode = SMOOTH_LINEAR;
		bool aim_vis_check = true;
		bool aim_smoke_check = false;
		bool aim_flash_check = false;
		bool aim_scoped_only = false;
		bool aim_hitboxes[HB_COUNT]{};
		float aim_reaction_delay_ms = 0.f;
		float aim_target_switch_delay_ms = 0.f;
		float aim_first_shot_delay_ms = 0.f;
		bool rcs = false;
		bool rcs_standalone = false;
		float rcs_scale_x = 0.5f;
		float rcs_scale_y = 0.5f;
		float autofire_fov = 5.f;
		float autofire_hitchance = 70.f;
		int autofire_mode = AF_MODE_HITCHANCE;
		bool autofire_autostop = false;
		bool autofire_autoscope = false;
		bool autofire_scoped_only = false;
		bool autofire_autowall = false;
		float autofire_mindamage = 1.f;
		float autofire_mindamage_aw = 1.f;
		int autofire_target_select = AF_TARGET_CROSSHAIR;
		bool autofire_vis_check = true;
		bool autofire_flash_check = true;
		bool autofire_smoke_check = false;
		bool autofire_focus_target = true;
		bool autofire_multipoint_dynamic = true;
		bool autofire_body_if_lethal = false;
		bool autofire_prefer_body = false;
		bool autofire_hitboxes[HB_COUNT]{};     // lock-on hitboxes
		bool autofire_multipoint[HB_COUNT]{};   // edge multipoint enable (subset)
		float autofire_multipoint_scale[HB_COUNT]{};

		// Triggerbot (per group) — fires only when crosshair already on enemy
		float trigger_delay_ms = 0.f;      // 0 = instant; flicks ignore delay anyway
		float trigger_hitchance = 0.f;     // 0 = off (HC mode only)
		bool trigger_autowall = false;
		float trigger_mindamage = 1.f;     // visible — independent of autofire
		float trigger_mindamage_aw = 1.f;  // wallbang — independent of autofire
		bool trigger_scoped_only = false;  // useful for AWPs
		bool trigger_flash_check = true;   // skip while local is flashed
		bool trigger_smoke_check = false;  // skip fire through smoke
		bool trigger_hitboxes[HB_COUNT]{}; // lock/fire hitboxes (no multipoint list)
		bool trigger_autostop = false;
		int trigger_mode = TR_MODE_HITCHANCE; // Hitchance | Seed Nospread (separate modules)
		// Magnet (per group — same as other trigger knobs)
		bool trigger_magnet = false;
		float trigger_magnet_smooth = 12.f;
		float trigger_magnet_fov = 4.f;
		bool trigger_magnet_silent = false;     // stamp cmd only (no camera)
		bool trigger_magnet_lead = false;       // velocity lead
		bool trigger_magnet_head_prio = true;   // head > body when FOV close
		bool trigger_magnet_only_ready = false; // pull only when weapon can fire
		float trigger_magnet_deadzone = 0.12f;  // deg — stop micro-pull
		bool trigger_magnet_hitboxes[HB_COUNT]{}; // empty = use trigger_hitboxes
	};

	extern AimWeaponProfile weapon_profiles[WG_COUNT];
	extern int weapon_group_ui;     // menu editor selection
	extern int weapon_group_active; // runtime group from held weapon

	AimWeaponProfile& MenuAimProfile();
	void InitWeaponProfilesDefaults();
	// profile[group] → live Config::* (does not change weapon_group_active)
	void ApplyProfileToLive(int group);
	void ApplyWeaponGroup(::C_CSWeaponBase* weapon); // classify + ApplyProfileToLive
	void PullLiveIntoProfile(int group); // live Config::* → profile (migrate / copy)
	const char* WeaponGroupName(int group);
	int ClassifyWeaponGroup(::C_CSWeaponBase* weapon);

	// Live mirrors (filled by ApplyWeaponGroup)
	extern float trigger_delay_ms;
	extern float trigger_hitchance;
	extern bool trigger_autowall;
	extern float trigger_mindamage;     // visible — independent of autofire_mindamage
	extern float trigger_mindamage_aw;  // wallbang — independent of autofire_mindamage_aw
	extern bool trigger_scoped_only;
	extern bool trigger_flash_check;
	extern bool trigger_smoke_check;
	extern bool trigger_hitboxes[HB_COUNT];
	extern bool trigger_autostop;
	extern int trigger_mode; // TriggerMode
	// Magnet: soft-aim into trigger FOV while key held (not only when already on-target)
	extern bool trigger_magnet;
	extern float trigger_magnet_smooth; // 0 = snap, 50 = slow (no aimbot humanize)
	extern float trigger_magnet_fov;    // deg FOV to pick bone (default 4)
	extern bool trigger_magnet_silent;
	extern bool trigger_magnet_lead;
	extern bool trigger_magnet_head_prio;
	extern bool trigger_magnet_only_ready;
	extern float trigger_magnet_deadzone;
	extern bool trigger_magnet_hitboxes[HB_COUNT];

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
	// Jumpbug / Edgebug / Edgejump (Misc movement) — keybinds via keybind system
	extern bool jumpbug;
	extern int jumpbug_key;
	extern int jumpbug_key_mode;
	extern bool edgebug;
	extern int edgebug_key;
	extern int edgebug_key_mode;
	extern bool edgejump;
	extern int edgejump_key;
	extern int edgejump_key_mode;
	// Backtrack — global master (Misc). AF/TR shoot lag heads when on.
	// Skeleton only draws when toggle on AND master on.
	extern bool backtrack;
	extern float backtrack_ms;         // 50..400
	extern bool backtrack_skeleton;    // Visuals — lag ghost skeleton
	extern ImVec4 backtrack_color;
	// Legacy aliases (cfg migrate / old code paths) — mirror master
	extern bool autofire_backtrack;
	extern float autofire_backtrack_ms;
	extern bool trigger_backtrack;
	extern float trigger_backtrack_ms;
	extern bool backtrack_aim;
	// Hit log panel (Misc)
	extern bool hitlog;
	extern bool hitlog_console; // also echo hits to game developer console
	extern float hitlog_duration;
	extern ImVec2 hitlog_pos; // <0 = auto
	extern float hitlog_width;       // panel width px
	extern int hitlog_max_rows;      // visible rows (4..16)
	extern bool hitlog_show_hp;      // remaining victim HP after hit
	extern bool hitlog_show_stats;   // session H/HS/K/DMG footer
	extern ImVec4 hitlog_color;
	extern ImVec4 hitlog_head_color;
	extern ImVec4 hitlog_kill_color;
	// subtick_move + pred_upgrade: always on (hardcoded, no config)
	extern bool auto_accept;    // Misc — accept matchmaking popup via Lobby ReadyUp

	// Sound ESP (Visuals) — ground walk rings on ESP-visible enemies
	extern bool sound_esp;
	extern float sound_esp_duration; // marker life sec
	extern float sound_esp_ring_size; // scale 0.5..3
	extern ImVec4 sound_esp_color;

	// Vote reveal / auto-vote (Misc)
	extern bool vote_reveal;       // toast + chat who voted yes/no
	extern bool vote_auto;         // auto cast after delay
	extern int  vote_auto_choice;  // 0 Yes, 1 No
	extern float vote_auto_delay_ms;

	// Hitmarker (Misc) — COD screen X + world 3D at hit bone
	extern bool hitmarker;
	extern bool hitmarker_screen;
	extern bool hitmarker_world;
	extern bool hitmarker_show_damage;
	extern float hitmarker_size;       // screen arm length px
	extern float hitmarker_gap;        // center gap px
	extern float hitmarker_thickness;
	extern float hitmarker_world_size; // world arm length px
	extern float hitmarker_duration;   // life scale 0.25..2.5
	extern ImVec4 hitmarker_color;
	extern ImVec4 hitmarker_head_color;
	extern ImVec4 hitmarker_kill_color;

	// Hitsound (Misc) — custom .wav from csgo/sounds/hitsounds
	extern bool hitsound;
	extern char hitsound_file[160];   // normal hit basename
	extern char hitsound_head[160];   // empty = use hitsound_file
	extern char hitsound_kill[160];   // empty = use hitsound_file

	// HUD watermark (Misc)
	extern bool watermark;

	// Menu design (Config → Design)
	extern ImVec4 menu_accent;
	extern ImVec4 menu_bg;
	extern ImVec4 menu_child_bg;
	extern ImVec4 menu_sidebar_bg;
	extern ImVec4 menu_border;
	extern ImVec4 menu_text;
	extern ImVec4 menu_text_muted;
	extern float menu_rounding;  // 0..10
	extern float menu_opacity;   // shell glass alpha 0.55..1 (0.90+ readable)
	extern bool menu_compact;    // tighter spacing
	extern int menu_dpi_scale;   // percent 100..200 (4K / high-DPI)

	// Panorama HUD (Misc) — native UI via RunScript
	extern bool hud_keybind_strip; // bottom-center keybind chips

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
	extern bool widget_radar;
	extern ImVec2 widget_radar_pos;
	extern float widget_radar_size;
	extern ImVec4 widget_radar_accent;
	// 0 = circle, 1 = square
	extern int widget_radar_shape;

	// Custom paint colors — global g_vColor0..3 injected into every weapon material
	extern bool custom_paint_color;
	extern ImVec4 custom_color0;
	extern ImVec4 custom_color1;
	extern ImVec4 custom_color2;
	extern ImVec4 custom_color3;

	// Skinchanger — knife / gloves / weapons
	extern bool knife_changer;
	extern int  knife_index; // index into SkinChanger::Knives()
	extern int  knife_paint_kit; // index into KnifePaintKits() for selected knife
	extern int  knife_paint_kit_id; // actual paint kit id (source of truth for apply)
	extern float knife_wear;
	extern int  knife_seed;
	extern char knife_custom_name[161];
	extern bool knife_custom_color;
	extern float knife_colors[16];
	// true only after seed found real paint-kit colors, or user edited a picker
	extern bool knife_colors_active;
	extern bool knife_colors_edited;

	extern bool glove_changer;
	extern int  glove_index; // index into SkinChanger::Gloves()
	extern int  glove_paint_kit;
	extern int  glove_paint_kit_id;
	extern float glove_wear;
	extern int  glove_seed;
	extern bool glove_custom_color;
	extern float glove_colors[16];
	extern bool glove_colors_active;
	extern bool glove_colors_edited;
	extern bool weapon_skins;
	extern int  weapon_selected; // def index 1..70 for menu
	struct WeaponSkinSlot {
		int paint_kit = 0;
		int paint_kit_id = 0;
		float wear = 0.0001f;
		int seed = 0;
		bool custom_color = false;
		bool colors_active = false; // seed hit or user edited
		bool colors_edited = false; // user touched a picker — don't reseed / gate inject
		float colors[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
	};
	extern WeaponSkinSlot weapon_skin[71]; // indexed by def_index

	extern bool agent_changer;
	extern int  agent_ct_def; // 0 = default / off
	extern int  agent_t_def;
	// Custom .vmdl from csgo/characters/models (overrides stock agents when on)
	extern bool custom_model;
	extern char custom_model_path[260]; // characters/models/.../name.vmdl
	// Custom knife mesh from csgo/lefrizzel_models/knives (needs knife_changer + stock knife)
	extern bool custom_knife;
	extern char custom_knife_path[260]; // weapons/.../name.vmdl

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

	// Grenade lineup helper (UC 9xth style — stand circle + aim marker)
	extern bool nade_lineup;
	extern bool nade_lineup_only_held;     // only show when holding matching nade
	extern float nade_lineup_stand_dist;  // fade/draw stand circle
	extern float nade_lineup_aim_dist;    // show aim marker when this close to stand
	extern float nade_lineup_select_dist; // nearest-lineup pick radius
	extern ImVec4 nade_lineup_color;
	extern ImVec4 nade_lineup_aim_color;
	// Capture keybind host (like aimbot/trigger) — edge-press captures
	extern bool nade_lineup_capture;
	extern int nade_lineup_capture_key;
	extern int nade_lineup_capture_key_mode; // 0 Always, 1 Hold, 2 Toggle
	extern int nade_lineup_capture_throw; // 0 Stand 1 Stand+Jump 2 Walk 3 Run 4 Crouch 5 Run+Jump
	extern int nade_lineup_capture_kind;  // 0 Any … 5 Decoy
	extern char nade_lineup_capture_name[64];

	// Restore every field to config.cpp defaults (used before Load)
	void ResetToDefaults();

	// True while ConfigManager::Load mutates Config::* — World/Glow/Skin must skip
	// so ResetToDefaults (Night/knife briefly off) cannot race FSN / Present.
	extern std::atomic<bool> loading;
}
