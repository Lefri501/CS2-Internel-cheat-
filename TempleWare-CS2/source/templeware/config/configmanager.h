#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <Windows.h>
#include <Shellapi.h>

#include "config.h"
#include "../keybinds/keybinds.h"
#include "../../../external/json/json.hpp"

namespace internal_config
{
    class ConfigManager
    {
    private:

        static std::filesystem::path GetConfigFolder()
        {
            // C:\Users\<user>\Documents\TempleWare\Configs
            std::filesystem::path folder;
            char* userProfile = nullptr;
            size_t len = 0;
            if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile && len > 0) {
                folder = userProfile;
                free(userProfile);
                folder /= "Documents";
                folder /= "TempleWare";
                folder /= "Configs";
            } else {
                folder = "TempleWare";
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
            j["weather"] = Config::weather;
            j["weather_mode"] = Config::weather_mode;
            j["weather_intensity"] = Config::weather_intensity;

            j["flag_flashed"] = Config::flag_flashed;
            j["flag_bomb"] = Config::flag_bomb;
            j["flag_scoped"] = Config::flag_scoped;
            j["flag_reloading"] = Config::flag_reloading;
            j["flag_defusing"] = Config::flag_defusing;

            j["world_esp_weapons"] = Config::world_esp_weapons;
            j["world_esp_bomb"] = Config::world_esp_bomb;
            j["world_esp_smoke"] = Config::world_esp_smoke;
            j["world_esp_molotov"] = Config::world_esp_molotov;
            j["world_esp_he"] = Config::world_esp_he;
            j["world_esp_flash"] = Config::world_esp_flash;
            j["world_esp_decoy"] = Config::world_esp_decoy;
            j["world_esp_distance"] = Config::world_esp_distance;
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

            j["knife_changer"] = Config::knife_changer;
            j["knife_index"] = Config::knife_index;
            j["knife_paint_kit"] = Config::knife_paint_kit;
            j["knife_paint_kit_id"] = Config::knife_paint_kit_id;
            j["knife_wear"] = Config::knife_wear;
            j["knife_seed"] = Config::knife_seed;
            j["knife_custom_name"] = Config::knife_custom_name;

            j["glove_changer"] = Config::glove_changer;
            j["glove_index"] = Config::glove_index;
            j["glove_paint_kit"] = Config::glove_paint_kit;
            j["glove_paint_kit_id"] = Config::glove_paint_kit_id;
            j["glove_wear"] = Config::glove_wear;
            j["glove_seed"] = Config::glove_seed;

            j["weapon_skins"] = Config::weapon_skins;
            j["weapon_selected"] = Config::weapon_selected;
            {
                nlohmann::json ws = nlohmann::json::object();
                for (int i = 1; i <= 70; ++i) {
                    if (Config::weapon_skin[i].paint_kit_id <= 0 && Config::weapon_skin[i].paint_kit <= 0)
                        continue;
                    ws[std::to_string(i)] = {
                        {"paint_kit", Config::weapon_skin[i].paint_kit},
                        {"paint_kit_id", Config::weapon_skin[i].paint_kit_id},
                        {"wear", Config::weapon_skin[i].wear},
                        {"seed", Config::weapon_skin[i].seed}
                    };
                }
                j["weapon_skin"] = ws;
            }

            j["agent_changer"] = Config::agent_changer;
            j["agent_ct_def"] = Config::agent_ct_def;
            j["agent_t_def"] = Config::agent_t_def;

            j["aimbot"] = Config::aimbot;
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
            j["scope_no_overlay"] = Config::scope_no_overlay;
            j["scope_remove_blur"] = Config::scope_remove_blur;
            j["scope_remove_bars"] = Config::scope_remove_bars;
            j["scope_remove_texture"] = Config::scope_remove_texture;
            j["scope_custom_look"] = Config::scope_custom_look;
            j["scope_size_scale"] = Config::scope_size_scale;
            j["scope_blur_amount"] = Config::scope_blur_amount;
            j["rcs"] = Config::rcs;
            j["rcs_standalone"] = Config::rcs_standalone;
            j["rcs_scale_x"] = Config::rcs_scale_x;
            j["rcs_scale_y"] = Config::rcs_scale_y;
            j["aimbot_key"] = keybind.getKey(Config::aimbot);
            j["aimbot_key_mode"] = keybind.getMode(Config::aimbot);
            j["autofire"] = Config::autofire;
            j["autofire_silent"] = Config::autofire_silent;
            j["autofire_hitchance"] = Config::autofire_hitchance;
            j["autofire_autostop"] = Config::autofire_autostop;
            j["autofire_autowall"] = Config::autofire_autowall;
            j["autofire_mindamage"] = Config::autofire_mindamage;
            j["autofire_mindamage_aw"] = Config::autofire_mindamage_aw;
            j["autofire_target_select"] = Config::autofire_target_select;
            j["autofire_multipoint_dynamic"] = Config::autofire_multipoint_dynamic;
            {
                nlohmann::json afHb = nlohmann::json::array();
                nlohmann::json afScale = nlohmann::json::array();
                for (int i = 0; i < Config::AF_MP_COUNT; ++i) {
                    afHb.push_back(Config::autofire_hitboxes[i]);
                    afScale.push_back(Config::autofire_multipoint_scale[i]);
                }
                j["autofire_hitboxes"] = afHb;
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
                    jp["vis_check"] = p.aim_vis_check;
                    jp["reaction_ms"] = p.aim_reaction_delay_ms;
                    jp["switch_ms"] = p.aim_target_switch_delay_ms;
                    jp["first_shot_ms"] = p.aim_first_shot_delay_ms;
                    jp["rcs"] = p.rcs;
                    jp["rcs_standalone"] = p.rcs_standalone;
                    jp["rcs_x"] = p.rcs_scale_x;
                    jp["rcs_y"] = p.rcs_scale_y;
                    jp["af_silent"] = p.autofire_silent;
                    jp["af_hc"] = p.autofire_hitchance;
                    jp["af_autostop"] = p.autofire_autostop;
                    jp["af_autowall"] = p.autofire_autowall;
                    jp["af_mindmg"] = p.autofire_mindamage;
                    jp["af_mindmg_aw"] = p.autofire_mindamage_aw;
                    jp["af_target"] = p.autofire_target_select;
                    jp["af_mp_dyn"] = p.autofire_multipoint_dynamic;
                    jp["tr_delay"] = p.trigger_delay_ms;
                    jp["tr_hc"] = p.trigger_hitchance;
                    jp["tr_aw"] = p.trigger_autowall;
                    jp["tr_scoped"] = p.trigger_scoped_only;
                    jp["tr_autostop"] = p.trigger_autostop;
                    jp["tr_mode"] = p.trigger_mode;
                    nlohmann::json trHb = nlohmann::json::array();
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        trHb.push_back(p.trigger_hitboxes[i]);
                    jp["tr_hitboxes"] = trHb;
                    nlohmann::json hb = nlohmann::json::array();
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        hb.push_back(p.aim_hitboxes[i]);
                    jp["hitboxes"] = hb;
                    nlohmann::json afHb = nlohmann::json::array();
                    nlohmann::json afSc = nlohmann::json::array();
                    for (int i = 0; i < Config::AF_MP_COUNT; ++i) {
                        afHb.push_back(p.autofire_hitboxes[i]);
                        afSc.push_back(p.autofire_multipoint_scale[i]);
                    }
                    jp["af_hitboxes"] = afHb;
                    jp["af_mp_scale"] = afSc;
                    groups.push_back(jp);
                }
                j["weapon_profiles"] = groups;
            }
            j["fov_circle"] = Config::fov_circle;
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
            j["bhop"] = Config::bhop;
            j["autostrafe"] = Config::autostrafe;
            j["autostrafe_mode"] = Config::autostrafe_mode;

            j["enemyChamsInvisible"] = Config::enemyChamsInvisible;
            j["enemyChams"] = Config::enemyChams;
            j["teamChams"] = Config::teamChams;
            j["teamChamsInvisible"] = Config::teamChamsInvisible;
            j["chamsMaterial"] = Config::chamsMaterial;
            j["chamsMaterialXQZ"] = Config::chamsMaterialXQZ;
            j["armChamsMaterial"] = Config::armChamsMaterial;
            j["viewmodelChamsMaterial"] = Config::viewmodelChamsMaterial;
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
            j["fovCircleColor"] = {
                Config::fovCircleColor.x,
                Config::fovCircleColor.y,
                Config::fovCircleColor.z,
                Config::fovCircleColor.w
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

            // Clear previous config state so missing keys don't leak across switches
            Config::ResetToDefaults();

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
                if (j.contains(key) && j[key].is_array() && j[key].size() == 4) {
                    auto arr = j[key];
                    col.x = arr[0].get<float>();
                    col.y = arr[1].get<float>();
                    col.z = arr[2].get<float>();
                    col.w = arr[3].get<float>();
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
                && !j.contains("night_exposure"))
                Config::night_exposure = j["NightColor"][3].get<float>();
            if (Config::night_exposure < 0.f) Config::night_exposure = 0.f;
            if (Config::night_exposure > 1.f) Config::night_exposure = 1.f;
            Config::skybox = j.value("skybox", false);
            loadCol("skybox_color", Config::skybox_color);
            Config::lighting = j.value("lighting", false);
            loadCol("lighting_color", Config::lighting_color);
            Config::map_color = j.value("map_color", false);
            loadCol("map_color_value", Config::map_color_value);
            Config::weather = j.value("weather", false);
            Config::weather_mode = j.value("weather_mode", 1);
            if (Config::weather_mode < 0 || Config::weather_mode > 3)
                Config::weather_mode = 1;
            Config::weather_intensity = j.value("weather_intensity", 0.55f);
            if (Config::weather_intensity < 0.f) Config::weather_intensity = 0.f;
            if (Config::weather_intensity > 1.f) Config::weather_intensity = 1.f;

            Config::flag_flashed = j.value("flag_flashed", false);
            Config::flag_bomb = j.value("flag_bomb", false);
            Config::flag_scoped = j.value("flag_scoped", false);
            Config::flag_reloading = j.value("flag_reloading", false);
            Config::flag_defusing = j.value("flag_defusing", false);

            Config::world_esp_weapons = j.value("world_esp_weapons", false);
            Config::world_esp_bomb = j.value("world_esp_bomb", true);
            Config::world_esp_smoke = j.value("world_esp_smoke", false);
            Config::world_esp_molotov = j.value("world_esp_molotov", false);
            Config::world_esp_he = j.value("world_esp_he", false);
            Config::world_esp_flash = j.value("world_esp_flash", false);
            Config::world_esp_decoy = j.value("world_esp_decoy", false);
            Config::world_esp_distance = j.value("world_esp_distance", false);
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

            Config::enemyChamsInvisible = j.value("enemyChamsInvisible", false);
            Config::enemyChams = j.value("enemyChams", false);
            Config::teamChams = j.value("teamChams", false);
            Config::teamChamsInvisible = j.value("teamChamsInvisible", false);
            Config::chamsMaterial = j.value("chamsMaterial", 0);
            Config::chamsMaterialXQZ = j.value("chamsMaterialXQZ", Config::chamsMaterial);
            Config::armChamsMaterial = j.value("armChamsMaterial", Config::chamsMaterial);
            Config::viewmodelChamsMaterial = j.value("viewmodelChamsMaterial", Config::chamsMaterial);
            loadCol("colVisualChams", Config::colVisualChams);
            loadCol("colVisualChamsIgnoreZ", Config::colVisualChamsIgnoreZ);
            loadCol("teamcolVisualChamsIgnoreZ", Config::teamcolVisualChamsIgnoreZ);
            loadCol("teamcolVisualChams", Config::teamcolVisualChams);

            Config::fov_circle = j.value("fov_circle", false);
            loadCol("fovCircleColor", Config::fovCircleColor);
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
            loadCol("widget_keybinds_accent", Config::widget_keybinds_accent);
            loadCol("widget_bomb_accent", Config::widget_bomb_accent);
            loadCol("widget_bomb_urgent", Config::widget_bomb_urgent);
            loadCol("widget_spectators_accent", Config::widget_spectators_accent);
            if (j.contains("widget_keybinds_pos") && j["widget_keybinds_pos"].is_array()
                && j["widget_keybinds_pos"].size() == 2) {
                Config::widget_keybinds_pos.x = j["widget_keybinds_pos"][0].get<float>();
                Config::widget_keybinds_pos.y = j["widget_keybinds_pos"][1].get<float>();
            }
            if (j.contains("widget_bomb_pos") && j["widget_bomb_pos"].is_array()
                && j["widget_bomb_pos"].size() == 2) {
                Config::widget_bomb_pos.x = j["widget_bomb_pos"][0].get<float>();
                Config::widget_bomb_pos.y = j["widget_bomb_pos"][1].get<float>();
            }
            if (j.contains("widget_spectators_pos") && j["widget_spectators_pos"].is_array()
                && j["widget_spectators_pos"].size() == 2) {
                Config::widget_spectators_pos.x = j["widget_spectators_pos"][0].get<float>();
                Config::widget_spectators_pos.y = j["widget_spectators_pos"][1].get<float>();
            }
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
                    for (int i = 0; i < Config::HB_COUNT && i < (int)arr.size(); ++i)
                        Config::aim_hitboxes[i] = arr[i].get<bool>();
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

            Config::autofire = j.value("autofire", false);
            Config::autofire_silent = j.value("autofire_silent", false);
            Config::autofire_hitchance = j.value("autofire_hitchance", 70.f);
            Config::autofire_autostop = j.value("autofire_autostop", false);
            Config::autofire_autowall = j.value("autofire_autowall", false);
            Config::autofire_mindamage = j.value("autofire_mindamage", 1.f);
            Config::autofire_mindamage_aw = j.value("autofire_mindamage_aw", 1.f);
            Config::autofire_target_select = j.value("autofire_target_select", Config::AF_TARGET_CROSSHAIR);
            if (Config::autofire_target_select < 0 || Config::autofire_target_select >= Config::AF_TARGET_COUNT)
                Config::autofire_target_select = Config::AF_TARGET_CROSSHAIR;
            Config::autofire_multipoint_dynamic = j.value("autofire_multipoint_dynamic", true);
            {
                for (int i = 0; i < Config::AF_MP_COUNT; ++i) {
                    Config::autofire_hitboxes[i] = false;
                    Config::autofire_multipoint_scale[i] = 0.55f;
                }
                if (j.contains("autofire_hitboxes") && j["autofire_hitboxes"].is_array()) {
                    auto& arr = j["autofire_hitboxes"];
                    for (int i = 0; i < Config::AF_MP_COUNT && i < (int)arr.size(); ++i)
                        Config::autofire_hitboxes[i] = arr[i].get<bool>();
                } else {
                    Config::autofire_hitboxes[Config::AF_MP_HEAD] = true;
                    Config::autofire_hitboxes[Config::AF_MP_CHEST] = true;
                }
                if (j.contains("autofire_multipoint_scale") && j["autofire_multipoint_scale"].is_array()) {
                    auto& arr = j["autofire_multipoint_scale"];
                    for (int i = 0; i < Config::AF_MP_COUNT && i < (int)arr.size(); ++i)
                        Config::autofire_multipoint_scale[i] = arr[i].get<float>();
                }
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

            if (j.contains("weapon_profiles") && j["weapon_profiles"].is_array()) {
                auto& arr = j["weapon_profiles"];
                const int n = (std::min)((int)Config::WG_COUNT, (int)arr.size());
                for (int g = 0; g < n; ++g) {
                    auto& jp = arr[g];
                    if (!jp.is_object()) continue;
                    auto& p = Config::weapon_profiles[g];
                    p.aimbot_fov = jp.value("fov", p.aimbot_fov);
                    p.aimbot_smooth = jp.value("smooth", p.aimbot_smooth);
                    p.aim_vis_check = jp.value("vis_check", p.aim_vis_check);
                    p.aim_reaction_delay_ms = jp.value("reaction_ms", p.aim_reaction_delay_ms);
                    p.aim_target_switch_delay_ms = jp.value("switch_ms", p.aim_target_switch_delay_ms);
                    p.aim_first_shot_delay_ms = jp.value("first_shot_ms", p.aim_first_shot_delay_ms);
                    p.rcs = jp.value("rcs", p.rcs);
                    p.rcs_standalone = jp.value("rcs_standalone", p.rcs_standalone);
                    p.rcs_scale_x = jp.value("rcs_x", p.rcs_scale_x);
                    p.rcs_scale_y = jp.value("rcs_y", p.rcs_scale_y);
                    p.autofire_silent = jp.value("af_silent", p.autofire_silent);
                    p.autofire_hitchance = jp.value("af_hc", p.autofire_hitchance);
                    p.autofire_autostop = jp.value("af_autostop", p.autofire_autostop);
                    p.autofire_autowall = jp.value("af_autowall", p.autofire_autowall);
                    p.autofire_mindamage = jp.value("af_mindmg", p.autofire_mindamage);
                    p.autofire_mindamage_aw = jp.value("af_mindmg_aw", p.autofire_mindamage_aw);
                    p.autofire_target_select = jp.value("af_target", p.autofire_target_select);
                    p.autofire_multipoint_dynamic = jp.value("af_mp_dyn", p.autofire_multipoint_dynamic);
                    p.trigger_delay_ms = jp.value("tr_delay", p.trigger_delay_ms);
                    p.trigger_hitchance = jp.value("tr_hc", p.trigger_hitchance);
                    p.trigger_autowall = jp.value("tr_aw", p.trigger_autowall);
                    p.trigger_scoped_only = jp.value("tr_scoped", p.trigger_scoped_only);
                    p.trigger_autostop = jp.value("tr_autostop", p.trigger_autostop);
                    // Migrate old tr_spread_comp bool → mode
                    if (jp.contains("tr_mode"))
                        p.trigger_mode = jp.value("tr_mode", p.trigger_mode);
                    else if (jp.value("tr_spread_comp", false))
                        p.trigger_mode = Config::TR_MODE_SEED_NOSPREAD;
                    if (p.trigger_mode < 0 || p.trigger_mode >= Config::TR_MODE_COUNT)
                        p.trigger_mode = Config::TR_MODE_HITCHANCE;
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        p.trigger_hitboxes[i] = false;
                    if (jp.contains("tr_hitboxes") && jp["tr_hitboxes"].is_array()) {
                        auto& thb = jp["tr_hitboxes"];
                        for (int i = 0; i < Config::HB_COUNT && i < (int)thb.size(); ++i)
                            p.trigger_hitboxes[i] = thb[i].get<bool>();
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
                        auto& hb = jp["hitboxes"];
                        for (int i = 0; i < Config::HB_COUNT && i < (int)hb.size(); ++i)
                            p.aim_hitboxes[i] = hb[i].get<bool>();
                    }
                    if (jp.contains("af_hitboxes") && jp["af_hitboxes"].is_array()) {
                        auto& hb = jp["af_hitboxes"];
                        for (int i = 0; i < Config::AF_MP_COUNT && i < (int)hb.size(); ++i)
                            p.autofire_hitboxes[i] = hb[i].get<bool>();
                    }
                    if (jp.contains("af_mp_scale") && jp["af_mp_scale"].is_array()) {
                        auto& sc = jp["af_mp_scale"];
                        for (int i = 0; i < Config::AF_MP_COUNT && i < (int)sc.size(); ++i)
                            p.autofire_multipoint_scale[i] = sc[i].get<float>();
                    }
                }
            } else {
                // Migrate old flat config into every group
                for (int g = 0; g < Config::WG_COUNT; ++g)
                    Config::PullLiveIntoProfile(g);
            }
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
            Config::scope_no_overlay = j.value("scope_no_overlay", false);
            Config::scope_remove_blur = j.value("scope_remove_blur", false);
            Config::scope_remove_bars = j.value("scope_remove_bars", false);
            Config::scope_remove_texture = j.value("scope_remove_texture", false);
            Config::scope_custom_look = j.value("scope_custom_look", false);
            Config::scope_size_scale = j.value("scope_size_scale", 1.f);
            Config::scope_blur_amount = j.value("scope_blur_amount", 0.f);
            Config::bhop = j.value("bhop", false);
            Config::autostrafe = j.value("autostrafe", false);
            Config::autostrafe_mode = j.value("autostrafe_mode", 0);
            if (Config::autostrafe_mode < 0 || Config::autostrafe_mode > 1)
                Config::autostrafe_mode = 0;

            Config::knife_changer = j.value("knife_changer", false);
            Config::knife_index = j.value("knife_index", 0);
            Config::knife_paint_kit = j.value("knife_paint_kit", 0);
            Config::knife_paint_kit_id = j.value("knife_paint_kit_id", 0);
            Config::knife_wear = j.value("knife_wear", 0.0001f);
            if (Config::knife_wear < 0.0001f) Config::knife_wear = 0.0001f; else if (Config::knife_wear > 1.0f) Config::knife_wear = 1.0f;
            Config::knife_seed = j.value("knife_seed", 0);
            {
                const std::string cn = j.value("knife_custom_name", std::string{});
                strncpy_s(Config::knife_custom_name, cn.c_str(), _TRUNCATE);
            }

            Config::glove_changer = j.value("glove_changer", false);
            Config::glove_index = j.value("glove_index", 0);
            Config::glove_paint_kit = j.value("glove_paint_kit", 0);
            Config::glove_paint_kit_id = j.value("glove_paint_kit_id", 0);
            Config::glove_wear = j.value("glove_wear", 0.0001f);
            if (Config::glove_wear < 0.0001f) Config::glove_wear = 0.0001f; else if (Config::glove_wear > 1.0f) Config::glove_wear = 1.0f;
            Config::glove_seed = j.value("glove_seed", 0);

            Config::weapon_skins = j.value("weapon_skins", false);
            Config::weapon_selected = j.value("weapon_selected", 7);
            for (int i = 0; i < 71; ++i)
                Config::weapon_skin[i] = {};
            if (j.contains("weapon_skin") && j["weapon_skin"].is_object()) {
                for (auto it = j["weapon_skin"].begin(); it != j["weapon_skin"].end(); ++it) {
                    int def = 0;
                    try { def = std::stoi(it.key()); } catch (...) { continue; }
                    if (def < 1 || def > 70 || !it.value().is_object()) continue;
                    Config::weapon_skin[def].paint_kit = it.value().value("paint_kit", 0);
                    Config::weapon_skin[def].paint_kit_id = it.value().value("paint_kit_id", 0);
                    Config::weapon_skin[def].wear = it.value().value("wear", 0.0001f);
                    if (Config::weapon_skin[def].wear < 0.0001f) Config::weapon_skin[def].wear = 0.0001f; else if (Config::weapon_skin[def].wear > 1.0f) Config::weapon_skin[def].wear = 1.0f;
                    Config::weapon_skin[def].seed = it.value().value("seed", 0);
                }
            }

            Config::agent_changer = j.value("agent_changer", false);
            Config::agent_ct_def = j.value("agent_ct_def", 0);
            Config::agent_t_def = j.value("agent_t_def", 0);

            Config::armChams = j.value("armChams", false);
            Config::viewmodelChams = j.value("viewmodelChams", false);

            // Prefer new keys; fall back to legacy armChams_color / viewmodelChams_color
            if (j.contains("colArmChams") && j["colArmChams"].is_array() && j["colArmChams"].size() == 4) {
                auto arr = j["colArmChams"];
                Config::colArmChams.x = arr[0].get<float>();
                Config::colArmChams.y = arr[1].get<float>();
                Config::colArmChams.z = arr[2].get<float>();
                Config::colArmChams.w = arr[3].get<float>();
            } else if (j.contains("armChams_color") && j["armChams_color"].is_array() && j["armChams_color"].size() == 4) {
                auto arr = j["armChams_color"];
                Config::colArmChams.x = arr[0].get<float>();
                Config::colArmChams.y = arr[1].get<float>();
                Config::colArmChams.z = arr[2].get<float>();
                Config::colArmChams.w = arr[3].get<float>();
            }

            if (j.contains("colViewmodelChams") && j["colViewmodelChams"].is_array() && j["colViewmodelChams"].size() == 4) {
                auto arr = j["colViewmodelChams"];
                Config::colViewmodelChams.x = arr[0].get<float>();
                Config::colViewmodelChams.y = arr[1].get<float>();
                Config::colViewmodelChams.z = arr[2].get<float>();
                Config::colViewmodelChams.w = arr[3].get<float>();
            } else if (j.contains("viewmodelChams_color") && j["viewmodelChams_color"].is_array() && j["viewmodelChams_color"].size() == 4) {
                auto arr = j["viewmodelChams_color"];
                Config::colViewmodelChams.x = arr[0].get<float>();
                Config::colViewmodelChams.y = arr[1].get<float>();
                Config::colViewmodelChams.z = arr[2].get<float>();
                Config::colViewmodelChams.w = arr[3].get<float>();
            }

            if (j.contains("colVisualChams") && j["colVisualChams"].is_array() && j["colVisualChams"].size() == 4)
            {
                auto arr = j["colVisualChams"];
                Config::colVisualChams.x = arr[0].get<float>();
                Config::colVisualChams.y = arr[1].get<float>();
                Config::colVisualChams.z = arr[2].get<float>();
                Config::colVisualChams.w = arr[3].get<float>();
            }

            if (j.contains("colVisualChamsIgnoreZ") && j["colVisualChamsIgnoreZ"].is_array() && j["colVisualChamsIgnoreZ"].size() == 4)
            {
                auto arr = j["colVisualChamsIgnoreZ"];
                Config::colVisualChamsIgnoreZ.x = arr[0].get<float>();
                Config::colVisualChamsIgnoreZ.y = arr[1].get<float>();
                Config::colVisualChamsIgnoreZ.z = arr[2].get<float>();
                Config::colVisualChamsIgnoreZ.w = arr[3].get<float>();
            }

            if (j.contains("teamcolVisualChamsIgnoreZ") && j["teamcolVisualChamsIgnoreZ"].is_array() && j["teamcolVisualChamsIgnoreZ"].size() == 4)
            {
                auto arr = j["teamcolVisualChamsIgnoreZ"];
                Config::teamcolVisualChamsIgnoreZ.x = arr[0].get<float>();
                Config::teamcolVisualChamsIgnoreZ.y = arr[1].get<float>();
                Config::teamcolVisualChamsIgnoreZ.z = arr[2].get<float>();
                Config::teamcolVisualChamsIgnoreZ.w = arr[3].get<float>();
            }

            if (j.contains("teamcolVisualChams") && j["teamcolVisualChams"].is_array() && j["teamcolVisualChams"].size() == 4)
            {
                auto arr = j["teamcolVisualChams"];
                Config::teamcolVisualChams.x = arr[0].get<float>();
                Config::teamcolVisualChams.y = arr[1].get<float>();
                Config::teamcolVisualChams.z = arr[2].get<float>();
                Config::teamcolVisualChams.w = arr[3].get<float>();
            }

            if (j.contains("fovCircleColor") && j["fovCircleColor"].is_array() && j["fovCircleColor"].size() == 4) {
                auto arr = j["fovCircleColor"];
                Config::fovCircleColor.x = arr[0].get<float>();
                Config::fovCircleColor.y = arr[1].get<float>();
                Config::fovCircleColor.z = arr[2].get<float>();
                Config::fovCircleColor.w = arr[3].get<float>();
            }

            // Keep Config::*key mirrors in sync with live keybind state
            Config::aimbot_key = keybind.getKey(Config::aimbot);
            Config::aimbot_key_mode = keybind.getMode(Config::aimbot);
            Config::autofire_key = keybind.getKey(Config::autofire);
            Config::autofire_key_mode = keybind.getMode(Config::autofire);
            Config::triggerbot_key = keybind.getKey(Config::triggerbot);
            Config::triggerbot_key_mode = keybind.getMode(Config::triggerbot);
            Config::thirdperson_key = keybind.getKey(Config::thirdperson);
            Config::thirdperson_key_mode = keybind.getMode(Config::thirdperson);

            // Config switch must never leave a previous Toggle latch active
            keybind.clearAllToggles();

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
