#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <Windows.h>
#include <Shellapi.h>

#include "config.h"
#include "../keybinds/keybinds.h"
#include "../features/skinchanger/skinchanger.h"
#include "../../../external/json/json.hpp"

namespace internal_config
{
    class ConfigManager
    {
    private:

        static std::filesystem::path GetConfigFolder()
        {
            // C:\Users\<user>\Documents\Lefrizzel AI\Configs
            std::filesystem::path folder;
            char* userProfile = nullptr;
            size_t len = 0;
            if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile && len > 0) {
                folder = userProfile;
                free(userProfile);
                folder /= "Documents";
                folder /= "Lefrizzel AI";
                folder /= "Configs";
            } else {
                folder = "Lefrizzel AI";
                folder /= "Configs";
            }

            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            return folder;
        }

        static std::string SanitizeName(std::string name)
        {
            // trim
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                name.erase(name.begin());
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();

            // strip invalid filename chars
            std::string out;
            out.reserve(name.size());
            for (char c : name) {
                if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\'
                    || c == '|' || c == '?' || c == '*' || c < 32)
                    continue;
                out.push_back(c);
            }
            if (out.size() > 64)
                out.resize(64);
            return out;
        }

        static std::filesystem::path GetConfigPath(const std::string& configName)
        {
            return GetConfigFolder() / (SanitizeName(configName) + ".json");
        }

        // Old AF hitbox arrays were 4 slots: Head/Chest/Stomach/Pelvis.
        // New = full HB_COUNT. Map old indices so Chest does not land on Neck.
        static void LoadAfHitboxesFromJson(const nlohmann::json& arr, bool* outHb, float* outScale)
        {
            for (int i = 0; i < Config::HB_COUNT; ++i) {
                outHb[i] = false;
                if (outScale)
                    outScale[i] = 0.50f;
            }
            if (!arr.is_array()) {
                outHb[Config::HB_HEAD] = true;
                outHb[Config::HB_NECK] = true;
                outHb[Config::HB_CHEST] = true;
                return;
            }
            const int n = (int)arr.size();
            if (n == 4) {
                // Legacy AF_MP order
                static constexpr int kOldToHb[4] = {
                    Config::HB_HEAD, Config::HB_CHEST, Config::HB_STOMACH, Config::HB_PELVIS
                };
                for (int i = 0; i < 4; ++i) {
                    if (arr[i].is_boolean())
                        outHb[kOldToHb[i]] = arr[i].get<bool>();
                    else if (arr[i].is_number() && outScale)
                        outScale[kOldToHb[i]] = arr[i].get<float>();
                }
            } else {
                for (int i = 0; i < Config::HB_COUNT && i < n; ++i) {
                    if (arr[i].is_boolean())
                        outHb[i] = arr[i].get<bool>();
                    else if (arr[i].is_number() && outScale)
                        outScale[i] = arr[i].get<float>();
                }
            }
            bool any = false;
            for (int i = 0; i < Config::HB_COUNT; ++i) {
                if (outHb[i]) { any = true; break; }
            }
            if (!any) {
                outHb[Config::HB_HEAD] = true;
                outHb[Config::HB_NECK] = true;
                outHb[Config::HB_CHEST] = true;
            }
        }

        static void LoadAfScalesFromJson(const nlohmann::json& arr, float* outScale)
        {
            if (!arr.is_array() || !outScale)
                return;
            const int n = (int)arr.size();
            if (n == 4) {
                static constexpr int kOldToHb[4] = {
                    Config::HB_HEAD, Config::HB_CHEST, Config::HB_STOMACH, Config::HB_PELVIS
                };
                for (int i = 0; i < 4; ++i) {
                    if (arr[i].is_number())
                        outScale[kOldToHb[i]] = arr[i].get<float>();
                }
            } else {
                for (int i = 0; i < Config::HB_COUNT && i < n; ++i) {
                    if (arr[i].is_number())
                        outScale[i] = arr[i].get<float>();
                }
            }
        }

        // Multipoint enable list — all-false is valid (center-only on every lock box).
        static void LoadAfMultipointFromJson(const nlohmann::json& arr, bool* outMp)
        {
            for (int i = 0; i < Config::HB_COUNT; ++i)
                outMp[i] = false;
            if (!arr.is_array())
                return;
            const int n = (int)arr.size();
            if (n == 4) {
                static constexpr int kOldToHb[4] = {
                    Config::HB_HEAD, Config::HB_CHEST, Config::HB_STOMACH, Config::HB_PELVIS
                };
                for (int i = 0; i < 4; ++i) {
                    if (arr[i].is_boolean())
                        outMp[kOldToHb[i]] = arr[i].get<bool>();
                }
            } else {
                for (int i = 0; i < Config::HB_COUNT && i < n; ++i) {
                    if (arr[i].is_boolean())
                        outMp[i] = arr[i].get<bool>();
                }
            }
        }

    public:

        static std::filesystem::path Folder()
        {
            return GetConfigFolder();
        }

        static void OpenFolder()
        {
            const auto folder = GetConfigFolder();
            ShellExecuteW(nullptr, L"open", folder.wstring().c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }

        static std::vector<std::string> ListConfigs()
        {
            std::vector<std::string> list;
            const auto folder = GetConfigFolder();
            std::error_code ec;
            if (!std::filesystem::exists(folder, ec))
                return list;

            for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
                if (ec)
                    break;
                if (!entry.is_regular_file(ec))
                    continue;
                if (entry.path().extension() == ".json")
                    list.push_back(entry.path().stem().string());
            }
            std::sort(list.begin(), list.end(),
                [](const std::string& a, const std::string& b) {
                    return _stricmp(a.c_str(), b.c_str()) < 0;
                });
            return list;
        }

        static bool Save(const std::string& configName)
        {
            const std::string name = SanitizeName(configName);
            if (name.empty())
                return false;

            nlohmann::json j;
            j["esp"] = Config::esp;
            j["showHealth"] = Config::showHealth;
            j["showArmor"] = Config::showArmor;
            j["showDistance"] = Config::showDistance;
            j["showWeapon"] = Config::showWeapon;
            j["showWeaponIcon"] = Config::showWeaponIcon;
            j["showNameTags"] = Config::showNameTags;
            j["esp_name_avatar"] = Config::esp_name_avatar;
            j["esp_skeleton"] = Config::esp_skeleton;
            j["esp_skeleton_thickness"] = Config::esp_skeleton_thickness;
            j["esp_skeleton_color"] = {
                Config::esp_skeleton_color.x, Config::esp_skeleton_color.y,
                Config::esp_skeleton_color.z, Config::esp_skeleton_color.w
            };
            j["esp_skeleton_color_invisible"] = {
                Config::esp_skeleton_color_invisible.x, Config::esp_skeleton_color_invisible.y,
                Config::esp_skeleton_color_invisible.z, Config::esp_skeleton_color_invisible.w
            };
            j["esp_vis_check"] = Config::esp_vis_check;
            j["glow"] = Config::glow;
            j["glow_team"] = Config::glow_team;
            j["glow_enemy"] = Config::glow_enemy;
            j["glow_only_visible"] = Config::glow_only_visible;
            j["glow_color"] = { Config::glow_color.x, Config::glow_color.y, Config::glow_color.z, Config::glow_color.w };
            j["glow_color_invis"] = { Config::glow_color_invis.x, Config::glow_color_invis.y, Config::glow_color_invis.z, Config::glow_color_invis.w };
            j["glow_world_weapons"] = Config::glow_world_weapons;
            j["glow_world_bomb"] = Config::glow_world_bomb;
            j["glow_world_grenades"] = Config::glow_world_grenades;
            j["teamCheck"] = Config::teamCheck;
            j["espFill"] = Config::espFill;
            j["espThickness"] = Config::espThickness;
            j["espFillOpacity"] = Config::espFillOpacity;
            j["esp_box_style"] = Config::esp_box_style;
            j["esp_box_width"] = Config::esp_box_width;
            j["esp_bar_width"] = Config::esp_bar_width;
            j["esp_health_auto"] = Config::esp_health_auto;

            j["fovEnabled"] = Config::fovEnabled;
            j["aspect_ratio_enabled"] = Config::aspect_ratio_enabled;
            j["aspect_ratio"] = Config::aspect_ratio;
            j["fov"] = Config::fov;
            j["viewmodel_changer"] = Config::viewmodel_changer;
            j["viewmodel_fov"] = Config::viewmodel_fov;
            j["viewmodel_x"] = Config::viewmodel_x;
            j["viewmodel_y"] = Config::viewmodel_y;
            j["viewmodel_z"] = Config::viewmodel_z;
            j["thirdperson"] = Config::thirdperson;
            j["thirdperson_distance"] = Config::thirdperson_distance;
            j["thirdperson_key"] = keybind.getKey(Config::thirdperson);
            j["thirdperson_key_mode"] = keybind.getMode(Config::thirdperson);

            j["espColor"] = {
                Config::espColor.x, Config::espColor.y, Config::espColor.z, Config::espColor.w
            };
            j["espColorInvisible"] = {
                Config::espColorInvisible.x, Config::espColorInvisible.y,
                Config::espColorInvisible.z, Config::espColorInvisible.w
            };
            j["esp_health_color"] = {
                Config::esp_health_color.x, Config::esp_health_color.y,
                Config::esp_health_color.z, Config::esp_health_color.w
            };
            j["esp_armor_color"] = {
                Config::esp_armor_color.x, Config::esp_armor_color.y,
                Config::esp_armor_color.z, Config::esp_armor_color.w
            };
            j["esp_name_color"] = {
                Config::esp_name_color.x, Config::esp_name_color.y,
                Config::esp_name_color.z, Config::esp_name_color.w
            };
            j["esp_weapon_color"] = {
                Config::esp_weapon_color.x, Config::esp_weapon_color.y,
                Config::esp_weapon_color.z, Config::esp_weapon_color.w
            };
            j["esp_distance_color"] = {
                Config::esp_distance_color.x, Config::esp_distance_color.y,
                Config::esp_distance_color.z, Config::esp_distance_color.w
            };

            j["Night"] = Config::Night;
            j["night_exposure"] = Config::night_exposure;
            j["skybox"] = Config::skybox;
            j["skybox_color"] = {
                Config::skybox_color.x, Config::skybox_color.y,
                Config::skybox_color.z, Config::skybox_color.w
            };
            j["lighting"] = Config::lighting;
            j["lighting_color"] = {
                Config::lighting_color.x, Config::lighting_color.y,
                Config::lighting_color.z, Config::lighting_color.w
            };
            j["map_color"] = Config::map_color;
            j["map_color_value"] = {
                Config::map_color_value.x, Config::map_color_value.y,
                Config::map_color_value.z, Config::map_color_value.w
            };
            j["custom_fog"] = Config::custom_fog;
            j["custom_fog_color"] = {
                Config::custom_fog_color.x, Config::custom_fog_color.y,
                Config::custom_fog_color.z, Config::custom_fog_color.w
            };
            j["custom_fog_start"] = Config::custom_fog_start;
            j["custom_fog_end"] = Config::custom_fog_end;
            j["custom_fog_falloff"] = Config::custom_fog_falloff;
            j["trigger_magnet"] = Config::trigger_magnet;
            j["trigger_magnet_smooth"] = Config::trigger_magnet_smooth;
            j["trigger_magnet_fov"] = Config::trigger_magnet_fov;
            j["trigger_magnet_silent"] = Config::trigger_magnet_silent;
            j["trigger_magnet_lead"] = Config::trigger_magnet_lead;
            j["trigger_magnet_head_prio"] = Config::trigger_magnet_head_prio;
            j["trigger_magnet_only_ready"] = Config::trigger_magnet_only_ready;
            j["trigger_magnet_deadzone"] = Config::trigger_magnet_deadzone;
            {
                nlohmann::json mhb = nlohmann::json::array();
                for (int i = 0; i < Config::HB_COUNT; ++i)
                    mhb.push_back(Config::trigger_magnet_hitboxes[i]);
                j["trigger_magnet_hitboxes"] = mhb;
            }
            j["weather"] = Config::weather;
            j["weather_mode"] = Config::weather_mode;
            j["weather_intensity"] = Config::weather_intensity;

            j["flag_flashed"] = Config::flag_flashed;
            j["flag_bomb"] = Config::flag_bomb;
            j["flag_scoped"] = Config::flag_scoped;
            j["flag_reloading"] = Config::flag_reloading;
            j["flag_defusing"] = Config::flag_defusing;
            j["flag_money"] = Config::flag_money;
            j["flag_kit"] = Config::flag_kit;
            j["flag_helmet"] = Config::flag_helmet;
            j["flag_nades"] = Config::flag_nades;
            j["esp_rank"] = Config::esp_rank;
            j["esp_3d_box"] = Config::esp_3d_box;
            j["esp_oof"] = Config::esp_oof;
            j["esp_oof_radius"] = Config::esp_oof_radius;
            j["esp_oof_size"] = Config::esp_oof_size;
            j["esp_oof_color"] = { Config::esp_oof_color.x, Config::esp_oof_color.y, Config::esp_oof_color.z, Config::esp_oof_color.w };
            j["esp_3d_box_color"] = { Config::esp_3d_box_color.x, Config::esp_3d_box_color.y, Config::esp_3d_box_color.z, Config::esp_3d_box_color.w };
            j["esp_rank_color"] = { Config::esp_rank_color.x, Config::esp_rank_color.y, Config::esp_rank_color.z, Config::esp_rank_color.w };
            j["float_damage"] = Config::float_damage;
            j["float_damage_duration"] = Config::float_damage_duration;
            j["float_damage_speed"] = Config::float_damage_speed;
            j["float_damage_color"] = { Config::float_damage_color.x, Config::float_damage_color.y, Config::float_damage_color.z, Config::float_damage_color.w };
            j["float_damage_head_color"] = { Config::float_damage_head_color.x, Config::float_damage_head_color.y, Config::float_damage_head_color.z, Config::float_damage_head_color.w };
            j["float_damage_kill_color"] = { Config::float_damage_kill_color.x, Config::float_damage_kill_color.y, Config::float_damage_kill_color.z, Config::float_damage_kill_color.w };

            j["world_esp_weapons"] = Config::world_esp_weapons;
            j["world_esp_bomb"] = Config::world_esp_bomb;
            j["world_esp_smoke"] = Config::world_esp_smoke;
            j["world_esp_molotov"] = Config::world_esp_molotov;
            j["world_esp_he"] = Config::world_esp_he;
            j["world_esp_flash"] = Config::world_esp_flash;
            j["world_esp_decoy"] = Config::world_esp_decoy;
            j["world_esp_distance"] = Config::world_esp_distance;
            j["tracers"] = Config::tracers;
            j["tracers_style"] = Config::tracers_style;
            j["tracers_duration"] = Config::tracers_duration;
            j["tracers_thickness"] = Config::tracers_thickness;
            j["tracers_color"] = {
                Config::tracers_color.x, Config::tracers_color.y,
                Config::tracers_color.z, Config::tracers_color.w
            };
            j["world_esp_weapon_color"] = {
                Config::world_esp_weapon_color.x, Config::world_esp_weapon_color.y,
                Config::world_esp_weapon_color.z, Config::world_esp_weapon_color.w
            };
            j["world_esp_bomb_color"] = {
                Config::world_esp_bomb_color.x, Config::world_esp_bomb_color.y,
                Config::world_esp_bomb_color.z, Config::world_esp_bomb_color.w
            };
            j["world_esp_smoke_color"] = {
                Config::world_esp_smoke_color.x, Config::world_esp_smoke_color.y,
                Config::world_esp_smoke_color.z, Config::world_esp_smoke_color.w
            };
            j["world_esp_molotov_color"] = {
                Config::world_esp_molotov_color.x, Config::world_esp_molotov_color.y,
                Config::world_esp_molotov_color.z, Config::world_esp_molotov_color.w
            };
            j["world_esp_he_color"] = {
                Config::world_esp_he_color.x, Config::world_esp_he_color.y,
                Config::world_esp_he_color.z, Config::world_esp_he_color.w
            };
            j["world_esp_flash_color"] = {
                Config::world_esp_flash_color.x, Config::world_esp_flash_color.y,
                Config::world_esp_flash_color.z, Config::world_esp_flash_color.w
            };
            j["world_esp_decoy_color"] = {
                Config::world_esp_decoy_color.x, Config::world_esp_decoy_color.y,
                Config::world_esp_decoy_color.z, Config::world_esp_decoy_color.w
            };

            j["nade_pred"] = Config::nade_pred;
            j["nade_pred_local"] = Config::nade_pred_local;
            j["nade_pred_local_only_pin"] = Config::nade_pred_local_only_pin;
            j["nade_pred_projectiles"] = Config::nade_pred_projectiles;
            j["nade_pred_radius"] = Config::nade_pred_radius;
            j["nade_pred_thickness"] = Config::nade_pred_thickness;
            j["nade_pred_color"] = {
                Config::nade_pred_color.x, Config::nade_pred_color.y,
                Config::nade_pred_color.z, Config::nade_pred_color.w
            };
            j["nade_pred_local_color"] = {
                Config::nade_pred_local_color.x, Config::nade_pred_local_color.y,
                Config::nade_pred_local_color.z, Config::nade_pred_local_color.w
            };
            j["nade_pred_land_color"] = {
                Config::nade_pred_land_color.x, Config::nade_pred_land_color.y,
                Config::nade_pred_land_color.z, Config::nade_pred_land_color.w
            };
            j["nade_warn"] = Config::nade_warn;
            j["nade_warn_only_near"] = Config::nade_warn_only_near;
            j["nade_warn_range"] = Config::nade_warn_range;
            j["nade_warn_icon_size"] = Config::nade_warn_icon_size;
            j["nade_warn_color"] = {
                Config::nade_warn_color.x, Config::nade_warn_color.y,
                Config::nade_warn_color.z, Config::nade_warn_color.w
            };
            j["nade_pred_damage"] = Config::nade_pred_damage;
            j["nade_pred_damage_color"] = {
                Config::nade_pred_damage_color.x, Config::nade_pred_damage_color.y,
                Config::nade_pred_damage_color.z, Config::nade_pred_damage_color.w
            };
            j["nade_pred_damage_lethal_color"] = {
                Config::nade_pred_damage_lethal_color.x, Config::nade_pred_damage_lethal_color.y,
                Config::nade_pred_damage_lethal_color.z, Config::nade_pred_damage_lethal_color.w
            };
            j["nade_lineup"] = Config::nade_lineup;
            j["nade_lineup_only_held"] = Config::nade_lineup_only_held;
            j["nade_lineup_stand_dist"] = Config::nade_lineup_stand_dist;
            j["nade_lineup_aim_dist"] = Config::nade_lineup_aim_dist;
            j["nade_lineup_select_dist"] = Config::nade_lineup_select_dist;
            j["nade_lineup_color"] = {
                Config::nade_lineup_color.x, Config::nade_lineup_color.y,
                Config::nade_lineup_color.z, Config::nade_lineup_color.w
            };
            j["nade_lineup_aim_color"] = {
                Config::nade_lineup_aim_color.x, Config::nade_lineup_aim_color.y,
                Config::nade_lineup_aim_color.z, Config::nade_lineup_aim_color.w
            };
            j["nade_lineup_capture"] = Config::nade_lineup_capture;
            j["nade_lineup_capture_key"] = keybind.getKey(Config::nade_lineup_capture);
            j["nade_lineup_capture_key_mode"] = keybind.getMode(Config::nade_lineup_capture);
            j["nade_lineup_capture_throw"] = Config::nade_lineup_capture_throw;
            j["nade_lineup_capture_kind"] = Config::nade_lineup_capture_kind;
            j["nade_lineup_capture_name"] = Config::nade_lineup_capture_name;

            j["armChams"] = Config::armChams;
            j["viewmodelChams"] = Config::viewmodelChams;
            j["colArmChams"] = {
                Config::colArmChams.x, Config::colArmChams.y,
                Config::colArmChams.z, Config::colArmChams.w
            };
            j["colViewmodelChams"] = {
                Config::colViewmodelChams.x, Config::colViewmodelChams.y,
                Config::colViewmodelChams.z, Config::colViewmodelChams.w
            };

            j["custom_paint_color"] = Config::custom_paint_color;
            j["custom_color0"] = { Config::custom_color0.x, Config::custom_color0.y, Config::custom_color0.z, Config::custom_color0.w };
            j["custom_color1"] = { Config::custom_color1.x, Config::custom_color1.y, Config::custom_color1.z, Config::custom_color1.w };
            j["custom_color2"] = { Config::custom_color2.x, Config::custom_color2.y, Config::custom_color2.z, Config::custom_color2.w };
            j["custom_color3"] = { Config::custom_color3.x, Config::custom_color3.y, Config::custom_color3.z, Config::custom_color3.w };
            j["knife_changer"] = Config::knife_changer;
            j["knife_index"] = Config::knife_index;
            j["knife_paint_kit"] = Config::knife_paint_kit;
            j["knife_paint_kit_id"] = Config::knife_paint_kit_id;
            j["knife_wear"] = Config::knife_wear;
            j["knife_seed"] = Config::knife_seed;
            j["knife_custom_name"] = Config::knife_custom_name;
            j["knife_custom_color"] = Config::knife_custom_color;
            if (Config::knife_custom_color) {
                nlohmann::json cols = nlohmann::json::array();
                for (int ci = 0; ci < 16; ++ci) cols.push_back(Config::knife_colors[ci]);
                j["knife_colors"] = cols;
            }

            j["glove_changer"] = Config::glove_changer;
            j["glove_index"] = Config::glove_index;
            j["glove_paint_kit"] = Config::glove_paint_kit;
            j["glove_paint_kit_id"] = Config::glove_paint_kit_id;
            j["glove_wear"] = Config::glove_wear;
            j["glove_seed"] = Config::glove_seed;
            j["glove_custom_color"] = Config::glove_custom_color;
            if (Config::glove_custom_color) {
                nlohmann::json cols = nlohmann::json::array();
                for (int ci = 0; ci < 16; ++ci) cols.push_back(Config::glove_colors[ci]);
                j["glove_colors"] = cols;
            }

            j["weapon_skins"] = Config::weapon_skins;
            j["weapon_selected"] = Config::weapon_selected;
            {
                nlohmann::json ws = nlohmann::json::object();
                for (int i = 1; i <= 70; ++i) {
                    if (Config::weapon_skin[i].paint_kit_id <= 0 && Config::weapon_skin[i].paint_kit <= 0)
                        continue;
                    nlohmann::json ws_entry = {
                        {"paint_kit", Config::weapon_skin[i].paint_kit},
                        {"paint_kit_id", Config::weapon_skin[i].paint_kit_id},
                        {"wear", Config::weapon_skin[i].wear},
                        {"seed", Config::weapon_skin[i].seed}
                    };
                    if (Config::weapon_skin[i].custom_color) {
                        ws_entry["custom_color"] = true;
                        nlohmann::json cols = nlohmann::json::array();
                        for (int ci = 0; ci < 16; ++ci)
                            cols.push_back(Config::weapon_skin[i].colors[ci]);
                        ws_entry["colors"] = cols;
                    }
                    ws[std::to_string(i)] = ws_entry;
                }
                j["weapon_skin"] = ws;
            }

            j["agent_changer"] = Config::agent_changer;
            j["agent_ct_def"] = Config::agent_ct_def;
            j["agent_t_def"] = Config::agent_t_def;
            j["custom_model"] = Config::custom_model;
            j["custom_model_path"] = Config::custom_model_path;
            j["custom_knife"] = Config::custom_knife;
            j["custom_knife_path"] = Config::custom_knife_path;

            j["aimbot"] = Config::aimbot;
            j["knifebot"] = Config::knifebot;
            j["knifebot_prefer_stab"] = Config::knifebot_prefer_stab;
            j["knifebot_key"] = keybind.getKey(Config::knifebot);
            j["knifebot_key_mode"] = keybind.getMode(Config::knifebot);
            j["auto_pistol"] = Config::auto_pistol;
            j["auto_pistol_delay_ms"] = Config::auto_pistol_delay_ms;
            j["enemy_spectate"] = Config::enemy_spectate;
            j["enemy_spectate_thirdperson"] = Config::enemy_spectate_thirdperson;
            j["auto_defuse"] = Config::auto_defuse;
            j["weapon_group_ui"] = Config::weapon_group_ui;
            // Flat keys kept for backwards compat (= live / active group snapshot)
            j["aimbot_fov"] = Config::aimbot_fov;
            j["aimbot_smooth"] = Config::aimbot_smooth;
            j["team_check"] = Config::team_check;
            j["team_check_auto"] = Config::team_check_auto;
            j["aim_vis_check"] = Config::aim_vis_check;
            j["aim_reaction_delay_ms"] = Config::aim_reaction_delay_ms;
            j["aim_target_switch_delay_ms"] = Config::aim_target_switch_delay_ms;
            j["aim_first_shot_delay_ms"] = Config::aim_first_shot_delay_ms;
            {
                nlohmann::json hb = nlohmann::json::array();
                for (int i = 0; i < Config::HB_COUNT; ++i)
                    hb.push_back(Config::aim_hitboxes[i]);
                j["aim_hitboxes"] = hb;
            }
            j["antiflash"] = Config::antiflash_amount > 0.01f;
            j["antiflash_amount"] = Config::antiflash_amount;
            j["remove_legs"] = Config::remove_legs;
            j["remove_smoke"] = Config::remove_smoke;
            j["remove_decals"] = Config::remove_decals;
            j["remove_particles"] = Config::remove_particles;
            j["smoke_color"] = Config::smoke_color;
            j["smoke_color_value"] = {
                Config::smoke_color_value.x, Config::smoke_color_value.y,
                Config::smoke_color_value.z, Config::smoke_color_value.w
            };
            j["fire_color"] = Config::fire_color;
            j["fire_color_value"] = {
                Config::fire_color_value.x, Config::fire_color_value.y,
                Config::fire_color_value.z, Config::fire_color_value.w
            };
            j["explosion_color"] = Config::explosion_color;
            j["explosion_color_value"] = {
                Config::explosion_color_value.x, Config::explosion_color_value.y,
                Config::explosion_color_value.z, Config::explosion_color_value.w
            };
            j["remove_crosshair"] = Config::remove_crosshair;
            j["force_crosshair"] = Config::force_crosshair;
            j["remove_hud"] = Config::remove_hud;
            j["remove_postprocess"] = Config::remove_postprocess;
            j["remove_recoil"] = Config::remove_recoil;
            j["scope_custom_lines"] = Config::scope_custom_lines;
            j["scope_line_size"] = Config::scope_line_size;
            j["scope_line_gap"] = Config::scope_line_gap;
            j["scope_line_thickness"] = Config::scope_line_thickness;
            j["scope_line_color"] = {
                Config::scope_line_color.x, Config::scope_line_color.y,
                Config::scope_line_color.z, Config::scope_line_color.w
            };
            j["scope_zoom_fov"] = Config::scope_zoom_fov;
            j["scope_fov_1"] = Config::scope_fov_1;
            j["scope_fov_2"] = Config::scope_fov_2;
            j["scope_hide_viewmodel"] = Config::scope_hide_viewmodel;
            j["rcs"] = Config::rcs;
            j["rcs_standalone"] = Config::rcs_standalone;
            j["rcs_scale_x"] = Config::rcs_scale_x;
            j["rcs_scale_y"] = Config::rcs_scale_y;
            j["aimbot_key"] = keybind.getKey(Config::aimbot);
            j["aimbot_key_mode"] = keybind.getMode(Config::aimbot);
            j["autofire"] = Config::autofire;
            j["autofire_silent"] = Config::autofire_silent;
            j["autofire_fov"] = Config::autofire_fov;
            j["autofire_hitchance"] = Config::autofire_hitchance;
            j["autofire_mode"] = Config::autofire_mode;
            j["autofire_autostop"] = Config::autofire_autostop;
            j["autofire_autoscope"] = Config::autofire_autoscope;
            j["autofire_scoped_only"] = Config::autofire_scoped_only;
            j["autofire_autowall"] = Config::autofire_autowall;
            j["autowall_key"] = keybind.getKey(Config::autowall);
            j["autowall_key_mode"] = keybind.getMode(Config::autowall);
            j["autofire_mindamage"] = Config::autofire_mindamage;
            j["autofire_mindamage_aw"] = Config::autofire_mindamage_aw;
            j["autofire_target_select"] = Config::autofire_target_select;
            j["autofire_vis_check"] = Config::autofire_vis_check;
            j["autofire_flash_check"] = Config::autofire_flash_check;
            j["autofire_smoke_check"] = Config::autofire_smoke_check;
            j["autofire_focus_target"] = Config::autofire_focus_target;
            j["autofire_multipoint_dynamic"] = Config::autofire_multipoint_dynamic;
            j["autofire_body_if_lethal"] = Config::autofire_body_if_lethal;
            j["autofire_prefer_body"] = Config::autofire_prefer_body;
            {
                nlohmann::json afHb = nlohmann::json::array();
                nlohmann::json afMp = nlohmann::json::array();
                nlohmann::json afScale = nlohmann::json::array();
                for (int i = 0; i < Config::HB_COUNT; ++i) {
                    afHb.push_back(Config::autofire_hitboxes[i]);
                    afMp.push_back(Config::autofire_multipoint[i]);
                    afScale.push_back(Config::autofire_multipoint_scale[i]);
                }
                j["autofire_hitboxes"] = afHb;
                j["autofire_multipoint"] = afMp;
                j["autofire_multipoint_scale"] = afScale;
            }
            j["autofire_key"] = keybind.getKey(Config::autofire);
            j["autofire_key_mode"] = keybind.getMode(Config::autofire);
            j["triggerbot"] = Config::triggerbot;
            j["triggerbot_key"] = keybind.getKey(Config::triggerbot);
            j["triggerbot_key_mode"] = keybind.getMode(Config::triggerbot);
            // Per weapon-group profiles
            {
                nlohmann::json groups = nlohmann::json::array();
                for (int g = 0; g < Config::WG_COUNT; ++g) {
                    const auto& p = Config::weapon_profiles[g];
                    nlohmann::json jp;
                    jp["fov"] = p.aimbot_fov;
                    jp["smooth"] = p.aimbot_smooth;
                    jp["humanize"] = p.aimbot_humanize;
                    jp["smooth_mode"] = p.aimbot_smooth_mode;
                    jp["vis_check"] = p.aim_vis_check;
                    jp["aim_smoke"] = p.aim_smoke_check;
                    jp["aim_flash"] = p.aim_flash_check;
                    jp["aim_scoped"] = p.aim_scoped_only;
                    jp["reaction_ms"] = p.aim_reaction_delay_ms;
                    jp["switch_ms"] = p.aim_target_switch_delay_ms;
                    jp["first_shot_ms"] = p.aim_first_shot_delay_ms;
                    jp["rcs"] = p.rcs;
                    jp["rcs_standalone"] = p.rcs_standalone;
                    jp["rcs_x"] = p.rcs_scale_x;
                    jp["rcs_y"] = p.rcs_scale_y;
                    jp["af_fov"] = p.autofire_fov;
                    jp["af_hc"] = p.autofire_hitchance;
                    jp["af_mode"] = p.autofire_mode;
                    jp["af_autostop"] = p.autofire_autostop;
                    jp["af_autoscope"] = p.autofire_autoscope;
                    jp["af_autowall"] = p.autofire_autowall;
                    jp["af_mindmg"] = p.autofire_mindamage;
                    jp["af_mindmg_aw"] = p.autofire_mindamage_aw;
                    jp["af_target"] = p.autofire_target_select;
                    jp["af_vis"] = p.autofire_vis_check;
                    jp["af_flash"] = p.autofire_flash_check;
                    jp["af_smoke"] = p.autofire_smoke_check;
                    jp["af_focus"] = p.autofire_focus_target;
                    jp["af_mp_dyn"] = p.autofire_multipoint_dynamic;
                    jp["af_body_lethal"] = p.autofire_body_if_lethal;
                    jp["af_prefer_body"] = p.autofire_prefer_body;
                    jp["tr_delay"] = p.trigger_delay_ms;
                    jp["tr_hc"] = p.trigger_hitchance;
                    jp["tr_aw"] = p.trigger_autowall;
                    jp["tr_mindmg"] = p.trigger_mindamage;
                    jp["tr_mindmg_aw"] = p.trigger_mindamage_aw;
                    jp["tr_flash"] = p.trigger_flash_check;
                    jp["tr_smoke"] = p.trigger_smoke_check;
                    jp["tr_autostop"] = p.trigger_autostop;
                    jp["tr_mode"] = p.trigger_mode;
                    jp["tr_magnet"] = p.trigger_magnet;
                    jp["tr_magnet_smooth"] = p.trigger_magnet_smooth;
                    jp["tr_magnet_fov"] = p.trigger_magnet_fov;
                    jp["tr_magnet_silent"] = p.trigger_magnet_silent;
                    jp["tr_magnet_lead"] = p.trigger_magnet_lead;
                    jp["tr_magnet_head_prio"] = p.trigger_magnet_head_prio;
                    jp["tr_magnet_only_ready"] = p.trigger_magnet_only_ready;
                    jp["tr_magnet_deadzone"] = p.trigger_magnet_deadzone;
                    {
                        nlohmann::json mhb = nlohmann::json::array();
                        for (int i = 0; i < Config::HB_COUNT; ++i)
                            mhb.push_back(p.trigger_magnet_hitboxes[i]);
                        jp["tr_magnet_hitboxes"] = mhb;
                    }
                    // Scope: single source (aim_scoped); af/tr mirror for old readers
                    jp["af_scoped"] = p.aim_scoped_only;
                    jp["tr_scoped"] = p.aim_scoped_only;
                    nlohmann::json trHb = nlohmann::json::array();
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        trHb.push_back(p.trigger_hitboxes[i]);
                    jp["tr_hitboxes"] = trHb;
                    nlohmann::json hb = nlohmann::json::array();
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        hb.push_back(p.aim_hitboxes[i]);
                    jp["hitboxes"] = hb;
                    nlohmann::json afHb = nlohmann::json::array();
                    nlohmann::json afMp = nlohmann::json::array();
                    nlohmann::json afSc = nlohmann::json::array();
                    for (int i = 0; i < Config::HB_COUNT; ++i) {
                        afHb.push_back(p.autofire_hitboxes[i]);
                        afMp.push_back(p.autofire_multipoint[i]);
                        afSc.push_back(p.autofire_multipoint_scale[i]);
                    }
                    jp["af_hitboxes"] = afHb;
                    jp["af_multipoint"] = afMp;
                    jp["af_mp_scale"] = afSc;
                    groups.push_back(jp);
                }
                j["weapon_profiles"] = groups;
            }
            j["fov_circle"] = Config::fov_circle;
            j["fov_circle_autofire"] = Config::fov_circle_autofire;
            j["fov_circle_magnet"] = Config::fov_circle_magnet;
            j["fovCircleColor"] = {
                Config::fovCircleColor.x, Config::fovCircleColor.y,
                Config::fovCircleColor.z, Config::fovCircleColor.w
            };
            j["fovCircleColorAf"] = {
                Config::fovCircleColorAf.x, Config::fovCircleColorAf.y,
                Config::fovCircleColorAf.z, Config::fovCircleColorAf.w
            };
            j["fovCircleColorMagnet"] = {
                Config::fovCircleColorMagnet.x, Config::fovCircleColorMagnet.y,
                Config::fovCircleColorMagnet.z, Config::fovCircleColorMagnet.w
            };
            j["watermark"] = Config::watermark;
            j["hud_keybind_strip"] = Config::hud_keybind_strip;
            j["menu_accent"] = {
                Config::menu_accent.x, Config::menu_accent.y,
                Config::menu_accent.z, Config::menu_accent.w
            };
            j["menu_bg"] = {
                Config::menu_bg.x, Config::menu_bg.y,
                Config::menu_bg.z, Config::menu_bg.w
            };
            j["menu_child_bg"] = {
                Config::menu_child_bg.x, Config::menu_child_bg.y,
                Config::menu_child_bg.z, Config::menu_child_bg.w
            };
            j["menu_sidebar_bg"] = {
                Config::menu_sidebar_bg.x, Config::menu_sidebar_bg.y,
                Config::menu_sidebar_bg.z, Config::menu_sidebar_bg.w
            };
            j["menu_border"] = {
                Config::menu_border.x, Config::menu_border.y,
                Config::menu_border.z, Config::menu_border.w
            };
            j["menu_text"] = {
                Config::menu_text.x, Config::menu_text.y,
                Config::menu_text.z, Config::menu_text.w
            };
            j["menu_text_muted"] = {
                Config::menu_text_muted.x, Config::menu_text_muted.y,
                Config::menu_text_muted.z, Config::menu_text_muted.w
            };
            j["menu_rounding"] = Config::menu_rounding;
            j["menu_opacity"] = Config::menu_opacity;
            j["menu_compact"] = Config::menu_compact;
            j["menu_dpi_scale"] = Config::menu_dpi_scale;
            j["widget_keybinds"] = Config::widget_keybinds;
            j["widget_bomb"] = Config::widget_bomb;
            j["widget_spectators"] = Config::widget_spectators;
            j["widget_keybinds_pos"] = { Config::widget_keybinds_pos.x, Config::widget_keybinds_pos.y };
            j["widget_bomb_pos"] = { Config::widget_bomb_pos.x, Config::widget_bomb_pos.y };
            j["widget_spectators_pos"] = { Config::widget_spectators_pos.x, Config::widget_spectators_pos.y };
            j["widget_keybinds_only_when_active"] = Config::widget_keybinds_only_when_active;
            j["widget_keybinds_show_all"] = Config::widget_keybinds_show_all;
            j["widget_keybinds_accent"] = {
                Config::widget_keybinds_accent.x, Config::widget_keybinds_accent.y,
                Config::widget_keybinds_accent.z, Config::widget_keybinds_accent.w
            };
            j["widget_bomb_accent"] = {
                Config::widget_bomb_accent.x, Config::widget_bomb_accent.y,
                Config::widget_bomb_accent.z, Config::widget_bomb_accent.w
            };
            j["widget_bomb_urgent"] = {
                Config::widget_bomb_urgent.x, Config::widget_bomb_urgent.y,
                Config::widget_bomb_urgent.z, Config::widget_bomb_urgent.w
            };
            j["widget_bomb_show_damage"] = Config::widget_bomb_show_damage;
            j["widget_bomb_show_defuse"] = Config::widget_bomb_show_defuse;
            j["widget_spectators_accent"] = {
                Config::widget_spectators_accent.x, Config::widget_spectators_accent.y,
                Config::widget_spectators_accent.z, Config::widget_spectators_accent.w
            };
            j["widget_spectators_show_avatars"] = Config::widget_spectators_show_avatars;
            j["widget_spectators_max"] = Config::widget_spectators_max;
            j["widget_radar"] = Config::widget_radar;
            j["widget_radar_pos"] = { Config::widget_radar_pos.x, Config::widget_radar_pos.y };
            j["widget_radar_size"] = Config::widget_radar_size;
            j["widget_radar_shape"] = Config::widget_radar_shape;
            j["widget_radar_accent"] = {
                Config::widget_radar_accent.x, Config::widget_radar_accent.y,
                Config::widget_radar_accent.z, Config::widget_radar_accent.w
            };
            j["bhop"] = Config::bhop;
            j["autostrafe"] = Config::autostrafe;
            j["autostrafe_mode"] = Config::autostrafe_mode;
            j["jumpbug"] = Config::jumpbug;
            j["jumpbug_key"] = keybind.getKey(Config::jumpbug);
            j["jumpbug_key_mode"] = keybind.getMode(Config::jumpbug);
            j["edgejump"] = Config::edgejump;
            j["edgejump_key"] = keybind.getKey(Config::edgejump);
            j["edgejump_key_mode"] = keybind.getMode(Config::edgejump);
            j["edgebug"] = Config::edgebug;
            j["edgebug_key"] = keybind.getKey(Config::edgebug);
            j["edgebug_key_mode"] = keybind.getMode(Config::edgebug);
            j["backtrack"] = Config::backtrack;
            j["backtrack_ms"] = Config::backtrack_ms;
            j["backtrack_skeleton"] = Config::backtrack_skeleton;
            j["backtrack_color"] = {
                Config::backtrack_color.x, Config::backtrack_color.y,
                Config::backtrack_color.z, Config::backtrack_color.w
            };
            // legacy mirrors for older loaders
            j["autofire_backtrack"] = Config::backtrack;
            j["autofire_backtrack_ms"] = Config::backtrack_ms;
            j["trigger_backtrack"] = Config::backtrack;
            j["trigger_backtrack_ms"] = Config::backtrack_ms;
            j["hitlog"] = Config::hitlog;
            j["hitlog_console"] = Config::hitlog_console;
            j["hitlog_duration"] = Config::hitlog_duration;
            j["hitlog_pos"] = { Config::hitlog_pos.x, Config::hitlog_pos.y };
            j["hitlog_width"] = Config::hitlog_width;
            j["hitlog_max_rows"] = Config::hitlog_max_rows;
            j["hitlog_show_hp"] = Config::hitlog_show_hp;
            j["hitlog_show_stats"] = Config::hitlog_show_stats;
            j["hitlog_color"] = {
                Config::hitlog_color.x, Config::hitlog_color.y,
                Config::hitlog_color.z, Config::hitlog_color.w
            };
            j["hitlog_head_color"] = {
                Config::hitlog_head_color.x, Config::hitlog_head_color.y,
                Config::hitlog_head_color.z, Config::hitlog_head_color.w
            };
            j["hitlog_kill_color"] = {
                Config::hitlog_kill_color.x, Config::hitlog_kill_color.y,
                Config::hitlog_kill_color.z, Config::hitlog_kill_color.w
            };
            j["auto_accept"] = Config::auto_accept;
            j["sound_esp"] = Config::sound_esp;
            j["sound_esp_duration"] = Config::sound_esp_duration;
            j["sound_esp_ring_size"] = Config::sound_esp_ring_size;
            j["sound_esp_color"] = {
                Config::sound_esp_color.x, Config::sound_esp_color.y,
                Config::sound_esp_color.z, Config::sound_esp_color.w
            };
            j["vote_reveal"] = Config::vote_reveal;
            j["vote_auto"] = Config::vote_auto;
            j["vote_auto_choice"] = Config::vote_auto_choice;
            j["vote_auto_delay_ms"] = Config::vote_auto_delay_ms;
            j["hitmarker"] = Config::hitmarker;
            j["hitmarker_screen"] = Config::hitmarker_screen;
            j["hitmarker_world"] = Config::hitmarker_world;
            j["hitmarker_show_damage"] = Config::hitmarker_show_damage;
            j["hitmarker_size"] = Config::hitmarker_size;
            j["hitmarker_gap"] = Config::hitmarker_gap;
            j["hitmarker_thickness"] = Config::hitmarker_thickness;
            j["hitmarker_world_size"] = Config::hitmarker_world_size;
            j["hitmarker_duration"] = Config::hitmarker_duration;
            j["hitmarker_color"] = {
                Config::hitmarker_color.x, Config::hitmarker_color.y,
                Config::hitmarker_color.z, Config::hitmarker_color.w
            };
            j["hitmarker_head_color"] = {
                Config::hitmarker_head_color.x, Config::hitmarker_head_color.y,
                Config::hitmarker_head_color.z, Config::hitmarker_head_color.w
            };
            j["hitmarker_kill_color"] = {
                Config::hitmarker_kill_color.x, Config::hitmarker_kill_color.y,
                Config::hitmarker_kill_color.z, Config::hitmarker_kill_color.w
            };
            j["hitsound"] = Config::hitsound;
            j["hitsound_file"] = Config::hitsound_file;
            j["hitsound_head"] = Config::hitsound_head;
            j["hitsound_kill"] = Config::hitsound_kill;

            j["enemyChamsInvisible"] = Config::enemyChamsInvisible;
            j["enemyChams"] = Config::enemyChams;
            j["teamChams"] = Config::teamChams;
            j["teamChamsInvisible"] = Config::teamChamsInvisible;
            j["chamsMaterial"] = Config::chamsMaterial;
            j["chamsMaterialXQZ"] = Config::chamsMaterialXQZ;
            j["armChamsMaterial"] = Config::armChamsMaterial;
            j["viewmodelChamsMaterial"] = Config::viewmodelChamsMaterial;
            j["localChams"] = Config::localChams;
            j["localChamsMaterial"] = Config::localChamsMaterial;
            j["colLocalChams"] = { Config::colLocalChams.x, Config::colLocalChams.y, Config::colLocalChams.z, Config::colLocalChams.w };
            j["ragdollChams"] = Config::ragdollChams;
            j["ragdollChamsMaterial"] = Config::ragdollChamsMaterial;
            j["colRagdollChams"] = { Config::colRagdollChams.x, Config::colRagdollChams.y, Config::colRagdollChams.z, Config::colRagdollChams.w };
            j["colVisualChams"] = {
                Config::colVisualChams.x,
                Config::colVisualChams.y,
                Config::colVisualChams.z,
                Config::colVisualChams.w
            };
            j["colVisualChamsIgnoreZ"] = {
                Config::colVisualChamsIgnoreZ.x,
                Config::colVisualChamsIgnoreZ.y,
                Config::colVisualChamsIgnoreZ.z,
                Config::colVisualChamsIgnoreZ.w
            };
            j["teamcolVisualChamsIgnoreZ"] = {
                Config::teamcolVisualChamsIgnoreZ.x,
                Config::teamcolVisualChamsIgnoreZ.y,
                Config::teamcolVisualChamsIgnoreZ.z,
                Config::teamcolVisualChamsIgnoreZ.w
            };
            j["teamcolVisualChams"] = {
                Config::teamcolVisualChams.x,
                Config::teamcolVisualChams.y,
                Config::teamcolVisualChams.z,
                Config::teamcolVisualChams.w
            };

            auto filePath = GetConfigPath(name);
            std::error_code ec;
            std::filesystem::create_directories(filePath.parent_path(), ec);

            std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
            if (!ofs.is_open())
                return false;

            ofs << j.dump(4);
            ofs.flush();
            const bool ok = ofs.good();
            ofs.close();
            return ok;
        }

        static bool Load(const std::string& configName)
        {
            const std::string name = SanitizeName(configName);
            if (name.empty())
                return false;

            auto filePath = GetConfigPath(name);
            if (!std::filesystem::exists(filePath))
                return false;

            std::ifstream ifs(filePath);
            if (!ifs.is_open())
                return false;

            nlohmann::json j;
            try {
                ifs >> j;
            } catch (...) {
                return false;
            }

            // Pause World/Glow/Skin while we Reset+apply — otherwise Night/knife
            // briefly false mid-load races FSN restore / RunApply → crash.
            Config::loading.store(true, std::memory_order_release);

            try {
            // Hard reset first: any feature not present (or false) in JSON stays off.
            // Never leave previous config's enabled flags sticky.
            Config::ResetToDefaults();
            keybind.clearAllToggles();

            Config::esp = j.value("esp", false);
            Config::showHealth = j.value("showHealth", false);
            Config::showArmor = j.value("showArmor", false);
            Config::showDistance = j.value("showDistance", false);
            Config::showWeapon = j.value("showWeapon", false);
            Config::showWeaponIcon = j.value("showWeaponIcon", false);
            Config::showNameTags = j.value("showNameTags", false);
            Config::esp_name_avatar = j.value("esp_name_avatar", false);
            Config::esp_skeleton = j.value("esp_skeleton", false);
            Config::esp_skeleton_thickness = j.value("esp_skeleton_thickness", 1.5f);
            Config::teamCheck = j.value("teamCheck", false);
            Config::espFill = j.value("espFill", false);
            Config::espThickness = j.value("espThickness", 1.0f);
            Config::espFillOpacity = j.value("espFillOpacity", 0.5f);
            Config::esp_box_style = j.value("esp_box_style", 0);
            Config::esp_box_width = j.value("esp_box_width", 0.42f);
            Config::esp_bar_width = j.value("esp_bar_width", 3.0f);
            Config::esp_health_auto = j.value("esp_health_auto", true);

            Config::fovEnabled = j.value("fovEnabled", false);
            Config::fov = j.value("fov", 90.0f);
            Config::aspect_ratio_enabled = j.value("aspect_ratio_enabled", false);
            Config::aspect_ratio = j.value("aspect_ratio", 1.777778f);
            if (Config::aspect_ratio < 0.1f) Config::aspect_ratio = 0.1f;
            if (Config::aspect_ratio > 5.f) Config::aspect_ratio = 5.f;
            Config::viewmodel_changer = j.value("viewmodel_changer", false);
            Config::viewmodel_fov = j.value("viewmodel_fov", 68.f);
            Config::viewmodel_x = j.value("viewmodel_x", 0.f);
            Config::viewmodel_y = j.value("viewmodel_y", 0.f);
            Config::viewmodel_z = j.value("viewmodel_z", 0.f);
            Config::thirdperson = j.value("thirdperson", false);
            Config::thirdperson_distance = j.value("thirdperson_distance", 150.f);
            Config::thirdperson_key = j.value("thirdperson_key", 0x04);
            Config::thirdperson_key_mode = j.value("thirdperson_key_mode", 2);
            if (Config::thirdperson_key_mode < 0 || Config::thirdperson_key_mode > 2)
                Config::thirdperson_key_mode = 2;
            keybind.setKey(Config::thirdperson, Config::thirdperson_key);
            keybind.setMode(Config::thirdperson, Config::thirdperson_key_mode);

            auto loadCol = [&](const char* key, ImVec4& col) {
                if (!j.contains(key) || !j[key].is_array() || j[key].size() != 4)
                    return;
                auto& arr = j[key];
                float* c[4] = { &col.x, &col.y, &col.z, &col.w };
                for (int i = 0; i < 4; ++i) {
                    if (arr[i].is_number())
                        *c[i] = arr[i].get<float>();
                }
            };
            loadCol("espColor", Config::espColor);
            loadCol("espColorInvisible", Config::espColorInvisible);
            loadCol("esp_health_color", Config::esp_health_color);
            loadCol("esp_armor_color", Config::esp_armor_color);
            loadCol("esp_name_color", Config::esp_name_color);
            loadCol("esp_weapon_color", Config::esp_weapon_color);
            loadCol("esp_distance_color", Config::esp_distance_color);
            loadCol("esp_skeleton_color", Config::esp_skeleton_color);
            loadCol("esp_skeleton_color_invisible", Config::esp_skeleton_color_invisible);
            Config::esp_vis_check = j.value("esp_vis_check", true);
            Config::glow = j.value("glow", false);
            Config::glow_team = j.value("glow_team", true);
            Config::glow_enemy = j.value("glow_enemy", true);
            Config::glow_only_visible = j.value("glow_only_visible", false);
            loadCol("glow_color", Config::glow_color);
            loadCol("glow_color_invis", Config::glow_color_invis);
            Config::glow_world_weapons = j.value("glow_world_weapons", false);
            Config::glow_world_bomb = j.value("glow_world_bomb", false);
            Config::glow_world_grenades = j.value("glow_world_grenades", false);

            Config::Night = j.value("Night", false);
            Config::night_exposure = j.value("night_exposure", 0.45f);
            // migrate old NightColor.w if present
            if (j.contains("NightColor") && j["NightColor"].is_array() && j["NightColor"].size() == 4
                && !j.contains("night_exposure") && j["NightColor"][3].is_number())
                Config::night_exposure = j["NightColor"][3].get<float>();
            if (Config::night_exposure < 0.f) Config::night_exposure = 0.f;
            if (Config::night_exposure > 1.f) Config::night_exposure = 1.f;
            Config::skybox = j.value("skybox", false);
            loadCol("skybox_color", Config::skybox_color);
            Config::lighting = j.value("lighting", false);
            loadCol("lighting_color", Config::lighting_color);
            Config::map_color = j.value("map_color", false);
            loadCol("map_color_value", Config::map_color_value);
            Config::custom_fog = j.value("custom_fog", false);
            loadCol("custom_fog_color", Config::custom_fog_color);
            Config::custom_fog_start = j.value("custom_fog_start", 100.f);
            Config::custom_fog_end = j.value("custom_fog_end", 3000.f);
            Config::custom_fog_falloff = j.value("custom_fog_falloff", 1.f);
            Config::trigger_magnet = j.value("trigger_magnet", false);
            Config::trigger_magnet_smooth = j.value("trigger_magnet_smooth", 12.f);
            Config::trigger_magnet_fov = j.value("trigger_magnet_fov", 4.f);
            if (Config::trigger_magnet_fov < 0.5f) Config::trigger_magnet_fov = 0.5f;
            if (Config::trigger_magnet_fov > 30.f) Config::trigger_magnet_fov = 30.f;
            Config::trigger_magnet_silent = j.value("trigger_magnet_silent", false);
            Config::trigger_magnet_lead = j.value("trigger_magnet_lead", false);
            Config::trigger_magnet_head_prio = j.value("trigger_magnet_head_prio", true);
            Config::trigger_magnet_only_ready = j.value("trigger_magnet_only_ready", false);
            Config::trigger_magnet_deadzone = j.value("trigger_magnet_deadzone", 0.12f);
            if (Config::trigger_magnet_deadzone < 0.f) Config::trigger_magnet_deadzone = 0.f;
            if (Config::trigger_magnet_deadzone > 1.f) Config::trigger_magnet_deadzone = 1.f;
            for (int i = 0; i < Config::HB_COUNT; ++i)
                Config::trigger_magnet_hitboxes[i] = false;
            if (j.contains("trigger_magnet_hitboxes") && j["trigger_magnet_hitboxes"].is_array()) {
                auto& mhb = j["trigger_magnet_hitboxes"];
                for (int i = 0; i < Config::HB_COUNT && i < (int)mhb.size(); ++i) {
                    if (mhb[i].is_boolean())
                        Config::trigger_magnet_hitboxes[i] = mhb[i].get<bool>();
                    else if (mhb[i].is_number())
                        Config::trigger_magnet_hitboxes[i] = mhb[i].get<int>() != 0;
                }
            }
            Config::weather = j.value("weather", false);
            Config::weather_mode = j.value("weather_mode", 1);
            if (Config::weather_mode == 5) // legacy Storm → Rain
                Config::weather_mode = 4;
            if (Config::weather_mode < 0 || Config::weather_mode > 4)
                Config::weather_mode = 1;
            Config::weather_intensity = j.value("weather_intensity", 0.55f);
            if (Config::weather_intensity < 0.f) Config::weather_intensity = 0.f;
            if (Config::weather_intensity > 1.f) Config::weather_intensity = 1.f;

            Config::flag_flashed = j.value("flag_flashed", false);
            Config::flag_bomb = j.value("flag_bomb", false);
            Config::flag_scoped = j.value("flag_scoped", false);
            Config::flag_reloading = j.value("flag_reloading", false);
            Config::flag_defusing = j.value("flag_defusing", false);
            Config::flag_money = j.value("flag_money", false);
            Config::flag_kit = j.value("flag_kit", false);
            Config::flag_helmet = j.value("flag_helmet", false);
            Config::flag_nades = j.value("flag_nades", false);
            Config::esp_rank = j.value("esp_rank", false);
            Config::esp_3d_box = j.value("esp_3d_box", false);
            Config::esp_oof = j.value("esp_oof", false);
            Config::esp_oof_radius = j.value("esp_oof_radius", 280.f);
            Config::esp_oof_size = j.value("esp_oof_size", 14.f);
            loadCol("esp_oof_color", Config::esp_oof_color);
            loadCol("esp_3d_box_color", Config::esp_3d_box_color);
            loadCol("esp_rank_color", Config::esp_rank_color);
            Config::float_damage = j.value("float_damage", false);
            Config::float_damage_duration = j.value("float_damage_duration", 1.1f);
            Config::float_damage_speed = j.value("float_damage_speed", 55.f);
            loadCol("float_damage_color", Config::float_damage_color);
            loadCol("float_damage_head_color", Config::float_damage_head_color);
            loadCol("float_damage_kill_color", Config::float_damage_kill_color);

            Config::world_esp_weapons = j.value("world_esp_weapons", false);
            Config::world_esp_bomb = j.value("world_esp_bomb", true);
            Config::world_esp_smoke = j.value("world_esp_smoke", false);
            Config::world_esp_molotov = j.value("world_esp_molotov", false);
            Config::world_esp_he = j.value("world_esp_he", false);
            Config::world_esp_flash = j.value("world_esp_flash", false);
            Config::world_esp_decoy = j.value("world_esp_decoy", false);
            Config::world_esp_distance = j.value("world_esp_distance", false);
            Config::tracers = j.value("tracers", false);
            Config::tracers_style = j.value("tracers_style", 0);
            if (Config::tracers_style < 0) Config::tracers_style = 0;
            if (Config::tracers_style > 4) Config::tracers_style = 4;
            Config::tracers_duration = j.value("tracers_duration", 2.0f);
            if (Config::tracers_duration < 0.5f) Config::tracers_duration = 0.5f;
            if (Config::tracers_duration > 8.f) Config::tracers_duration = 8.f;
            Config::tracers_thickness = j.value("tracers_thickness", 2.2f);
            if (Config::tracers_thickness < 0.5f) Config::tracers_thickness = 0.5f;
            if (Config::tracers_thickness > 8.f) Config::tracers_thickness = 8.f;
            loadCol("tracers_color", Config::tracers_color);
            loadCol("world_esp_weapon_color", Config::world_esp_weapon_color);
            loadCol("world_esp_bomb_color", Config::world_esp_bomb_color);
            loadCol("world_esp_smoke_color", Config::world_esp_smoke_color);
            loadCol("world_esp_molotov_color", Config::world_esp_molotov_color);
            loadCol("world_esp_he_color", Config::world_esp_he_color);
            loadCol("world_esp_flash_color", Config::world_esp_flash_color);
            loadCol("world_esp_decoy_color", Config::world_esp_decoy_color);

            Config::nade_pred = j.value("nade_pred", false);
            Config::nade_pred_local = j.value("nade_pred_local", true);
            Config::nade_pred_local_only_pin = j.value("nade_pred_local_only_pin", true);
            Config::nade_pred_projectiles = j.value("nade_pred_projectiles", true);
            Config::nade_pred_radius = j.value("nade_pred_radius", true);
            Config::nade_pred_thickness = j.value("nade_pred_thickness", 1.8f);
            loadCol("nade_pred_color", Config::nade_pred_color);
            loadCol("nade_pred_local_color", Config::nade_pred_local_color);
            loadCol("nade_pred_land_color", Config::nade_pred_land_color);
            Config::nade_warn = j.value("nade_warn", true);
            Config::nade_warn_only_near = j.value("nade_warn_only_near", false);
            Config::nade_warn_range = j.value("nade_warn_range", 35.f);
            Config::nade_warn_icon_size = j.value("nade_warn_icon_size", 28.f);
            loadCol("nade_warn_color", Config::nade_warn_color);
            Config::nade_pred_damage = j.value("nade_pred_damage", true);
            loadCol("nade_pred_damage_color", Config::nade_pred_damage_color);
            loadCol("nade_pred_damage_lethal_color", Config::nade_pred_damage_lethal_color);
Config::nade_lineup = j.value("nade_lineup", false);
Config::nade_lineup_only_held = j.value("nade_lineup_only_held", false);
Config::nade_lineup_stand_dist = j.value("nade_lineup_stand_dist", 350.f);
Config::nade_lineup_aim_dist = j.value("nade_lineup_aim_dist", 20.f);
Config::nade_lineup_select_dist = j.value("nade_lineup_select_dist", 500.f);
            loadCol("nade_lineup_color", Config::nade_lineup_color);
            loadCol("nade_lineup_aim_color", Config::nade_lineup_aim_color);
            Config::nade_lineup_capture = j.value("nade_lineup_capture", true);
            Config::nade_lineup_capture_key = j.value("nade_lineup_capture_key", (int)VK_F6);
            Config::nade_lineup_capture_key_mode = j.value("nade_lineup_capture_key_mode", 1);
            if (Config::nade_lineup_capture_key_mode < 0 || Config::nade_lineup_capture_key_mode > 2)
                Config::nade_lineup_capture_key_mode = 1;
            keybind.setKey(Config::nade_lineup_capture, Config::nade_lineup_capture_key);
            keybind.setMode(Config::nade_lineup_capture, Config::nade_lineup_capture_key_mode);
            Config::nade_lineup_capture_throw = j.value("nade_lineup_capture_throw", 0);
            if (Config::nade_lineup_capture_throw < 0 || Config::nade_lineup_capture_throw > 5)
                Config::nade_lineup_capture_throw = 0;
            Config::nade_lineup_capture_kind = j.value("nade_lineup_capture_kind", 0);
            {
                const std::string nm = j.value("nade_lineup_capture_name", std::string("Lineup"));
                std::snprintf(Config::nade_lineup_capture_name, sizeof(Config::nade_lineup_capture_name), "%s", nm.c_str());
            }

            Config::enemyChamsInvisible = j.value("enemyChamsInvisible", false);
            Config::enemyChams = j.value("enemyChams", false);
            Config::teamChams = j.value("teamChams", false);
            Config::teamChamsInvisible = j.value("teamChamsInvisible", false);
            Config::chamsMaterial = j.value("chamsMaterial", 0);
            Config::chamsMaterialXQZ = j.value("chamsMaterialXQZ", Config::chamsMaterial);
            Config::armChamsMaterial = j.value("armChamsMaterial", Config::chamsMaterial);
            Config::viewmodelChamsMaterial = j.value("viewmodelChamsMaterial", Config::chamsMaterial);
            Config::localChams = j.value("localChams", false);
            Config::localChamsMaterial = j.value("localChamsMaterial", Config::chamsMaterial);
            loadCol("colLocalChams", Config::colLocalChams);
            Config::ragdollChams = j.value("ragdollChams", false);
            Config::ragdollChamsMaterial = j.value("ragdollChamsMaterial", Config::chamsMaterial);
            loadCol("colRagdollChams", Config::colRagdollChams);
            loadCol("colVisualChams", Config::colVisualChams);
            loadCol("colVisualChamsIgnoreZ", Config::colVisualChamsIgnoreZ);
            loadCol("teamcolVisualChamsIgnoreZ", Config::teamcolVisualChamsIgnoreZ);
            loadCol("teamcolVisualChams", Config::teamcolVisualChams);

            Config::fov_circle = j.value("fov_circle", false);
            Config::fov_circle_autofire = j.value("fov_circle_autofire", false);
            Config::fov_circle_magnet = j.value("fov_circle_magnet", false);
            loadCol("fovCircleColor", Config::fovCircleColor);
            loadCol("fovCircleColorAf", Config::fovCircleColorAf);
            loadCol("fovCircleColorMagnet", Config::fovCircleColorMagnet);
            Config::watermark = j.value("watermark", true);
            Config::hud_keybind_strip = j.value("hud_keybind_strip", true);
            loadCol("menu_accent", Config::menu_accent);
            loadCol("menu_bg", Config::menu_bg);
            loadCol("menu_child_bg", Config::menu_child_bg);
            loadCol("menu_sidebar_bg", Config::menu_sidebar_bg);
            loadCol("menu_border", Config::menu_border);
            loadCol("menu_text", Config::menu_text);
            loadCol("menu_text_muted", Config::menu_text_muted);
            Config::menu_rounding = j.value("menu_rounding", 6.0f);
            if (Config::menu_rounding < 0.f) Config::menu_rounding = 0.f;
            if (Config::menu_rounding > 12.f) Config::menu_rounding = 12.f;
            Config::menu_opacity = j.value("menu_opacity", 0.98f);
            if (Config::menu_opacity < 0.55f) Config::menu_opacity = 0.55f;
            if (Config::menu_opacity > 1.f) Config::menu_opacity = 1.f;
            Config::menu_compact = j.value("menu_compact", true);
            Config::menu_dpi_scale = j.value("menu_dpi_scale", 100);
            if (Config::menu_dpi_scale < 100) Config::menu_dpi_scale = 100;
            if (Config::menu_dpi_scale > 200) Config::menu_dpi_scale = 200;
            // Snap to 25% steps
            Config::menu_dpi_scale = ((Config::menu_dpi_scale + 12) / 25) * 25;
            if (Config::menu_dpi_scale < 100) Config::menu_dpi_scale = 100;
            if (Config::menu_dpi_scale > 200) Config::menu_dpi_scale = 200;
            Config::widget_keybinds = j.value("widget_keybinds", true);
            Config::widget_bomb = j.value("widget_bomb", true);
            Config::widget_spectators = j.value("widget_spectators", true);
            Config::widget_keybinds_only_when_active = j.value("widget_keybinds_only_when_active", false);
            Config::widget_keybinds_show_all = j.value("widget_keybinds_show_all", true);
            Config::widget_bomb_show_damage = j.value("widget_bomb_show_damage", true);
            Config::widget_bomb_show_defuse = j.value("widget_bomb_show_defuse", true);
            Config::widget_spectators_show_avatars = j.value("widget_spectators_show_avatars", true);
            Config::widget_spectators_max = j.value("widget_spectators_max", 8);
            if (Config::widget_spectators_max < 1) Config::widget_spectators_max = 1;
            if (Config::widget_spectators_max > 16) Config::widget_spectators_max = 16;
            Config::widget_radar = j.value("widget_radar", false);
            Config::widget_radar_size = j.value("widget_radar_size", 160.f);
            if (Config::widget_radar_size < 90.f) Config::widget_radar_size = 90.f;
            if (Config::widget_radar_size > 280.f) Config::widget_radar_size = 280.f;
            Config::widget_radar_shape = j.value("widget_radar_shape", 0);
            if (Config::widget_radar_shape < 0) Config::widget_radar_shape = 0;
            if (Config::widget_radar_shape > 1) Config::widget_radar_shape = 1;
            loadCol("widget_keybinds_accent", Config::widget_keybinds_accent);
            loadCol("widget_bomb_accent", Config::widget_bomb_accent);
            loadCol("widget_bomb_urgent", Config::widget_bomb_urgent);
            loadCol("widget_spectators_accent", Config::widget_spectators_accent);
            loadCol("widget_radar_accent", Config::widget_radar_accent);
            auto loadPos2 = [&](const char* key, ImVec2& pos) {
                if (!j.contains(key) || !j[key].is_array() || j[key].size() != 2)
                    return;
                auto& arr = j[key];
                if (arr[0].is_number()) pos.x = arr[0].get<float>();
                if (arr[1].is_number()) pos.y = arr[1].get<float>();
            };
            loadPos2("widget_keybinds_pos", Config::widget_keybinds_pos);
            loadPos2("widget_bomb_pos", Config::widget_bomb_pos);
            loadPos2("widget_spectators_pos", Config::widget_spectators_pos);
            loadPos2("widget_radar_pos", Config::widget_radar_pos);
            Config::aimbot = j.value("aimbot", false);
            Config::rcs = j.value("rcs", false);
            Config::rcs_standalone = j.value("rcs_standalone", false);
            // Defaults must match config.cpp / ResetToDefaults (0.5), not 1.0
            Config::rcs_scale_x = j.value("rcs_scale_x", 0.5f);
            Config::rcs_scale_y = j.value("rcs_scale_y", 0.5f);
            if (Config::rcs_scale_x < 0.f) Config::rcs_scale_x = 0.f;
            if (Config::rcs_scale_x > 1.f) Config::rcs_scale_x = 1.f;
            if (Config::rcs_scale_y < 0.f) Config::rcs_scale_y = 0.f;
            if (Config::rcs_scale_y > 1.f) Config::rcs_scale_y = 1.f;
            Config::aimbot_fov = j.value("aimbot_fov", 5.f);
            Config::aimbot_smooth = j.value("aimbot_smooth", 5.f);
            Config::team_check = j.value("team_check", true);
            Config::team_check_auto = j.value("team_check_auto", true);
            Config::aim_vis_check = j.value("aim_vis_check", true);
            Config::aim_reaction_delay_ms = j.value("aim_reaction_delay_ms", 0.f);
            Config::aim_target_switch_delay_ms = j.value("aim_target_switch_delay_ms", 0.f);
            Config::aim_first_shot_delay_ms = j.value("aim_first_shot_delay_ms", 0.f);
            {
                // reset then fill so switching configs never leaves stale hitboxes
                for (int i = 0; i < Config::HB_COUNT; ++i)
                    Config::aim_hitboxes[i] = false;
                if (j.contains("aim_hitboxes") && j["aim_hitboxes"].is_array()) {
                    auto& arr = j["aim_hitboxes"];
                    for (int i = 0; i < Config::HB_COUNT && i < (int)arr.size(); ++i) {
                        if (arr[i].is_boolean())
                            Config::aim_hitboxes[i] = arr[i].get<bool>();
                        else if (arr[i].is_number())
                            Config::aim_hitboxes[i] = arr[i].get<int>() != 0;
                    }
                } else {
                    Config::aim_hitboxes[Config::HB_HEAD] = true;
                    Config::aim_hitboxes[Config::HB_NECK] = true;
                    Config::aim_hitboxes[Config::HB_CHEST] = true;
                }
            }
            Config::aimbot_key = j.value("aimbot_key", 0x05);
            Config::aimbot_key_mode = j.value("aimbot_key_mode", 1);
            if (Config::aimbot_key_mode < 0 || Config::aimbot_key_mode > 2)
                Config::aimbot_key_mode = 1;
            keybind.setKey(Config::aimbot, Config::aimbot_key);
            keybind.setMode(Config::aimbot, Config::aimbot_key_mode);

            Config::knifebot = j.value("knifebot", false);
            Config::knifebot_prefer_stab = j.value("knifebot_prefer_stab", true);
            Config::knifebot_key = j.value("knifebot_key", 0);
            Config::knifebot_key_mode = j.value("knifebot_key_mode", 0);
            if (Config::knifebot_key_mode < 0 || Config::knifebot_key_mode > 2)
                Config::knifebot_key_mode = 0;
            keybind.setKey(Config::knifebot, Config::knifebot_key);
            keybind.setMode(Config::knifebot, Config::knifebot_key_mode);
            Config::auto_pistol = j.value("auto_pistol", false);
            Config::auto_pistol_delay_ms = j.value("auto_pistol_delay_ms", 0.f);
            if (Config::auto_pistol_delay_ms < 0.f) Config::auto_pistol_delay_ms = 0.f;
            if (Config::auto_pistol_delay_ms > 500.f) Config::auto_pistol_delay_ms = 500.f;
            Config::enemy_spectate = j.value("enemy_spectate", false);
            Config::enemy_spectate_thirdperson = j.value("enemy_spectate_thirdperson", false);
            Config::auto_defuse = j.value("auto_defuse", false);

            Config::autofire = j.value("autofire", false);
            Config::autofire_silent = j.value("autofire_silent", false);
            // Migrate: old configs shared aimbot_fov with autofire
            Config::autofire_fov = j.value("autofire_fov",
                j.value("aimbot_fov", Config::autofire_fov));
            Config::autofire_hitchance = j.value("autofire_hitchance", 70.f);
            Config::autofire_mode = j.value("autofire_mode", Config::AF_MODE_HITCHANCE);
            if (Config::autofire_mode < 0 || Config::autofire_mode >= Config::AF_MODE_COUNT)
                Config::autofire_mode = Config::AF_MODE_HITCHANCE;
            Config::autofire_autostop = j.value("autofire_autostop", false);
            Config::autofire_autoscope = j.value("autofire_autoscope", false);
            Config::autofire_scoped_only = j.value("autofire_scoped_only", false);
            Config::autofire_autowall = j.value("autofire_autowall", false);
            Config::autowall = true; // host always enabled; bind mode gates runtime
            Config::autowall_key = j.value("autowall_key", 0);
            Config::autowall_key_mode = j.value("autowall_key_mode", 0);
            if (Config::autowall_key_mode < 0 || Config::autowall_key_mode > 2)
                Config::autowall_key_mode = 0;
            keybind.setKey(Config::autowall, Config::autowall_key);
            keybind.setMode(Config::autowall, Config::autowall_key_mode);
            Config::autofire_mindamage = j.value("autofire_mindamage", 1.f);
            Config::autofire_mindamage_aw = j.value("autofire_mindamage_aw", 1.f);
            Config::autofire_target_select = j.value("autofire_target_select", Config::AF_TARGET_CROSSHAIR);
            if (Config::autofire_target_select < 0 || Config::autofire_target_select >= Config::AF_TARGET_COUNT)
                Config::autofire_target_select = Config::AF_TARGET_CROSSHAIR;
            Config::autofire_vis_check = j.value("autofire_vis_check", true);
            Config::autofire_flash_check = j.value("autofire_flash_check", true);
            Config::autofire_smoke_check = j.value("autofire_smoke_check", false);
            Config::autofire_focus_target = j.value("autofire_focus_target", true);
            Config::autofire_multipoint_dynamic = j.value("autofire_multipoint_dynamic", true);
            Config::autofire_body_if_lethal = j.value("autofire_body_if_lethal", false);
            Config::autofire_prefer_body = j.value("autofire_prefer_body", false);
            {
                if (j.contains("autofire_hitboxes") && j["autofire_hitboxes"].is_array())
                    LoadAfHitboxesFromJson(j["autofire_hitboxes"], Config::autofire_hitboxes,
                        Config::autofire_multipoint_scale);
                else
                    LoadAfHitboxesFromJson(nlohmann::json(), Config::autofire_hitboxes,
                        Config::autofire_multipoint_scale);
                if (j.contains("autofire_multipoint") && j["autofire_multipoint"].is_array())
                    LoadAfMultipointFromJson(j["autofire_multipoint"], Config::autofire_multipoint);
                else {
                    // Migrate: MP only on core boxes that were locked
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        Config::autofire_multipoint[i] = false;
                    Config::autofire_multipoint[Config::HB_HEAD] =
                        Config::autofire_hitboxes[Config::HB_HEAD];
                    Config::autofire_multipoint[Config::HB_CHEST] =
                        Config::autofire_hitboxes[Config::HB_CHEST];
                    Config::autofire_multipoint[Config::HB_STOMACH] =
                        Config::autofire_hitboxes[Config::HB_STOMACH];
                    Config::autofire_multipoint[Config::HB_PELVIS] =
                        Config::autofire_hitboxes[Config::HB_PELVIS];
                }
                // Enforce MP list = head/chest/stomach/pelvis only
                Config::autofire_multipoint[Config::HB_NECK] = false;
                Config::autofire_multipoint[Config::HB_ARMS] = false;
                Config::autofire_multipoint[Config::HB_LEGS] = false;
                Config::autofire_multipoint[Config::HB_FEET] = false;
                if (j.contains("autofire_multipoint_scale") && j["autofire_multipoint_scale"].is_array())
                    LoadAfScalesFromJson(j["autofire_multipoint_scale"], Config::autofire_multipoint_scale);
            }
            Config::autofire_key = j.value("autofire_key", 0x06);
            Config::autofire_key_mode = j.value("autofire_key_mode", 1);
            if (Config::autofire_key_mode < 0 || Config::autofire_key_mode > 2)
                Config::autofire_key_mode = 1;
            keybind.setKey(Config::autofire, Config::autofire_key);
            keybind.setMode(Config::autofire, Config::autofire_key_mode);

            Config::triggerbot = j.value("triggerbot", false);
            Config::triggerbot_key = j.value("triggerbot_key", 0x12);
            Config::triggerbot_key_mode = j.value("triggerbot_key_mode", 1);
            if (Config::triggerbot_key_mode < 0 || Config::triggerbot_key_mode > 2)
                Config::triggerbot_key_mode = 1;
            keybind.setKey(Config::triggerbot, Config::triggerbot_key);
            keybind.setMode(Config::triggerbot, Config::triggerbot_key_mode);

            Config::weapon_group_ui = j.value("weapon_group_ui", Config::WG_RIFLE);
            if (Config::weapon_group_ui < 0 || Config::weapon_group_ui >= Config::WG_COUNT)
                Config::weapon_group_ui = Config::WG_GENERAL;

            // Old configs stored magnet as global flat keys — seed profiles if missing tr_magnet
            const bool hadGlobalMagnet = j.contains("trigger_magnet");
            const bool globalMagnet = j.value("trigger_magnet", false);
            const float globalMagSmooth = j.value("trigger_magnet_smooth", 12.f);
            float globalMagFov = j.value("trigger_magnet_fov", 4.f);
            if (globalMagFov < 0.5f) globalMagFov = 0.5f;
            if (globalMagFov > 30.f) globalMagFov = 30.f;

            if (j.contains("weapon_profiles") && j["weapon_profiles"].is_array()) {
                auto& arr = j["weapon_profiles"];
                const int n = (std::min)((int)Config::WG_COUNT, (int)arr.size());
                // Clean defaults so missing keys cannot stick prior config flags
                Config::InitWeaponProfilesDefaults();
                for (int g = 0; g < n; ++g) {
                    auto& jp = arr[g];
                    if (!jp.is_object()) continue;
                    auto& p = Config::weapon_profiles[g];
                    // Defaults must match MakeDefaultProfile / InitWeaponProfilesDefaults
                    // — never use p.* as j.value fallback (that re-sticks prior load).
                    p.aimbot_fov = jp.value("fov", 5.f);
                    p.aimbot_smooth = jp.value("smooth", 5.f);
                    p.aimbot_humanize = jp.value("humanize", 0.f);
                    p.aimbot_smooth_mode = jp.value("smooth_mode", Config::SMOOTH_LINEAR);
                    if (p.aimbot_smooth_mode < 0 || p.aimbot_smooth_mode >= Config::SMOOTH_MODE_COUNT)
                        p.aimbot_smooth_mode = Config::SMOOTH_LINEAR;
                    p.aim_vis_check = jp.value("vis_check", true);
                    p.aim_smoke_check = jp.value("aim_smoke", false);
                    p.aim_flash_check = jp.value("aim_flash", false);
                    // Scope: aim_scoped is source of truth; migrate af/tr if aim missing
                    {
                        bool scoped = false;
                        if (jp.contains("aim_scoped"))
                            scoped = jp.value("aim_scoped", false);
                        else if (jp.contains("af_scoped"))
                            scoped = jp.value("af_scoped", false);
                        else if (jp.contains("tr_scoped"))
                            scoped = jp.value("tr_scoped", false);
                        p.aim_scoped_only = scoped;
                        p.autofire_scoped_only = scoped;
                        p.trigger_scoped_only = scoped;
                    }
                    p.aim_reaction_delay_ms = jp.value("reaction_ms", 0.f);
                    p.aim_target_switch_delay_ms = jp.value("switch_ms", 0.f);
                    p.aim_first_shot_delay_ms = jp.value("first_shot_ms", 0.f);
                    p.rcs = jp.value("rcs", false);
                    p.rcs_standalone = jp.value("rcs_standalone", false);
                    p.rcs_scale_x = jp.value("rcs_x", 0.5f);
                    p.rcs_scale_y = jp.value("rcs_y", 0.5f);
                    // Migrate old shared FOV → autofire_fov
                    p.autofire_fov = jp.value("af_fov", jp.value("fov", 5.f));
                    p.autofire_hitchance = jp.value("af_hc", 70.f);
                    p.autofire_mode = jp.value("af_mode", Config::AF_MODE_HITCHANCE);
                    if (p.autofire_mode < 0 || p.autofire_mode >= Config::AF_MODE_COUNT)
                        p.autofire_mode = Config::AF_MODE_HITCHANCE;
                    p.autofire_autostop = jp.value("af_autostop", false);
                    p.autofire_autoscope = jp.value("af_autoscope", false);
                    p.autofire_autowall = jp.value("af_autowall", false);
                    p.autofire_mindamage = jp.value("af_mindmg", 1.f);
                    p.autofire_mindamage_aw = jp.value("af_mindmg_aw", 1.f);
                    p.autofire_target_select = jp.value("af_target", Config::AF_TARGET_CROSSHAIR);
                    p.autofire_vis_check = jp.value("af_vis", true);
                    p.autofire_flash_check = jp.value("af_flash", true);
                    p.autofire_smoke_check = jp.value("af_smoke", false);
                    p.autofire_focus_target = jp.value("af_focus", true);
                    p.autofire_multipoint_dynamic = jp.value("af_mp_dyn", true);
                    p.autofire_body_if_lethal = jp.value("af_body_lethal", false);
                    p.autofire_prefer_body = jp.value("af_prefer_body", false);
                    p.trigger_delay_ms = jp.value("tr_delay", 0.f);
                    p.trigger_hitchance = jp.value("tr_hc", 0.f);
                    p.trigger_autowall = jp.value("tr_aw", false);
                    // Independent of AF; migrate old shared af_mindmg if tr keys missing
                    p.trigger_mindamage = jp.value("tr_mindmg",
                        jp.value("af_mindmg", 1.f));
                    p.trigger_mindamage_aw = jp.value("tr_mindmg_aw",
                        jp.value("af_mindmg_aw", 1.f));
                    // trigger_scoped already set from aim_scoped above
                    p.trigger_flash_check = jp.value("tr_flash", true);
                    p.trigger_smoke_check = jp.value("tr_smoke", false);
                    p.trigger_autostop = jp.value("tr_autostop", false);
                    // Migrate old tr_spread_comp bool → mode
                    if (jp.contains("tr_mode"))
                        p.trigger_mode = jp.value("tr_mode", p.trigger_mode);
                    else if (jp.value("tr_spread_comp", false))
                        p.trigger_mode = Config::TR_MODE_SEED_NOSPREAD;
                    if (p.trigger_mode < 0 || p.trigger_mode >= Config::TR_MODE_COUNT)
                        p.trigger_mode = Config::TR_MODE_HITCHANCE;
                    p.trigger_magnet = jp.value("tr_magnet", false);
                    p.trigger_magnet_smooth = jp.value("tr_magnet_smooth", 12.f);
                    p.trigger_magnet_fov = jp.value("tr_magnet_fov", 4.f);
                    if (p.trigger_magnet_fov < 0.5f) p.trigger_magnet_fov = 0.5f;
                    if (p.trigger_magnet_fov > 30.f) p.trigger_magnet_fov = 30.f;
                    p.trigger_magnet_silent = jp.value("tr_magnet_silent", false);
                    p.trigger_magnet_lead = jp.value("tr_magnet_lead", false);
                    p.trigger_magnet_head_prio = jp.value("tr_magnet_head_prio", true);
                    p.trigger_magnet_only_ready = jp.value("tr_magnet_only_ready", false);
                    p.trigger_magnet_deadzone = jp.value("tr_magnet_deadzone", 0.12f);
                    if (p.trigger_magnet_deadzone < 0.f) p.trigger_magnet_deadzone = 0.f;
                    if (p.trigger_magnet_deadzone > 1.f) p.trigger_magnet_deadzone = 1.f;
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        p.trigger_magnet_hitboxes[i] = false;
                    if (jp.contains("tr_magnet_hitboxes") && jp["tr_magnet_hitboxes"].is_array()) {
                        auto& mhb = jp["tr_magnet_hitboxes"];
                        for (int i = 0; i < Config::HB_COUNT && i < (int)mhb.size(); ++i) {
                            if (mhb[i].is_boolean())
                                p.trigger_magnet_hitboxes[i] = mhb[i].get<bool>();
                            else if (mhb[i].is_number())
                                p.trigger_magnet_hitboxes[i] = mhb[i].get<int>() != 0;
                        }
                    }
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        p.trigger_hitboxes[i] = false;
                    if (jp.contains("tr_hitboxes") && jp["tr_hitboxes"].is_array()) {
                        auto& thb = jp["tr_hitboxes"];
                        for (int i = 0; i < Config::HB_COUNT && i < (int)thb.size(); ++i) {
                            if (thb[i].is_boolean())
                                p.trigger_hitboxes[i] = thb[i].get<bool>();
                            else if (thb[i].is_number())
                                p.trigger_hitboxes[i] = thb[i].get<int>() != 0;
                        }
                    } else if (jp.value("tr_head", false)) {
                        p.trigger_hitboxes[Config::HB_HEAD] = true;
                    } else {
                        p.trigger_hitboxes[Config::HB_HEAD] = true;
                        p.trigger_hitboxes[Config::HB_NECK] = true;
                        p.trigger_hitboxes[Config::HB_CHEST] = true;
                    }
                    {
                        bool any = false;
                        for (int i = 0; i < Config::HB_COUNT; ++i)
                            if (p.trigger_hitboxes[i]) { any = true; break; }
                        if (!any)
                            p.trigger_hitboxes[Config::HB_HEAD] = true;
                    }
                    if (jp.contains("hitboxes") && jp["hitboxes"].is_array()) {
                        // Zero first so short arrays cannot keep default head/neck/chest
                        for (int i = 0; i < Config::HB_COUNT; ++i)
                            p.aim_hitboxes[i] = false;
                        auto& hb = jp["hitboxes"];
                        for (int i = 0; i < Config::HB_COUNT && i < (int)hb.size(); ++i) {
                            if (hb[i].is_boolean())
                                p.aim_hitboxes[i] = hb[i].get<bool>();
                            else if (hb[i].is_number())
                                p.aim_hitboxes[i] = hb[i].get<int>() != 0;
                        }
                    }
                    if (jp.contains("af_hitboxes") && jp["af_hitboxes"].is_array())
                        LoadAfHitboxesFromJson(jp["af_hitboxes"], p.autofire_hitboxes,
                            p.autofire_multipoint_scale);
                    if (jp.contains("af_multipoint") && jp["af_multipoint"].is_array())
                        LoadAfMultipointFromJson(jp["af_multipoint"], p.autofire_multipoint);
                    else {
                        for (int i = 0; i < Config::HB_COUNT; ++i)
                            p.autofire_multipoint[i] = false;
                        p.autofire_multipoint[Config::HB_HEAD] = p.autofire_hitboxes[Config::HB_HEAD];
                        p.autofire_multipoint[Config::HB_CHEST] = p.autofire_hitboxes[Config::HB_CHEST];
                        p.autofire_multipoint[Config::HB_STOMACH] = p.autofire_hitboxes[Config::HB_STOMACH];
                        p.autofire_multipoint[Config::HB_PELVIS] = p.autofire_hitboxes[Config::HB_PELVIS];
                    }
                    p.autofire_multipoint[Config::HB_NECK] = false;
                    p.autofire_multipoint[Config::HB_ARMS] = false;
                    p.autofire_multipoint[Config::HB_LEGS] = false;
                    p.autofire_multipoint[Config::HB_FEET] = false;
                    if (jp.contains("af_mp_scale") && jp["af_mp_scale"].is_array())
                        LoadAfScalesFromJson(jp["af_mp_scale"], p.autofire_multipoint_scale);
                    // Pre-per-group magnet: copy flat global into profile when key absent
                    if (!jp.contains("tr_magnet") && hadGlobalMagnet) {
                        p.trigger_magnet = globalMagnet;
                        p.trigger_magnet_smooth = globalMagSmooth;
                        p.trigger_magnet_fov = globalMagFov;
                    }
                }
            } else {
                // Migrate old flat config into every group
                for (int g = 0; g < Config::WG_COUNT; ++g)
                    Config::PullLiveIntoProfile(g);
                if (hadGlobalMagnet) {
                    for (int g = 0; g < Config::WG_COUNT; ++g) {
                        auto& p = Config::weapon_profiles[g];
                        p.trigger_magnet = globalMagnet;
                        p.trigger_magnet_smooth = globalMagSmooth;
                        p.trigger_magnet_fov = globalMagFov;
                    }
                }
            }
            // Live mirrors active group (held weapon unknown at load → General)
            Config::ApplyWeaponGroup(nullptr);

            Config::antiflash_amount = j.value("antiflash_amount", -1.f);
            if (Config::antiflash_amount < 0.f) {
                // migrate old bool configs
                Config::antiflash_amount = j.value("antiflash", false) ? 100.f : 0.f;
            }
            Config::antiflash = Config::antiflash_amount > 0.01f;
            Config::remove_legs = j.value("remove_legs", false);
            Config::remove_smoke = j.value("remove_smoke", false);
            Config::remove_decals = j.value("remove_decals", false);
            Config::remove_particles = j.value("remove_particles", false);
            Config::smoke_color = j.value("smoke_color", false);
            loadCol("smoke_color_value", Config::smoke_color_value);
            Config::fire_color = j.value("fire_color", false);
            loadCol("fire_color_value", Config::fire_color_value);
            Config::explosion_color = j.value("explosion_color", false);
            loadCol("explosion_color_value", Config::explosion_color_value);
            Config::remove_crosshair = j.value("remove_crosshair", false);
            Config::force_crosshair = j.value("force_crosshair", false);
            Config::remove_hud = j.value("remove_hud", false);
            Config::remove_postprocess = j.value("remove_postprocess", false);
            Config::remove_recoil = j.value("remove_recoil", false);
            Config::scope_custom_lines = j.value("scope_custom_lines", false);
            Config::scope_line_size = j.value("scope_line_size", 1.f);
            Config::scope_line_gap = j.value("scope_line_gap", 4.f);
            Config::scope_line_thickness = j.value("scope_line_thickness", 0.5f);
            if (Config::scope_line_size < 0.f) Config::scope_line_size = 0.f;
            if (Config::scope_line_size > 1.f) Config::scope_line_size = 1.f;
            if (Config::scope_line_gap < 0.f) Config::scope_line_gap = 0.f;
            if (Config::scope_line_gap > 80.f) Config::scope_line_gap = 80.f;
            if (Config::scope_line_thickness < 0.1f) Config::scope_line_thickness = 0.1f;
            if (Config::scope_line_thickness > 6.f) Config::scope_line_thickness = 6.f;
            loadCol("scope_line_color", Config::scope_line_color);
            Config::scope_zoom_fov = j.value("scope_zoom_fov", false);
            Config::scope_fov_1 = j.value("scope_fov_1", 40.f);
            Config::scope_fov_2 = j.value("scope_fov_2", 15.f);
            if (Config::scope_fov_1 < 1.f) Config::scope_fov_1 = 1.f;
            if (Config::scope_fov_1 > 90.f) Config::scope_fov_1 = 90.f;
            if (Config::scope_fov_2 < 1.f) Config::scope_fov_2 = 1.f;
            if (Config::scope_fov_2 > 90.f) Config::scope_fov_2 = 90.f;
            Config::scope_hide_viewmodel = j.value("scope_hide_viewmodel", false);
            Config::bhop = j.value("bhop", false);
            Config::autostrafe = j.value("autostrafe", false);
            Config::autostrafe_mode = j.value("autostrafe_mode", 0);
            if (Config::autostrafe_mode < 0 || Config::autostrafe_mode > 1)
                Config::autostrafe_mode = 0;
            Config::jumpbug = j.value("jumpbug", false);
            Config::jumpbug_key = j.value("jumpbug_key", 0);
            Config::jumpbug_key_mode = j.value("jumpbug_key_mode", 0);
            if (Config::jumpbug_key_mode < 0 || Config::jumpbug_key_mode > 2)
                Config::jumpbug_key_mode = 0;
            keybind.setKey(Config::jumpbug, Config::jumpbug_key);
            keybind.setMode(Config::jumpbug, Config::jumpbug_key_mode);
            Config::edgejump = j.value("edgejump", false);
            Config::edgejump_key = j.value("edgejump_key", 0);
            Config::edgejump_key_mode = j.value("edgejump_key_mode", 0);
            if (Config::edgejump_key_mode < 0 || Config::edgejump_key_mode > 2)
                Config::edgejump_key_mode = 0;
            keybind.setKey(Config::edgejump, Config::edgejump_key);
            keybind.setMode(Config::edgejump, Config::edgejump_key_mode);
            Config::edgebug = j.value("edgebug", false);
            Config::edgebug_key = j.value("edgebug_key", 0);
            Config::edgebug_key_mode = j.value("edgebug_key_mode", 0);
            if (Config::edgebug_key_mode < 0 || Config::edgebug_key_mode > 2)
                Config::edgebug_key_mode = 0;
            keybind.setKey(Config::edgebug, Config::edgebug_key);
            keybind.setMode(Config::edgebug, Config::edgebug_key_mode);
            {
                // Master: prefer global keys; migrate old AF/TR-only cfgs
                const bool legacyAf = j.value("autofire_backtrack", false);
                const bool legacyTr = j.value("trigger_backtrack", false);
                const float legacyAfMs = j.value("autofire_backtrack_ms", 200.f);
                const float legacyTrMs = j.value("trigger_backtrack_ms", 200.f);
                Config::backtrack = j.value("backtrack", legacyAf || legacyTr);
                Config::backtrack_ms = j.value("backtrack_ms",
                    (std::max)(legacyAfMs, legacyTrMs));
                if (Config::backtrack_ms < 50.f) Config::backtrack_ms = 50.f;
                if (Config::backtrack_ms > 400.f) Config::backtrack_ms = 400.f;
                Config::backtrack_skeleton = j.value("backtrack_skeleton", false);
                // Keep aliases in sync for any leftover code paths
                Config::autofire_backtrack = Config::backtrack;
                Config::autofire_backtrack_ms = Config::backtrack_ms;
                Config::trigger_backtrack = Config::backtrack;
                Config::trigger_backtrack_ms = Config::backtrack_ms;
                Config::backtrack_aim = false;
                loadCol("backtrack_color", Config::backtrack_color);
            }
            Config::hitlog = j.value("hitlog", false);
            Config::hitlog_console = j.value("hitlog_console", false);
            Config::hitlog_duration = j.value("hitlog_duration", 4.f);
            if (Config::hitlog_duration < 1.f) Config::hitlog_duration = 1.f;
            if (Config::hitlog_duration > 12.f) Config::hitlog_duration = 12.f;
            if (j.contains("hitlog_pos") && j["hitlog_pos"].is_array() && j["hitlog_pos"].size() >= 2) {
                Config::hitlog_pos.x = j["hitlog_pos"][0].get<float>();
                Config::hitlog_pos.y = j["hitlog_pos"][1].get<float>();
            }
            Config::hitlog_width = j.value("hitlog_width", 268.f);
            if (Config::hitlog_width < 200.f) Config::hitlog_width = 200.f;
            if (Config::hitlog_width > 420.f) Config::hitlog_width = 420.f;
            Config::hitlog_max_rows = j.value("hitlog_max_rows", 8);
            if (Config::hitlog_max_rows < 4) Config::hitlog_max_rows = 4;
            if (Config::hitlog_max_rows > 16) Config::hitlog_max_rows = 16;
            Config::hitlog_show_hp = j.value("hitlog_show_hp", true);
            Config::hitlog_show_stats = j.value("hitlog_show_stats", true);
            loadCol("hitlog_color", Config::hitlog_color);
            loadCol("hitlog_head_color", Config::hitlog_head_color);
            loadCol("hitlog_kill_color", Config::hitlog_kill_color);
            Config::auto_accept = j.value("auto_accept", false);
            Config::sound_esp = j.value("sound_esp", false);
            Config::sound_esp_duration = j.value("sound_esp_duration", 1.4f);
            Config::sound_esp_ring_size = j.value("sound_esp_ring_size", 1.f);
            if (Config::sound_esp_duration < 0.4f) Config::sound_esp_duration = 0.4f;
            if (Config::sound_esp_duration > 4.f) Config::sound_esp_duration = 4.f;
            if (Config::sound_esp_ring_size < 0.5f) Config::sound_esp_ring_size = 0.5f;
            if (Config::sound_esp_ring_size > 3.f) Config::sound_esp_ring_size = 3.f;
            loadCol("sound_esp_color", Config::sound_esp_color);
            Config::vote_reveal = j.value("vote_reveal", false);
            Config::vote_auto = j.value("vote_auto", false);
            Config::vote_auto_choice = j.value("vote_auto_choice", 0);
            if (Config::vote_auto_choice < 0 || Config::vote_auto_choice > 1)
                Config::vote_auto_choice = 0;
            Config::vote_auto_delay_ms = j.value("vote_auto_delay_ms", 250.f);
            if (Config::vote_auto_delay_ms < 0.f) Config::vote_auto_delay_ms = 0.f;
            if (Config::vote_auto_delay_ms > 5000.f) Config::vote_auto_delay_ms = 5000.f;
            Config::hitmarker = j.value("hitmarker", true);
            Config::hitmarker_screen = j.value("hitmarker_screen", true);
            Config::hitmarker_world = j.value("hitmarker_world", true);
            Config::hitmarker_show_damage = j.value("hitmarker_show_damage", false);
            Config::hitmarker_size = j.value("hitmarker_size", 14.f);
            Config::hitmarker_gap = j.value("hitmarker_gap", 4.f);
            Config::hitmarker_thickness = j.value("hitmarker_thickness", 2.2f);
            Config::hitmarker_world_size = j.value("hitmarker_world_size", 11.f);
            Config::hitmarker_duration = j.value("hitmarker_duration", 1.f);
            if (Config::hitmarker_size < 4.f) Config::hitmarker_size = 4.f;
            if (Config::hitmarker_size > 40.f) Config::hitmarker_size = 40.f;
            if (Config::hitmarker_duration < 0.25f) Config::hitmarker_duration = 0.25f;
            if (Config::hitmarker_duration > 2.5f) Config::hitmarker_duration = 2.5f;
            loadCol("hitmarker_color", Config::hitmarker_color);
            loadCol("hitmarker_head_color", Config::hitmarker_head_color);
            loadCol("hitmarker_kill_color", Config::hitmarker_kill_color);
            Config::hitsound = j.value("hitsound", true);
            {
                const std::string f = j.value("hitsound_file", std::string());
                const std::string h = j.value("hitsound_head", std::string());
                const std::string k = j.value("hitsound_kill", std::string());
                std::snprintf(Config::hitsound_file, sizeof(Config::hitsound_file), "%s", f.c_str());
                std::snprintf(Config::hitsound_head, sizeof(Config::hitsound_head), "%s", h.c_str());
                std::snprintf(Config::hitsound_kill, sizeof(Config::hitsound_kill), "%s", k.c_str());
            }

            // Skinchanger — always default first so missing keys don't keep previous config
            Config::custom_paint_color = j.value("custom_paint_color", false);
            loadCol("custom_color0", Config::custom_color0);
            loadCol("custom_color1", Config::custom_color1);
            loadCol("custom_color2", Config::custom_color2);
            loadCol("custom_color3", Config::custom_color3);
            Config::knife_changer = j.value("knife_changer", false);
            Config::knife_index = j.value("knife_index", 0);
            if (Config::knife_index < 0
                || Config::knife_index >= SkinChanger::KnifeCount())
                Config::knife_index = 0;
            Config::knife_paint_kit = j.value("knife_paint_kit", 0);
            if (Config::knife_paint_kit < 0) Config::knife_paint_kit = 0;
            Config::knife_paint_kit_id = j.value("knife_paint_kit_id", 0);
            if (Config::knife_paint_kit_id < 0) Config::knife_paint_kit_id = 0;
            Config::knife_wear = j.value("knife_wear", 0.0001f);
            if (!(Config::knife_wear >= 0.0001f && Config::knife_wear <= 1.f)) // NaN/inf/out-of-range
                Config::knife_wear = 0.0001f;
            Config::knife_seed = j.value("knife_seed", 0);
            if (Config::knife_seed < 0) Config::knife_seed = 0;
            if (Config::knife_seed > 1000) Config::knife_seed = 1000;
            {
                Config::knife_custom_name[0] = '\0';
                const std::string cn = j.value("knife_custom_name", std::string{});
                if (!cn.empty())
                    strncpy_s(Config::knife_custom_name, cn.c_str(), _TRUNCATE);
                Config::knife_custom_name[sizeof(Config::knife_custom_name) - 1] = '\0';
            }
            Config::knife_custom_color = j.value("knife_custom_color", false);
            if (Config::knife_custom_color && j.contains("knife_colors") && j["knife_colors"].is_array()) {
                auto& carr = j["knife_colors"];
                for (int ci = 0; ci < 16 && ci < (int)carr.size(); ++ci) {
                    if (carr[ci].is_number())
                        Config::knife_colors[ci] = carr[ci].get<float>();
                }
                Config::knife_colors_active = true;
                Config::knife_colors_edited = true;
            }

            Config::glove_changer = j.value("glove_changer", false);
            Config::glove_index = j.value("glove_index", 0);
            if (Config::glove_index < 0
                || Config::glove_index >= SkinChanger::GloveCount())
                Config::glove_index = 0;
            Config::glove_paint_kit = j.value("glove_paint_kit", 0);
            if (Config::glove_paint_kit < 0) Config::glove_paint_kit = 0;
            Config::glove_paint_kit_id = j.value("glove_paint_kit_id", 0);
            if (Config::glove_paint_kit_id < 0) Config::glove_paint_kit_id = 0;
            Config::glove_wear = j.value("glove_wear", 0.0001f);
            if (!(Config::glove_wear >= 0.0001f && Config::glove_wear <= 1.f))
                Config::glove_wear = 0.0001f;
            Config::glove_seed = j.value("glove_seed", 0);
            if (Config::glove_seed < 0) Config::glove_seed = 0;
            if (Config::glove_seed > 1000) Config::glove_seed = 1000;
            Config::glove_custom_color = j.value("glove_custom_color", false);
            if (Config::glove_custom_color && j.contains("glove_colors") && j["glove_colors"].is_array()) {
                auto& carr = j["glove_colors"];
                for (int ci = 0; ci < 16 && ci < (int)carr.size(); ++ci) {
                    if (carr[ci].is_number())
                        Config::glove_colors[ci] = carr[ci].get<float>();
                }
                Config::glove_colors_active = true;
                Config::glove_colors_edited = true;
            }

            Config::weapon_skins = j.value("weapon_skins", false);
            Config::weapon_selected = j.value("weapon_selected", 7);
            if (Config::weapon_selected < 1 || Config::weapon_selected > 70)
                Config::weapon_selected = 7;
            for (int i = 0; i < 71; ++i)
                Config::weapon_skin[i] = {};
            if (j.contains("weapon_skin") && j["weapon_skin"].is_object()) {
                for (auto it = j["weapon_skin"].begin(); it != j["weapon_skin"].end(); ++it) {
                    int def = 0;
                    try { def = std::stoi(it.key()); } catch (...) { continue; }
                    if (def < 1 || def > 70 || !it.value().is_object()) continue;
                    Config::weapon_skin[def].paint_kit = it.value().value("paint_kit", 0);
                    if (Config::weapon_skin[def].paint_kit < 0) Config::weapon_skin[def].paint_kit = 0;
                    Config::weapon_skin[def].paint_kit_id = it.value().value("paint_kit_id", 0);
                    if (Config::weapon_skin[def].paint_kit_id < 0) Config::weapon_skin[def].paint_kit_id = 0;
                    Config::weapon_skin[def].wear = it.value().value("wear", 0.0001f);
                    if (!(Config::weapon_skin[def].wear >= 0.0001f && Config::weapon_skin[def].wear <= 1.f))
                        Config::weapon_skin[def].wear = 0.0001f;
                    Config::weapon_skin[def].seed = it.value().value("seed", 0);
                    if (Config::weapon_skin[def].seed < 0) Config::weapon_skin[def].seed = 0;
                    if (Config::weapon_skin[def].seed > 1000) Config::weapon_skin[def].seed = 1000;
                    Config::weapon_skin[def].custom_color = it.value().value("custom_color", false);
                    if (Config::weapon_skin[def].custom_color && it.value().contains("colors") && it.value()["colors"].is_array()) {
                        auto& carr = it.value()["colors"];
                        for (int ci = 0; ci < 16 && ci < (int)carr.size(); ++ci) {
                            if (carr[ci].is_number())
                                Config::weapon_skin[def].colors[ci] = carr[ci].get<float>();
                        }
                        Config::weapon_skin[def].colors_active = true;
                        Config::weapon_skin[def].colors_edited = true;
                    }
                }
            }

            Config::agent_changer = j.value("agent_changer", false);
            Config::agent_ct_def = j.value("agent_ct_def", 0);
            Config::agent_t_def = j.value("agent_t_def", 0);
            if (Config::agent_ct_def < 0 || Config::agent_ct_def > 100000) Config::agent_ct_def = 0;
            if (Config::agent_t_def < 0 || Config::agent_t_def > 100000) Config::agent_t_def = 0;
            Config::custom_model = j.value("custom_model", false);
            {
                const std::string cmp = j.value("custom_model_path", std::string{});
                if (cmp.size() < sizeof(Config::custom_model_path)
                    && cmp.find("..") == std::string::npos
                    && (cmp.find("characters/") == 0 || cmp.find("models/") == 0)) {
                    std::snprintf(Config::custom_model_path, sizeof(Config::custom_model_path), "%s", cmp.c_str());
                } else {
                    Config::custom_model_path[0] = '\0';
                }
            }
            Config::custom_knife = j.value("custom_knife", false);
            {
                const std::string ckp = j.value("custom_knife_path", std::string{});
                if (ckp.size() < sizeof(Config::custom_knife_path)
                    && ckp.find("..") == std::string::npos
                    && (ckp.find("weapons/") == 0 || ckp.find("models/") == 0
                        || ckp.find("characters/") == 0)) {
                    std::snprintf(Config::custom_knife_path, sizeof(Config::custom_knife_path), "%s", ckp.c_str());
                } else {
                    Config::custom_knife_path[0] = '\0';
                }
            }

            Config::armChams = j.value("armChams", false);
            Config::viewmodelChams = j.value("viewmodelChams", false);

            // Prefer new keys; fall back to legacy armChams_color / viewmodelChams_color
            if (j.contains("colArmChams"))
                loadCol("colArmChams", Config::colArmChams);
            else
                loadCol("armChams_color", Config::colArmChams);

            if (j.contains("colViewmodelChams"))
                loadCol("colViewmodelChams", Config::colViewmodelChams);
            else
                loadCol("viewmodelChams_color", Config::colViewmodelChams);

            loadCol("colVisualChams", Config::colVisualChams);
            loadCol("colVisualChamsIgnoreZ", Config::colVisualChamsIgnoreZ);
            loadCol("teamcolVisualChamsIgnoreZ", Config::teamcolVisualChamsIgnoreZ);
            loadCol("teamcolVisualChams", Config::teamcolVisualChams);
            // fov colors already applied above; re-apply if present (safe)
            loadCol("fovCircleColor", Config::fovCircleColor);
            loadCol("fovCircleColorAf", Config::fovCircleColorAf);
            loadCol("fovCircleColorMagnet", Config::fovCircleColorMagnet);

            // Keep Config::*key mirrors in sync with live keybind state
            Config::aimbot_key = keybind.getKey(Config::aimbot);
            Config::aimbot_key_mode = keybind.getMode(Config::aimbot);
            Config::autofire_key = keybind.getKey(Config::autofire);
            Config::autofire_key_mode = keybind.getMode(Config::autofire);
            Config::autowall_key = keybind.getKey(Config::autowall);
            Config::autowall_key_mode = keybind.getMode(Config::autowall);
            Config::knifebot_key = keybind.getKey(Config::knifebot);
            Config::knifebot_key_mode = keybind.getMode(Config::knifebot);
            Config::triggerbot_key = keybind.getKey(Config::triggerbot);
            Config::triggerbot_key_mode = keybind.getMode(Config::triggerbot);
            Config::thirdperson_key = keybind.getKey(Config::thirdperson);
            Config::thirdperson_key_mode = keybind.getMode(Config::thirdperson);
            Config::nade_lineup_capture_key = keybind.getKey(Config::nade_lineup_capture);
            Config::nade_lineup_capture_key_mode = keybind.getMode(Config::nade_lineup_capture);
            Config::jumpbug_key = keybind.getKey(Config::jumpbug);
            Config::jumpbug_key_mode = keybind.getMode(Config::jumpbug);
            Config::edgejump_key = keybind.getKey(Config::edgejump);
            Config::edgejump_key_mode = keybind.getMode(Config::edgejump);
            Config::edgebug_key = keybind.getKey(Config::edgebug);
            Config::edgebug_key_mode = keybind.getMode(Config::edgebug);

            // Config switch must never leave a previous Toggle latch active
            keybind.clearAllToggles();

            // Skin invalidate while still loading (FSN/RunApply gated)
            SkinChanger::OnConfigLoaded();

            } catch (...) {
                Config::ResetToDefaults();
                keybind.clearAllToggles();
                Config::loading.store(false, std::memory_order_release);
                ifs.close();
                return false;
            }

            Config::loading.store(false, std::memory_order_release);
            ifs.close();
            return true;
        }

        static bool Remove(const std::string& configName)
        {
            const std::string name = SanitizeName(configName);
            if (name.empty())
                return false;

            auto filePath = GetConfigPath(name);
            if (!std::filesystem::exists(filePath))
                return false;

            std::error_code ec;
            return std::filesystem::remove(filePath, ec) && !ec;
        }
    };
}
