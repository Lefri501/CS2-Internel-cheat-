#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../../../external/imgui/imgui.h"
#include "../config/config.h"

extern ImFont* g_MenuIconFont;

namespace MenuUI {

struct Layout {
    float windowW    = 860.f;
    float windowH    = 560.f;
    float sidebar    = 56.f;
    float shellPad   = 6.f;
    float contentPad = 10.f;
    float gap        = 6.f;
    float cardPad    = 8.f;
    float navBtnH    = 36.f;
    float subNavH    = 24.f;
    float rounding   = 4.f;
    float childRound = 3.f;
    bool  compact    = true;
    float dpi        = 1.f; // 1.0 = 100%, 2.0 = 200%

    static float DpiMul() {
        int p = Config::menu_dpi_scale;
        if (p < 100) p = 100;
        if (p > 200) p = 200;
        // snap 25%
        p = ((p + 12) / 25) * 25;
        if (p < 100) p = 100;
        if (p > 200) p = 200;
        return static_cast<float>(p) * 0.01f;
    }

    static Layout Current() {
        Layout L;
        L.compact    = Config::menu_compact;
        L.dpi        = DpiMul();
        // Tight corners — tool panel, not soft SaaS card
        L.rounding   = (std::clamp)((std::max)(Config::menu_rounding, 2.f), 2.f, 8.f);
        L.childRound = (std::max)(2.f, L.rounding - 1.f);
        if (L.compact) {
            L.windowW    = 820.f;
            L.windowH    = 540.f;
            L.gap        = 6.f;
            L.cardPad    = 8.f;
            L.contentPad = 10.f;
            L.shellPad   = 6.f;
            L.navBtnH    = 34.f;
            L.subNavH    = 22.f;
            L.sidebar    = 52.f;
        } else {
            L.windowW    = 920.f;
            L.windowH    = 600.f;
            L.gap        = 8.f;
            L.cardPad    = 10.f;
            L.contentPad = 12.f;
            L.shellPad   = 8.f;
            L.navBtnH    = 38.f;
            L.subNavH    = 26.f;
            L.sidebar    = 58.f;
        }
        // DPI scale for 4K / high-res monitors
        if (L.dpi > 1.001f) {
            const float s = L.dpi;
            L.windowW    = floorf(L.windowW * s);
            L.windowH    = floorf(L.windowH * s);
            L.gap        = floorf(L.gap * s);
            L.cardPad    = floorf(L.cardPad * s);
            L.contentPad = floorf(L.contentPad * s);
            L.shellPad   = floorf(L.shellPad * s);
            L.navBtnH    = floorf(L.navBtnH * s);
            L.subNavH    = floorf(L.subNavH * s);
            L.sidebar    = floorf(L.sidebar * s);
        }
        return L;
    }

    float colLeft(float avail) const {
        return floorf((std::max)(0.f, avail - gap) * 0.5f);
    }
};

// ── Animation ────────────────────────────────────────────────────────
// Call Tick each frame before drawing. openTarget = menu wants open.
void AnimTick(bool openTarget);
bool AnimVisible();          // true while open or closing fade
float OpenAlpha();           // 0..1 eased
float OpenSlide();           // pixels down when opening (0 when full)
void NotifyTab(int tab);     // content crossfade
float ContentAlpha();        // 0..1 for tab body
float ContentOffsetY();      // slight slide on tab change

inline ImVec4 Accent()     { return Config::menu_accent; }
inline ImVec4 TextMuted()  { return Config::menu_text_muted; }
inline ImVec4 TextBright() { return Config::menu_text; }
inline ImVec4 WithA(ImVec4 c, float a) { c.w = a; return c; }
inline ImU32  ToU32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32  AccentU32(float a = 1.f) {
    ImVec4 c = Config::menu_accent;
    c.w *= a;
    return ToU32(c);
}
inline ImU32  BorderU32(float a = 1.f) {
    ImVec4 c = Config::menu_border;
    c.w *= a;
    return ToU32(c);
}

void ApplyTheme();
void ApplyPreset(int idx);

void DrawWindowFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding);

void Section(const char* label);
void Gap(float mult = 1.f);
void SoftSeparator();
void PageTitle(const char* title, const char* subtitle = nullptr);

void BeginCard(const char* id, float width, bool autoY = false);
void EndCard();
void BeginStrip(const char* id);
void EndStrip();

bool Slider(const char* label, const char* id, float* v, float vmin, float vmax, const char* fmt = "%.3f");
bool Combo(const char* label, const char* id, int* cur, const char* const items[], int count);
void Tip(const char* text);
bool CheckboxTip(const char* label, bool* v, const char* tip = nullptr);
bool ColorQuadRow(const char* idPrefix, float colors[16]);
bool FeatureToggle(const char* label, bool* v, const char* tip = nullptr);
void PopupTitle(const char* title);

// index used for hover/select animation state (0..7)
bool NavIcon(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index = 0);
bool NavButton(const char* label, const char* iconUtf8, bool selected, const ImVec2& size);
bool NavButton(const char* label, bool selected, const ImVec2& size);
void SubNav(const char* const* labels, int count, int* selected);

int  ButtonRow(const char* const* labels, int count, float height = 0.f);
void SplitRow(float gap, float* leftW, float* rightW);

void BeginSidebar(const Layout& L, float height);
void EndSidebar();
void SidebarBrandMark();

void BeginContent(const Layout& L, float contentW, float contentH);
void EndContent();

void DrawTopSheen(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, int alpha = 14);
void BeginHeader(const Layout& L, float windowW);
void EndHeader();
void HeaderBrand(const char* name, const char* accentSuffix, const char* badge);
void HeaderRightHint(const char* text);

} // namespace MenuUI
