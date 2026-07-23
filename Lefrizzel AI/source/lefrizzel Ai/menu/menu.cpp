#include "menu.h"
#include "../config/config.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include "../config/configmanager.h"

#include "../keybinds/keybinds.h"
#include "../features/skinchanger/skinchanger.h"
#include "../features/skinchanger/custom_paint.h"
#include "../features/skinchanger/agent_changer.h"
#include "../features/skinchanger/knife_custom.h"
#include "../features/skinchanger/skin_preview.h"
#include "../features/gamemode/gamemode.h"
#include "../features/nade_lineup/nade_lineup.h"
#include "../features/notify/notify.h"
#include "../features/hitsound/hitsound.h"
#include "../features/hitmarker/hitmarker.h"
#include <string>

#include "../utils/logging/log.h"
#include "../utils/console/console.h"
#include "../utils/security/vacdetect.h"
#include "../features/visuals/assets/obs_icons.hpp"
#include "icons/fa_solid_font.hpp"

ImFont* g_WeaponIconFont = nullptr;
ImFont* g_MenuIconFont = nullptr;

// Font Awesome 6 Free solid codepoints (UTF-8) for sidebar tabs
namespace MenuTabIcon {
    // crosshairs f05b, eye f06e, palette f53f, sliders f1de, folder-open f07c
    static const char* kAim     = "\xef\x81\x9b"; // U+F05B
    static const char* kVisuals = "\xef\x81\xae"; // U+F06E
    static const char* kSkin    = "\xef\x94\xbf"; // U+F53F
    static const char* kMisc    = "\xef\x87\x9e"; // U+F1DE
    static const char* kConfig  = "\xef\x81\xbc"; // U+F07C
    static const char* const kAll[5] = { kAim, kVisuals, kSkin, kMisc, kConfig };
}

#include "menu_ui.h"

// Thin layout facade — chrome lives in menu_ui.*; skin widgets stay local.
namespace ui {
    using MenuUI::Accent;
    using MenuUI::TextMuted;
    using MenuUI::TextBright;
    using MenuUI::WithA;
    using MenuUI::ToU32;
    using MenuUI::AccentU32;
    using MenuUI::BorderU32;
    using MenuUI::DrawTopSheen;
    using MenuUI::DrawWindowFrame;

    static void SectionLabel(const char* label) { MenuUI::Section(label); }
    static void BeginCard(const char* id, float width, bool autoY = false) {
        MenuUI::BeginCard(id, width, autoY);
    }
    static void EndCard() { MenuUI::EndCard(); }
    static void BeginStrip(const char* id) { MenuUI::BeginStrip(id); }
    static void EndStrip() { MenuUI::EndStrip(); }

    static bool SliderFull(const char* label, const char* id, float* v, float vmin, float vmax,
                           const char* fmt = "%.3f") {
        return MenuUI::Slider(label, id, v, vmin, vmax, fmt);
    }
    static bool ComboFull(const char* label, const char* id, int* cur, const char* const items[],
                          int count) {
        return MenuUI::Combo(label, id, cur, items, count);
    }
    static bool ColorQuadRow(const char* idPrefix, float colors[16]) {
        return MenuUI::ColorQuadRow(idPrefix, colors);
    }
    static bool FeatureToggle(const char* label, bool* v, const char* tip = nullptr) {
        return MenuUI::FeatureToggle(label, v, tip);
    }
    static void PopupTitle(const char* title) { MenuUI::PopupTitle(title); }

    static bool NavButton(const char* label, const char* iconUtf8, bool selected, const ImVec2& size) {
        return MenuUI::NavButton(label, iconUtf8, selected, size);
    }
    static bool NavButton(const char* label, bool selected, const ImVec2& size) {
        return MenuUI::NavButton(label, selected, size);
    }
    static void SubNav(const char* const* labels, int count, int* selected) {
        MenuUI::SubNav(labels, count, selected);
    }
    static void ApplyMenuPreset(int idx) { MenuUI::ApplyPreset(idx); }

    static bool StrContainsI(const char* hay, const char* needle) {
        if (!needle || !*needle) return true;
        if (!hay || !*hay) return false;
        const size_t nlen = std::strlen(needle);
        const size_t hlen = std::strlen(hay);
        if (nlen > hlen) return false;
        for (size_t i = 0; i + nlen <= hlen; ++i) {
            bool ok = true;
            for (size_t j = 0; j < nlen; ++j) {
                const unsigned char a = (unsigned char)hay[i + j];
                const unsigned char b = (unsigned char)needle[j];
                if (std::tolower(a) != std::tolower(b)) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }


    static const char* RarityLabel(int r) {
        static const char* names[] = {
            "Default", "Consumer Grade", "Industrial Grade", "Mil-Spec",
            "Restricted", "Classified", "Covert", "Extraordinary"
        };
        if (r < 0 || r > 7) return "Other";
        return names[r];
    }


    static ImU32 RarityColor(int r) {
        static const ImU32 cols[] = {
            IM_COL32(176, 195, 217, 255),
            IM_COL32(176, 195, 217, 255),
            IM_COL32(94, 152, 217, 255),
            IM_COL32(75, 105, 255, 255),
            IM_COL32(136, 71, 255, 255),
            IM_COL32(211, 44, 230, 255),
            IM_COL32(235, 75, 75, 255),
            IM_COL32(228, 174, 57, 255)
        };
        return cols[(r >= 0 && r < 8) ? r : 1];
    }


    static void DrawRarityStripeColored(ImU32 col) {
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p0.x + 1.f, p0.y + 1.f),
            ImVec2(p0.x + 4.f, p1.y - 1.f), col);
    }


    // Searchable paint-kit combo grouped by rarity (high → low). Returns true if selection changed.
    // simpleName: schema simple weapon name for hover/preview images (nullable).
    static bool SkinPaintCombo(const char* comboId, const char* searchId,
        char* search, int searchCap,
        const std::vector<SkinChanger::KnifePaintKit>& kits,
        int* paintIdx, int* paintId,
        const char* simpleName = nullptr)
    {
        if (!paintIdx || !paintId || kits.empty()) return false;

        // paint_kit_id is source of truth — resolve list index from id when stale
        if (*paintId > 0) {
            bool idxOk = (*paintIdx > 0 && *paintIdx < (int)kits.size()
                && kits[*paintIdx].id == *paintId);
            if (!idxOk) {
                int found = -1;
                for (int i = 0; i < (int)kits.size(); ++i) {
                    if (kits[i].id == *paintId) { found = i; break; }
                }
                if (found >= 0)
                    *paintIdx = found;
            }
        }

        if (*paintIdx < 0 || *paintIdx >= (int)kits.size())
            *paintIdx = 0;

        // Sync id from index only when index is a real selection.
        // NEVER invent paintId from a bare index while kits incomplete — that
        // applied random/wrong kits on weapons user never picked.
        if (*paintIdx > 0 && *paintIdx < (int)kits.size() && kits.size() > 1)
            *paintId = kits[*paintIdx].id;
        else if (*paintIdx <= 0) {
            // Explicit Vanilla only when full kit list present
            if (kits.size() > 1 && *paintId > 0) {
                // id set but index not resolved — keep id
            } else if (kits.size() > 1) {
                *paintId = 0;
            }
            // kits.size()<=1: leave paintId alone (loading)
        }

        const char* preview = (*paintIdx >= 0 && *paintIdx < (int)kits.size())
            ? kits[*paintIdx].name.c_str() : "Vanilla";
        ImGui::TextUnformatted("Skin");
        ImGui::SetNextItemWidth(-1.f);
        bool changed = false;
        if (!ImGui::BeginCombo(comboId, preview, ImGuiComboFlags_HeightLarge))
            return false;

        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint(searchId, "Search skins...", search, searchCap);
        ImGui::Separator();

        // Build filtered index list sorted: index 0 (Vanilla) first, then by rarity desc
        std::vector<int> order;
        order.reserve(kits.size());
        for (int i = 0; i < (int)kits.size(); ++i) {
            if (!StrContainsI(kits[i].name.c_str(), search))
                continue;
            order.push_back(i);
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            if (a == 0) return true;
            if (b == 0) return false;
            if (kits[a].rarity != kits[b].rarity)
                return kits[a].rarity > kits[b].rarity;
            return kits[a].name < kits[b].name;
        });

        int lastRarity = -999;
        int shown = 0;
        for (int i : order) {
            const int r = kits[i].rarity;
            if (i != 0 && r != lastRarity) {
                ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
                ImGui::TextUnformatted(RarityLabel(r));
                ImGui::PopStyleColor();
                lastRarity = r;
            } else if (i == 0) {
                lastRarity = -1;
            }

            ImGui::PushID(comboId);
            ImGui::PushID(i);
            const bool sel = (*paintIdx == i);
            if (ImGui::Selectable(kits[i].name.c_str(), sel)) {
                *paintIdx = i;
                *paintId = (i <= 0) ? 0 : kits[i].id;
                changed = true;
            }
            DrawRarityStripeColored(RarityColor(r));
            if (simpleName && *simpleName) {
                SkinPreview::DrawHover(SkinPreview::GetPaint(
                    simpleName, kits[i].token.c_str(), kits[i].id));
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
            ImGui::TextUnformatted("No skins match.");
            ImGui::PopStyleColor();
        }
        ImGui::EndCombo();
        return changed;
    }


    // Seed colour bugs from the selected paint kit. Re-runs when kit id changes.
    // GPU sample can SEH (D3D staging) — must not run naked inside BeginCard or
    // EndChild is skipped → ImGui "Missing EndChild()" assert.
    // Returns true when pickers were freshly filled (caller should invalidate skin).
    static CustomPaint::SeedStatus SafeSeedPaintKit(int paintKitId, float colors[16],
        const char* simpleName, const char* kitToken)
    {
        CustomPaint::SeedStatus st = CustomPaint::SeedStatus::Fail;
        __try {
            st = CustomPaint::SeedFromPaintKit(paintKitId, colors, simpleName, kitToken);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Con::Seh("CustomPaint.Seed", GetExceptionCode());
            CustomPaint::SetNeutral(colors);
            st = CustomPaint::SeedStatus::Fail;
        }
        return st;
    }


    static bool SyncCustomColorPickers(bool enabled, int paintKitId,
        float colors[16], bool* active, int* lastSeededId,
        const char* simpleName, const char* kitToken, bool* userEdited)
    {
        if (!enabled) {
            *lastSeededId = -1;
            return false;
        }
        // User already picked colours — never reseed
        if (userEdited && *userEdited) {
            *lastSeededId = paintKitId;
            *active = true;
            return false;
        }
        if (paintKitId <= 0) {
            if (*lastSeededId == 0)
                return false;
            CustomPaint::SetNeutral(colors);
            *active = false;
            if (userEdited) *userEdited = false;
            *lastSeededId = 0;
            return true;
        }
        auto isWhite = [](const float c[16]) {
            for (int i = 0; i < 4; ++i)
                if (c[i * 4] < 0.97f || c[i * 4 + 1] < 0.97f || c[i * 4 + 2] < 0.97f)
                    return false;
            return true;
        };
        // Done for this kit with real (non-white) colours
        if (*lastSeededId == paintKitId && *active && !isWhite(colors))
            return false;

        const auto st = SafeSeedPaintKit(paintKitId, colors, simpleName, kitToken);
        // Seed always writes non-white into colors (defaults / kit / preview).
        // Pending: show defaults in pickers, do NOT arm inject yet (avoids wash).
        if (st == CustomPaint::SeedStatus::Pending) {
            *active = false;
            return false;
        }
        if (st == CustomPaint::SeedStatus::Ok) {
            *active = !isWhite(colors);
            *lastSeededId = paintKitId;
            return *active;
        }
        *active = false;
        return false;
    }


    static const char* KitTokenAt(const std::vector<SkinChanger::KnifePaintKit>& kits, int idx) {
        if (idx < 0 || idx >= (int)kits.size()) return "";
        return kits[idx].token.c_str();
    }


    // Categorized weapon picker with search. Returns true if selection changed.
    static bool WeaponCategoryCombo(const char* comboId, const char* searchId,
        char* search, int searchCap, int* selectedDef)
    {
        const auto* menu = SkinChanger::WeaponMenu();
        const int n = SkinChanger::WeaponMenuCount();
        const char* preview = "AK-47";
        for (int i = 0; i < n; ++i) {
            if ((int)menu[i].def_index == *selectedDef) {
                preview = menu[i].display;
                break;
            }
        }

        ImGui::TextUnformatted("Weapon");
        ImGui::SetNextItemWidth(-1.f);
        bool changed = false;
        if (!ImGui::BeginCombo(comboId, preview, ImGuiComboFlags_HeightLargest))
            return false;

        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint(searchId, "Search weapons...", search, searchCap);
        ImGui::Separator();

        for (int cat = 0; cat <= 4; ++cat) {
            bool any = false;
            for (int i = 0; i < n; ++i) {
                if (menu[i].category != cat) continue;
                if (!StrContainsI(menu[i].display, search)) continue;
                any = true;
                break;
            }
            if (!any) continue;

            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::TextUnformatted(SkinChanger::WeaponCategoryName(cat));
            ImGui::PopStyleColor();

            for (int i = 0; i < n; ++i) {
                if (menu[i].category != cat) continue;
                if (!StrContainsI(menu[i].display, search)) continue;
                ImGui::PushID(3000 + i);
                const bool sel = (*selectedDef == (int)menu[i].def_index);
                if (ImGui::Selectable(menu[i].display, sel)) {
                    *selectedDef = (int)menu[i].def_index;
                    changed = true;
                }
                // Hover preview only for selected/focused row — not every item
                // (was N panorama loads while scrolling weapon list).
                if (sel && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
                    const char* sn = SkinChanger::SimpleNameFor(menu[i].def_index);
                    if (sn && *sn)
                        SkinPreview::DrawHover(SkinPreview::Get(SkinPreview::ModelPath(sn)));
                }
                if (sel) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
        return changed;
    }


    // optionalName: knife subclass (weapon_knife_karambit) when schema name missing
    static void DrawSkinPreviewPanel(std::uint16_t def, const char* kitToken, int paintKitId,
        const char* optionalName = nullptr)
    {
        if (def == 0 && (!optionalName || !*optionalName)) {
            SkinPreview::DrawPanel(ImTextureID_Invalid, 160.f);
            return;
        }
        // Static SimpleNameFor first (weapon_knife_m9_bayonet). Subclass last.
        // Never use bare "weapon_knife" — that is default CT knife icon.
        const char* sn = (def != 0) ? SkinChanger::SimpleNameFor(def) : nullptr;
        if ((!sn || !*sn || std::strcmp(sn, "weapon_knife") == 0)
            && optionalName && *optionalName)
            sn = optionalName;
        if (!sn || !*sn) {
            SkinPreview::DrawPanel(ImTextureID_Invalid, 160.f);
            return;
        }
        SkinPreview::DrawPanel(SkinPreview::GetPaint(sn, kitToken, paintKitId), 160.f);
    }


} // namespace ui

void ApplyImGuiTheme() {
    MenuUI::ApplyTheme();
}


Menu::Menu() {
    activeTab = 0;
    showMenu = false; // must start closed — open menu disables relative mouse and drifts the cursor
}


// Multi-select Smoke / Flash / Scope checks (aimbot / autofire / trigger)
static void ChecksDropdown(const char* label, const char* id,
    bool* smoke, bool* flash, bool* scope)
{
    char preview[96];
    preview[0] = '\0';
    int n = 0;
    auto append = [&](const char* s) {
        if (n > 0) strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
        strncat_s(preview, sizeof(preview), s, _TRUNCATE);
        ++n;
    };
    if (smoke && *smoke) append("Smoke");
    if (flash && *flash) append("Flash");
    if (scope && *scope) append("Scope");
    if (n == 0) strcpy_s(preview, "None");

    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo(id, preview)) {
        if (smoke) {
            ImGui::Checkbox("Smoke Check", smoke);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Skip targets inside smoke.");
        }
        if (flash) {
            ImGui::Checkbox("Flash Check", flash);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pause aim/fire while you are flashed.");
        }
        if (scope) {
            ImGui::Checkbox("Scope Check", scope);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Only aim/fire when scoped (snipers).");
        }
        ImGui::EndCombo();
    }
}

// Scope Check is one flag on the profile (aim + AF + trigger).
// Never write live Config here — only when editing active group via PushLive.
static void SyncSharedScope(Config::AimWeaponProfile& p) {
    p.autofire_scoped_only = p.aim_scoped_only;
    p.trigger_scoped_only = p.aim_scoped_only;
}

// Push full edited profile → live only when UI group == held weapon group.
static void PushLiveIfEditingActive() {
    if (Config::weapon_group_ui == Config::weapon_group_active)
        Config::ApplyProfileToLive(Config::weapon_group_ui);
}

// Mode + bind: side-by-side when wide enough, else stacked.
// Explicit widths — never let Button(w=0) auto-size off the card edge.
static void KeybindRow(bool& feature) {
    const float gap = 8.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail < 220.f) {
        ImGui::TextDisabled("Mode");
        keybind.menuMode(feature, -1.f);
        ImGui::TextDisabled("Bind");
        keybind.menuButton(feature, -1.f);
        return;
    }

    const float colW = floorf((avail - gap) * 0.5f);

    ImGui::BeginGroup();
    ImGui::TextDisabled("Mode");
    keybind.menuMode(feature, colW);
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);

    ImGui::BeginGroup();
    ImGui::TextDisabled("Bind");
    // Use remaining line width (not colW) so rounding never overflows
    keybind.menuButton(feature, -1.f);
    ImGui::EndGroup();
}

// Stacked full-width keybind (thirdperson / lineup)
static void KeybindStack(bool& feature) {
    ImGui::TextDisabled("Mode");
    keybind.menuMode(feature, -1.f);
    ImGui::TextDisabled("Bind");
    keybind.menuButton(feature, -1.f);
}

// Shared multi-select hitbox combo
static void HitboxMultiSelect(const char* title, const char* id,
    bool* boxes, int count, const char* const* names,
    int idBase, const char* tip)
{
    char preview[96];
    preview[0] = '\0';
    int nSel = 0;
    for (int i = 0; i < count; ++i) {
        if (!boxes[i]) continue;
        if (nSel > 0)
            strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
        strncat_s(preview, sizeof(preview), names[i], _TRUNCATE);
        ++nSel;
    }
    if (nSel == 0)
        strcpy_s(preview, "None");

    ImGui::TextUnformatted(title);
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(idBase + i);
            if (ImGui::Checkbox(names[i], &boxes[i])) {
                bool any = false;
                for (int j = 0; j < count; ++j)
                    if (boxes[j]) { any = true; break; }
                if (!any)
                    boxes[i] = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (tip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
}

static bool IsEditingActiveGroup() {
    return Config::weapon_group_ui == Config::weapon_group_active;
}

void Menu::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;
    // SEH mid-frame (GPU paint-kit seed) can leave BeginChild open.
    // Recover stack without CRT assert popup (Missing EndChild).
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = true;
    io.ConfigErrorRecoveryEnableTooltip = false;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(pDevice, pContext);

    SkinPreview::Init(pDevice);

    MenuUI::ApplyTheme();

    // Prefer clean UI fonts
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 16.0f);

    // UC CS2 weapon ESP icon font (embedded TTF)
    {
        ImFontConfig iconCfg{};
        iconCfg.FontDataOwnedByAtlas = false;
        iconCfg.OversampleH = 1;
        iconCfg.OversampleV = 1;
        iconCfg.PixelSnapH = true;
        static const ImWchar iconRanges[] = { 0xE000, 0xE204, 0 };
        // Same pixel size as UI/ESP text font so icon band matches name/weapon text
        g_WeaponIconFont = io.Fonts->AddFontFromMemoryTTF(
            obs_icons::font_data,
            obs_icons::font_size,
            16.0f,
            &iconCfg,
            iconRanges);
    }

    // Font Awesome 6 Free Solid — sidebar tab icons (embedded, no runtime download)
    {
        ImFontConfig faCfg{};
        faCfg.FontDataOwnedByAtlas = false;
        faCfg.OversampleH = 2;
        faCfg.OversampleV = 2;
        faCfg.PixelSnapH = true;
        // Only glyphs we use (smaller atlas)
        static const ImWchar faRanges[] = {
            0xF05B, 0xF05B, // crosshairs — Aim
            0xF06E, 0xF06E, // eye — Visuals
            0xF53F, 0xF53F, // palette — Skin
            0xF1DE, 0xF1DE, // sliders — Misc
            0xF07C, 0xF07C, // folder-open — Config
            0
        };
        g_MenuIconFont = io.Fonts->AddFontFromMemoryTTF(
            MenuIconsFont::font_data,
            MenuIconsFont::font_size,
            15.0f,
            &faCfg,
            faRanges);
        if (!g_MenuIconFont)
            Con::Error("Menu icon font failed to load");
        else
            Con::Ok("Menu tab icons (Font Awesome solid)");
    }

    std::cout << "initialized menu\n";
}

void Menu::render() {
    keybind.pollInputs(showMenu);

    // Instant close: AnimTick(false) zeros state; no draw after toggle off
    MenuUI::AnimTick(showMenu);
    if (!showMenu || !MenuUI::AnimVisible()) {
        // DPI only while menu open — keep ESP/HUD at 1x
        if (ImGui::GetIO().FontGlobalScale != 1.f)
            ImGui::GetIO().FontGlobalScale = 1.f;
        return;
    }

    MenuUI::ApplyTheme();

    const MenuUI::Layout L = MenuUI::Layout::Current();
    const float openA = MenuUI::OpenAlpha();
    const float openY = MenuUI::OpenSlide();

    const float dpi = L.dpi;
    ImGui::SetNextWindowSize(ImVec2(L.windowW, L.windowH), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(80.f * dpi, 80.f * dpi), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(720.f * dpi, 480.f * dpi),
        ImVec2(1280.f * dpi, 900.f * dpi));
    ImGui::SetNextWindowBgAlpha((std::clamp)(Config::menu_opacity, 0.70f, 1.f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    // Open fade only (close never draws)
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (std::max)(0.85f, openA));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
        (std::clamp)((std::max)(Config::menu_rounding, 2.f), 2.f, 8.f));

    ImGui::Begin("Lefrizzel AI", nullptr, flags);

    if (openY > 0.1f)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + openY);

    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    const float round = ImGui::GetStyle().WindowRounding;
    const float pad = L.shellPad;

    ImGui::SetCursorPos(ImVec2(pad, pad + openY));
    const float bodyH = (std::max)(80.f, wsize.y - pad * 2.f - openY);

    // ── Rounded icon rail ─────────────────────────────────────────────
    MenuUI::BeginSidebar(L, bodyH);
    {
        MenuUI::SidebarBrandMark();

        static const char* tabs[] = { "Aim", "Visuals", "Skin", "Misc", "Config" };
        const float btnW = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < 5; ++i) {
            if (MenuUI::NavIcon(tabs[i], MenuTabIcon::kAll[i], activeTab == i,
                    ImVec2(btnW, L.navBtnH), i)) {
                activeTab = i;
                MenuUI::NotifyTab(i);
            }
        }

        if (VacDetect::IsSoftPaused()) {
            const float y = ImGui::GetWindowHeight()
                - ImGui::GetStyle().WindowPadding.y - 40.f;
            if (y > ImGui::GetCursorPosY() + 8.f)
                ImGui::SetCursorPosY(y);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.2f, 1.f));
            ImGui::TextUnformatted("!");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                const unsigned sec = VacDetect::SoftPauseRemainingMs() / 1000u;
                ImGui::SetTooltip("Soft-pause %us — click Resume.", sec);
            }
            if (ImGui::SmallButton("Go"))
                VacDetect::SoftResume();
        } else {
            const float footerY = ImGui::GetWindowHeight()
                - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight();
            if (footerY > ImGui::GetCursorPosY() + 8.f)
                ImGui::SetCursorPosY(footerY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.22f));
            ImGui::TextUnformatted("v1");
            ImGui::PopStyleColor();
        }
    }
    MenuUI::EndSidebar();

    ImGui::SameLine(0, L.gap);

    const float contentW = (std::max)(1.f,
        wsize.x - pad * 2.f - L.sidebar - L.gap);

    // Tab content fade + slight rise
    const float contentA = MenuUI::ContentAlpha();
    const float contentY = MenuUI::ContentOffsetY();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, openA * contentA);
    if (contentY > 0.1f)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + contentY);

    MenuUI::BeginContent(L, contentW, bodyH - contentY);
    {
        static const char* kTabNames[] = { "Aim", "Visuals", "Skin", "Misc", "Config" };
        const int tabIdx = (activeTab >= 0 && activeTab < 5) ? activeTab : 0;
        MenuUI::PageTitle(kTabNames[tabIdx], nullptr);

        if (VacDetect::IsSoftPaused()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.2f, 1.f));
            ImGui::Text("Soft-pause active — %us remaining",
                VacDetect::SoftPauseRemainingMs() / 1000u);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 10);
            if (ImGui::SmallButton("Resume now"))
                VacDetect::SoftResume();
            MenuUI::Gap(0.5f);
        }

        const float gap = L.gap;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail < gap + 2.f)
            avail = gap + 2.f;
        const float half = L.colLeft(avail);

        switch (activeTab) {
        case 0: // Aim
        {
            static int aimSub = 0; // 0 Aimbot | 1 Autofire | 2 Trigger | 3 Extra
            static const char* kHitboxNames[] = {
                "Head", "Neck", "Chest", "Stomach", "Pelvis", "Arms", "Legs", "Feet"
            };
            static const char* kGroups[] = {
                "General", "Pistols", "SMGs", "Rifles", "Shotguns", "Snipers", "LMGs"
            };

            // Sticky weapon group (full width, all aim pages)
            {
                ui::BeginStrip("##aim_wg");

                ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent());
                ImGui::TextUnformatted("Weapon Group");
                ImGui::PopStyleColor();

                if (Config::weapon_group_ui < 0 || Config::weapon_group_ui >= Config::WG_COUNT)
                    Config::weapon_group_ui = Config::WG_GENERAL;

                const float rowGap = 6.f;
                const float stripAvail = ImGui::GetContentRegionAvail().x;
                const float minBtn = 100.f;
                const bool rowOk = stripAvail >= (minBtn * 2.f + 160.f + rowGap * 2.f);

                if (rowOk) {
                    const float btnW = minBtn + 18.f;
                    const float comboW = stripAvail - (btnW * 2.f) - rowGap * 2.f;
                    ImGui::SetNextItemWidth((std::max)(80.f, comboW));
                    ImGui::Combo("##weapon_group", &Config::weapon_group_ui, kGroups, IM_ARRAYSIZE(kGroups));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Settings for this weapon type.");

                    ImGui::SameLine(0, rowGap);
                    if (ImGui::Button("Jump Active", ImVec2(btnW, 0)))
                        Config::weapon_group_ui = Config::weapon_group_active;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Jump to the gun you are holding.");

                    ImGui::SameLine(0, rowGap);
                    if (ImGui::Button("Copy → All", ImVec2(btnW, 0))) {
                        const Config::AimWeaponProfile src = Config::MenuAimProfile();
                        for (int g = 0; g < Config::WG_COUNT; ++g)
                            Config::weapon_profiles[g] = src;
                        Config::ApplyProfileToLive(Config::weapon_group_active);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Copy these settings to all weapon types.");
                } else {
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::Combo("##weapon_group", &Config::weapon_group_ui, kGroups, IM_ARRAYSIZE(kGroups));
                    const float stackAvail = ImGui::GetContentRegionAvail().x;
                    const float half0 = floorf((std::max)(0.f, stackAvail - rowGap) * 0.5f);
                    const float half1 = (std::max)(1.f, stackAvail - half0 - rowGap);
                    if (ImGui::Button("Jump Active", ImVec2(half0, 0)))
                        Config::weapon_group_ui = Config::weapon_group_active;
                    ImGui::SameLine(0, rowGap);
                    if (ImGui::Button("Copy → All", ImVec2(half1, 0))) {
                        const Config::AimWeaponProfile src = Config::MenuAimProfile();
                        for (int g = 0; g < Config::WG_COUNT; ++g)
                            Config::weapon_profiles[g] = src;
                        Config::ApplyProfileToLive(Config::weapon_group_active);
                    }
                }

                const bool editingActive = IsEditingActiveGroup();
                ImGui::TextDisabled("Edit: %s   ·   Live: %s%s",
                    Config::WeaponGroupName(Config::weapon_group_ui),
                    Config::WeaponGroupName(Config::weapon_group_active),
                    editingActive ? "  (synced)" : "");

                ui::EndStrip();
            }

            MenuUI::Gap(0.75f);

            {
                static const char* kAimTabs[] = { "Aimbot", "Autofire", "Trigger", "Extra" };
                ui::SubNav(kAimTabs, 4, &aimSub);
            }

            Config::AimWeaponProfile& aimP = Config::MenuAimProfile();
            const bool live = IsEditingActiveGroup();
            (void)live; // live push is PushLiveIfEditingActive at end of aim tab

            // ════════════ AIMBOT ════════════
            if (aimSub == 0) {
                ui::BeginCard("##aim_ab_l", half);
                ui::SectionLabel("Aimbot");
                ImGui::Checkbox("Enable", &Config::aimbot);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Lock aim onto enemies.");
                KeybindRow(Config::aimbot);

                ui::SliderFull("FOV", "##ab_fov", &aimP.aimbot_fov, 0.f, 90.f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How wide aim can lock on.");

                ui::SliderFull("Smooth", "##ab_smooth", &aimP.aimbot_smooth, 0.f, 50.f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = instant. Higher = slower, smoother aim.");

                {
                    static const char* kSmoothModes[] = { "Constant", "Linear", "Sine" };
                    if (aimP.aimbot_smooth_mode < 0 || aimP.aimbot_smooth_mode >= Config::SMOOTH_MODE_COUNT)
                        aimP.aimbot_smooth_mode = Config::SMOOTH_LINEAR;
                    ui::ComboFull("Smooth Mode", "##sm_mode", &aimP.aimbot_smooth_mode, kSmoothModes, Config::SMOOTH_MODE_COUNT);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How aim eases onto the target.");
                }

                ui::SliderFull("Humanize", "##ab_human", &aimP.aimbot_humanize, 0.f, 100.f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Adds natural aim movement. 0 = perfect.");

                HitboxMultiSelect("Hitboxes", "##aim_hitboxes",
                    aimP.aim_hitboxes, Config::HB_COUNT, kHitboxNames, 0,
                    "Which body parts aimbot targets.");

                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_ab_r", 0);
                ui::SectionLabel("Filters");
                ImGui::Checkbox("Visibility Check", &aimP.aim_vis_check);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Only aim at visible enemies.");
                ChecksDropdown("Checks", "##aim_checks",
                    &aimP.aim_smoke_check, &aimP.aim_flash_check, &aimP.aim_scoped_only);
                SyncSharedScope(aimP);

                ImGui::Spacing();
                ui::SectionLabel("Overlay");
                ImGui::Checkbox("Aim FOV Circle", &Config::fov_circle);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show aim FOV circle on screen.");
                if (Config::fov_circle)
                    ImGui::ColorEdit4("Aim FOV Color", (float*)&Config::fovCircleColor,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

                ImGui::Checkbox("Autofire FOV Circle", &Config::fov_circle_autofire);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show autofire FOV circle on screen.");
                if (Config::fov_circle_autofire)
                    ImGui::ColorEdit4("AF FOV Color", (float*)&Config::fovCircleColorAf,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

                ImGui::Spacing();
                ui::SectionLabel("Timing");
                ui::SliderFull("Reaction", "##ab_react", &aimP.aim_reaction_delay_ms, 0.f, 500.f, "%.0f ms");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Wait after locking a target before aiming/firing.");
                ui::SliderFull("Target Switch", "##ab_tswitch", &aimP.aim_target_switch_delay_ms, 0.f, 500.f, "%.0f ms");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Delay before switching to a new target.");
                ui::SliderFull("First Shot", "##ab_first", &aimP.aim_first_shot_delay_ms, 0.f, 500.f, "%.0f ms");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Extra delay before the first shot.");
                ui::EndCard();
            }
            // ════════════ AUTOFIRE ════════════
            else if (aimSub == 1) {
                ui::BeginCard("##aim_af_l", half);
                ui::SectionLabel("Autofire");
                ImGui::Checkbox("Enable", &Config::autofire);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Automatically aim and shoot for you.");
                KeybindRow(Config::autofire);

                ui::SliderFull("FOV", "##af_fov", &aimP.autofire_fov, 0.f, 90.f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How wide autofire can target.");

                ImGui::Checkbox("Silent Aim", &Config::autofire_silent);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Shoot without moving your crosshair.");

                {
                    static const char* kAfModes[] = { "Hitchance", "Seed Nospread" };
                    if (aimP.autofire_mode < 0 || aimP.autofire_mode >= Config::AF_MODE_COUNT)
                        aimP.autofire_mode = Config::AF_MODE_HITCHANCE;
                    ui::ComboFull("Mode", "##af_mode", &aimP.autofire_mode, kAfModes, Config::AF_MODE_COUNT);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Hitchance: chance to hit. Seed: perfect spread.");
                }

                if (aimP.autofire_mode == Config::AF_MODE_HITCHANCE) {
                    ui::SliderFull("Hitchance", "##af_hc", &aimP.autofire_hitchance, 0.f, 100.f, "%.0f%%");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Minimum hit chance before shooting. 0 = off.");
                } else {
                    ImGui::TextDisabled("Seed mode — HC %% unused");
                }

                ImGui::Checkbox("Autostop", &aimP.autofire_autostop);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stop moving before shooting for accuracy.");

                ImGui::Checkbox("Autoscope", &aimP.autofire_autoscope);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Auto-zoom when a target is ready.");

                {
                    static const char* kTargetSel[] = {
                        "Crosshair", "Distance", "Best Damage"
                    };
                    if (aimP.autofire_target_select < 0
                        || aimP.autofire_target_select >= Config::AF_TARGET_COUNT)
                        aimP.autofire_target_select = Config::AF_TARGET_CROSSHAIR;
                    ui::ComboFull("Target", "##af_target_sel", &aimP.autofire_target_select,
                        kTargetSel, IM_ARRAYSIZE(kTargetSel));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How to pick which enemy to shoot.");
                }

                ImGui::Checkbox("Focus Target", &aimP.autofire_focus_target);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Keep shooting the same target.");

                ImGui::Checkbox("Body if Lethal", &aimP.autofire_body_if_lethal);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Shoot body if it will kill in one hit.");

                ImGui::Checkbox("Prefer Body", &aimP.autofire_prefer_body);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Prefer body shots over head.");

                if (Config::backtrack)
                    ImGui::TextDisabled("Backtrack: Misc tab (AF shoots lag heads)");

                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_af_r", 0);
                ui::SectionLabel("Targeting");
                ImGui::Checkbox("Visibility", &aimP.autofire_vis_check);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Only shoot visible enemies.");
                ChecksDropdown("Checks", "##af_checks",
                    &aimP.autofire_smoke_check, &aimP.autofire_flash_check, &aimP.aim_scoped_only);
                SyncSharedScope(aimP);

                // Hitboxes + multipoint
                {
                    static constexpr int kAfMpHb[] = {
                        Config::HB_HEAD, Config::HB_CHEST, Config::HB_STOMACH, Config::HB_PELVIS
                    };
                    static constexpr int kAfMpCount = 4;
                    static const char* kAfMpNames[] = {
                        "Head", "Chest", "Stomach", "Pelvis"
                    };
                    aimP.autofire_multipoint[Config::HB_NECK] = false;
                    aimP.autofire_multipoint[Config::HB_ARMS] = false;
                    aimP.autofire_multipoint[Config::HB_LEGS] = false;
                    aimP.autofire_multipoint[Config::HB_FEET] = false;

                    HitboxMultiSelect("Hitboxes", "##af_hitboxes",
                        aimP.autofire_hitboxes, Config::HB_COUNT, kHitboxNames, 200,
                        "Which body parts autofire targets.");
                    // Clear MP when parent hitbox disabled
                    for (int i = 0; i < Config::HB_COUNT; ++i)
                        if (!aimP.autofire_hitboxes[i])
                            aimP.autofire_multipoint[i] = false;

                    {
                        char preview[96];
                        preview[0] = '\0';
                        int nSel = 0;
                        for (int mi = 0; mi < kAfMpCount; ++mi) {
                            const int hb = kAfMpHb[mi];
                            if (!aimP.autofire_multipoint[hb]) continue;
                            if (nSel > 0)
                                strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
                            strncat_s(preview, sizeof(preview), kAfMpNames[mi], _TRUNCATE);
                            ++nSel;
                        }
                        if (nSel == 0)
                            strcpy_s(preview, "None");
                        ImGui::TextUnformatted("Multipoint");
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::BeginCombo("##af_multipoint", preview)) {
                            for (int mi = 0; mi < kAfMpCount; ++mi) {
                                const int hb = kAfMpHb[mi];
                                ImGui::PushID(220 + mi);
                                ImGui::BeginDisabled(!aimP.autofire_hitboxes[hb]);
                                if (ImGui::Checkbox(kAfMpNames[mi], &aimP.autofire_multipoint[hb])) {
                                    if (!aimP.autofire_hitboxes[hb])
                                        aimP.autofire_multipoint[hb] = false;
                                }
                                ImGui::EndDisabled();
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Aim at edges of the hitbox, not only the center.");
                    }

                    ui::FeatureToggle("Dynamic Multipoint", &aimP.autofire_multipoint_dynamic, "Auto-adjust multipoint by spread and distance.");
                    ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Appearing);
                    if (ImGui::BeginPopupContextItem("##af_mp_pop")) {
                        ui::PopupTitle("Multipoint Scale");
                        ImGui::TextDisabled("0%% = center  ·  100%% = full edge");
                        ImGui::Spacing();
                        int shown = 0;
                        for (int mi = 0; mi < 4; ++mi) {
                            const int hb = kAfMpHb[mi];
                            if (!aimP.autofire_multipoint[hb] || !aimP.autofire_hitboxes[hb])
                                continue;
                            float pct = aimP.autofire_multipoint_scale[hb] * 100.f;
                            ImGui::PushID(300 + mi);
                            if (ImGui::SliderFloat(kAfMpNames[mi], &pct, 0.f, 100.f, "%.0f%%"))
                                aimP.autofire_multipoint_scale[hb] = pct / 100.f;
                            ImGui::PopID();
                            ++shown;
                        }
                        if (shown == 0)
                            ImGui::TextDisabled("Enable Multipoint boxes first");
                        ImGui::EndPopup();
                    }
                }

                ImGui::Spacing();
                ui::SectionLabel("Damage");
                ui::SliderFull("Min Damage", "##af_md", &aimP.autofire_mindamage, 0.f, 120.f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Minimum damage required to shoot. 0 = any hit.");

                ui::FeatureToggle("Autowall", &aimP.autofire_autowall, "Shoot through walls.");
                ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_Appearing);
                if (ImGui::BeginPopupContextItem("##af_aw_pop")) {
                    ui::PopupTitle("Autowall Damage");
                    ui::SliderFull("Min Damage (AW)", "##af_md_aw", &aimP.autofire_mindamage_aw, 0.f, 120.f, "%.0f");
                    ImGui::TextDisabled("Shared with Trigger when AW on");
                    ImGui::EndPopup();
                }
                if (aimP.autofire_autowall) {
                    ImGui::TextDisabled("Autowall bind (global)");
                    KeybindRow(Config::autowall);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Key to enable shooting through walls.");
                }
                ui::EndCard();
            }
            // ════════════ TRIGGER ════════════
            else if (aimSub == 2) {
                ui::BeginCard("##aim_tr_l", half);
                ui::SectionLabel("Triggerbot");
                ImGui::Checkbox("Enable", &Config::triggerbot);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Shoots when your crosshair is on an enemy.");
                KeybindRow(Config::triggerbot);

                ImGui::Checkbox("Magnet", &aimP.trigger_magnet);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Sticky soft-aim into FOV while trigger key held.\n"
                        "Punch-aware · multipoint · no aimbot humanize.\n"
                        "0 smooth = snap. Fire can open when lock is near bone.");
                if (aimP.trigger_magnet) {
                    ui::SliderFull("Magnet FOV", "##tr_mag_fov",
                        &aimP.trigger_magnet_fov, 0.5f, 15.f, "%.1f°");
                    ui::SliderFull("Magnet Smooth", "##tr_mag_sm",
                        &aimP.trigger_magnet_smooth, 0.f, 50.f, "%.0f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0 = instant snap. Higher = slower pull (no humanize).");
                    ui::SliderFull("Deadzone", "##tr_mag_dz",
                        &aimP.trigger_magnet_deadzone, 0.f, 0.8f, "%.2f°");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Stop micro-pull when already this close to bone.");
                    ImGui::Checkbox("Silent Magnet", &aimP.trigger_magnet_silent);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Stamp cmd angles only — camera stays (rage/silent).");
                    ImGui::Checkbox("Head Priority", &aimP.trigger_magnet_head_prio);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Prefer head/neck when FOV nearly equal.");
                    ImGui::Checkbox("Magnet FOV Circle", &Config::fov_circle_magnet);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Show magnet FOV circle on screen.");
                    if (Config::fov_circle_magnet)
                        ImGui::ColorEdit4("Magnet FOV Color", (float*)&Config::fovCircleColorMagnet,
                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::TextDisabled("Vis always · AW = Trigger Autowall + bind");
                    ImGui::TextDisabled("Sticky lock · punch RCS · multipoint aim");
                }

                ui::SliderFull("Delay", "##tr_delay", &aimP.trigger_delay_ms, 0.f, 250.f, "%.0f ms");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Wait on target before shooting. 0 = instant.");

                {
                    static const char* kTrModes[] = { "Hitchance", "Seed Nospread" };
                    if (aimP.trigger_mode < 0 || aimP.trigger_mode >= Config::TR_MODE_COUNT)
                        aimP.trigger_mode = Config::TR_MODE_HITCHANCE;
                    ui::ComboFull("Mode", "##tr_mode", &aimP.trigger_mode, kTrModes, Config::TR_MODE_COUNT);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Hitchance: chance to hit. Seed: perfect spread.");
                }

                if (aimP.trigger_mode == Config::TR_MODE_HITCHANCE) {
                    ui::SliderFull("Hitchance", "##tr_hc", &aimP.trigger_hitchance, 0.f, 100.f, "%.0f%%");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Minimum hit chance before shooting. 0 = off.");
                } else {
                    ImGui::TextDisabled("Seed — HC %% unused");
                }

                ImGui::Checkbox("Autostop", &aimP.trigger_autostop);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stop moving before trigger fires.");

                if (Config::backtrack)
                    ImGui::TextDisabled("Backtrack: Misc tab (TR fires lag heads)");

                ImGui::Checkbox("Autowall", &aimP.trigger_autowall);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fire through walls when crosshair is on them.");
                if (aimP.trigger_autowall) {
                    ImGui::TextDisabled("Autowall bind (global)");
                    KeybindRow(Config::autowall);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Key to enable shooting through walls.");
                }

                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_tr_r", 0);
                ui::SectionLabel("Filters");
                ChecksDropdown("Checks", "##tr_checks",
                    &aimP.trigger_smoke_check, &aimP.trigger_flash_check, &aimP.aim_scoped_only);
                SyncSharedScope(aimP);

                HitboxMultiSelect("Hitboxes", "##tr_hitboxes",
                    aimP.trigger_hitboxes, Config::HB_COUNT, kHitboxNames, 100,
                    "Which body parts trigger can fire on.");

                if (aimP.trigger_magnet) {
                    ImGui::Spacing();
                    HitboxMultiSelect("Magnet Hitboxes", "##tr_mag_hb",
                        aimP.trigger_magnet_hitboxes, Config::HB_COUNT, kHitboxNames, 100,
                        "Empty = same as fire hitboxes. Else magnet only these.");
                }

                ImGui::Spacing();
                ui::SectionLabel("Damage");
                ImGui::TextDisabled("Trigger only — not Autofire");
                ui::SliderFull("Min Damage", "##tr_md", &aimP.trigger_mindamage, 0.f, 120.f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Minimum damage to fire. 0 = any hit.");
                if (aimP.trigger_autowall) {
                    ui::SliderFull("Min Damage (AW)", "##tr_md_aw", &aimP.trigger_mindamage_aw, 0.f, 120.f, "%.0f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Minimum wallbang damage. 0 = any hit.");
                }
                ui::EndCard();
            }
            // ════════════ EXTRA (RCS / knife / team) ════════════
            else {
                ui::BeginCard("##aim_ex_l", half);
                ui::SectionLabel("Recoil");
                ImGui::Checkbox("Aimbot RCS", &aimP.rcs);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Control recoil while aimbot is locking.");
                ImGui::Checkbox("Standalone RCS", &aimP.rcs_standalone);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Control recoil while holding fire, no aimbot needed.");
                ui::SliderFull("Scale X (Yaw)", "##rcs_x", &aimP.rcs_scale_x, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Left/right recoil control. 1 = full.");
                ui::SliderFull("Scale Y (Pitch)", "##rcs_y", &aimP.rcs_scale_y, 0.f, 1.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Up/down recoil control. 1 = full.");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##aim_ex_r", 0);
                ui::SectionLabel("Knife");
                ImGui::Checkbox("Knife Bot", &Config::knifebot);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Auto knife when an enemy is in range.");
                if (Config::knifebot) {
                    ImGui::Checkbox("Prefer Stab", &Config::knifebot_prefer_stab);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Use backstab when possible.");
                    KeybindRow(Config::knifebot);
                }

                ImGui::Spacing();
                ui::SectionLabel("Team");
                ImGui::Checkbox("Auto Team Check", &Config::team_check_auto);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Auto skip teammates in team modes.");
                {
                    const bool eff = GameMode::WantTeamCheck(Config::team_check);
                    ImGui::TextDisabled("%s  ·  TeamCheck %s",
                        GameMode::ModeLabel(), eff ? "ON" : "OFF");
                }
                ImGui::BeginDisabled(Config::team_check_auto);
                ImGui::Checkbox("Team Check", &Config::team_check);
                ImGui::EndDisabled();
                ui::EndCard();
            }

            // Profile edits → live only when UI group == held weapon group.
            // Editing another group never stomps live Config (FOV circles, etc.).
            PushLiveIfEditingActive();
        }
        break;

        case 1: // Visuals
        {
            // 0 Players | 1 World (atmosphere + item/nade ESP) | 2 Nade Pred | 3 View | 4 Removals
            static int visSub = 0;
            {
                static const char* kVisTabs[] = {
                    "Players", "World", "Nade Pred", "View", "Removals"
                };
                ui::SubNav(kVisTabs, 5, &visSub);
            }

            const ImGuiColorEditFlags colFlags =
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

            if (visSub == 0) {
                ui::BeginCard("##vis_left", half);
                ui::SectionLabel("Player ESP");
                ImGui::TextDisabled("Right-click a feature for settings");

                ui::FeatureToggle("Box", &Config::esp, "Draw boxes around players.");
                if (ImGui::BeginPopupContextItem("##esp_box_pop")) {
                    ui::PopupTitle("Box Settings");
                    ImGui::ColorEdit4("Visible", (float*)&Config::espColor, colFlags);
                    ImGui::ColorEdit4("Invisible", (float*)&Config::espColorInvisible, colFlags);
                    ImGui::SliderFloat("Thickness", &Config::espThickness, 1.0f, 5.0f, "%.1f");
                    ImGui::SliderFloat("Width Scale", &Config::esp_box_width, 0.28f, 0.70f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How wide the box is.");
                    const char* styles[] = { "Full", "Corner" };
                    ImGui::Combo("Style", &Config::esp_box_style, styles, IM_ARRAYSIZE(styles));
                    ImGui::Checkbox("Fill", &Config::espFill);
                    if (Config::espFill)
                        ImGui::SliderFloat("Fill Opacity", &Config::espFillOpacity, 0.0f, 1.0f, "%.2f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Health Bar", &Config::showHealth, "Show health bar next to players.");
                if (ImGui::BeginPopupContextItem("##esp_hp_pop")) {
                    ui::PopupTitle("Health Bar Settings");
                    ImGui::Checkbox("Auto Color (HP)", &Config::esp_health_auto);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Bar color follows health.");
                    if (!Config::esp_health_auto)
                        ImGui::ColorEdit4("Color", (float*)&Config::esp_health_color, colFlags);
                    ImGui::SliderFloat("Bar Width", &Config::esp_bar_width, 2.0f, 8.0f, "%.0f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Armor Bar", &Config::showArmor, "Show armor bar next to players.");
                if (ImGui::BeginPopupContextItem("##esp_armor_pop")) {
                    ui::PopupTitle("Armor Bar Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_armor_color, colFlags);
                    ImGui::SliderFloat("Bar Width", &Config::esp_bar_width, 2.0f, 8.0f, "%.0f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Name Tags", &Config::showNameTags, "Show player names.");
                if (ImGui::BeginPopupContextItem("##esp_name_pop")) {
                    ui::PopupTitle("Name Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_name_color, colFlags);
                    ImGui::Checkbox("Avatar", &Config::esp_name_avatar);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Show Steam avatar next to name.");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Weapon ESP", &Config::showWeapon, "Show weapon name on players.");
                if (ImGui::BeginPopupContextItem("##esp_wep_pop")) {
                    ui::PopupTitle("Weapon Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_weapon_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Weapon Icons", &Config::showWeaponIcon, "Show weapon icon on players.");
                if (ImGui::BeginPopupContextItem("##esp_wep_icon_pop")) {
                    ui::PopupTitle("Weapon Icon Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_weapon_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Distance ESP", &Config::showDistance, "Show distance to players.");
                if (ImGui::BeginPopupContextItem("##esp_dist_pop")) {
                    ui::PopupTitle("Distance Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_distance_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Skeleton", &Config::esp_skeleton, "Draw player bones.");
                if (ImGui::BeginPopupContextItem("##esp_skel_pop")) {
                    ui::PopupTitle("Skeleton Settings");
                    ImGui::ColorEdit4("Skel Visible##skel_vis", (float*)&Config::esp_skeleton_color, colFlags);
                    ImGui::ColorEdit4("Skel Invisible##skel_invis", (float*)&Config::esp_skeleton_color_invisible, colFlags);
                    ImGui::SliderFloat("Thickness", &Config::esp_skeleton_thickness, 1.0f, 4.0f, "%.1f");
                    ImGui::EndPopup();
                }

                if (Config::backtrack) {
                    ui::FeatureToggle("BT Skeleton", &Config::backtrack_skeleton,
                        "Lag ghost skeleton (behind player when moving). Needs Misc → Backtrack.");
                    if (ImGui::BeginPopupContextItem("##esp_bt_skel_pop")) {
                        ui::PopupTitle("Backtrack Skeleton");
                        ImGui::ColorEdit4("Color##bt_skel", (float*)&Config::backtrack_color, colFlags);
                        ImGui::TextDisabled("Time: Misc → Backtrack");
                        ImGui::EndPopup();
                    }
                }

                ui::FeatureToggle("Glow", &Config::glow, "Outline glow on players.");
                if (ImGui::BeginPopupContextItem("##esp_glow_pop")) {
                    ui::PopupTitle("Player Glow Settings");
                    ImGui::Checkbox("Team", &Config::glow_team);
                    ImGui::Checkbox("Enemy", &Config::glow_enemy);
                    ImGui::Checkbox("Only Visible", &Config::glow_only_visible);
                    ImGui::ColorEdit4("Glow Visible##glow_vis", (float*)&Config::glow_color, colFlags);
                    ImGui::ColorEdit4("Glow Invisible##glow_invis", (float*)&Config::glow_color_invis, colFlags);
                    ImGui::TextDisabled("RGB + Alpha own glow only");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Glow color is separate from chams.");
                    ImGui::EndPopup();
                }

                ImGui::Checkbox("Auto Team Check##esp", &Config::team_check_auto);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Auto skip teammates in team modes.");
                {
                    const bool eff = GameMode::WantTeamCheck(Config::teamCheck);
                    ImGui::TextDisabled("Mode: %s  |  TeamCheck: %s",
                        GameMode::ModeLabel(), eff ? "ON" : "OFF");
                }
                ImGui::BeginDisabled(Config::team_check_auto);
                ImGui::Checkbox("Team Check##esp", &Config::teamCheck);
                ImGui::EndDisabled();
                ImGui::Checkbox("Visibility Check##esp", &Config::esp_vis_check);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Different colors for visible vs behind wall.");

                ui::SectionLabel("Flags");
                ImGui::TextDisabled("Active state labels (right of box)");
                ImGui::Checkbox("Flashed", &Config::flag_flashed);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show when enemy is flashed.");
                ImGui::Checkbox("Bomb", &Config::flag_bomb);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Shows if enemy has the bomb.");
                ImGui::Checkbox("Scoped", &Config::flag_scoped);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show when enemy is scoped.");
                ImGui::Checkbox("Reloading", &Config::flag_reloading);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show when enemy is reloading.");
                ImGui::Checkbox("Defusing", &Config::flag_defusing);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show when enemy is defusing.");
                ImGui::Checkbox("Money", &Config::flag_money);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show enemy money.");
                ImGui::Checkbox("Kit", &Config::flag_kit);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show if enemy has a defuse kit.");
                ImGui::Checkbox("Helmet", &Config::flag_helmet);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show if enemy has a helmet.");
                ImGui::Checkbox("Nades (H/F/S/M/D)", &Config::flag_nades);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show grenades they are holding.");

                ui::SectionLabel("Extra ESP");
                ui::FeatureToggle("Rank ESP", &Config::esp_rank, "Show competitive rank.");
                if (ImGui::BeginPopupContextItem("##esp_rank_pop")) {
                    ui::PopupTitle("Rank");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_rank_color, colFlags);
                    ImGui::TextDisabled("Competitive rank label");
                    ImGui::EndPopup();
                }
                ui::FeatureToggle("3D Box", &Config::esp_3d_box, "3D box around players.");
                if (ImGui::BeginPopupContextItem("##esp_3d_pop")) {
                    ui::PopupTitle("3D Box");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_3d_box_color, colFlags);
                    ImGui::TextDisabled("3D box around player");
                    ImGui::EndPopup();
                }
                ui::FeatureToggle("Offscreen Arrows", &Config::esp_oof, "Arrows for enemies off screen.");
                if (ImGui::BeginPopupContextItem("##esp_oof_pop")) {
                    ui::PopupTitle("OOF Arrows");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_oof_color, colFlags);
                    ImGui::SliderFloat("Radius", &Config::esp_oof_radius, 80.f, 420.f, "%.0f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How far from center the arrows sit.");
                    ImGui::SliderFloat("Size", &Config::esp_oof_size, 8.f, 28.f, "%.0f");
                    ImGui::TextDisabled("Distance label · pulse · low-HP pip");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Sound ESP", &Config::sound_esp, "Ring when enemies move nearby.");
                if (ImGui::BeginPopupContextItem("##esp_sound_pop")) {
                    ui::PopupTitle("Sound ESP");
                    ImGui::SliderFloat("Ring Size", &Config::sound_esp_ring_size, 0.5f, 3.f, "%.2fx");
                    ImGui::SliderFloat("Life", &Config::sound_esp_duration, 0.4f, 4.f, "%.1fs");
                    ImGui::ColorEdit4("Color", (float*)&Config::sound_esp_color, colFlags);
                    ImGui::TextDisabled("Shows when ESP would show that enemy");
                    ImGui::EndPopup();
                }
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##vis_right", 0);
                ui::SectionLabel("Chams");
                const char* mats[] = {
                    "Flat", "Illuminate", "Rim", "Ghost", "Latex", "Metallic", "Ghost2",
                    "Pulse", "Rainbow", "Holo", "Energy"
                };
                const ImGuiColorEditFlags chamCol =
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;

                ImGui::Checkbox("Enemy Chams", &Config::enemyChams);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Color enemy models when visible.");
                if (Config::enemyChams) {
                    ui::ComboFull("Visible Material", "##chams_vis_mat", &Config::chamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Chams Visible##chams_vis", (float*)&Config::colVisualChams, chamCol);
                }

                ImGui::Checkbox("Chams XQZ", &Config::enemyChamsInvisible);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("See enemy models through walls.");
                if (Config::enemyChamsInvisible) {
                    ui::ComboFull("XQZ Material", "##chams_xqz_mat", &Config::chamsMaterialXQZ, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Chams XQZ##chams_xqz", (float*)&Config::colVisualChamsIgnoreZ, chamCol);
                }

                ImGui::Checkbox("Team Chams", &Config::teamChams);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Color teammates when visible (team check on).");
                if (Config::teamChams) {
                    ImGui::ColorEdit4("Team Visible##chams_team", (float*)&Config::teamcolVisualChams, chamCol);
                }
                ImGui::Checkbox("Team XQZ", &Config::teamChamsInvisible);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("See teammates through walls.");
                if (Config::teamChamsInvisible) {
                    ImGui::ColorEdit4("Team XQZ##chams_team_xqz", (float*)&Config::teamcolVisualChamsIgnoreZ, chamCol);
                }

                ui::SectionLabel("Local");
                ImGui::Checkbox("Local Body Chams", &Config::localChams);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Color your own body in third person.");
                if (Config::localChams) {
                    ui::ComboFull("Local Material", "##chams_local_mat", &Config::localChamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Local Color", (float*)&Config::colLocalChams, chamCol);
                }
                ImGui::Checkbox("Ragdoll Chams", &Config::ragdollChams);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Color dead bodies.");
                if (Config::ragdollChams) {
                    ui::ComboFull("Ragdoll Material", "##chams_rag_mat", &Config::ragdollChamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Ragdoll Color", (float*)&Config::colRagdollChams, chamCol);
                }
                ImGui::Checkbox("Hand Chams", &Config::armChams);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Color your hands / arms.");
                if (Config::armChams) {
                    ui::ComboFull("Hand Material", "##chams_hand_mat", &Config::armChamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Hand Color", (float*)&Config::colArmChams, chamCol);
                }
                ImGui::Checkbox("Weapon Chams", &Config::viewmodelChams);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Color your held weapon.");
                if (Config::viewmodelChams) {
                    ui::ComboFull("Weapon Material", "##chams_wep_mat", &Config::viewmodelChamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Weapon Color", (float*)&Config::colViewmodelChams, chamCol);
                }
                ui::EndCard();
            }
            else if (visSub == 1) {
                // ── World: 2 columns, all cards AutoResizeY (no bottom strip scroll) ──
                const ImGuiColorEditFlags worldCol =
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;

                // LEFT column
                ui::BeginCard("##world_left", half, true);
                ui::SectionLabel("Atmosphere");
                ImGui::TextDisabled("Map lighting & environment");

                ImGui::Checkbox("Night Mode", &Config::Night);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Make the map darker.");
                if (Config::Night) {
                    ui::SliderFull("Darkness", "##night_dark", &Config::night_exposure, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0 = normal. 1 = darkest.");
                }

                ImGui::Checkbox("Skybox Color", &Config::skybox);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change the sky color.");
                if (Config::skybox)
                    ImGui::ColorEdit4("Sky Color", (float*)&Config::skybox_color, worldCol);

                ImGui::Checkbox("Lighting Color", &Config::lighting);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change world lighting color.");
                if (Config::lighting)
                    ImGui::ColorEdit4("Light Color", (float*)&Config::lighting_color, worldCol);

                ImGui::Checkbox("Map Color", &Config::map_color);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tint map walls and props.");
                if (Config::map_color) {
                    ImGui::ColorEdit4("World Color", (float*)&Config::map_color_value, worldCol);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Color and strength of the map tint.");
                }

                ImGui::Spacing();
                ui::SectionLabel("Bullet Tracers");
                ImGui::TextDisabled("Local shots · eye → impact");
                ImGui::Checkbox("Tracers", &Config::tracers);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Draw beam from your eye to server bullet impact.\n"
                        "Uses bullet_impact event (same as world hitmarker pin).");
                if (Config::tracers) {
                    const char* trStyles[] = {
                        "Beam",   // 2-layer + flare
                        "Laser",  // thin white needle
                        "Glow",   // soft wide haze
                        "Dashed", // broken trail
                        "Energy"  // dual rail + pulse
                    };
                    int st = Config::tracers_style;
                    if (st < 0) st = 0;
                    if (st > 4) st = 4;
                    if (ui::ComboFull("Style", "##tr_style", &st, trStyles, IM_ARRAYSIZE(trStyles)))
                        Config::tracers_style = st;
                    ui::SliderFull("Duration", "##tr_life", &Config::tracers_duration, 0.5f, 6.f, "%.2f s");
                    ui::SliderFull("Thickness", "##tr_th", &Config::tracers_thickness, 0.5f, 6.f, "%.1f");
                    ImGui::ColorEdit4("Tracer Color", (float*)&Config::tracers_color, worldCol);
                }

                ImGui::Spacing();
                ui::SectionLabel("Items");
                ImGui::TextDisabled("Right-click for color + glow");

                ui::FeatureToggle("Dropped Weapons", &Config::world_esp_weapons, "Show guns on the ground.");
                if (ImGui::BeginPopupContextItem("##wesp_wep_pop")) {
                    ui::PopupTitle("Weapon ESP");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_weapon_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_weapons);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Glow outline on dropped weapons.");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Planted Bomb", &Config::world_esp_bomb, "Bomb site, timer, and defuse info.");
                if (ImGui::BeginPopupContextItem("##wesp_bomb_pop")) {
                    ui::PopupTitle("Bomb ESP");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_bomb_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_bomb);
                    ImGui::EndPopup();
                }

                ImGui::Checkbox("Show Distance", &Config::world_esp_distance);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show distance on world items.");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                // RIGHT column
                ui::BeginCard("##world_right", 0, true);
                ui::SectionLabel("Weather & Fog");
                ImGui::TextDisabled("Particles + gradient fog");

                ImGui::Checkbox("Weather", &Config::weather);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Add snow, stars, ash, or rain to the map.");
                if (Config::weather) {
                    const char* weatherModes[] = {
                        "Snow", "Stars", "Ash", "Rain"
                    };
                    int modeIdx = Config::weather_mode - 1;
                    if (modeIdx < 0) modeIdx = 0;
                    if (modeIdx > 3) modeIdx = 3;
                    if (ui::ComboFull("Mode", "##weather_mode", &modeIdx, weatherModes, IM_ARRAYSIZE(weatherModes)))
                        Config::weather_mode = modeIdx + 1;
                    ui::SliderFull("Intensity", "##weather_int", &Config::weather_intensity, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How strong the weather effect is.");
                }

                ImGui::Spacing();
                ImGui::Checkbox("Custom Fog", &Config::custom_fog);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Add custom fog to the map.");
                if (Config::custom_fog) {
                    ImGui::ColorEdit4("Fog Color", (float*)&Config::custom_fog_color, worldCol);
                    ui::SliderFull("Fog Start", "##fog_start", &Config::custom_fog_start, 0.f, 4096.f, "%.0f");
                    ui::SliderFull("Fog End", "##fog_end", &Config::custom_fog_end, 0.f, 4096.f, "%.0f");
                    ui::SliderFull("Fog Falloff", "##fog_fall", &Config::custom_fog_falloff, 0.1f, 8.f, "%.2f");
                }

                ImGui::Spacing();
                ui::SectionLabel("FX Tint");
                ImGui::TextDisabled("Particle / volume colors");

                ImGui::Checkbox("Smoke Color", &Config::smoke_color);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change smoke color.");
                if (Config::smoke_color && !Config::remove_smoke) {
                    ImGui::ColorEdit4("Smoke Tint", (float*)&Config::smoke_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);
                }

                ImGui::Checkbox("Fire Color", &Config::fire_color);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tint molotov / inferno particles + hull fill.");
                if (Config::fire_color) {
                    ImGui::ColorEdit4("Molly / Inferno", (float*)&Config::fire_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Particle tint + inferno hull / flame markers.");
                }

                ImGui::Checkbox("Explosion Color", &Config::explosion_color);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change explosion particle color.");
                if (Config::explosion_color) {
                    ImGui::ColorEdit4("Explosion Tint", (float*)&Config::explosion_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);
                }

                ImGui::Spacing();
                ui::SectionLabel("Projectiles");
                ImGui::TextDisabled("Right-click for color + glow");

                ui::FeatureToggle("Smoke", &Config::world_esp_smoke, "Show smoke grenades in the world.");
                if (ImGui::BeginPopupContextItem("##wesp_smoke_pop")) {
                    ui::PopupTitle("Smoke");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_smoke_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Molotov / Fire", &Config::world_esp_molotov, "Show molotov and fire.");
                if (ImGui::BeginPopupContextItem("##wesp_molly_pop")) {
                    ui::PopupTitle("Molotov / Inferno");
                    ImGui::ColorEdit4("ESP Color", (float*)&Config::world_esp_molotov_color, colFlags);
                    ImGui::Checkbox("Particle Fire Color", &Config::fire_color);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Tint real fire particles + hull fill.");
                    if (Config::fire_color) {
                        ImGui::ColorEdit4("Fire Color", (float*)&Config::fire_color_value, colFlags);
                    }
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("HE Grenade", &Config::world_esp_he, "Show HE grenades.");
                if (ImGui::BeginPopupContextItem("##wesp_he_pop")) {
                    ui::PopupTitle("HE");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_he_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Flashbang", &Config::world_esp_flash, "Show flashbangs.");
                if (ImGui::BeginPopupContextItem("##wesp_flash_pop")) {
                    ui::PopupTitle("Flash");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_flash_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Decoy", &Config::world_esp_decoy, "Show decoy grenades.");
                if (ImGui::BeginPopupContextItem("##wesp_decoy_pop")) {
                    ui::PopupTitle("Decoy");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_decoy_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }
                ui::EndCard();
            }
            else if (visSub == 2) {
                // Grenade prediction
                ui::BeginCard("##nade_left", half);
                ui::SectionLabel("Grenade Prediction");
                ImGui::TextDisabled("Shows path and landing");

                ui::FeatureToggle("Enable", &Config::nade_pred, "Show where grenades will land.");

                ImGui::BeginDisabled(!Config::nade_pred);
                ImGui::Checkbox("Local Throw Preview", &Config::nade_pred_local);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Preview throw path while holding a grenade.");
                if (Config::nade_pred_local) {
                    ImGui::Checkbox("Only When Pin Pulled", &Config::nade_pred_local_only_pin);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Only show path after you pull the pin.");
                }
                ImGui::Checkbox("In-Air Projectiles", &Config::nade_pred_projectiles);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Track grenades already in the air.");
                ImGui::Checkbox("Effect Radius", &Config::nade_pred_radius);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show blast/effect radius at landing.");
                ImGui::Checkbox("Damage Indicator", &Config::nade_pred_damage);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show estimated HE damage on enemies.");
                ImGui::EndDisabled();

                // Warn works standalone — all teams, full map, fixed size
                ui::SectionLabel("Warning");
                ImGui::Checkbox("Grenade Warning", &Config::nade_warn);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Icon warning for all flying grenades.");
                ImGui::TextDisabled("Shows all teams · no range limit");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##nade_right", 0);
                ui::SectionLabel("Style");
                ImGui::BeginDisabled(!Config::nade_pred);
                ui::SliderFull("Line Thickness", "##np_thick", &Config::nade_pred_thickness, 1.0f, 5.0f, "%.1f");
                ImGui::ColorEdit4("Path Color", (float*)&Config::nade_pred_color, colFlags);
                ImGui::ColorEdit4("Local Path", (float*)&Config::nade_pred_local_color, colFlags);
                ImGui::ColorEdit4("Land Marker", (float*)&Config::nade_pred_land_color, colFlags);
                ImGui::TextDisabled("Projectile paths use World ESP colors");
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::BeginDisabled(!Config::nade_warn);
                ui::SliderFull("Icon Size", "##nw_icon", &Config::nade_warn_icon_size, 18.f, 48.f, "%.0f");
                ImGui::ColorEdit4("Warn Accent", (float*)&Config::nade_warn_color, colFlags);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Badge color when World ESP for that nade is off.");
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!Config::nade_pred || !Config::nade_pred_damage);
                ImGui::ColorEdit4("Damage Color", (float*)&Config::nade_pred_damage_color, colFlags);
                ImGui::ColorEdit4("Lethal Color", (float*)&Config::nade_pred_damage_lethal_color, colFlags);
                ImGui::EndDisabled();

                ImGui::Separator();
                ui::SectionLabel("Lineup Helper");
                ImGui::TextDisabled("Stand pad + aim reticle");
                ui::FeatureToggle("Enable Lineups", &Config::nade_lineup, "Save and show grenade lineup spots.");
                ImGui::BeginDisabled(!Config::nade_lineup);
                ImGui::Checkbox("Only Matching Nade", &Config::nade_lineup_only_held);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Only show lineups for the grenade you hold.");
                ui::SliderFull("Stand Draw Dist", "##nl_stand", &Config::nade_lineup_stand_dist, 50.f, 1500.f, "%.0f");
                ui::SliderFull("Aim Marker Dist", "##nl_aim", &Config::nade_lineup_aim_dist, 5.f, 80.f, "%.0f");
                ImGui::ColorEdit4("Stand Color", (float*)&Config::nade_lineup_color, colFlags);
                ImGui::ColorEdit4("Aim Color", (float*)&Config::nade_lineup_aim_color, colFlags);
                ImGui::TextUnformatted("Name");
                ImGui::SetNextItemWidth(-1.f);
                ImGui::InputText("##lineup_name", Config::nade_lineup_capture_name, sizeof(Config::nade_lineup_capture_name));
                const char* throwItems[] = { "Stand", "Stand+Jump", "Walk", "Run", "Crouch", "Run+Jump" };
                ImGui::BeginDisabled(true);
                ui::ComboFull("Throw", "##lineup_throw", &Config::nade_lineup_capture_throw, throwItems, 6);
                ImGui::EndDisabled();
                ImGui::TextDisabled("Throw style is auto-detected when you throw");
                if (Config::nade_lineup_capture_throw < 0 || Config::nade_lineup_capture_throw > 5)
                    Config::nade_lineup_capture_throw = 0;
                const char* kindItems[] = { "Any / Held", "HE", "Flash", "Smoke", "Molly", "Decoy" };
                ui::ComboFull("Kind", "##lineup_kind", &Config::nade_lineup_capture_kind, kindItems, 6);
                ImGui::TextDisabled("Capture keybind");
                KeybindStack(Config::nade_lineup_capture);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Press to start saving a lineup, then throw.");
                if (ImGui::Button(NadeLineup::IsCapturing() ? "Cancel Capture" : "Arm Capture", ImVec2(-1, 0))) {
                    if (NadeLineup::IsCapturing())
                        NadeLineup::CancelCapture();
                    else {
                        NadeLineup::ArmCapture(
                            Config::nade_lineup_capture_name,
                            static_cast<NadeLineup::NadeKind>(std::clamp(Config::nade_lineup_capture_kind, 0, 5)));
                    }
                }
                ImGui::TextDisabled("Map: %s  (%d on this map / %d total)",
                    NadeLineup::CurrentMap()[0] ? NadeLineup::CurrentMap() : "(none)",
                    NadeLineup::CountCurrentMap(),
                    static_cast<int>(NadeLineup::All().size()));
                ImGui::TextDisabled("Save: Documents\\Lefrizzel AI\\NadeLineups\\");
                if (!NadeLineup::CurrentMap()[0]) {
                    ImGui::TextDisabled("Join a map to list its lineups.");
                } else {
                    if (ImGui::BeginChild("##lineup_list", ImVec2(0, 120), true)) {
                        int shown = 0;
                        for (int i = static_cast<int>(NadeLineup::All().size()) - 1; i >= 0; --i) {
                            const auto& L = NadeLineup::All()[i];
                            if (!NadeLineup::IsCurrentMap(L))
                                continue;
                            ImGui::PushID(i);
                            ImGui::Text("%s | %s | %s",
                                L.name.c_str(),
                                NadeLineup::KindName(L.kind), NadeLineup::ThrowName(L.throwType));
                            // Right-align delete so long names never push it off the row
                            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 18.f);
                            if (ImGui::SmallButton("X")) {
                                NadeLineup::RemoveAt(i);
                                ImGui::PopID();
                                break;
                            }
                            ImGui::PopID();
                            ++shown;
                        }
                        if (shown == 0)
                            ImGui::TextDisabled("No lineups for this map.");
                    }
                    ImGui::EndChild();
                }
                if (ImGui::Button("Clear This Map"))
                    NadeLineup::ClearCurrentMap();
                ImGui::SameLine();
                if (ImGui::Button("Reload File"))
                    NadeLineup::Load();
                ImGui::EndDisabled();
                ui::EndCard();
            }
            else if (visSub == 3) {
                ui::BeginCard("##view_left", half);
                ui::SectionLabel("Viewmodel");
                ImGui::TextDisabled("Move and size your gun on screen");

                ImGui::Checkbox("Enable##viewmodel_changer", &Config::viewmodel_changer);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Move and resize your weapon viewmodel.");

                ImGui::BeginDisabled(!Config::viewmodel_changer);
                ui::SliderFull("Offset X", "##vm_x", &Config::viewmodel_x, -20.f, 20.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Move gun left or right.");
                ui::SliderFull("Offset Y", "##vm_y", &Config::viewmodel_y, -20.f, 20.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Move gun up or down.");
                ui::SliderFull("Offset Z", "##vm_z", &Config::viewmodel_z, -20.f, 20.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Move gun closer or farther.");
                ui::SliderFull("Viewmodel FOV", "##vm_fov", &Config::viewmodel_fov, 40.f, 120.f, "%.0f");
                ImGui::EndDisabled();
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##view_right", 0);
                ui::SectionLabel("Third Person");
                ImGui::Checkbox("Enable##thirdperson", &Config::thirdperson);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Camera behind your player.");

                ImGui::BeginDisabled(!Config::thirdperson);
                {
                    KeybindStack(Config::thirdperson);
                }
                ui::SliderFull("Distance", "##tp_dist", &Config::thirdperson_distance, 50.f, 300.f, "%.0f");
                ImGui::EndDisabled();

                ImGui::Spacing();
                ui::SectionLabel("World FOV");
                ImGui::Checkbox("Custom FOV", &Config::fovEnabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change how wide your camera sees.");
                ImGui::BeginDisabled(!Config::fovEnabled);
                ui::SliderFull("FOV Value", "##world_fov", &Config::fov, 20.0f, 160.0f, "%.0f");
                ImGui::EndDisabled();

                ImGui::Spacing();
                ui::SectionLabel("Aspect Ratio");
                ImGui::Checkbox("Custom Aspect", &Config::aspect_ratio_enabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stretch the screen (e.g. 4:3 on 16:9).");
                ImGui::BeginDisabled(!Config::aspect_ratio_enabled);
                ui::SliderFull("Ratio", "##aspect_ratio", &Config::aspect_ratio, 0.5f, 3.5f, "%.3f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Common ratios: 1.33 = 4:3, 1.78 = 16:9.");
                if (ImGui::Button("4:3"))  Config::aspect_ratio = 4.f / 3.f;
                ImGui::SameLine();
                if (ImGui::Button("16:10")) Config::aspect_ratio = 16.f / 10.f;
                ImGui::SameLine();
                if (ImGui::Button("16:9")) Config::aspect_ratio = 16.f / 9.f;
                ImGui::SameLine();
                if (ImGui::Button("21:9")) Config::aspect_ratio = 21.f / 9.f;
                ImGui::EndDisabled();

                ui::EndCard();
            }
            else if (visSub == 4) {
                ui::BeginCard("##removals_left", half);
                ui::SectionLabel("World / FX");
                ImGui::TextDisabled("Turn off visual clutter");

                ImGui::TextUnformatted("Flash Reduce");
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##flash_reduce", &Config::antiflash_amount, 0.f, 100.f, "%.0f%%");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = full flash. 100 = no flash.");
                Config::antiflash = Config::antiflash_amount > 0.01f;

                ImGui::Checkbox("Smoke", &Config::remove_smoke);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove smoke completely.");

                ImGui::Checkbox("Decals", &Config::remove_decals);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove bullet holes, blood, and scorch marks.");

                ImGui::Checkbox("Particles", &Config::remove_particles);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove muzzle flash, blood, and impact FX.");

                if (ImGui::Checkbox("Crosshair", &Config::remove_crosshair)) {
                    if (Config::remove_crosshair)
                        Config::force_crosshair = false;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hide the default crosshair.");

                if (ImGui::Checkbox("Force Crosshair", &Config::force_crosshair)) {
                    if (Config::force_crosshair)
                        Config::remove_crosshair = false;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Always show crosshair on snipers when unscoped.");

                ImGui::Checkbox("HUD Elements", &Config::remove_hud);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hide HUD elements.");

                ImGui::Checkbox("Post Process", &Config::remove_postprocess);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Disable blur and post-process effects.");

                ImGui::Checkbox("Visual Recoil", &Config::remove_recoil);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove camera kick from aim punch. Bullets still use real punch (seed/RCS ok).");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##removals_right", 0);
                ui::SectionLabel("View / Scope");
                ImGui::Checkbox("Firstperson Legs", &Config::remove_legs);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hide your legs in first person.");

                ImGui::Checkbox("Hide Viewmodel When Scoped", &Config::scope_hide_viewmodel);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hide your gun while scoped.");

                ImGui::Spacing();
                ui::SectionLabel("Scope Lines");
                ImGui::Checkbox("Custom Scope Lines", &Config::scope_custom_lines);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Replace default scope lines with custom ones.");
                if (Config::scope_custom_lines) {
                    ImGui::SliderFloat("##scope_size", &Config::scope_line_size, 0.f, 1.f, "Size %.2f");
                    ImGui::SliderFloat("##scope_gap", &Config::scope_line_gap, 0.f, 40.f, "Gap %.0f");
                    ImGui::SliderFloat("##scope_th", &Config::scope_line_thickness, 0.1f, 6.f, "Thick %.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How thick the scope lines are.");
                    ImGui::ColorEdit4("##scope_col", (float*)&Config::scope_line_color,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::SameLine();
                    ImGui::TextUnformatted("Color");
                }

                ImGui::Spacing();
                ui::SectionLabel("Scope Zoom FOV");
                ImGui::Checkbox("Custom Zoom FOV", &Config::scope_zoom_fov);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Custom zoom amount when scoped.");
                if (Config::scope_zoom_fov) {
                    ImGui::SliderFloat("##scope_fov1", &Config::scope_fov_1, 1.f, 90.f, "Zoom 1  %.0f");
                    ImGui::SliderFloat("##scope_fov2", &Config::scope_fov_2, 1.f, 90.f, "Zoom 2  %.0f");
                }
                ui::EndCard();
            }
        }
        break;

        case 2: // Skin
        {
            static int skinSub = 0; // 0 Knives | 1 Gloves | 2 Weapons | 3 Agents
            {
                static const char* kSkinTabs[] = { "Knives", "Gloves", "Weapons", "Agents" };
                ui::SubNav(kSkinTabs, 4, &skinSub);
            }

            if (skinSub == 0) {
                ui::BeginCard("##skin_knives_left", half);
                ui::SectionLabel("Knife Changer");
                if (ImGui::Checkbox("Enable", &Config::knife_changer)) {
                    SkinChanger::Invalidate();
                    if (Config::knife_changer)
                        Notify::Success("Knife", "Changer enabled");
                    else
                        Notify::Info("Knife", "Changer disabled");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change your knife model and skin.");
                ImGui::Spacing();
                {
                    const auto* knives = SkinChanger::Knives();
                    const int n = SkinChanger::KnifeCount();
                    const char* preview = (Config::knife_index >= 0 && Config::knife_index < n)
                        ? knives[Config::knife_index].display : "Default";
                    ImGui::TextUnformatted("Knife");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::BeginCombo("##knife_model", preview)) {
                        for (int i = 0; i < n; ++i) {
                            const bool sel = (Config::knife_index == i);
                            if (ImGui::Selectable(knives[i].display, sel)) {
                                Config::knife_index = i;
                                Config::knife_paint_kit = 0;
                                Config::knife_paint_kit_id = 0;
                                // Build kit list for this knife now (not wait for frame spam)
                                if (knives[i].def_index)
                                    SkinChanger::PrefetchDef(knives[i].def_index);
                                SkinChanger::Invalidate();
                                Notify::Success("Knife", knives[i].display);
                            }
                            if (knives[i].def_index && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
                                const char* sn = SkinChanger::SimpleNameFor(knives[i].def_index);
                                if ((!sn || !*sn) && knives[i].subclass)
                                    sn = knives[i].subclass;
                                if (sn && *sn)
                                    SkinPreview::DrawHover(SkinPreview::GetPaint(sn, "Vanilla", 0));
                            }
                            if (sel)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    // Kit build: one EnsureKnife/frame via KnifeSkinsReady only
                    // (was Prefetch every frame → double chunk + hitch)
                }
                ImGui::Spacing();
                {
                    std::uint16_t def = 0;
                    const char* tok = "Vanilla";
                    int paintId = 0;
                    const char* subclass = nullptr;
                    if (Config::knife_index > 0 && Config::knife_index < SkinChanger::KnifeCount()) {
                        const auto& k = SkinChanger::Knives()[Config::knife_index];
                        def = k.def_index;
                        subclass = k.subclass; // weapon_knife_karambit — panorama path
                        // paint_kit_id first (config truth); index only for display token
                        paintId = Config::knife_paint_kit_id;
                        if (paintId <= 0 && Config::knife_paint_kit > 0)
                            paintId = SkinChanger::KnifePaintKitId(Config::knife_paint_kit);
                        if (SkinChanger::KnifeSkinsReady()) {
                            const auto& kits = SkinChanger::KnifePaintKits();
                            // Resolve token from id
                            if (paintId > 0) {
                                for (const auto& kit : kits) {
                                    if (kit.id == paintId) {
                                        tok = kit.token.c_str();
                                        break;
                                    }
                                }
                            } else if (Config::knife_paint_kit > 0
                                && Config::knife_paint_kit < (int)kits.size()) {
                                tok = kits[Config::knife_paint_kit].token.c_str();
                                paintId = kits[Config::knife_paint_kit].id;
                            }
                        }
                    }
                    ui::DrawSkinPreviewPanel(def, tok, paintId, subclass);
                }
                ImGui::Spacing();
                ui::SectionLabel("Custom Knife Model");
                if (ImGui::Checkbox("Enable##custom_knife", &Config::custom_knife)) {
                    SkinChanger::InvalidateCustomKnife();
                    if (Config::custom_knife) {
                        SkinChanger::RefreshCustomKnives();
                        if (Config::custom_knife_path[0]) {
                            const int s = SkinChanger::AutoSelectStockKnifeForCustom(
                                Config::custom_knife_path, nullptr);
                            if (s > 0)
                                Notify::Success("Custom Knife",
                                    SkinChanger::Knives()[s].display);
                            else
                                Notify::Success("Custom Knife", "Mesh override on");
                        } else {
                            Notify::Success("Custom Knife", "Mesh override on");
                        }
                    } else {
                        Notify::Info("Custom Knife", "Disabled");
                    }
                    SkinChanger::Invalidate();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Custom mesh. Stock knife auto-picks from pack (karambit/butterfly/...).");
                if (Config::custom_knife) {
                    static bool ckListReady = false;
                    if (!ckListReady) {
                        SkinChanger::RefreshCustomKnives();
                        ckListReady = true;
                    }
                    const auto& ckModels = SkinChanger::CustomKnives();
                    ImGui::SetNextItemWidth(-1.f);
                    const char* ckPreview = Config::custom_knife_path[0]
                        ? Config::custom_knife_path : "[ pick a knife model ]";
                    int ckCur = SkinChanger::CustomKnifeIndexOf(Config::custom_knife_path);
                    if (ckCur >= 0 && ckCur < (int)ckModels.size())
                        ckPreview = ckModels[ckCur].display.c_str();
                    else if (Config::custom_knife_path[0] && ckCur < 0)
                        ckPreview = "[ missing — Refresh or pick ]";
                    if (ImGui::BeginCombo("##custom_knife_pick", ckPreview)) {
                        for (int i = 0; i < (int)ckModels.size(); ++i) {
                            const bool sel = (i == ckCur);
                            if (ImGui::Selectable(ckModels[i].display.c_str(), sel)) {
                                std::snprintf(Config::custom_knife_path,
                                    sizeof(Config::custom_knife_path), "%s",
                                    ckModels[i].model_path.c_str());
                                Config::custom_knife = true;
                                Config::knife_changer = true;
                                // Prefer pre-scanned stock_index (from .vmdl_c embeds)
                                int s = ckModels[i].stock_index;
                                if (s <= 0)
                                    s = SkinChanger::AutoSelectStockKnifeForCustom(
                                        ckModels[i].model_path.c_str(),
                                        ckModels[i].display.c_str());
                                else {
                                    Config::knife_index = s;
                                    SkinChanger::AutoSelectStockKnifeForCustom(
                                        ckModels[i].model_path.c_str(),
                                        ckModels[i].display.c_str());
                                }
                                SkinChanger::InvalidateCustomKnife();
                                SkinChanger::Invalidate();
                                {
                                    char msg[96];
                                    if (s > 0 && s < SkinChanger::KnifeCount())
                                        std::snprintf(msg, sizeof(msg), "Base: %s",
                                            SkinChanger::Knives()[s].display);
                                    else
                                        std::snprintf(msg, sizeof(msg), "Selected");
                                    Notify::Success("Custom Knife", msg);
                                }
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    // Live hint under combo
                    if (ckCur >= 0 && ckCur < (int)ckModels.size()
                        && ckModels[ckCur].stock_index > 0
                        && ckModels[ckCur].stock_index < SkinChanger::KnifeCount()) {
                        ImGui::TextDisabled("Stock base: %s",
                            SkinChanger::Knives()[ckModels[ckCur].stock_index].display);
                    } else if (Config::custom_knife_path[0]) {
                        ImGui::TextDisabled("Stock base: unknown — pick knife manually");
                    }
                    if (ImGui::Button("Refresh knife models", ImVec2(-1.f, 0))) {
                        SkinChanger::RefreshCustomKnives();
                        SkinChanger::InvalidateCustomKnife();
                        ckListReady = true;
                        const int n = (int)SkinChanger::CustomKnives().size();
                        char msg[96];
                        if (n <= 0)
                            std::snprintf(msg, sizeof(msg), "No packs in lefrizzel_models\\knives\\");
                        else
                            std::snprintf(msg, sizeof(msg), "%d knife model(s)", n);
                        Notify::Info("Custom Knife", msg);
                    }
                    if (ckModels.empty())
                        ImGui::TextDisabled("Drop packs in csgo\\lefrizzel_models\\knives\\");
                    else
                        ImGui::TextDisabled("%d packs  |  stock knife + custom mesh", (int)ckModels.size());
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Drop: csgo\\lefrizzel_models\\knives\\");
                ImGui::TextDisabled("Client-side only. Equip knife in-game.");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##skin_knives_right", 0);
                ui::SectionLabel("Skins");
                if (Config::knife_index <= 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                    ImGui::TextWrapped("Select a knife model first.");
                    ImGui::PopStyleColor();
                } else if (!SkinChanger::KnifeSkinsReady()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                    ImGui::TextWrapped("Loading paint kits...");
                    ImGui::PopStyleColor();
                } else {
                    static char knifeSkinSearch[64] = {};
                    static int s_knifeSeededId = -1;
                    const auto& kits = SkinChanger::KnifePaintKits();
                    const char* sn = SkinChanger::SimpleNameFor(
                        SkinChanger::Knives()[Config::knife_index].def_index);
                    if (ui::SkinPaintCombo("##knife_skin", "##knife_skin_search",
                            knifeSkinSearch, (int)sizeof(knifeSkinSearch),
                            kits, &Config::knife_paint_kit, &Config::knife_paint_kit_id, sn)) {
                        s_knifeSeededId = -1; // force Sync to accept new / pending preview
                        Config::knife_colors_edited = false;
                        if (Config::knife_custom_color) {
                            const char* tok = ui::KitTokenAt(kits, Config::knife_paint_kit);
                            if (ui::SafeSeedPaintKit(Config::knife_paint_kit_id,
                                    Config::knife_colors, sn, tok) == CustomPaint::SeedStatus::Ok) {
                                Config::knife_colors_active = true;
                                s_knifeSeededId = Config::knife_paint_kit_id;
                            }
                        }
                        SkinChanger::InvalidateSkin(true);
                        Notify::Success("Skin", kits[Config::knife_paint_kit].name.c_str());
                    }

                    ImGui::TextUnformatted("Wear");
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::SliderFloat("##knife_wear", &Config::knife_wear, 0.f, 1.f, "%.4f");
                    // Repaint only on release — not every drag tick
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        SkinChanger::InvalidateSkin();

                    ImGui::TextUnformatted("Seed");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::InputInt("##knife_seed", &Config::knife_seed)) {
                        if (Config::knife_seed < 0) Config::knife_seed = 0;
                        SkinChanger::InvalidateSkin();
                    }

                    ImGui::TextUnformatted("Nametag");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::InputText("##knife_nametag", Config::knife_custom_name,
                            sizeof(Config::knife_custom_name)))
                        SkinChanger::InvalidateSkin();

                    ImGui::Spacing();
                    ImGui::Separator();
                    if (ImGui::Checkbox("Custom Color##k", &Config::knife_custom_color)) {
                        if (!Config::knife_custom_color) {
                            Config::knife_colors_active = false;
                            Config::knife_colors_edited = false;
                            CustomPaint::RestoreKitColors(Config::knife_paint_kit_id);
                        } else {
                            // Force stock-colour seed on enable (was stuck white)
                            s_knifeSeededId = -1;
                            Config::knife_colors_edited = false;
                            const char* tokEn = ui::KitTokenAt(kits, Config::knife_paint_kit);
                            if (Config::knife_paint_kit_id > 0
                                && ui::SafeSeedPaintKit(Config::knife_paint_kit_id,
                                    Config::knife_colors, sn, tokEn) == CustomPaint::SeedStatus::Ok) {
                                Config::knife_colors_active = true;
                                s_knifeSeededId = Config::knife_paint_kit_id;
                            }
                        }
                        SkinChanger::InvalidateSkin();
                    }
                    {
                        const char* tok = ui::KitTokenAt(kits, Config::knife_paint_kit);
                        if (ui::SyncCustomColorPickers(
                                Config::knife_custom_color, Config::knife_paint_kit_id,
                                Config::knife_colors, &Config::knife_colors_active, &s_knifeSeededId,
                                sn, tok, &Config::knife_colors_edited))
                            SkinChanger::InvalidateSkin();
                    }
                    if (Config::knife_custom_color) {
                        if (ui::ColorQuadRow("##kc", Config::knife_colors)) {
                            Config::knife_colors_active = true;
                            Config::knife_colors_edited = true;
                            SkinChanger::InvalidateSkin();
                        }
                    }
                }
                ui::EndCard();
            } else if (skinSub == 1) {
                ui::BeginCard("##skin_gloves_left", half);
                ui::SectionLabel("Glove Changer");
                if (ImGui::Checkbox("Enable##glove", &Config::glove_changer)) {
                    SkinChanger::InvalidateGloves();
                    if (Config::glove_changer)
                        Notify::Success("Gloves", "Changer enabled");
                    else
                        Notify::Info("Gloves", "Changer disabled");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change your gloves.");
                ImGui::Spacing();
                {
                    const auto* gloves = SkinChanger::Gloves();
                    const int n = SkinChanger::GloveCount();
                    const char* preview = (Config::glove_index >= 0 && Config::glove_index < n)
                        ? gloves[Config::glove_index].display : "Default";
                    ImGui::TextUnformatted("Gloves");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::BeginCombo("##glove_model", preview)) {
                        for (int i = 0; i < n; ++i) {
                            const bool sel = (Config::glove_index == i);
                            if (ImGui::Selectable(gloves[i].display, sel)) {
                                Config::glove_index = i;
                                Config::glove_paint_kit = 0;
                                Config::glove_paint_kit_id = 0;
                                SkinChanger::InvalidateGloves();
                                Notify::Success("Gloves", gloves[i].display);
                            }
                            if (gloves[i].def_index && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
                                const char* sn = SkinChanger::SimpleNameFor(gloves[i].def_index);
                                if (sn && *sn)
                                    SkinPreview::DrawHover(SkinPreview::Get(SkinPreview::ModelPath(sn)));
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
                ImGui::Spacing();
                {
                    std::uint16_t def = 0;
                    const char* tok = "Vanilla";
                    int paintId = 0;
                    if (Config::glove_index > 0 && Config::glove_index < SkinChanger::GloveCount()) {
                        def = SkinChanger::Gloves()[Config::glove_index].def_index;
                        (void)SkinChanger::GloveSkinsReady();
                        const auto& kits = SkinChanger::GlovePaintKits();
                        if (Config::glove_paint_kit >= 0 && Config::glove_paint_kit < (int)kits.size()) {
                            tok = kits[Config::glove_paint_kit].token.c_str();
                            paintId = kits[Config::glove_paint_kit].id;
                        }
                    }
                    ui::DrawSkinPreviewPanel(def, tok, paintId);
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Client-side. Reapply on spawn / round start.");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##skin_gloves_right", 0);
                ui::SectionLabel("Skins");
                if (Config::glove_index <= 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                    ImGui::TextWrapped("Select a glove model first.");
                    ImGui::PopStyleColor();
                } else if (!SkinChanger::GloveSkinsReady()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                    ImGui::TextWrapped("Loading paint kits...");
                    ImGui::PopStyleColor();
                } else {
                    static char gloveSkinSearch[64] = {};
                    static int s_gloveSeededId = -1;
                    const auto& kits = SkinChanger::GlovePaintKits();
                    const char* sn = SkinChanger::SimpleNameFor(
                        SkinChanger::Gloves()[Config::glove_index].def_index);
                    if (ui::SkinPaintCombo("##glove_skin", "##glove_skin_search",
                            gloveSkinSearch, (int)sizeof(gloveSkinSearch),
                            kits, &Config::glove_paint_kit, &Config::glove_paint_kit_id, sn)) {
                        s_gloveSeededId = -1;
                        Config::glove_colors_edited = false;
                        if (Config::glove_custom_color) {
                            const char* tok = ui::KitTokenAt(kits, Config::glove_paint_kit);
                            if (ui::SafeSeedPaintKit(Config::glove_paint_kit_id,
                                    Config::glove_colors, sn, tok) == CustomPaint::SeedStatus::Ok) {
                                Config::glove_colors_active = true;
                                s_gloveSeededId = Config::glove_paint_kit_id;
                            }
                        }
                        SkinChanger::InvalidateGloves();
                        Notify::Success("Glove skin", kits[Config::glove_paint_kit].name.c_str());
                    }

                    ImGui::TextUnformatted("Wear");
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::SliderFloat("##glove_wear", &Config::glove_wear, 0.f, 1.f, "%.4f");
                    // Repaint only on release — not every drag tick
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        SkinChanger::InvalidateGloves();

                    ImGui::TextUnformatted("Seed");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::InputInt("##glove_seed", &Config::glove_seed)) {
                        if (Config::glove_seed < 0) Config::glove_seed = 0;
                        SkinChanger::InvalidateGloves();
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    if (ImGui::Checkbox("Custom Color##g", &Config::glove_custom_color)) {
                        if (!Config::glove_custom_color) {
                            Config::glove_colors_active = false;
                            Config::glove_colors_edited = false;
                            CustomPaint::RestoreKitColors(Config::glove_paint_kit_id);
                        } else {
                            s_gloveSeededId = -1;
                            Config::glove_colors_edited = false;
                            const char* tokEn = ui::KitTokenAt(kits, Config::glove_paint_kit);
                            if (Config::glove_paint_kit_id > 0
                                && ui::SafeSeedPaintKit(Config::glove_paint_kit_id,
                                    Config::glove_colors, sn, tokEn) == CustomPaint::SeedStatus::Ok) {
                                Config::glove_colors_active = true;
                                s_gloveSeededId = Config::glove_paint_kit_id;
                            }
                        }
                        SkinChanger::InvalidateGloves();
                    }
                    {
                        const char* tok = ui::KitTokenAt(kits, Config::glove_paint_kit);
                        if (ui::SyncCustomColorPickers(
                                Config::glove_custom_color, Config::glove_paint_kit_id,
                                Config::glove_colors, &Config::glove_colors_active, &s_gloveSeededId,
                                sn, tok, &Config::glove_colors_edited))
                            SkinChanger::InvalidateGloves();
                    }
                    if (Config::glove_custom_color) {
                        if (ui::ColorQuadRow("##gc", Config::glove_colors)) {
                            Config::glove_colors_active = true;
                            Config::glove_colors_edited = true;
                            SkinChanger::InvalidateGloves();
                        }
                    }
                }
                ui::EndCard();
            } else if (skinSub == 2) {
                ui::BeginCard("##skin_weapons_left", half);
                ui::SectionLabel("Weapon Changer");
                if (ImGui::Checkbox("Enable##weapon_skins", &Config::weapon_skins)) {
                    SkinChanger::InvalidateWeapons();
                    if (Config::weapon_skins)
                        Notify::Success("Weapons", "Skins enabled");
                    else
                        Notify::Info("Weapons", "Skins disabled");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change skins on your guns.");
                ImGui::Spacing();
                {
                    static char weaponSearch[64] = {};
                    if (ui::WeaponCategoryCombo("##weapon_model", "##weapon_search",
                            weaponSearch, (int)sizeof(weaponSearch), &Config::weapon_selected)) {
                        const auto* menu = SkinChanger::WeaponMenu();
                        const int n = SkinChanger::WeaponMenuCount();
                        for (int i = 0; i < n; ++i) {
                            if ((int)menu[i].def_index == Config::weapon_selected) {
                                Notify::Success("Weapon", menu[i].display);
                                break;
                            }
                        }
                    }
                }
                ImGui::Spacing();
                {
                    const std::uint16_t def = (std::uint16_t)Config::weapon_selected;
                    const char* tok = "Vanilla";
                    int paintId = 0;
                    if (def >= 1 && def <= 70) {
                        (void)SkinChanger::WeaponSkinsReady(def);
                        auto& slot = Config::weapon_skin[def];
                        const auto& kits = SkinChanger::WeaponPaintKits(def);
                        if (slot.paint_kit >= 0 && slot.paint_kit < (int)kits.size()) {
                            tok = kits[slot.paint_kit].token.c_str();
                            paintId = kits[slot.paint_kit].id;
                        }
                    }
                    ui::DrawSkinPreviewPanel(def, tok, paintId);
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Grouped: Pistols / Heavy / SMGs / Rifles / Snipers");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##skin_weapons_right", 0);
                ui::SectionLabel("Skins");
                {
                    const std::uint16_t def = (std::uint16_t)Config::weapon_selected;
                    if (def < 1 || def > 70) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                        ImGui::TextWrapped("Select a weapon first.");
                        ImGui::PopStyleColor();
                    } else if (!SkinChanger::WeaponSkinsReady(def)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                        ImGui::TextWrapped("Loading paint kits...");
                        ImGui::PopStyleColor();
                    } else {
                        auto& slot = Config::weapon_skin[def];
                        const auto& kits = SkinChanger::WeaponPaintKits(def);
                        static char wepSkinSearch[64] = {};
                        static int wepSearchDef = -1;
                        static int s_wepSeededId = -1;
                        static int s_wepSeededDef = -1;
                        if (wepSearchDef != (int)def) {
                            wepSkinSearch[0] = '\0';
                            wepSearchDef = (int)def;
                        }
                        if (s_wepSeededDef != (int)def) {
                            s_wepSeededId = -1;
                            s_wepSeededDef = (int)def;
                        }
                        const char* sn = SkinChanger::SimpleNameFor(def);
                        if (ui::SkinPaintCombo("##weapon_skin", "##weapon_skin_search",
                                wepSkinSearch, (int)sizeof(wepSkinSearch),
                                kits, &slot.paint_kit, &slot.paint_kit_id, sn)) {
                            s_wepSeededId = -1;
                            slot.colors_edited = false;
                            if (slot.custom_color) {
                                const char* tok = ui::KitTokenAt(kits, slot.paint_kit);
                                if (ui::SafeSeedPaintKit(slot.paint_kit_id,
                                        slot.colors, sn, tok) == CustomPaint::SeedStatus::Ok) {
                                    slot.colors_active = true;
                                    s_wepSeededId = slot.paint_kit_id;
                                }
                            }
                            SkinChanger::InvalidateWeapons();
                            Notify::Success("Weapon skin", kits[slot.paint_kit].name.c_str());
                        }

                        ImGui::TextUnformatted("Wear");
                        ImGui::SetNextItemWidth(-1.f);
                        ImGui::SliderFloat("##weapon_wear", &slot.wear, 0.f, 1.f, "%.4f");
                        // Repaint only on release — not every drag tick
                        if (ImGui::IsItemDeactivatedAfterEdit())
                            SkinChanger::InvalidateWeapons();

                        ImGui::TextUnformatted("Seed");
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::InputInt("##weapon_seed", &slot.seed)) {
                            if (slot.seed < 0) slot.seed = 0;
                            SkinChanger::InvalidateWeapons();
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        if (ImGui::Checkbox("Custom Color##w", &slot.custom_color)) {
                            if (!slot.custom_color) {
                                slot.colors_active = false;
                                slot.colors_edited = false;
                                CustomPaint::RestoreKitColors(slot.paint_kit_id);
                            } else {
                                s_wepSeededId = -1;
                                slot.colors_edited = false;
                                const char* tokEn = ui::KitTokenAt(kits, slot.paint_kit);
                                if (slot.paint_kit_id > 0
                                    && ui::SafeSeedPaintKit(slot.paint_kit_id,
                                        slot.colors, sn, tokEn) == CustomPaint::SeedStatus::Ok) {
                                    slot.colors_active = true;
                                    s_wepSeededId = slot.paint_kit_id;
                                }
                            }
                            SkinChanger::InvalidateWeapons();
                        }
                        {
                            const char* tok = ui::KitTokenAt(kits, slot.paint_kit);
                            if (ui::SyncCustomColorPickers(
                                    slot.custom_color, slot.paint_kit_id,
                                    slot.colors, &slot.colors_active, &s_wepSeededId,
                                    sn, tok, &slot.colors_edited))
                                SkinChanger::InvalidateWeapons();
                        }
                        if (slot.custom_color) {
                            if (ui::ColorQuadRow("##wc", slot.colors)) {
                                slot.colors_active = true;
                                slot.colors_edited = true;
                                SkinChanger::InvalidateWeapons();
                            }
                        }
                    }
                }
                ui::EndCard();
            } else {
                auto agentPreviewTex = [](const SkinChanger::AgentInfo& a) -> ImTextureID {
                    if (a.def_index == 0) return ImTextureID_Invalid;
                    if (!a.icon_path.empty()) {
                        ImTextureID t = SkinPreview::Get(a.icon_path);
                        if (t != ImTextureID_Invalid) return t;
                    }
                    if (!a.icon_fallback.empty() && a.icon_fallback != a.icon_path)
                        return SkinPreview::Get(a.icon_fallback);
                    return ImTextureID_Invalid;
                };
                auto findAgent = [](int def, int team) -> const SkinChanger::AgentInfo* {
                    if (def <= 0) return nullptr;
                    // Agents() holds stable storage; AgentsForTeam returns temporaries
                    const auto& all = SkinChanger::Agents();
                    for (const auto& a : all) {
                        if ((int)a.def_index != def) continue;
                        if (a.team == team || a.team == 0) return &a;
                    }
                    for (const auto& a : all) {
                        if ((int)a.def_index == def) return &a;
                    }
                    return nullptr;
                };
                auto agentPreviewLabel = [&](int def, int team) -> const char* {
                    if (def <= 0) return "Default";
                    if (const auto* a = findAgent(def, team))
                        return a->display.c_str();
                    return "Default";
                };
                auto drawAgentCombo = [&](const char* label, const char* id, int* defOut, int team) {
                    (void)SkinChanger::AgentsReady();
                    const auto list = SkinChanger::AgentsForTeam(team);
                    ImGui::TextUnformatted(label);
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::BeginCombo(id, agentPreviewLabel(*defOut, team))) {
                        for (const auto& a : list) {
                            const bool sel = ((int)a.def_index == *defOut);
                            if (ImGui::Selectable(a.display.c_str(), sel)) {
                                *defOut = (int)a.def_index;
                                SkinChanger::InvalidateAgents();
                                Notify::Success(team == 3 ? "CT Agent" : "T Agent", a.display.c_str());
                            }
                            if (a.def_index && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
                                SkinPreview::DrawHover(agentPreviewTex(a));
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                };
                auto drawAgentPanel = [&](int def, int team) {
                    if (const auto* a = findAgent(def, team))
                        SkinPreview::DrawPanel(agentPreviewTex(*a), 160.f);
                    else
                        SkinPreview::DrawPanel(ImTextureID_Invalid, 160.f);
                };

                ui::BeginCard("##skin_agents_ct", half);
                ui::SectionLabel("Agent Changer");
                if (ImGui::Checkbox("Enable##agent_changer", &Config::agent_changer)) {
                    SkinChanger::InvalidateAgents();
                    if (Config::agent_changer)
                        Notify::Success("Agents", "Changer enabled");
                    else
                        Notify::Info("Agents", "Changer disabled");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Change your player agent model.");
                ImGui::BeginDisabled(Config::custom_model);
                ImGui::Spacing();
                drawAgentCombo("CT Agent", "##agent_ct", &Config::agent_ct_def, 3);
                ImGui::Spacing();
                drawAgentPanel(Config::agent_ct_def, 3);
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::TextDisabled("Changes your agent model in-game.");
                if (!SkinChanger::AgentsReady())
                    ImGui::TextDisabled("Agent list empty — join a map / wait for econ.");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##skin_agents_t", 0);
                ui::SectionLabel("Terrorist");
                ImGui::BeginDisabled(Config::custom_model);
                drawAgentCombo("T Agent", "##agent_t", &Config::agent_t_def, 2);
                ImGui::Spacing();
                drawAgentPanel(Config::agent_t_def, 2);
                ImGui::EndDisabled();
                ImGui::Spacing();
                ui::SectionLabel("Custom Model");
                if (ImGui::Checkbox("Enable##custom_model", &Config::custom_model)) {
                    SkinChanger::InvalidateAgents();
                    if (Config::custom_model) {
                        SkinChanger::RefreshCustomModels();
                        Notify::Success("Custom Model", "Overrides stock agents");
                    } else {
                        Notify::Info("Custom Model", "Disabled");
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Use a custom player model (local only).");
                if (Config::custom_model) {
                    static bool cmListReady = false;
                    if (!cmListReady) {
                        SkinChanger::RefreshCustomModels();
                        cmListReady = true;
                    }
                    const auto& models = SkinChanger::CustomModels();
                    ImGui::SetNextItemWidth(-1.f);
                    const char* preview = Config::custom_model_path[0]
                        ? Config::custom_model_path : "[ pick a model ]";
                    // Show stem in closed combo when possible
                    int cur = SkinChanger::CustomModelIndexOf(Config::custom_model_path);
                    if (cur >= 0 && cur < (int)models.size())
                        preview = models[cur].display.c_str();
                    else if (Config::custom_model_path[0] && cur < 0)
                        preview = "[ missing — Refresh or pick ]";
                    if (ImGui::BeginCombo("##custom_model_pick", preview)) {
                        for (int i = 0; i < (int)models.size(); ++i) {
                            const bool sel = (i == cur);
                            if (ImGui::Selectable(models[i].display.c_str(), sel)) {
                                std::snprintf(Config::custom_model_path,
                                    sizeof(Config::custom_model_path), "%s",
                                    models[i].model_path.c_str());
                                SkinChanger::InvalidateAgents();
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button("Refresh models", ImVec2(-1.f, 0))) {
                        SkinChanger::RefreshCustomModels();
                        SkinChanger::InvalidateAgents();
                        cmListReady = true;
                        const int n = (int)SkinChanger::CustomModels().size();
                        char msg[96];
                        if (n <= 0)
                            std::snprintf(msg, sizeof(msg), "No packs in lefrizzel_models\\agents\\");
                        else
                            std::snprintf(msg, sizeof(msg), "%d model(s) in drop folder", n);
                        Notify::Info("Custom Model", msg);
                    }
                    if (models.empty())
                        ImGui::TextDisabled("Drop packs in csgo\\lefrizzel_models\\agents\\");
                    else
                        ImGui::TextDisabled("%d models  |  local only", (int)models.size());
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Drop: csgo\\lefrizzel_models\\agents\\");
                ui::EndCard();
            }
        }
        break;

        case 3: // Misc
        {
            ui::BeginCard("##misc_left", half);
            ui::SectionLabel("Movement");
            ImGui::Checkbox("Bunny Hop", &Config::bhop);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto-jump to keep speed. Hold space.");
            ImGui::Checkbox("Auto Strafe", &Config::autostrafe);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto air-strafe while holding space.");
            {
                static const char* kStrafeMode[] = { "Mouse", "Silent WASD Subtick" };
                ui::ComboFull("Strafe Mode", "##strafe_mode", &Config::autostrafe_mode, kStrafeMode, 2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Mouse: A/D from mouse. Silent: subtick analog + WASD (hold space in air). "
                        "WASD keys bias direction; none = full circle strafe.");
            }
            ImGui::Checkbox("Jumpbug", &Config::jumpbug);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Unduck + jump at land frac (crouch-land cancel). Hold space optional with bhop.");
            if (Config::jumpbug)
                KeybindRow(Config::jumpbug);
            ImGui::Checkbox("Edgebug", &Config::edgebug);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Duck on edge land to keep XY speed. Bind key.");
            if (Config::edgebug)
                KeybindRow(Config::edgebug);
            ImGui::Checkbox("Edgejump", &Config::edgejump);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Jump the frame you leave a ledge (probe ahead). Bind key.");
            if (Config::edgejump)
                KeybindRow(Config::edgejump);
            ImGui::Spacing();
            ui::SectionLabel("Combat");
            ImGui::Checkbox("Auto Pistol", &Config::auto_pistol);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hold M1 on semi pistols — re-fires when ready.");
            if (Config::auto_pistol) {
                ui::SliderFull("Delay", "##ap_delay",
                    &Config::auto_pistol_delay_ms, 0.f, 250.f, "%.0f ms");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Extra wait after weapon ready. 0 = as fast as cycle.");
            }
            ImGui::Spacing();
            ui::SectionLabel("Backtrack");
            if (ImGui::Checkbox("Backtrack", &Config::backtrack)) {
                // Keep legacy aliases synced for any leftover paths
                Config::autofire_backtrack = Config::backtrack;
                Config::trigger_backtrack = Config::backtrack;
                Config::autofire_backtrack_ms = Config::backtrack_ms;
                Config::trigger_backtrack_ms = Config::backtrack_ms;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Global lag records. AF/TR shoot lag heads. Skeleton/Chams are visual sub-toggles.");
            if (Config::backtrack) {
                if (ui::SliderFull("Time", "##bt_ms",
                        &Config::backtrack_ms, 50.f, 400.f, "%.0f ms")) {
                    Config::autofire_backtrack_ms = Config::backtrack_ms;
                    Config::trigger_backtrack_ms = Config::backtrack_ms;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Max lag age (ms). Skeleton in Visuals, chams in Chams.");
            }
            ImGui::Spacing();
            ui::SectionLabel("Hit Log");
            ImGui::Checkbox("Hit Log", &Config::hitlog);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Floating damage log panel (player_hurt).\n"
                    "Menu open: drag panel, right-click for options.");
            ImGui::SetNextWindowSize(ImVec2(280.f, 0.f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupContextItem("##hitlog_pop")) {
                ui::PopupTitle("Hit Log");
                ImGui::Checkbox("Game Console", &Config::hitlog_console);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Also print hits to developer console (~).");
                ImGui::Checkbox("Show HP left", &Config::hitlog_show_hp);
                ImGui::Checkbox("Session stats", &Config::hitlog_show_stats);
                ImGui::EndPopup();
            }
            if (Config::hitlog) {
                ImGui::SliderFloat("##hitlog_life", &Config::hitlog_duration, 1.f, 12.f, "Duration %.1f s");
                ImGui::SliderFloat("##hitlog_w", &Config::hitlog_width, 200.f, 380.f, "Width %.0f");
                ImGui::SliderInt("##hitlog_rows", &Config::hitlog_max_rows, 4, 16, "Rows %d");
                ImGui::Checkbox("HP left##hl", &Config::hitlog_show_hp);
                ImGui::SameLine();
                ImGui::Checkbox("Stats##hl", &Config::hitlog_show_stats);
                ImGui::ColorEdit4("Log", (float*)&Config::hitlog_color, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit4("Head##hl", (float*)&Config::hitlog_head_color, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit4("Kill##hl", (float*)&Config::hitlog_kill_color, ImGuiColorEditFlags_NoInputs);
            }
            // Subtick Move: always on (hardcoded)
            ImGui::Spacing();
            ui::SectionLabel("Matchmaking");
            ImGui::Checkbox("Auto Accept", &Config::auto_accept);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto-accept match found.");
            ImGui::Spacing();
            ui::SectionLabel("Vote");
            ImGui::Checkbox("Vote Reveal", &Config::vote_reveal);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toast + chat + developer console (~) who voted yes/no.");
            ImGui::Checkbox("Auto Vote", &Config::vote_auto);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto-vote yes or no after a short delay.");
            if (Config::vote_auto) {
                static const char* kVoteChoice[] = { "Yes", "No" };
                ui::ComboFull("Vote Choice", "##vote_choice", &Config::vote_auto_choice, kVoteChoice, 2);
                ImGui::SliderFloat("##vote_delay", &Config::vote_auto_delay_ms, 0.f, 2000.f, "Delay %.0f ms");
            }
            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##misc_right", 0);
            try {
            ui::SectionLabel("Hitsound");
            ImGui::Checkbox("Hitsound", &Config::hitsound);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Play a sound when you hit someone.");
            if (Config::hitsound) {
                static bool hsListReady = false;
                if (!hsListReady) {
                    Hitsound::RefreshList();
                    hsListReady = true;
                }
                const int n = Hitsound::Count();
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);

                static std::vector<std::string> hsNames;
                static std::vector<const char*> hsPtrs;
                hsNames.clear();
                hsPtrs.clear();
                hsNames.reserve(static_cast<size_t>(n) + 1);
                for (int i = 0; i < n; ++i) {
                    const char* nm = Hitsound::NameAt(i);
                    hsNames.emplace_back(nm ? nm : "");
                }
                for (auto& s : hsNames) hsPtrs.push_back(s.c_str());

                ImGui::TextUnformatted("Hit sound");
                if (n <= 0) {
                    ImGui::TextDisabled("No .wav in hitsounds folder");
                } else {
                    int cur = Hitsound::IndexOf(Config::hitsound_file);
                    if (cur < 0) cur = 0;
                    if (cur >= n) cur = 0;
                    if (ImGui::Combo("##hs_file", &cur, hsPtrs.data(), n)) {
                        if (cur >= 0 && cur < n)
                            std::snprintf(Config::hitsound_file, sizeof(Config::hitsound_file), "%s", hsNames[cur].c_str());
                    }
                }

                // Optional head/kill: index 0 = same as hit
                auto drawOptional = [&](const char* id, char* buf, size_t bufSz) {
                    std::vector<std::string> names;
                    names.reserve(static_cast<size_t>(n) + 1);
                    names.emplace_back("(same as hit)");
                    for (int i = 0; i < n; ++i)
                        names.push_back(hsNames[static_cast<size_t>(i)]);
                    std::vector<const char*> ptrs;
                    ptrs.reserve(names.size());
                    for (auto& s : names) ptrs.push_back(s.c_str());
                    int cur = Hitsound::IndexOf(buf);
                    int sel = (cur < 0) ? 0 : cur + 1;
                    if (sel >= static_cast<int>(ptrs.size())) sel = 0;
                    if (ImGui::Combo(id, &sel, ptrs.data(), static_cast<int>(ptrs.size()))) {
                        if (sel == 0) buf[0] = 0;
                        else std::snprintf(buf, bufSz, "%s", names[static_cast<size_t>(sel)].c_str());
                    }
                };
                ImGui::TextUnformatted("Headshot (optional)");
                drawOptional("##hs_head", Config::hitsound_head, sizeof(Config::hitsound_head));
                ImGui::TextUnformatted("Kill (optional)");
                drawOptional("##hs_kill", Config::hitsound_kill, sizeof(Config::hitsound_kill));

                ImGui::PopItemWidth();
                if (ImGui::Button("Refresh list", ImVec2(-1.f, 0))) {
                    Hitsound::RefreshList();
                    hsListReady = true;
                }
                if (ImGui::Button("Test sound", ImVec2(-1.f, 0)))
                    Hitsound::PreviewSelected();
                ImGui::TextDisabled("Drop .wav into csgo\\sounds\\hitsounds");
            }
            ImGui::Spacing();
            ui::SectionLabel("Hitmarker");
            ImGui::Checkbox("Hitmarker", &Config::hitmarker);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show an X marker when you hit someone.");
            if (Config::hitmarker) {
                ImGui::Checkbox("Screen", &Config::hitmarker_screen);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("X in the center of your screen.");
                ImGui::Checkbox("World 3D", &Config::hitmarker_world);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("X at the spot you hit on the enemy.");
                ImGui::Checkbox("Show Damage", &Config::hitmarker_show_damage);

                // Sliders/colors first so preview reads same-frame values
                const float hmW = ImGui::GetContentRegionAvail().x;
                ImGui::PushItemWidth(hmW);
                ImGui::TextUnformatted("Size");
                ImGui::SliderFloat("##hm_size", &Config::hitmarker_size, 6.f, 32.f, "%.0f");
                ImGui::TextUnformatted("Gap");
                ImGui::SliderFloat("##hm_gap", &Config::hitmarker_gap, 1.f, 12.f, "%.0f");
                ImGui::TextUnformatted("Thickness");
                ImGui::SliderFloat("##hm_thick", &Config::hitmarker_thickness, 1.f, 5.f, "%.1f");
                ImGui::TextUnformatted("World Size");
                ImGui::SliderFloat("##hm_wsize", &Config::hitmarker_world_size, 4.f, 28.f, "%.0f");
                ImGui::TextUnformatted("Duration");
                ImGui::SliderFloat("##hm_dur", &Config::hitmarker_duration, 0.25f, 2.5f, "%.2f");
                ImGui::PopItemWidth();

                // Stack when narrow — three labeled pickers on one line clip half-cards
                {
                    const ImGuiColorEditFlags colHm = ImGuiColorEditFlags_NoInputs
                        | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoTooltip;
                    const float need = ImGui::CalcTextSize("Color").x
                        + ImGui::CalcTextSize("Head").x
                        + ImGui::CalcTextSize("Kill").x
                        + 3.f * 28.f + 24.f;
                    if (ImGui::GetContentRegionAvail().x >= need) {
                        ImGui::ColorEdit4("Color##hm", (float*)&Config::hitmarker_color, colHm);
                        ImGui::SameLine(0, 8.f);
                        ImGui::ColorEdit4("Head##hm", (float*)&Config::hitmarker_head_color, colHm);
                        ImGui::SameLine(0, 8.f);
                        ImGui::ColorEdit4("Kill##hm", (float*)&Config::hitmarker_kill_color, colHm);
                    } else {
                        ImGui::ColorEdit4("Color##hm", (float*)&Config::hitmarker_color, colHm);
                        ImGui::ColorEdit4("Head##hm", (float*)&Config::hitmarker_head_color, colHm);
                        ImGui::ColorEdit4("Kill##hm", (float*)&Config::hitmarker_kill_color, colHm);
                    }
                }

                // Live COD-X — after widgets so drag updates this frame; pulse + world sample
                {
                    static int s_hmPrevMode = 0; // 0 normal / 1 head / 2 kill
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Preview");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Live preview of the hitmarker.");
                    const float boxH = 88.f;
                    const float boxW = ImGui::GetContentRegionAvail().x;
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    const ImVec2 p1(p0.x + boxW, p0.y + boxH);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 20, 210), 6.f);
                    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 40), 6.f, 0, 1.f);
                    Hitmarker::DrawPreview(dl, p0, p1, s_hmPrevMode);
                    ImGui::Dummy(ImVec2(boxW, boxH));
                    ImGui::Spacing();
                    if (ImGui::RadioButton("Normal##hmp", s_hmPrevMode == 0)) s_hmPrevMode = 0;
                    ImGui::SameLine(0, 10.f);
                    if (ImGui::RadioButton("Head##hmp", s_hmPrevMode == 1)) s_hmPrevMode = 1;
                    ImGui::SameLine(0, 10.f);
                    if (ImGui::RadioButton("Kill##hmp", s_hmPrevMode == 2)) s_hmPrevMode = 2;
                }
            }
            ImGui::Checkbox("Floating Damage", &Config::float_damage);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Floating damage numbers on hit.");
            if (Config::float_damage) {
                ui::SliderFull("Float Life", "##fd_life", &Config::float_damage_duration, 0.3f, 2.5f, "%.2f");
                ui::SliderFull("Float Speed", "##fd_speed", &Config::float_damage_speed, 20.f, 120.f, "%.0f");
                {
                    const ImGuiColorEditFlags colFd = ImGuiColorEditFlags_NoInputs
                        | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoTooltip;
                    ImGui::ColorEdit4("Dmg##fd", (float*)&Config::float_damage_color, colFd);
                    ImGui::SameLine(0, 8.f);
                    ImGui::ColorEdit4("Head##fd", (float*)&Config::float_damage_head_color, colFd);
                    ImGui::SameLine(0, 8.f);
                    ImGui::ColorEdit4("Kill##fd", (float*)&Config::float_damage_kill_color, colFd);
                }
            }
            ImGui::Spacing();
            ui::SectionLabel("Widgets");
            ImGui::Checkbox("Watermark", &Config::watermark);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show FPS and time on screen.");
            ImGui::Checkbox("Keybind List", &Config::widget_keybinds);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("List of your keybinds. Drag in menu to move.");
            ImGui::Checkbox("Keybind Strip (HUD)", &Config::hud_keybind_strip);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Keybind bar at the bottom of the HUD.");
            ImGui::Checkbox("Bomb Info", &Config::widget_bomb);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bomb timer, site, and damage to you.");
            if (Config::widget_bomb) {
                ImGui::Checkbox("Auto Defuse", &Config::auto_defuse);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Auto start defuse when near the bomb.");
            }
            ImGui::Checkbox("Spectator List", &Config::widget_spectators);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Who is watching you.");
            ImGui::Checkbox("Free Spectate", &Config::enemy_spectate);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "When dead: watch any alive player (enemy + teammates).\n"
                    "Next: LMB / Right / ] / Mouse5\n"
                    "Prev: RMB / Left / [ / Mouse4");
            if (Config::enemy_spectate) {
                ImGui::Checkbox("3rd Person Cam", &Config::enemy_spectate_thirdperson);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Chase cam instead of first-person.");
            }
            ImGui::Checkbox("Radar", &Config::widget_radar);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mini-map radar of nearby players.");
            if (Config::widget_radar) {
                ImGui::SetNextItemWidth(-1);
                const char* shapes[] = { "Circle", "Square" };
                ImGui::Combo("##radar_shape", &Config::widget_radar_shape, shapes, IM_ARRAYSIZE(shapes));
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##radar_size", &Config::widget_radar_size, 90.f, 280.f, "Size %.0f");
            }
            } catch (...) {}
            ui::EndCard();
        }
        break;

        case 4: // Config
        {
            static char configName[128] = "";
            static std::vector<std::string> configList = internal_config::ConfigManager::ListConfigs();
            static int selectedConfigIndex = -1;
            static std::string statusMsg;
            static float statusUntil = 0.f;

            auto setStatus = [&](const char* msg, bool ok = true, bool warn = false) {
                statusMsg = msg;
                statusUntil = static_cast<float>(ImGui::GetTime()) + 2.5f;
                if (warn)
                    Notify::Warn("Config", msg);
                else if (ok)
                    Notify::Success("Config", msg);
                else
                    Notify::Error("Config", msg);
            };

            ui::BeginCard("##cfg_left", half);
            ui::SectionLabel("Manage");

            ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
            ImGui::TextWrapped("Documents\\Lefrizzel AI\\Configs");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##cfgname", "Config name...", configName, IM_ARRAYSIZE(configName));

            ImGui::Spacing();
            const float gapBtn = 6.f;
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float base = floorf((std::max)(0.f, avail - gapBtn * 2.f) / 3.f);
                const float last = (std::max)(1.f, avail - base * 2.f - gapBtn * 2.f);

                if (ImGui::Button("Save", ImVec2((std::max)(1.f, base), 0))) {
                    if (configName[0] == '\0') {
                        setStatus("Enter a config name", false, true);
                    } else if (internal_config::ConfigManager::Save(configName)) {
                        configList = internal_config::ConfigManager::ListConfigs();
                        setStatus("Saved");
                    } else {
                        setStatus("Save failed", false);
                    }
                }
                ImGui::SameLine(0, gapBtn);
                if (ImGui::Button("Load", ImVec2((std::max)(1.f, base), 0))) {
                    if (configName[0] == '\0') {
                        setStatus("Select or type a name", false, true);
                    } else if (internal_config::ConfigManager::Load(configName)) {
                        setStatus("Loaded");
                    } else {
                        setStatus("Load failed", false);
                    }
                }
                ImGui::SameLine(0, gapBtn);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.28f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.28f, 0.35f, 0.70f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.32f, 0.38f, 0.85f));
                if (ImGui::Button("Delete", ImVec2(last, 0))) {
                    if (configName[0] == '\0') {
                        setStatus("Select a config first", false, true);
                    } else if (internal_config::ConfigManager::Remove(configName)) {
                        configName[0] = '\0';
                        selectedConfigIndex = -1;
                        configList = internal_config::ConfigManager::ListConfigs();
                        setStatus("Deleted");
                    } else {
                        setStatus("Delete failed", false);
                    }
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float half0 = floorf((std::max)(0.f, avail - gapBtn) * 0.5f);
                const float half1 = (std::max)(1.f, avail - half0 - gapBtn);
                if (ImGui::Button("Open Folder", ImVec2(half0, 0))) {
                    internal_config::ConfigManager::OpenFolder();
                    Notify::Info("Config", "Opened configs folder");
                }
                ImGui::SameLine(0, gapBtn);
                if (ImGui::Button("Refresh", ImVec2(half1, 0))) {
                    configList = internal_config::ConfigManager::ListConfigs();
                    setStatus("Refreshed");
                }
            }

            if (!statusMsg.empty() && ImGui::GetTime() < statusUntil) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent());
                ImGui::TextUnformatted(statusMsg.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ui::SectionLabel("Design");

            const ImGuiColorEditFlags designCol =
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

            static int s_preset = 1; // Steel
            // Full skins: accent + bg/cards/sidebar/border/text + rounding/opacity/compact
            const char* presets[] = {
                "Mist", "Steel", "Neutral", "Sage", "Copper", "Rose", "Violet", "Amber"
            };
            if (ui::ComboFull("Preset", "##menu_preset", &s_preset, presets, IM_ARRAYSIZE(presets)))
                ui::ApplyMenuPreset(s_preset);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Accent + neutral shell. Not monochrome paint.");
            if (ImGui::Button("Reset Design", ImVec2(-1.f, 0))) {
                ui::ApplyMenuPreset(1); // Steel compact default
                s_preset = 1;
            }

            ImGui::Spacing();
            ImGui::ColorEdit4("Accent", (float*)&Config::menu_accent, designCol);
            ImGui::ColorEdit4("Background", (float*)&Config::menu_bg, designCol);
            ImGui::ColorEdit4("Cards", (float*)&Config::menu_child_bg, designCol);
            ImGui::ColorEdit4("Sidebar", (float*)&Config::menu_sidebar_bg, designCol);
            ImGui::ColorEdit4("Border", (float*)&Config::menu_border, designCol);
            ImGui::ColorEdit4("Text", (float*)&Config::menu_text, designCol);
            ImGui::ColorEdit4("Muted", (float*)&Config::menu_text_muted, designCol);

            ui::SliderFull("Rounding", "##menu_rounding", &Config::menu_rounding, 2.f, 8.f, "%.0f");
            ui::SliderFull("Opacity", "##menu_opacity", &Config::menu_opacity, 0.70f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shell opacity. Lower = more game visible.");
            {
                static const char* kDpi[] = {
                    "100%", "125%", "150%", "175%", "200%"
                };
                int dpiIdx = 0;
                const int p = Config::menu_dpi_scale;
                if (p >= 200) dpiIdx = 4;
                else if (p >= 175) dpiIdx = 3;
                else if (p >= 150) dpiIdx = 2;
                else if (p >= 125) dpiIdx = 1;
                else dpiIdx = 0;
                if (ui::ComboFull("DPI Scale", "##menu_dpi", &dpiIdx, kDpi, IM_ARRAYSIZE(kDpi))) {
                    static const int kPct[] = { 100, 125, 150, 175, 200 };
                    if (dpiIdx >= 0 && dpiIdx < 5)
                        Config::menu_dpi_scale = kPct[dpiIdx];
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Menu size for 4K / high-DPI. 100%% = default.");
            }
            ImGui::Checkbox("Compact spacing", &Config::menu_compact);

            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##cfg_right", 0);
            ui::SectionLabel("Saved Configs");

            if (configList.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted());
                ImGui::TextUnformatted("No configs yet.");
                ImGui::TextUnformatted("Save one to get started.");
                ImGui::PopStyleColor();
            } else {
                const float listH = (std::max)(80.f, ImGui::GetContentRegionAvail().y);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.15f));
                ImGui::BeginChild("##cfg_list", ImVec2(-1.f, listH),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_None);
                for (int i = 0; i < static_cast<int>(configList.size()); ++i) {
                    const bool sel = (selectedConfigIndex == i);
                    if (ImGui::Selectable(configList[i].c_str(), sel)) {
                        selectedConfigIndex = i;
                        strncpy_s(configName, sizeof(configName), configList[i].c_str(), _TRUNCATE);
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (internal_config::ConfigManager::Load(configList[i])) {
                            setStatus("Loaded");
                        } else {
                            setStatus("Load failed", false);
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ui::EndCard();
        }
        break;
        }
    }
    MenuUI::EndContent();
    ImGui::PopStyleVar(); // content alpha

    MenuUI::DrawWindowFrame(ImGui::GetWindowDrawList(), wpos,
        ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), round);

    ImGui::End();
    ImGui::PopStyleVar(2); // window alpha + rounding
}

void Menu::toggleMenu() {
    showMenu = !showMenu;
    if (showMenu) {
        MenuUI::NotifyTab(activeTab);
    } else {
        // Snap anim dead so nothing lingers next frame
        MenuUI::AnimTick(false);
    }
}
