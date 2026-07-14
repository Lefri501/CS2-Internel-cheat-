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
#include "../features/skinchanger/agent_changer.h"
#include "../features/skinchanger/skin_preview.h"
#include "../features/gamemode/gamemode.h"
#include "../features/notify/notify.h"

#include "../utils/logging/log.h"
#include "../utils/security/vacdetect.h"
#include "../features/visuals/assets/obs_icons.hpp"

ImFont* g_WeaponIconFont = nullptr;

namespace ui {
    static constexpr ImVec4 Accent       = ImVec4(0.58f, 0.38f, 0.98f, 1.00f);
    static constexpr ImVec4 AccentSoft   = ImVec4(0.58f, 0.38f, 0.98f, 0.55f);
    static constexpr ImVec4 AccentHover  = ImVec4(0.68f, 0.50f, 1.00f, 1.00f);
    static constexpr ImVec4 AccentDim    = ImVec4(0.40f, 0.24f, 0.72f, 1.00f);
    static constexpr ImVec4 TextMuted    = ImVec4(0.70f, 0.70f, 0.78f, 1.00f);
    static constexpr ImVec4 TextBright   = ImVec4(0.96f, 0.96f, 1.00f, 1.00f);

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
        if (*paintIdx < 0 || *paintIdx >= (int)kits.size()) {
            // Keep paint_kit_id if kits not fully resolved yet (only Vanilla present)
            if (kits.size() <= 1 && *paintId > 0)
                *paintIdx = 0;
            else
                *paintIdx = 0;
        }
        if (*paintIdx > 0 && *paintIdx < (int)kits.size())
            *paintId = kits[*paintIdx].id;
        else if (*paintIdx <= 0 && kits.size() > 1)
            *paintId = 0;
        // else: leave *paintId alone when only Vanilla loaded / id already set from config

        const char* preview = kits[*paintIdx].name.c_str();
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
                ImGui::PushStyleColor(ImGuiCol_Text, TextMuted);
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
            ImGui::PushStyleColor(ImGuiCol_Text, TextMuted);
            ImGui::TextUnformatted("No skins match.");
            ImGui::PopStyleColor();
        }
        ImGui::EndCombo();
        return changed;
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

            ImGui::PushStyleColor(ImGuiCol_Text, Accent);
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
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
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

    static void DrawSkinPreviewPanel(std::uint16_t def, const char* kitToken, int paintKitId) {
        const char* sn = (def != 0) ? SkinChanger::SimpleNameFor(def) : nullptr;
        if (!sn || !*sn) {
            SkinPreview::DrawPanel(ImTextureID_Invalid, 160.f);
            return;
        }
        SkinPreview::DrawPanel(SkinPreview::GetPaint(sn, kitToken, paintKitId), 160.f);
    }

    static void DrawGlassDecor(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding) {
        // Soft outer glow border
        dl->AddRect(min, max, IM_COL32(160, 110, 255, 70), rounding, 0, 1.5f);
        // Top glossy highlight strip
        dl->AddRectFilledMultiColor(
            ImVec2(min.x + 1.f, min.y + 1.f),
            ImVec2(max.x - 1.f, min.y + 28.f),
            IM_COL32(255, 255, 255, 28),
            IM_COL32(255, 255, 255, 28),
            IM_COL32(255, 255, 255, 0),
            IM_COL32(255, 255, 255, 0));
        // Accent top edge
        dl->AddLine(
            ImVec2(min.x + rounding, min.y + 1.f),
            ImVec2(max.x - rounding, min.y + 1.f),
            IM_COL32(180, 130, 255, 160), 1.5f);
    }

    static void SectionLabel(const char* label) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Accent);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x, p.y),
            ImVec2(p.x + w, p.y),
            IM_COL32(148, 100, 255, 90), 1.0f);
        ImGui::Dummy(ImVec2(0, 6));
    }

    static bool BeginCard(const char* id, float width) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.14f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.40f, 0.90f, 0.22f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        // Borders already apply padding; keep AlwaysUseWindowPadding for consistency
        const bool open = ImGui::BeginChild(id, ImVec2(width, 0),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        if (open) {
            ImVec2 min = ImGui::GetWindowPos();
            ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
                min, ImVec2(max.x, min.y + 22.f),
                IM_COL32(255, 255, 255, 16),
                IM_COL32(255, 255, 255, 16),
                IM_COL32(255, 255, 255, 0),
                IM_COL32(255, 255, 255, 0));
        }
        return open;
    }

    static void EndCard() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // Checkbox + right-click opens settings popup (call BeginPopup after this)
    static bool FeatureToggle(const char* label, bool* v) {
        const bool changed = ImGui::Checkbox(label, v);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Right-click for settings");
        return changed;
    }

    static void PopupTitle(const char* title) {
        ImGui::PushStyleColor(ImGuiCol_Text, Accent);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
    }

    static bool NavButton(const char* label, bool selected, const ImVec2& size) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.38f, 0.98f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.38f, 0.98f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.38f, 0.98f, 0.70f));
            ImGui::PushStyleColor(ImGuiCol_Text, TextBright);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.03f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.38f, 0.98f, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.38f, 0.98f, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_Text, TextMuted);
        }
        bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        if (selected) {
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(min.x, min.y + 4.f),
                ImVec2(min.x + 3.f, max.y - 4.f),
                IM_COL32(170, 120, 255, 230), 2.f);
        }
        return pressed;
    }
}

void ApplyImGuiTheme() {
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    // Glass base
    c[ImGuiCol_WindowBg]             = ImVec4(0.07f, 0.07f, 0.10f, 0.78f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.09f, 0.09f, 0.13f, 0.50f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.08f, 0.12f, 0.94f);
    c[ImGuiCol_Border]               = ImVec4(0.55f, 0.40f, 0.90f, 0.28f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text]                 = ImVec4(0.96f, 0.96f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.58f, 1.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.12f, 0.18f, 0.70f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.16f, 0.30f, 0.80f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.20f, 0.42f, 0.85f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.07f, 0.07f, 0.10f, 0.90f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.07f, 0.07f, 0.10f, 0.95f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.07f, 0.07f, 0.10f, 0.60f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.09f, 0.09f, 0.12f, 0.80f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.05f, 0.05f, 0.08f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.45f, 0.30f, 0.80f, 0.55f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.58f, 0.38f, 0.98f, 0.75f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.68f, 0.50f, 1.00f, 0.90f);

    c[ImGuiCol_CheckMark]            = ui::Accent;
    c[ImGuiCol_CheckboxSelectedBg]   = ImVec4(0.58f, 0.38f, 0.98f, 0.35f);
    c[ImGuiCol_SliderGrab]           = ui::Accent;
    c[ImGuiCol_SliderGrabActive]     = ui::AccentHover;

    c[ImGuiCol_Button]               = ImVec4(0.58f, 0.38f, 0.98f, 0.40f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.58f, 0.38f, 0.98f, 0.65f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.48f, 0.28f, 0.88f, 0.85f);

    c[ImGuiCol_Header]               = ImVec4(0.58f, 0.38f, 0.98f, 0.35f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.58f, 0.38f, 0.98f, 0.50f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.58f, 0.38f, 0.98f, 0.65f);

    c[ImGuiCol_Separator]            = ImVec4(0.55f, 0.40f, 0.90f, 0.30f);
    c[ImGuiCol_SeparatorHovered]     = ui::AccentSoft;
    c[ImGuiCol_SeparatorActive]      = ui::Accent;

    c[ImGuiCol_ResizeGrip]           = ImVec4(0.58f, 0.38f, 0.98f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.58f, 0.38f, 0.98f, 0.50f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.58f, 0.38f, 0.98f, 0.75f);

    c[ImGuiCol_Tab]                  = ImVec4(0.12f, 0.12f, 0.16f, 0.70f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.58f, 0.38f, 0.98f, 0.55f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.58f, 0.38f, 0.98f, 0.50f);
    c[ImGuiCol_TabSelectedOverline]  = ui::Accent;
    c[ImGuiCol_TabDimmed]            = ImVec4(0.10f, 0.10f, 0.14f, 0.60f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.40f, 0.24f, 0.72f, 0.55f);

    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.58f, 0.38f, 0.98f, 0.35f);
    c[ImGuiCol_NavCursor]            = ui::Accent;
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);

    // Rounded glossy geometry
    style.WindowRounding    = 14.0f;
    style.ChildRounding     = 10.0f;
    style.FrameRounding     = 7.0f;
    style.PopupRounding     = 10.0f;
    style.ScrollbarRounding = 10.0f;
    style.GrabRounding      = 8.0f;
    style.TabRounding       = 8.0f;

    style.WindowPadding     = ImVec2(0, 0);
    style.FramePadding      = ImVec2(10, 6);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(8, 5);
    style.CellPadding       = ImVec2(6, 4);
    style.IndentSpacing     = 16.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;

    style.ScrollbarSize     = 10.0f;
    style.GrabMinSize       = 10.0f;
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);

    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;
    style.Alpha             = 1.0f;
    style.DisabledAlpha     = 0.45f;
}

Menu::Menu() {
    activeTab = 0;
    showMenu = false; // must start closed — open menu disables relative mouse and drifts the cursor
}

void Menu::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(pDevice, pContext);

    SkinPreview::Init(pDevice);

    ApplyImGuiTheme();

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

    std::cout << "initialized menu\n";
}

void Menu::render() {
    // Always poll (handles menu-open / listening guards internally)
    keybind.pollInputs(showMenu);

    if (!showMenu)
        return;

    constexpr float kWidth = 960.f;
    constexpr float kHeight = 640.f;
    constexpr float kSidebar = 160.f;
    constexpr float kHeader = 52.f;

    // Apply larger default once per inject (old Cond_Once kept cramped 720x480)
    static bool s_appliedSize = false;
    if (!s_appliedSize) {
        ImGui::SetNextWindowSize(ImVec2(kWidth, kHeight), ImGuiCond_Always);
        s_appliedSize = true;
    }
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(820, 560), ImVec2(1400, 1000));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("TempleWare", nullptr, flags);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    ui::DrawGlassDecor(dl, wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), ImGui::GetStyle().WindowRounding);

    // ── Header ──────────────────────────────────────────────
    {
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
        ImGui::BeginChild("##header", ImVec2(0, kHeader),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);

        ImVec2 hmin = ImGui::GetWindowPos();
        ImVec2 hmax = ImVec2(hmin.x + ImGui::GetWindowSize().x, hmin.y + ImGui::GetWindowSize().y);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            hmin, hmax,
            IM_COL32(90, 55, 170, 55),
            IM_COL32(40, 30, 80, 20),
            IM_COL32(40, 30, 80, 20),
            IM_COL32(90, 55, 170, 55));

        ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent);
        ImGui::TextUnformatted("TempleWare");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
        ImGui::TextUnformatted("internal");
        ImGui::PopStyleColor();

        if (VacDetect::IsSoftPaused()) {
            char vacHint[64];
            snprintf(vacHint, sizeof(vacHint), "SOFT-PAUSE %us",
                VacDetect::SoftPauseRemainingMs() / 1000u);
            ImGui::SameLine(0, 14);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.2f, 1.f));
            ImGui::TextUnformatted(vacHint);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("VAC-like DLL detected — aim/ESP/glow/weather/skin paused.\nHooks stay live. Auto-resumes.");
            ImGui::SameLine(0, 8);
            if (ImGui::SmallButton("Resume"))
                VacDetect::SoftResume();
        }

        const char* hint = "INSERT  toggle";
        const float hintW = ImGui::CalcTextSize(hint).x;
        const float rightX = ImGui::GetWindowWidth() - hintW - ImGui::GetStyle().WindowPadding.x;
        if (rightX > ImGui::GetCursorPosX() + 20.f)
            ImGui::SameLine(rightX);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.65f, 0.90f));
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // ── Body: sidebar + content ─────────────────────────────
    ImGui::SetCursorPos(ImVec2(0, kHeader));

    // Sidebar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 16));
    ImGui::BeginChild("##sidebar", ImVec2(kSidebar, -1),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    {
        ImVec2 smin = ImGui::GetWindowPos();
        ImVec2 smax = ImVec2(smin.x + ImGui::GetWindowSize().x, smin.y + ImGui::GetWindowSize().y);
        ImGui::GetWindowDrawList()->AddRectFilled(smin, smax, IM_COL32(12, 12, 18, 120));
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(smax.x - 1.f, smin.y + 8.f),
            ImVec2(smax.x - 1.f, smax.y - 8.f),
            IM_COL32(140, 100, 255, 40), 1.f);

        const char* tabs[] = { "Aim", "Visuals", "Skin", "Misc", "Config" };
        const ImVec2 btnSize(ImGui::GetContentRegionAvail().x, 36.f);
        for (int i = 0; i < 5; ++i) {
            if (ui::NavButton(tabs[i], activeTab == i, btnSize))
                activeTab = i;
            ImGui::Dummy(ImVec2(0, 4));
        }

        // Footer pinned to bottom of sidebar
        const float footerY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight();
        if (footerY > ImGui::GetCursorPosY())
            ImGui::SetCursorPosY(footerY);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.55f, 0.80f));
        ImGui::TextUnformatted("v1.0  glass");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0, 0);

    // Content area — AlwaysUseWindowPadding so PushStyleVar padding actually applies
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##content", ImVec2(0, 0),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    {
        const float gap = 12.f;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail < gap + 2.f)
            avail = gap + 2.f;
        const float half = (avail - gap) * 0.5f;

        switch (activeTab) {
        case 0: // Aim
        {
            ui::BeginCard("##aim_left", half);
            ui::SectionLabel("Weapon Group");
            {
                static const char* kGroups[] = {
                    "General", "Pistols", "SMGs", "Rifles", "Shotguns", "Snipers", "LMGs"
                };
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##weapon_group", &Config::weapon_group_ui, kGroups, IM_ARRAYSIZE(kGroups));
                if (Config::weapon_group_ui < 0 || Config::weapon_group_ui >= Config::WG_COUNT)
                    Config::weapon_group_ui = Config::WG_GENERAL;
                ImGui::TextDisabled("Editing: %s  |  Active: %s",
                    Config::WeaponGroupName(Config::weapon_group_ui),
                    Config::WeaponGroupName(Config::weapon_group_active));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Each group has its own aimbot + autofire + triggerbot settings.\n"
                        "Active follows your held weapon (VData type).");
                if (ImGui::Button("Copy Current → All Groups", ImVec2(-1, 0))) {
                    const Config::AimWeaponProfile src = Config::MenuAimProfile();
                    for (int g = 0; g < Config::WG_COUNT; ++g)
                        Config::weapon_profiles[g] = src;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Duplicate this group's settings to every weapon group");
            }

            Config::AimWeaponProfile& aimP = Config::MenuAimProfile();

            ImGui::Spacing();
            ui::SectionLabel("General");
            ImGui::Checkbox("Enable Aimbot", &Config::aimbot);

            // Stack keybind controls so they never clip out of the card
            {
                const float rowW = ImGui::GetContentRegionAvail().x;
                ImGui::TextUnformatted("Key Mode");
                ImGui::SetNextItemWidth(rowW);
                keybind.menuMode(Config::aimbot);

                ImGui::TextUnformatted("Key");
                keybind.menuButton(Config::aimbot);
            }

            ImGui::Checkbox("Auto Team Check", &Config::team_check_auto);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "OFF in: Deathmatch, workshop/aim maps, single-team lobbies\n"
                    "ON in: Competitive / Casual / team modes\n"
                    "Uncheck Auto to use manual Team Check");
            {
                const bool eff = GameMode::WantTeamCheck(Config::team_check);
                ImGui::TextDisabled("Mode: %s  |  TeamCheck: %s",
                    GameMode::ModeLabel(), eff ? "ON" : "OFF");
            }
            ImGui::BeginDisabled(Config::team_check_auto);
            ImGui::Checkbox("Team Check", &Config::team_check);
            ImGui::EndDisabled();
            ImGui::Checkbox("Visibility Check", &aimP.aim_vis_check);
            ImGui::SliderFloat("FOV", &aimP.aimbot_fov, 0.f, 90.f, "%.1f");
            ImGui::SliderFloat("Smooth", &aimP.aimbot_smooth, 0.f, 50.f, "%.1f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = instant snap\nHigher = slower / human-like");

            // Multi-select hitbox combo
            {
                static const char* kHitboxNames[] = {
                    "Head", "Neck", "Chest", "Stomach", "Pelvis", "Arms", "Legs", "Feet"
                };
                char preview[96];
                preview[0] = '\0';
                int nSel = 0;
                for (int i = 0; i < Config::HB_COUNT; ++i) {
                    if (!aimP.aim_hitboxes[i]) continue;
                    if (nSel > 0)
                        strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
                    strncat_s(preview, sizeof(preview), kHitboxNames[i], _TRUNCATE);
                    ++nSel;
                }
                if (nSel == 0)
                    strcpy_s(preview, "None");

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::BeginCombo("Hitboxes", preview)) {
                    for (int i = 0; i < Config::HB_COUNT; ++i) {
                        ImGui::PushID(i);
                        if (ImGui::Checkbox(kHitboxNames[i], &aimP.aim_hitboxes[i])) {
                            bool any = false;
                            for (int j = 0; j < Config::HB_COUNT; ++j)
                                if (aimP.aim_hitboxes[j]) { any = true; break; }
                            if (!any)
                                aimP.aim_hitboxes[i] = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Multi-select. Aims closest selected hitbox in FOV.");
            }

            ImGui::Checkbox("Draw FOV Circle", &Config::fov_circle);
            if (Config::fov_circle)
                ImGui::ColorEdit4("Circle Color", (float*)&Config::fovCircleColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

            ImGui::Spacing();
            ui::SectionLabel("Autofire");
            ImGui::Checkbox("Enable Autofire", &Config::autofire);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Locks aim and shoots automatically.\n"
                    "Uses this group's FOV / Smooth.\n"
                    "Own hitboxes + multipoints below.\n"
                    "Separate keybind from Aimbot.");
            ImGui::Checkbox("Silent Aim", &aimP.autofire_silent);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "ON = crosshair stays still; shots aim at bone (subtick).\n"
                    "OFF = normal autofire (moves crosshair + shoots).\n"
                    "Still requires on-target + hitchance / mindamage.");

            {
                static const char* kTargetSel[] = {
                    "Crosshair",
                    "Distance",
                    "Best Damage"
                };
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::Combo("Target Select", &aimP.autofire_target_select, kTargetSel, IM_ARRAYSIZE(kTargetSel));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Industry SortTargets:\n"
                        "Crosshair = lowest FOV to aim point.\n"
                        "Distance = closest player origin in FOV.\n"
                        "Best Damage = highest dmg (lethal first, FOV ties).");
                if (aimP.autofire_target_select < 0 || aimP.autofire_target_select >= Config::AF_TARGET_COUNT)
                    aimP.autofire_target_select = Config::AF_TARGET_CROSSHAIR;
            }

            // Autofire multipoint hitbox combo
            {
                static const char* kAfMpNames[] = {
                    "Head", "Chest", "Stomach", "Pelvis"
                };
                char preview[96];
                preview[0] = '\0';
                int nSel = 0;
                for (int i = 0; i < Config::AF_MP_COUNT; ++i) {
                    if (!aimP.autofire_hitboxes[i]) continue;
                    if (nSel > 0)
                        strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
                    strncat_s(preview, sizeof(preview), kAfMpNames[i], _TRUNCATE);
                    ++nSel;
                }
                if (nSel == 0)
                    strcpy_s(preview, "None");

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::BeginCombo("AF Hitboxes", preview)) {
                    for (int i = 0; i < Config::AF_MP_COUNT; ++i) {
                        ImGui::PushID(200 + i);
                        if (ImGui::Checkbox(kAfMpNames[i], &aimP.autofire_hitboxes[i])) {
                            bool any = false;
                            for (int j = 0; j < Config::AF_MP_COUNT; ++j)
                                if (aimP.autofire_hitboxes[j]) { any = true; break; }
                            if (!any)
                                aimP.autofire_hitboxes[i] = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Hitboxes to multipoint-scan.\n"
                        "Points = capsule edges toward you / sides / top.");
            }

            ImGui::Checkbox("Dynamic Multipoint", &aimP.autofire_multipoint_dynamic);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Shrink pointscale by weapon bloom × distance\n"
                    "(Gamesense-style). Prevents aiming past the hitbox.");

            ImGui::TextUnformatted("Multipoint Scale");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "0% = center only.\n"
                    "100% = full capsule radius.\n"
                    "Dynamic mode can shrink this further.");
            auto MpScaleSlider = [](const char* label, float& scale) {
                float pct = scale * 100.f;
                if (ImGui::SliderFloat(label, &pct, 0.f, 100.f, "%.0f%%"))
                    scale = pct / 100.f;
            };
            if (aimP.autofire_hitboxes[Config::AF_MP_HEAD])
                MpScaleSlider("Head MP", aimP.autofire_multipoint_scale[Config::AF_MP_HEAD]);
            if (aimP.autofire_hitboxes[Config::AF_MP_CHEST])
                MpScaleSlider("Chest MP", aimP.autofire_multipoint_scale[Config::AF_MP_CHEST]);
            if (aimP.autofire_hitboxes[Config::AF_MP_STOMACH])
                MpScaleSlider("Stomach MP", aimP.autofire_multipoint_scale[Config::AF_MP_STOMACH]);
            if (aimP.autofire_hitboxes[Config::AF_MP_PELVIS])
                MpScaleSlider("Pelvis MP", aimP.autofire_multipoint_scale[Config::AF_MP_PELVIS]);

            ImGui::SliderFloat("Hitchance", &aimP.autofire_hitchance, 0.f, 100.f, "%.0f%%");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "0 = off. Recommended 65–80.\n"
                    "Only shoots when estimated hit % is high enough.");
            ImGui::Checkbox("Autostop", &aimP.autofire_autostop);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Counter-strafes before shooting (≤ walk speed).\n"
                    "Stops movement so accuracy / hitchance is clean.");
            ImGui::Checkbox("Autowall", &aimP.autofire_autowall);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "ON = ignore visibility; shoot through walls when\n"
                    "pen damage >= Min Damage (AW) and hitchance passes.\n"
                    "OFF = only shoot visible targets (Min Damage).");
            ImGui::SliderFloat("Min Damage", &aimP.autofire_mindamage, 0.f, 120.f, "%.0f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Visible targets.\n"
                    "0 = any damage > 0 is enough.");
            ImGui::SliderFloat("Min Damage (AW)", &aimP.autofire_mindamage_aw, 0.f, 120.f, "%.0f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Through-wall min damage (Autowall ON).\n"
                    "0 = any pen damage > 0 is enough.");
            {
                const float rowW = ImGui::GetContentRegionAvail().x;
                ImGui::TextUnformatted("Key Mode");
                ImGui::SetNextItemWidth(rowW);
                keybind.menuMode(Config::autofire);
                ImGui::TextUnformatted("Key");
                keybind.menuButton(Config::autofire);
            }
            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##aim_right", 0);
            ui::SectionLabel("Recoil");
            ImGui::Checkbox("Aimbot RCS", &aimP.rcs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Compensate punch when aimbot/autofire locks target");
            ImGui::Checkbox("Standalone RCS", &aimP.rcs_standalone);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Independent of aimbot — works while shooting (M1)");
            ImGui::SliderFloat("RCS Scale X", &aimP.rcs_scale_x, 0.f, 1.f, "%.2f");
            ImGui::SliderFloat("RCS Scale Y", &aimP.rcs_scale_y, 0.f, 1.f, "%.2f");

            ImGui::Spacing();
            ui::SectionLabel("Humanize");
            ImGui::SliderFloat("Reaction Time", &aimP.aim_reaction_delay_ms, 0.f, 500.f, "%.0f ms");
            ImGui::SliderFloat("Target Switch Delay", &aimP.aim_target_switch_delay_ms, 0.f, 500.f, "%.0f ms");
            ImGui::SliderFloat("First Shot Delay", &aimP.aim_first_shot_delay_ms, 0.f, 500.f, "%.0f ms");

            ImGui::Spacing();
            ui::SectionLabel("Triggerbot");
            ImGui::Checkbox("Enable Triggerbot", &Config::triggerbot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Fires only when your crosshair is already on an enemy.\n"
                    "Does not move aim (unlike Autofire).\n"
                    "Uses punch-compensated shot direction + hitchance.");
            {
                const float rowW = ImGui::GetContentRegionAvail().x;
                ImGui::TextUnformatted("Key Mode");
                ImGui::SetNextItemWidth(rowW);
                keybind.menuMode(Config::triggerbot);
                ImGui::TextUnformatted("Key");
                keybind.menuButton(Config::triggerbot);
            }
            ImGui::SliderFloat("Trigger Delay", &aimP.trigger_delay_ms, 0.f, 250.f, "%.0f ms");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Must stay on-target this long before firing.\n"
                    "Resets if you leave the target. 0 = instant.");

            {
                static const char* kTrModes[] = { "Hitchance", "Seed Nospread" };
                if (aimP.trigger_mode < 0 || aimP.trigger_mode >= Config::TR_MODE_COUNT)
                    aimP.trigger_mode = Config::TR_MODE_HITCHANCE;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::Combo("Trigger Mode", &aimP.trigger_mode, kTrModes, Config::TR_MODE_COUNT);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Hitchance — Monte Carlo %% gate (slider below).\n"
                        "Seed Nospread — exact IDA seed ray:\n"
                        "SPREADSEEDGEN(pitch/yaw@0.5°, tick) → CalcSpread(seed+1).\n"
                        "Only fires when that bullet hits a trigger hitbox.\n"
                        "No angle rewrite / no camera flick — wait for good seed.");
            }

            if (aimP.trigger_mode == Config::TR_MODE_HITCHANCE) {
                ImGui::SliderFloat("Trigger Hitchance", &aimP.trigger_hitchance, 0.f, 100.f, "%.0f%%");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "0 = off (instant when on hitbox).\n"
                        "Scoped snipers always skip HC (first-shot accurate).\n"
                        "Rifles: 55–70 recommended.");
            } else {
                ImGui::TextDisabled("Seed: exact ray wait (no view flick)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Crosshair on target, then only shoot if this tick's\n"
                        "real spread seed lands on a trigger hitbox.\n"
                        "Standing still / scoped = more good seeds.");
            }

            ImGui::Checkbox("Trigger Autostop", &aimP.trigger_autostop);
            ImGui::Checkbox("Trigger Autowall", &aimP.trigger_autowall);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Also fire when an enemy sits in a tight FOV cone\n"
                    "behind cover and pen damage reaches them.");
            ImGui::Checkbox("Scoped Only", &aimP.trigger_scoped_only);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Snipers only fire while scoped (AWP/SSG).");

            // Triggerbot-owned hitbox multi-select (independent of aimbot list)
            {
                static const char* kTrHbNames[] = {
                    "Head", "Neck", "Chest", "Stomach", "Pelvis", "Arms", "Legs", "Feet"
                };
                char preview[96];
                preview[0] = '\0';
                int nSel = 0;
                for (int i = 0; i < Config::HB_COUNT; ++i) {
                    if (!aimP.trigger_hitboxes[i]) continue;
                    if (nSel > 0)
                        strncat_s(preview, sizeof(preview), ", ", _TRUNCATE);
                    strncat_s(preview, sizeof(preview), kTrHbNames[i], _TRUNCATE);
                    ++nSel;
                }
                if (nSel == 0)
                    strcpy_s(preview, "None");

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::BeginCombo("Trigger Hitboxes", preview)) {
                    for (int i = 0; i < Config::HB_COUNT; ++i) {
                        ImGui::PushID(100 + i);
                        if (ImGui::Checkbox(kTrHbNames[i], &aimP.trigger_hitboxes[i])) {
                            bool any = false;
                            for (int j = 0; j < Config::HB_COUNT; ++j)
                                if (aimP.trigger_hitboxes[j]) { any = true; break; }
                            if (!any)
                                aimP.trigger_hitboxes[i] = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Hitboxes the triggerbot may fire on.\n"
                        "Separate from aimbot / autofire lists.");
            }
            ui::EndCard();
        }
        break;

        case 1: // Visuals
        {
            static int visSub = 0; // 0 Players | 1 World ESP | 2 Nade Pred | 3 View | 4 Removals
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float gapSub = 8.f;
                const float subW = floorf((avail - gapSub * 4.f) / 5.f);
                const float lastW = avail - subW * 4.f - gapSub * 4.f;
                if (ui::NavButton("Players", visSub == 0, ImVec2(subW, 28.f)))
                    visSub = 0;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("World ESP", visSub == 1, ImVec2(subW, 28.f)))
                    visSub = 1;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("Nade Pred", visSub == 2, ImVec2(subW, 28.f)))
                    visSub = 2;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("View", visSub == 3, ImVec2(subW, 28.f)))
                    visSub = 3;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("Removals", visSub == 4, ImVec2(lastW, 28.f)))
                    visSub = 4;
                ImGui::Dummy(ImVec2(0, 6));
            }

            const ImGuiColorEditFlags colFlags =
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

            if (visSub == 0) {
                ui::BeginCard("##vis_left", half);
                ui::SectionLabel("Player ESP");
                ImGui::TextDisabled("Right-click a feature for settings");

                ui::FeatureToggle("Box", &Config::esp);
                if (ImGui::BeginPopupContextItem("##esp_box_pop")) {
                    ui::PopupTitle("Box Settings");
                    ImGui::ColorEdit4("Visible", (float*)&Config::espColor, colFlags);
                    ImGui::ColorEdit4("Invisible", (float*)&Config::espColorInvisible, colFlags);
                    ImGui::SliderFloat("Thickness", &Config::espThickness, 1.0f, 5.0f, "%.1f");
                    ImGui::SliderFloat("Width Scale", &Config::esp_box_width, 0.28f, 0.70f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Box width as fraction of height");
                    const char* styles[] = { "Full", "Corner" };
                    ImGui::Combo("Style", &Config::esp_box_style, styles, IM_ARRAYSIZE(styles));
                    ImGui::Checkbox("Fill", &Config::espFill);
                    if (Config::espFill)
                        ImGui::SliderFloat("Fill Opacity", &Config::espFillOpacity, 0.0f, 1.0f, "%.2f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Health Bar", &Config::showHealth);
                if (ImGui::BeginPopupContextItem("##esp_hp_pop")) {
                    ui::PopupTitle("Health Bar Settings");
                    ImGui::Checkbox("Auto Color (HP)", &Config::esp_health_auto);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Green when full, red when low");
                    if (!Config::esp_health_auto)
                        ImGui::ColorEdit4("Color", (float*)&Config::esp_health_color, colFlags);
                    ImGui::SliderFloat("Bar Width", &Config::esp_bar_width, 2.0f, 8.0f, "%.0f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Armor Bar", &Config::showArmor);
                if (ImGui::BeginPopupContextItem("##esp_armor_pop")) {
                    ui::PopupTitle("Armor Bar Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_armor_color, colFlags);
                    ImGui::SliderFloat("Bar Width", &Config::esp_bar_width, 2.0f, 8.0f, "%.0f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Name Tags", &Config::showNameTags);
                if (ImGui::BeginPopupContextItem("##esp_name_pop")) {
                    ui::PopupTitle("Name Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_name_color, colFlags);
                    ImGui::Checkbox("Avatar", &Config::esp_name_avatar);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Show Steam profile avatar next to the name");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Weapon ESP", &Config::showWeapon);
                if (ImGui::BeginPopupContextItem("##esp_wep_pop")) {
                    ui::PopupTitle("Weapon Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_weapon_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Weapon Icons", &Config::showWeaponIcon);
                if (ImGui::BeginPopupContextItem("##esp_wep_icon_pop")) {
                    ui::PopupTitle("Weapon Icon Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_weapon_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Distance ESP", &Config::showDistance);
                if (ImGui::BeginPopupContextItem("##esp_dist_pop")) {
                    ui::PopupTitle("Distance Settings");
                    ImGui::ColorEdit4("Color", (float*)&Config::esp_distance_color, colFlags);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Skeleton", &Config::esp_skeleton);
                if (ImGui::BeginPopupContextItem("##esp_skel_pop")) {
                    ui::PopupTitle("Skeleton Settings");
                    ImGui::ColorEdit4("Visible", (float*)&Config::esp_skeleton_color, colFlags);
                    ImGui::ColorEdit4("Invisible", (float*)&Config::esp_skeleton_color_invisible, colFlags);
                    ImGui::SliderFloat("Thickness", &Config::esp_skeleton_thickness, 1.0f, 4.0f, "%.1f");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Glow", &Config::glow);
                if (ImGui::BeginPopupContextItem("##esp_glow_pop")) {
                    ui::PopupTitle("Player Glow Settings");
                    ImGui::Checkbox("Team", &Config::glow_team);
                    ImGui::Checkbox("Enemy", &Config::glow_enemy);
                    ImGui::Checkbox("Only Visible", &Config::glow_only_visible);
                    ImGui::ColorEdit4("Visible", (float*)&Config::glow_color, colFlags);
                    ImGui::ColorEdit4("Invisible", (float*)&Config::glow_color_invis, colFlags);
                    ImGui::TextDisabled("World glow → Visuals / World ESP");
                    ImGui::EndPopup();
                }

                ImGui::Checkbox("Auto Team Check##esp", &Config::team_check_auto);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "OFF in: Deathmatch, workshop/aim maps, single-team lobbies\n"
                        "ON in: Competitive / Casual / team modes\n"
                        "Uncheck Auto to use manual Team Check");
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
                    ImGui::SetTooltip("Color box/skeleton by LOS (visible vs wall)");

                ui::SectionLabel("Flags");
                ImGui::TextDisabled("Active state labels (right of box)");
                ImGui::Checkbox("Flashed", &Config::flag_flashed);
                ImGui::Checkbox("Bomb", &Config::flag_bomb);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Carrying C4 or planting");
                ImGui::Checkbox("Scoped", &Config::flag_scoped);
                ImGui::Checkbox("Reloading", &Config::flag_reloading);
                ImGui::Checkbox("Defusing", &Config::flag_defusing);

                ui::SectionLabel("Effects");
                ImGui::Checkbox("Night Mode", &Config::Night);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Post-process exposure. OFF restores normal.");
                if (Config::Night) {
                    ImGui::SliderFloat("Darkness", &Config::night_exposure, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0 = no darkness, 1 = darkest");
                }
                ImGui::Checkbox("Skybox Color", &Config::skybox);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tint C_EnvSky (m_vTintColor). OFF restores.");
                if (Config::skybox) {
                    ImGui::ColorEdit4("Sky Color", (float*)&Config::skybox_color,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }
                ImGui::Checkbox("Lighting Color", &Config::lighting);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("CGlobalLightBase m_LightColor + UpdateState. OFF restores.");
                if (Config::lighting) {
                    ImGui::ColorEdit4("Light Color", (float*)&Config::lighting_color,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }
                ImGui::Checkbox("Map Color", &Config::map_color);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tint world/prop meshes via DrawArray.");
                if (Config::map_color) {
                    ImGui::ColorEdit4("World Color", (float*)&Config::map_color_value,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }
                ImGui::Checkbox("Weather", &Config::weather);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Screen-space rain/snow/ash around camera + wetness. OFF clears.");
                if (Config::weather) {
                    const char* weatherModes[] = { "Off", "Rain", "Snow", "Ash" };
                    ImGui::Combo("Weather Mode", &Config::weather_mode, weatherModes, IM_ARRAYSIZE(weatherModes));
                    ImGui::SliderFloat("Intensity", &Config::weather_intensity, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0 = light, 1 = densest (more particle layers)");
                }
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##vis_right", 0);
                ui::SectionLabel("Chams");
                const char* mats[] = { "Flat", "Illuminate", "Glow", "Ghost", "Latex" };
                const ImGuiColorEditFlags chamCol =
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;

                ImGui::Checkbox("Enemy Chams", &Config::enemyChams);
                if (Config::enemyChams) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("Visible Material", &Config::chamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Visible Color", (float*)&Config::colVisualChams, chamCol);
                }

                ImGui::Checkbox("Chams XQZ", &Config::enemyChamsInvisible);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Through walls (behind cover)");
                if (Config::enemyChamsInvisible) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("XQZ Material", &Config::chamsMaterialXQZ, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("XQZ Color", (float*)&Config::colVisualChamsIgnoreZ, chamCol);
                }

                ui::SectionLabel("Local");
                ImGui::Checkbox("Hand Chams", &Config::armChams);
                if (Config::armChams) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("Hand Material", &Config::armChamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Hand Color", (float*)&Config::colArmChams, chamCol);
                }
                ImGui::Checkbox("Weapon Chams", &Config::viewmodelChams);
                if (Config::viewmodelChams) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("Weapon Material", &Config::viewmodelChamsMaterial, mats, IM_ARRAYSIZE(mats));
                    ImGui::ColorEdit4("Weapon Color", (float*)&Config::colViewmodelChams, chamCol);
                }
                ui::EndCard();
            }
            else if (visSub == 1) {
                // World ESP — side-by-side cards
                ui::BeginCard("##world_left", half);
                ui::SectionLabel("Items");
                ImGui::TextDisabled("Right-click for color + glow");

                ui::FeatureToggle("Dropped Weapons", &Config::world_esp_weapons);
                if (ImGui::BeginPopupContextItem("##wesp_wep_pop")) {
                    ui::PopupTitle("Weapon ESP");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_weapon_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_weapons);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Engine glow (uses color above; works without text ESP)");
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Planted Bomb", &Config::world_esp_bomb);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Site + timer + defuse state\nRight-click for settings");
                if (ImGui::BeginPopupContextItem("##wesp_bomb_pop")) {
                    ui::PopupTitle("Bomb ESP");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_bomb_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_bomb);
                    ImGui::EndPopup();
                }

                ImGui::Checkbox("Show Distance", &Config::world_esp_distance);
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##world_right", 0);
                ui::SectionLabel("Projectiles");
                ImGui::TextDisabled("Right-click for color + glow");

                ui::FeatureToggle("Smoke", &Config::world_esp_smoke);
                if (ImGui::BeginPopupContextItem("##wesp_smoke_pop")) {
                    ui::PopupTitle("Smoke");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_smoke_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Molotov / Fire", &Config::world_esp_molotov);
                if (ImGui::BeginPopupContextItem("##wesp_molly_pop")) {
                    ui::PopupTitle("Molotov / Inferno");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_molotov_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("HE Grenade", &Config::world_esp_he);
                if (ImGui::BeginPopupContextItem("##wesp_he_pop")) {
                    ui::PopupTitle("HE");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_he_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Flashbang", &Config::world_esp_flash);
                if (ImGui::BeginPopupContextItem("##wesp_flash_pop")) {
                    ui::PopupTitle("Flash");
                    ImGui::ColorEdit4("Color", (float*)&Config::world_esp_flash_color, colFlags);
                    ImGui::Checkbox("Glow", &Config::glow_world_grenades);
                    ImGui::EndPopup();
                }

                ui::FeatureToggle("Decoy", &Config::world_esp_decoy);
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
                ImGui::TextDisabled("Physics sim + wall bounce (Trace)");

                ui::FeatureToggle("Enable", &Config::nade_pred);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draw predicted nade path in world");

                ImGui::BeginDisabled(!Config::nade_pred);
                ImGui::Checkbox("Local Throw Preview", &Config::nade_pred_local);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Path while holding a grenade (from eye + strength)");
                if (Config::nade_pred_local) {
                    ImGui::Checkbox("Only When Pin Pulled", &Config::nade_pred_local_only_pin);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Hide preview until pin is pulled");
                }
                ImGui::Checkbox("In-Air Projectiles", &Config::nade_pred_projectiles);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Predict landing of flying HE/flash/smoke/molly/decoy");
                ImGui::Checkbox("Effect Radius", &Config::nade_pred_radius);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Ring at predicted land (HE/flash/smoke/molly)");
                ImGui::Checkbox("Damage Indicator", &Config::nade_pred_damage);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("HE damage on enemies (armor-aware) at land");
                ImGui::EndDisabled();

                // Warn works standalone — all teams, full map, fixed size
                ui::SectionLabel("Warning");
                ImGui::Checkbox("Grenade Warning", &Config::nade_warn);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Badge on every flying nade (team + enemy + own), full map");
                ImGui::TextDisabled("Shows all teams · no range limit");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##nade_right", 0);
                ui::SectionLabel("Style");
                ImGui::BeginDisabled(!Config::nade_pred);
                ImGui::SliderFloat("Line Thickness", &Config::nade_pred_thickness, 1.0f, 5.0f, "%.1f");
                ImGui::ColorEdit4("Path Color", (float*)&Config::nade_pred_color, colFlags);
                ImGui::ColorEdit4("Local Path", (float*)&Config::nade_pred_local_color, colFlags);
                ImGui::ColorEdit4("Land Marker", (float*)&Config::nade_pred_land_color, colFlags);
                ImGui::TextDisabled("Projectile paths use World ESP colors");
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::BeginDisabled(!Config::nade_warn);
                ImGui::SliderFloat("Icon Size", &Config::nade_warn_icon_size, 18.f, 48.f, "%.0f");
                ImGui::ColorEdit4("Warn Accent", (float*)&Config::nade_warn_color, colFlags);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!Config::nade_pred || !Config::nade_pred_damage);
                ImGui::ColorEdit4("Damage Color", (float*)&Config::nade_pred_damage_color, colFlags);
                ImGui::ColorEdit4("Lethal Color", (float*)&Config::nade_pred_damage_lethal_color, colFlags);
                ImGui::EndDisabled();
                ui::EndCard();
            }
            else if (visSub == 3) {
                ui::BeginCard("##view_left", half);
                ui::SectionLabel("Viewmodel");
                ImGui::TextDisabled("CalcViewModel hook — bypasses engine clamps");

                ImGui::Checkbox("Enable##viewmodel_changer", &Config::viewmodel_changer);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Override gun XYZ + FOV. OFF = game defaults.");

                ImGui::BeginDisabled(!Config::viewmodel_changer);
                ImGui::SliderFloat("Offset X", &Config::viewmodel_x, -20.f, 20.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Left / right");
                ImGui::SliderFloat("Offset Y", &Config::viewmodel_y, -20.f, 20.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Up / down");
                ImGui::SliderFloat("Offset Z", &Config::viewmodel_z, -20.f, 20.f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Forward / back");
                ImGui::SliderFloat("Viewmodel FOV", &Config::viewmodel_fov, 40.f, 120.f, "%.0f");
                ImGui::EndDisabled();
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##view_right", 0);
                ui::SectionLabel("Third Person");
                ImGui::Checkbox("Enable##thirdperson", &Config::thirdperson);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("OverrideView: pull camera behind you");

                ImGui::BeginDisabled(!Config::thirdperson);
                {
                    const float rowW = ImGui::GetContentRegionAvail().x;
                    ImGui::TextUnformatted("Key Mode");
                    ImGui::SetNextItemWidth(rowW);
                    keybind.menuMode(Config::thirdperson);
                    ImGui::TextUnformatted("Key");
                    keybind.menuButton(Config::thirdperson);
                }
                ImGui::SliderFloat("Distance", &Config::thirdperson_distance, 50.f, 300.f, "%.0f");
                ImGui::EndDisabled();

                ImGui::Spacing();
                ui::SectionLabel("World FOV");
                ImGui::Checkbox("Custom FOV", &Config::fovEnabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Camera / world FOV (GetRenderFov), not viewmodel");
                ImGui::BeginDisabled(!Config::fovEnabled);
                ImGui::SliderFloat("FOV Value", &Config::fov, 20.0f, 160.0f, "%.0f");
                ImGui::EndDisabled();
                ui::EndCard();
            }
            else if (visSub == 4) {
                ui::BeginCard("##removals_left", half);
                ui::SectionLabel("World / FX");
                ImGui::TextDisabled("Early-return hooks — toggle what to strip");

                ImGui::TextUnformatted("Flash Reduce");
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##flash_reduce", &Config::antiflash_amount, 0.f, 100.f, "%.0f%%");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "0%% = full flash\n"
                        "50%% = half overlay alpha\n"
                        "100%% = skip FlashbangOverlay entirely");
                Config::antiflash = Config::antiflash_amount > 0.01f;

                ImGui::Checkbox("Smoke", &Config::remove_smoke);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("DrawSmokeVertex — smoke volumes");

                ImGui::Checkbox("Decals", &Config::remove_decals);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("RenderDecals — bullet holes, blood, explosions");

                ImGui::Checkbox("Particles", &Config::remove_particles);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Reserved — hook currently targets SetControlPoint, not CreateEffect.\n"
                        "(Weather uses a separate unhooked SetCP path.)");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##removals_right", 0);
                ui::SectionLabel("View / Scope");
                ImGui::Checkbox("Firstperson Legs", &Config::remove_legs);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("DrawLegs — FirstpersonLegsPass");

                ImGui::Spacing();
                ui::SectionLabel("Scope Overlay");
                ImGui::Checkbox("No Scope Overlay", &Config::scope_no_overlay);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Kill entire scope HUD (no lines)");

                if (!Config::scope_no_overlay) {
                    ImGui::Checkbox("Scope Blur", &Config::scope_remove_blur);
                    ImGui::TextDisabled("Lines only");
                    ImGui::Checkbox("Black Bars##rem", &Config::scope_remove_bars);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Bars + texture off, redraw crosshair lines");
                    ImGui::Checkbox("Scope Texture##rem", &Config::scope_remove_texture);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Same lines-only mode as Black Bars");
                }
                ui::EndCard();
            }
        }
        break;

        case 2: // Skin
        {
            static int skinSub = 0; // 0 Knives | 1 Gloves | 2 Weapons | 3 Agents
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                const float gapSub = 8.f;
                const float subW = floorf((avail - gapSub * 3.f) / 4.f);
                const float lastW = avail - subW * 3.f - gapSub * 3.f;
                if (ui::NavButton("Knives", skinSub == 0, ImVec2(subW, 28.f)))
                    skinSub = 0;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("Gloves", skinSub == 1, ImVec2(subW, 28.f)))
                    skinSub = 1;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("Weapons", skinSub == 2, ImVec2(subW, 28.f)))
                    skinSub = 2;
                ImGui::SameLine(0, gapSub);
                if (ui::NavButton("Agents", skinSub == 3, ImVec2(lastW, 28.f)))
                    skinSub = 3;
                ImGui::Dummy(ImVec2(0, 6));
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
                                SkinChanger::Invalidate();
                                Notify::Success("Knife", knives[i].display);
                            }
                            if (knives[i].def_index && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
                                const char* sn = SkinChanger::SimpleNameFor(knives[i].def_index);
                                if ((!sn || !*sn) && knives[i].subclass)
                                    sn = knives[i].subclass;
                                if (sn && *sn)
                                    SkinPreview::DrawHover(SkinPreview::Get(SkinPreview::ModelPath(sn)));
                            }
                            if (sel)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
                ImGui::Spacing();
                {
                    std::uint16_t def = 0;
                    const char* tok = "Vanilla";
                    int paintId = 0;
                    if (Config::knife_index > 0 && Config::knife_index < SkinChanger::KnifeCount()) {
                        def = SkinChanger::Knives()[Config::knife_index].def_index;
                        (void)SkinChanger::KnifeSkinsReady();
                        const auto& kits = SkinChanger::KnifePaintKits();
                        if (Config::knife_paint_kit >= 0 && Config::knife_paint_kit < (int)kits.size()) {
                            tok = kits[Config::knife_paint_kit].token.c_str();
                            paintId = kits[Config::knife_paint_kit].id;
                        }
                    }
                    ui::DrawSkinPreviewPanel(def, tok, paintId);
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Client-side only. Equip knife in-game.");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##skin_knives_right", 0);
                ui::SectionLabel("Skins");
                if (Config::knife_index <= 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
                    ImGui::TextWrapped("Select a knife model first.");
                    ImGui::PopStyleColor();
                } else if (!SkinChanger::KnifeSkinsReady()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
                    ImGui::TextWrapped("Loading paint kits...");
                    ImGui::PopStyleColor();
                } else {
                    static char knifeSkinSearch[64] = {};
                    const auto& kits = SkinChanger::KnifePaintKits();
                    const char* sn = SkinChanger::SimpleNameFor(
                        SkinChanger::Knives()[Config::knife_index].def_index);
                    if (ui::SkinPaintCombo("##knife_skin", "##knife_skin_search",
                            knifeSkinSearch, (int)sizeof(knifeSkinSearch),
                            kits, &Config::knife_paint_kit, &Config::knife_paint_kit_id, sn)) {
                        SkinChanger::InvalidateSkin(true);
                        Notify::Success("Skin", kits[Config::knife_paint_kit].name.c_str());
                    }

                    static float tempWear = -1.f;
                    if (tempWear < 0.f) tempWear = Config::knife_wear;
                    ImGui::TextUnformatted("Wear");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::SliderFloat("##knife_wear", &tempWear, 0.f, 1.f, "%.4f")) {
                        if (!ImGui::IsMouseDown(0)) {
                            Config::knife_wear = tempWear;
                            SkinChanger::InvalidateSkin();
                        }
                    }
                    if (!ImGui::IsItemActive() && tempWear != Config::knife_wear) {
                        Config::knife_wear = tempWear;
                        SkinChanger::InvalidateSkin();
                    }

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
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
                    ImGui::TextWrapped("Select a glove model first.");
                    ImGui::PopStyleColor();
                } else if (!SkinChanger::GloveSkinsReady()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
                    ImGui::TextWrapped("Loading paint kits...");
                    ImGui::PopStyleColor();
                } else {
                    static char gloveSkinSearch[64] = {};
                    const auto& kits = SkinChanger::GlovePaintKits();
                    const char* sn = SkinChanger::SimpleNameFor(
                        SkinChanger::Gloves()[Config::glove_index].def_index);
                    if (ui::SkinPaintCombo("##glove_skin", "##glove_skin_search",
                            gloveSkinSearch, (int)sizeof(gloveSkinSearch),
                            kits, &Config::glove_paint_kit, &Config::glove_paint_kit_id, sn)) {
                        SkinChanger::InvalidateGloves();
                        Notify::Success("Glove skin", kits[Config::glove_paint_kit].name.c_str());
                    }

                    static float gloveWear = -1.f;
                    if (gloveWear < 0.f) gloveWear = Config::glove_wear;
                    ImGui::TextUnformatted("Wear");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::SliderFloat("##glove_wear", &gloveWear, 0.f, 1.f, "%.4f")) {
                        if (!ImGui::IsMouseDown(0)) {
                            Config::glove_wear = gloveWear;
                            SkinChanger::InvalidateGloves();
                        }
                    }
                    if (!ImGui::IsItemActive() && gloveWear != Config::glove_wear) {
                        Config::glove_wear = gloveWear;
                        SkinChanger::InvalidateGloves();
                    }

                    ImGui::TextUnformatted("Seed");
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::InputInt("##glove_seed", &Config::glove_seed)) {
                        if (Config::glove_seed < 0) Config::glove_seed = 0;
                        SkinChanger::InvalidateGloves();
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
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
                        ImGui::TextWrapped("Select a weapon first.");
                        ImGui::PopStyleColor();
                    } else if (!SkinChanger::WeaponSkinsReady(def)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
                        ImGui::TextWrapped("Loading paint kits...");
                        ImGui::PopStyleColor();
                    } else {
                        auto& slot = Config::weapon_skin[def];
                        const auto& kits = SkinChanger::WeaponPaintKits(def);
                        static char wepSkinSearch[64] = {};
                        static int wepSearchDef = -1;
                        if (wepSearchDef != (int)def) {
                            wepSkinSearch[0] = '\0';
                            wepSearchDef = (int)def;
                        }
                        const char* sn = SkinChanger::SimpleNameFor(def);
                        if (ui::SkinPaintCombo("##weapon_skin", "##weapon_skin_search",
                                wepSkinSearch, (int)sizeof(wepSkinSearch),
                                kits, &slot.paint_kit, &slot.paint_kit_id, sn)) {
                            SkinChanger::InvalidateWeapons();
                            Notify::Success("Weapon skin", kits[slot.paint_kit].name.c_str());
                        }

                        static float wepWear = -1.f;
                        static int wepWearDef = -1;
                        if (wepWearDef != (int)def) {
                            wepWear = slot.wear;
                            wepWearDef = (int)def;
                        }
                        ImGui::TextUnformatted("Wear");
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::SliderFloat("##weapon_wear", &wepWear, 0.f, 1.f, "%.4f")) {
                            if (!ImGui::IsMouseDown(0)) {
                                slot.wear = wepWear;
                                SkinChanger::InvalidateWeapons();
                            }
                        }
                        if (!ImGui::IsItemActive() && wepWear != slot.wear) {
                            slot.wear = wepWear;
                            SkinChanger::InvalidateWeapons();
                        }

                        ImGui::TextUnformatted("Seed");
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::InputInt("##weapon_seed", &slot.seed)) {
                            if (slot.seed < 0) slot.seed = 0;
                            SkinChanger::InvalidateWeapons();
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
                ImGui::Spacing();
                drawAgentCombo("CT Agent", "##agent_ct", &Config::agent_ct_def, 3);
                ImGui::Spacing();
                drawAgentPanel(Config::agent_ct_def, 3);
                ImGui::Spacing();
                ImGui::TextDisabled("Client-side SetModel each frame (no inventory).");
                ui::EndCard();

                ImGui::SameLine(0, gap);

                ui::BeginCard("##skin_agents_t", 0);
                ui::SectionLabel("Terrorist");
                drawAgentCombo("T Agent", "##agent_t", &Config::agent_t_def, 2);
                ImGui::Spacing();
                drawAgentPanel(Config::agent_t_def, 2);
                ImGui::Spacing();
                ImGui::TextDisabled("Uses your current team each spawn.");
                ui::EndCard();
            }
        }
        break;

        case 3: // Misc
        {
            ui::BeginCard("##misc_left", half);
            ui::SectionLabel("Movement");
            ImGui::Checkbox("Bunny Hop", &Config::bhop);
            ImGui::Checkbox("Auto Strafe", &Config::autostrafe);
            if (Config::autostrafe) {
                const char* modes[] = { "Mouse (legit)", "Vectorial (silent)" };
                ImGui::Combo("Strafe Mode", &Config::autostrafe_mode, modes, 2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Mouse: sidemove from mouse delta only — no camera move.\n"
                        "Vectorial: keeps view, remaps forward/side for air accel.\n"
                        "Neither mode writes viewangles (no flicks).");
            }

            ImGui::Spacing();
            ui::SectionLabel("Scope HUD");
            ImGui::Checkbox("No Scope Overlay", &Config::scope_no_overlay);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Force draw=false — disables entire scope HUD");

            if (!Config::scope_no_overlay) {
                ImGui::Checkbox("Remove Scope Blur", &Config::scope_remove_blur);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Zeros blur strength only (+8)");

                ImGui::TextDisabled("Remove (keeps only scope lines)");
                ImGui::Checkbox("Black Bars", &Config::scope_remove_bars);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Kills bars + lens texture.\n"
                        "Redraws crosshair lines only.");
                ImGui::Checkbox("Scope Texture", &Config::scope_remove_texture);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Same as Black Bars — clean scope with lines only.\n"
                        "Either checkbox enables lines-only mode.");

                ImGui::Spacing();
                ImGui::Checkbox("Custom Scope Look", &Config::scope_custom_look);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Only applies when lines-only / no-overlay are off");
                if (Config::scope_custom_look && !Config::scope_remove_bars && !Config::scope_remove_texture) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("Size Scale", &Config::scope_size_scale, 0.25f, 4.f, "%.2f");
                    if (!Config::scope_remove_blur) {
                        ImGui::SetNextItemWidth(-1);
                        ImGui::SliderFloat("Blur Amount", &Config::scope_blur_amount, 0.f, 1.f, "%.2f");
                    }
                }
            }
            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##misc_right", 0);
            ui::SectionLabel("Widgets");
            ImGui::Checkbox("Keybind List", &Config::widget_keybinds);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Compact overlay of all set keybinds.\n"
                    "Open menu → drag to move.\n"
                    "Right-click widget → settings.");
            ImGui::Checkbox("Bomb Info", &Config::widget_bomb);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Planted bomb HUD: site, fuse, defuse timer,\n"
                    "and estimated explosion damage to you.\n"
                    "Open menu → drag to move.");
            ImGui::Checkbox("Spectator List", &Config::widget_spectators);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Shows who is spectating you with Steam avatar\n"
                    "and name. Open menu → drag to move.\n"
                    "Right-click widget → reset position.");
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

            ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
            ImGui::TextWrapped("Documents\\TempleWare\\Configs");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##cfgname", "Config name...", configName, IM_ARRAYSIZE(configName));

            ImGui::Spacing();
            const float gapBtn = 6.f;
            const float rowW = ImGui::GetContentRegionAvail().x;
            const float bw = (rowW - gapBtn * 2.f) / 3.f;

            if (ImGui::Button("Save", ImVec2(bw, 0))) {
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
            if (ImGui::Button("Load", ImVec2(bw, 0))) {
                if (configName[0] == '\0') {
                    setStatus("Select or type a name", false, true);
                } else if (internal_config::ConfigManager::Load(configName)) {
                    SkinChanger::OnConfigLoaded();
                    setStatus("Loaded");
                } else {
                    setStatus("Load failed", false);
                }
            }
            ImGui::SameLine(0, gapBtn);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.28f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.28f, 0.35f, 0.70f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.32f, 0.38f, 0.85f));
            if (ImGui::Button("Delete", ImVec2(bw, 0))) {
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

            ImGui::Spacing();
            const float halfBtn = (ImGui::GetContentRegionAvail().x - gapBtn) * 0.5f;
            if (ImGui::Button("Open Folder", ImVec2(halfBtn, 0))) {
                internal_config::ConfigManager::OpenFolder();
                Notify::Info("Config", "Opened configs folder");
            }
            ImGui::SameLine(0, gapBtn);
            if (ImGui::Button("Refresh", ImVec2(halfBtn, 0))) {
                configList = internal_config::ConfigManager::ListConfigs();
                setStatus("Refreshed");
            }

            if (!statusMsg.empty() && ImGui::GetTime() < statusUntil) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent);
                ImGui::TextUnformatted(statusMsg.c_str());
                ImGui::PopStyleColor();
            }
            ui::EndCard();

            ImGui::SameLine(0, gap);

            ui::BeginCard("##cfg_right", 0);
            ui::SectionLabel("Saved Configs");

            if (!statusMsg.empty() && ImGui::GetTime() < statusUntil) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Accent);
                ImGui::TextUnformatted(statusMsg.c_str());
                ImGui::PopStyleColor();
            }

            if (configList.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::TextMuted);
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
                            SkinChanger::OnConfigLoaded();
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
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
}

void Menu::toggleMenu() {
    showMenu = !showMenu;
}
