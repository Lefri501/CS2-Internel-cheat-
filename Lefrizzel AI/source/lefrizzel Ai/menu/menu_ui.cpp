#include "menu_ui.h"

#include <cstring>

namespace MenuUI {
namespace {

struct StyleStack {
    int colors = 0;
    int vars   = 0;
    void pushColor(ImGuiCol idx, ImVec4 c) { ImGui::PushStyleColor(idx, c); ++colors; }
    void pushVar(ImGuiStyleVar idx, float v) { ImGui::PushStyleVar(idx, v); ++vars; }
    void pushVar(ImGuiStyleVar idx, ImVec2 v) { ImGui::PushStyleVar(idx, v); ++vars; }
    void pop() {
        if (vars)   { ImGui::PopStyleVar(vars); vars = 0; }
        if (colors) { ImGui::PopStyleColor(colors); colors = 0; }
    }
};

StyleStack g_cardStack;
bool       g_cardOpen = false;
bool       g_cardHasItemWidth = false;
StyleStack g_stripStack;
bool       g_stripOpen = false;
StyleStack g_sideStack;
bool       g_sideOpen = false;
StyleStack g_contentStack;
bool       g_contentOpen = false;

// ── Anim state ───────────────────────────────────────────────────────
struct Anim {
    float open      = 0.f;   // 0..1 raw
    float openEase  = 0.f;   // eased
    int   tab       = 0;
    float tabT      = 1.f;   // 0 = just changed, 1 = settled
    float navHover[8]{};
    float navSelect[8]{};
    float subPillX  = 0.f;
    float subPillW  = 0.f;
    bool  subInit   = false;
    int   subCount  = 0;
};
Anim g_anim;

inline float Saturate(float x) { return (std::clamp)(x, 0.f, 1.f); }

// Exp smooth toward target (frame-rate independent)
float Approach(float cur, float target, float speed, float dt) {
    const float t = 1.f - expf(-speed * dt);
    return cur + (target - cur) * t;
}

// Smoothstep
float EaseInOut(float t) {
    t = Saturate(t);
    return t * t * (3.f - 2.f * t);
}

// Soft out for open
float EaseOutCubic(float t) {
    t = Saturate(t);
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

struct ShellPalette {
    ImVec4 bg, side, card, frame, border, text, muted, track;
};

static ImVec4 Darker(ImVec4 c, float amt) {
    return ImVec4(
        (std::max)(0.f, c.x - amt),
        (std::max)(0.f, c.y - amt),
        (std::max)(0.f, c.z - amt),
        c.w);
}
static ImVec4 Lighter(ImVec4 c, float amt) {
    return ImVec4(
        (std::min)(1.f, c.x + amt),
        (std::min)(1.f, c.y + amt),
        (std::min)(1.f, c.z + amt),
        c.w);
}
// Mix toward neutral gray — kills monochrome “everything is accent” paste
static ImVec4 Mix(ImVec4 a, ImVec4 b, float t) {
    t = (std::clamp)(t, 0.f, 1.f);
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}
static float Lum(ImVec4 c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

ShellPalette Palette() {
    ShellPalette p;
    p.bg     = Config::menu_bg;
    p.side   = Config::menu_sidebar_bg;
    p.card   = Config::menu_child_bg;
    // Border: prefer cool white edge over full accent rim (contrast, not paint)
    {
        ImVec4 b = Config::menu_border;
        // If border is heavily hue-tinted, desaturate toward white for shell chrome
        const float avg = (b.x + b.y + b.z) * (1.f / 3.f);
        const float sat = std::fabs(b.x - avg) + std::fabs(b.y - avg) + std::fabs(b.z - avg);
        if (sat > 0.12f)
            b = Mix(b, ImVec4(1.f, 1.f, 1.f, b.w), 0.55f);
        p.border = b;
        if (p.border.w < 0.06f) p.border.w = 0.08f;
        if (p.border.w > 0.18f) p.border.w = 0.14f;
    }
    // Input wells: always darker than card so fields pop
    p.frame = Darker(p.card, 0.035f);
    p.track = Darker(p.bg, 0.045f);
    // Card must sit above bg — force lift if preset left them too close
    if (Lum(p.card) < Lum(p.bg) + 0.012f)
        p.card = Lighter(p.bg, 0.028f);
    p.frame.w = 1.f;
    p.track.w = 1.f;

    // Text: high contrast neutrals — never inherit accent hue for body copy
    p.text = Config::menu_text;
    if (Lum(p.text) < 0.72f)
        p.text = ImVec4(0.92f, 0.93f, 0.95f, 1.f);
    p.text.w = 1.f;
    p.muted = Config::menu_text_muted;
    // Muted: cool gray, readable but clearly secondary
    if (Lum(p.muted) > 0.55f || Lum(p.muted) < 0.28f)
        p.muted = ImVec4(0.52f, 0.54f, 0.58f, 1.f);
    // Desaturate heavy muted tints
    {
        const float avg = (p.muted.x + p.muted.y + p.muted.z) * (1.f / 3.f);
        p.muted = Mix(p.muted, ImVec4(avg, avg, avg, 1.f), 0.45f);
        p.muted.w = 1.f;
    }

    const float op = (std::clamp)(Config::menu_opacity, 0.70f, 1.f);
    p.bg.w = op;
    return p;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Animation API
// ═══════════════════════════════════════════════════════════════════════

void AnimTick(bool openTarget) {
    const float dt = ImGui::GetIO().DeltaTime;

    // Close = instant kill (no ghost frame / leftover alpha)
    if (!openTarget) {
        g_anim.open = 0.f;
        g_anim.openEase = 0.f;
        g_anim.tabT = 1.f;
        for (int i = 0; i < 8; ++i) {
            g_anim.navHover[i] = 0.f;
            g_anim.navSelect[i] = 0.f;
        }
        return;
    }

    // Open only: short fade-in
    g_anim.open = Approach(g_anim.open, 1.f, 18.f, dt);
    if (g_anim.open > 0.995f) g_anim.open = 1.f;
    g_anim.openEase = EaseOutCubic(g_anim.open);

    g_anim.tabT = Approach(g_anim.tabT, 1.f, 12.f, dt);
    if (g_anim.tabT > 0.995f) g_anim.tabT = 1.f;
}

bool AnimVisible() { return g_anim.open > 0.001f; }
float OpenAlpha()  { return g_anim.openEase; }
float OpenSlide()  { return (1.f - g_anim.openEase) * 12.f; }

void NotifyTab(int tab) {
    if (tab == g_anim.tab) return;
    g_anim.tab = tab;
    g_anim.tabT = 0.f;
}

float ContentAlpha() {
    // Minimal tab fade — stay readable, no ghost dim
    const float tabA = EaseOutCubic(g_anim.tabT);
    return 0.88f + 0.12f * tabA;
}

float ContentOffsetY() {
    return (1.f - EaseOutCubic(g_anim.tabT)) * 3.f;
}

// ═══════════════════════════════════════════════════════════════════════
// Theme
// ═══════════════════════════════════════════════════════════════════════

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    const ShellPalette p = Palette();
    const ImVec4 accent = Config::menu_accent;
    const float r = (std::clamp)((std::max)(Config::menu_rounding, 2.f), 2.f, 8.f);

    // Accent only for interactive chrome — keep alpha low so shell stays neutral
    auto tint = [&](float a) { return ImVec4(accent.x, accent.y, accent.z, a); };
    auto bright = [&](float add) {
        return ImVec4(
            (std::min)(1.f, accent.x + add),
            (std::min)(1.f, accent.y + add),
            (std::min)(1.f, accent.z + add), 1.f);
    };
    // Neutral hover (white lift) — used more than full accent wash
    auto lift = [&](float a) { return ImVec4(1.f, 1.f, 1.f, a); };

    c[ImGuiCol_WindowBg]             = p.bg;
    c[ImGuiCol_ChildBg]              = p.card;
    c[ImGuiCol_PopupBg]              = Mix(p.card, ImVec4(0.06f, 0.065f, 0.075f, 1.f), 0.25f);
    c[ImGuiCol_PopupBg].w            = 0.98f;
    c[ImGuiCol_Border]               = p.border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text]                 = p.text;
    c[ImGuiCol_TextDisabled]         = p.muted;

    // Dark wells — hover = slight lift + tiny accent, not solid accent fill
    c[ImGuiCol_FrameBg]              = p.frame;
    c[ImGuiCol_FrameBgHovered]       = Mix(p.frame, tint(1.f), 0.18f);
    c[ImGuiCol_FrameBgHovered].w     = 1.f;
    c[ImGuiCol_FrameBgActive]        = Mix(p.frame, tint(1.f), 0.28f);
    c[ImGuiCol_FrameBgActive].w      = 1.f;

    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = p.bg;
    c[ImGuiCol_MenuBarBg]            = p.side;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0.22f);
    c[ImGuiCol_ScrollbarGrab]        = lift(0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = tint(0.55f);
    c[ImGuiCol_ScrollbarGrabActive]  = tint(0.78f);

    c[ImGuiCol_CheckMark]            = bright(0.12f);
    c[ImGuiCol_CheckboxSelectedBg]   = tint(0.42f);
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = bright(0.16f);

    c[ImGuiCol_Button]               = lift(0.06f);
    c[ImGuiCol_ButtonHovered]        = Mix(lift(0.10f), tint(1.f), 0.35f);
    c[ImGuiCol_ButtonHovered].w      = 0.22f;
    c[ImGuiCol_ButtonActive]         = tint(0.38f);

    c[ImGuiCol_Header]               = lift(0.05f);
    c[ImGuiCol_HeaderHovered]        = Mix(lift(0.08f), tint(1.f), 0.40f);
    c[ImGuiCol_HeaderHovered].w      = 0.16f;
    c[ImGuiCol_HeaderActive]         = tint(0.28f);

    c[ImGuiCol_Separator]            = ImVec4(1.f, 1.f, 1.f, 0.10f);
    c[ImGuiCol_SeparatorHovered]     = tint(0.45f);
    c[ImGuiCol_SeparatorActive]      = accent;

    c[ImGuiCol_ResizeGrip]           = lift(0.04f);
    c[ImGuiCol_ResizeGripHovered]    = tint(0.28f);
    c[ImGuiCol_ResizeGripActive]     = tint(0.42f);

    c[ImGuiCol_Tab]                  = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]           = tint(0.14f);
    c[ImGuiCol_TabSelected]          = tint(0.12f);
    c[ImGuiCol_TabSelectedOverline]  = accent;
    c[ImGuiCol_TabDimmed]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected]    = tint(0.08f);

    c[ImGuiCol_TextSelectedBg]       = tint(0.32f);
    c[ImGuiCol_NavCursor]            = accent;
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.02f, 0.02f, 0.03f, 0.55f);

    c[ImGuiCol_TableHeaderBg]        = lift(0.04f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(1.f, 1.f, 1.f, 0.09f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(1.f, 1.f, 1.f, 0.05f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = lift(0.018f);

    style.WindowRounding    = r;
    style.ChildRounding     = (std::max)(2.f, r - 1.f);
    style.FrameRounding     = 2.f;
    style.PopupRounding     = (std::max)(2.f, r - 1.f);
    style.ScrollbarRounding = 2.f;
    style.GrabRounding      = 2.f;
    style.TabRounding       = 2.f;

    const bool compact = Config::menu_compact;
    const float dpi = Layout::DpiMul();
    style.WindowPadding     = ImVec2(0, 0);
    style.FramePadding      = compact ? ImVec2(6, 3) : ImVec2(8, 4);
    style.ItemSpacing       = compact ? ImVec2(6, 4) : ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(5, 3);
    style.CellPadding       = ImVec2(4, 3);
    style.IndentSpacing     = 10.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;

    style.ScrollbarSize     = 6.0f;
    style.GrabMinSize       = 8.0f;
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);

    // 4K / high-DPI: scale padding + fonts (base sizes assume 100%)
    if (dpi > 1.001f) {
        style.FramePadding.x     = floorf(style.FramePadding.x * dpi);
        style.FramePadding.y     = floorf(style.FramePadding.y * dpi);
        style.ItemSpacing.x      = floorf(style.ItemSpacing.x * dpi);
        style.ItemSpacing.y      = floorf(style.ItemSpacing.y * dpi);
        style.ItemInnerSpacing.x = floorf(style.ItemInnerSpacing.x * dpi);
        style.ItemInnerSpacing.y = floorf(style.ItemInnerSpacing.y * dpi);
        style.CellPadding.x      = floorf(style.CellPadding.x * dpi);
        style.CellPadding.y      = floorf(style.CellPadding.y * dpi);
        style.IndentSpacing      = floorf(style.IndentSpacing * dpi);
        style.ScrollbarSize      = floorf(style.ScrollbarSize * dpi);
        style.GrabMinSize        = floorf(style.GrabMinSize * dpi);
    }
    ImGui::GetIO().FontGlobalScale = dpi;

    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;
    style.Alpha             = 1.0f;
    style.DisabledAlpha     = 0.40f;
}

void ApplyPreset(int idx) {
    // Hierarchy: NEUTRAL charcoal shell + bright accent only on controls.
    // Do NOT paint bg/cards/text with accent hue — that looks monochrome/pasted.
    // idx: 0 Mist, 1 Steel, 2 Neutral, 3 Sage, 4 Copper, 5 Rose, 6 Violet, 7 Amber

    // Shared base — clear steps: side < bg < card, white-ish text, cool muted
    auto baseShell = [](float warm = 0.f) {
        // warm: tiny R/B shift only (±0.01), not full tint wash
        Config::menu_bg         = ImVec4(0.068f + warm * 0.01f, 0.070f, 0.078f - warm * 0.008f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.102f + warm * 0.008f, 0.105f, 0.118f - warm * 0.006f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.042f, 0.044f, 0.050f, 1.00f);
        Config::menu_border     = ImVec4(1.f, 1.f, 1.f, 0.09f);
        Config::menu_text       = ImVec4(0.93f, 0.94f, 0.96f, 1.f);
        Config::menu_text_muted = ImVec4(0.50f, 0.52f, 0.56f, 1.f);
        Config::menu_opacity    = 0.98f;
    };

    switch (idx) {
    case 0: // Mist — ice blue accent, cooler shell
        baseShell(-1.f);
        Config::menu_accent     = ImVec4(0.45f, 0.72f, 0.98f, 1.f);
        Config::menu_bg         = ImVec4(0.060f, 0.066f, 0.082f, 0.97f);
        Config::menu_child_bg   = ImVec4(0.095f, 0.102f, 0.122f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.038f, 0.042f, 0.055f, 1.00f);
        Config::menu_border     = ImVec4(0.85f, 0.90f, 1.00f, 0.10f);
        Config::menu_rounding   = 5.0f;
        Config::menu_compact    = false;
        break;
    case 1: // Steel — default
        baseShell(0.f);
        Config::menu_accent     = ImVec4(0.42f, 0.68f, 0.92f, 1.f);
        Config::menu_rounding   = 4.0f;
        Config::menu_compact    = true;
        break;
    case 2: // Neutral — silver accent, pure graphite
        Config::menu_accent     = ImVec4(0.88f, 0.90f, 0.93f, 1.f);
        Config::menu_bg         = ImVec4(0.052f, 0.053f, 0.056f, 1.00f);
        Config::menu_child_bg   = ImVec4(0.090f, 0.091f, 0.096f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.034f, 0.035f, 0.038f, 1.00f);
        Config::menu_border     = ImVec4(1.f, 1.f, 1.f, 0.08f);
        Config::menu_text       = ImVec4(0.94f, 0.94f, 0.95f, 1.f);
        Config::menu_text_muted = ImVec4(0.48f, 0.49f, 0.51f, 1.f);
        Config::menu_rounding   = 2.0f;
        Config::menu_opacity    = 1.00f;
        Config::menu_compact    = true;
        break;
    case 3: // Sage — mint accent on charcoal (not green walls)
        baseShell(0.f);
        Config::menu_accent     = ImVec4(0.38f, 0.82f, 0.58f, 1.f);
        Config::menu_bg         = ImVec4(0.062f, 0.070f, 0.066f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.096f, 0.108f, 0.100f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.038f, 0.046f, 0.042f, 1.00f);
        Config::menu_border     = ImVec4(0.90f, 0.96f, 0.92f, 0.09f);
        Config::menu_rounding   = 4.0f;
        Config::menu_compact    = true;
        break;
    case 4: // Copper — orange accent, warm gray base (not brown paste)
        baseShell(1.f);
        Config::menu_accent     = ImVec4(0.95f, 0.55f, 0.28f, 1.f);
        Config::menu_bg         = ImVec4(0.078f, 0.068f, 0.060f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.115f, 0.102f, 0.092f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.048f, 0.042f, 0.036f, 1.00f);
        Config::menu_border     = ImVec4(1.f, 0.95f, 0.90f, 0.09f);
        Config::menu_text       = ImVec4(0.96f, 0.94f, 0.90f, 1.f);
        Config::menu_text_muted = ImVec4(0.55f, 0.52f, 0.48f, 1.f);
        Config::menu_rounding   = 4.0f;
        Config::menu_compact    = false;
        break;
    case 5: // Rose — coral accent, cool dark base
        baseShell(0.f);
        Config::menu_accent     = ImVec4(0.96f, 0.40f, 0.52f, 1.f);
        Config::menu_bg         = ImVec4(0.072f, 0.060f, 0.068f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.110f, 0.094f, 0.104f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.044f, 0.036f, 0.042f, 1.00f);
        Config::menu_border     = ImVec4(1.f, 0.92f, 0.94f, 0.09f);
        Config::menu_text       = ImVec4(0.96f, 0.93f, 0.94f, 1.f);
        Config::menu_text_muted = ImVec4(0.54f, 0.50f, 0.52f, 1.f);
        Config::menu_rounding   = 5.0f;
        Config::menu_compact    = false;
        break;
    case 6: // Violet — purple accent, deep slate (not purple walls)
        baseShell(0.f);
        Config::menu_accent     = ImVec4(0.68f, 0.48f, 0.98f, 1.f);
        Config::menu_bg         = ImVec4(0.060f, 0.058f, 0.078f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.094f, 0.092f, 0.118f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.038f, 0.036f, 0.052f, 1.00f);
        Config::menu_border     = ImVec4(0.92f, 0.90f, 1.00f, 0.10f);
        Config::menu_text       = ImVec4(0.94f, 0.93f, 0.97f, 1.f);
        Config::menu_text_muted = ImVec4(0.52f, 0.50f, 0.58f, 1.f);
        Config::menu_rounding   = 4.0f;
        Config::menu_compact    = true;
        break;
    case 7: // Amber — gold accent, near-black shell
        baseShell(1.f);
        Config::menu_accent     = ImVec4(0.98f, 0.78f, 0.22f, 1.f);
        Config::menu_bg         = ImVec4(0.068f, 0.066f, 0.056f, 0.98f);
        Config::menu_child_bg   = ImVec4(0.105f, 0.102f, 0.088f, 1.00f);
        Config::menu_sidebar_bg = ImVec4(0.042f, 0.040f, 0.034f, 1.00f);
        Config::menu_border     = ImVec4(1.f, 0.97f, 0.88f, 0.09f);
        Config::menu_text       = ImVec4(0.96f, 0.95f, 0.90f, 1.f);
        Config::menu_text_muted = ImVec4(0.54f, 0.52f, 0.46f, 1.f);
        Config::menu_rounding   = 3.0f;
        Config::menu_compact    = true;
        break;
    default:
        baseShell(0.f);
        Config::menu_accent  = ImVec4(0.42f, 0.68f, 0.92f, 1.f);
        Config::menu_rounding = 4.0f;
        Config::menu_compact  = true;
        break;
    }
    ApplyTheme();
}

// ═══════════════════════════════════════════════════════════════════════
// Draw
// ═══════════════════════════════════════════════════════════════════════

void DrawTopSheen(ImDrawList*, ImVec2, ImVec2, float, int) {}

// Shell: dark drop + neutral rim + thin accent hairline (not full-color frame)
void DrawWindowFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding) {
    if (!dl) return;
    // Soft shadow
    dl->AddRect(
        ImVec2(min.x + 1.f, min.y + 2.f),
        ImVec2(max.x + 1.f, max.y + 3.f),
        IM_COL32(0, 0, 0, 56), rounding, 0, 1.f);
    // Neutral white rim — always readable on any preset
    dl->AddRect(min, max, IM_COL32(255, 255, 255, 28), rounding, 0, 1.f);
    // Inner dark edge for depth (card-vs-bg separation)
    dl->AddRect(
        ImVec2(min.x + 1.f, min.y + 1.f),
        ImVec2(max.x - 1.f, max.y - 1.f),
        IM_COL32(0, 0, 0, 40), (std::max)(0.f, rounding - 1.f), 0, 1.f);
    // Accent hairline top only — identity without painting whole UI
    const ImVec4 a = Config::menu_accent;
    dl->AddRectFilled(
        ImVec2(min.x + rounding + 2.f, min.y + 1.f),
        ImVec2(max.x - rounding - 2.f, min.y + 2.5f),
        IM_COL32((int)(a.x * 255.f), (int)(a.y * 255.f), (int)(a.z * 255.f), 200),
        0.f);
}

// Section — muted label + short accent tick (hierarchy without full paint)
void Section(const char* label) {
    if (!label) label = "";
    const bool compact = Config::menu_compact;
    ImGui::Dummy(ImVec2(0, compact ? 4.f : 6.f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFontSize();
    const ImVec4 ac = Config::menu_accent;
    // 2px accent bar left of label
    dl->AddRectFilled(
        ImVec2(p.x, p.y + 2.f),
        ImVec2(p.x + 2.f, p.y + h - 1.f),
        IM_COL32((int)(ac.x * 255.f), (int)(ac.y * 255.f), (int)(ac.z * 255.f), 210),
        1.f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 8.f, p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, compact ? 2.f : 3.f));
}

void Gap(float mult) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * mult));
}

void SoftSeparator() {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
        IM_COL32(255, 255, 255, 22), 1.f);
    ImGui::Dummy(ImVec2(0, Config::menu_compact ? 5.f : 7.f));
}

// Page title — title left, muted subtitle right. No underline chrome.
void PageTitle(const char* title, const char* subtitle) {
    if (!title) title = "";
    const bool compact = Config::menu_compact;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float contentW = ImGui::GetContentRegionAvail().x;

    const ImVec2 titleSz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, title);
    dl->AddText(font, fs, origin, ToU32(TextBright()), title);

    if (subtitle && subtitle[0]) {
        const ImVec2 sSz = font->CalcTextSizeA(fs * 0.90f, FLT_MAX, 0.f, subtitle);
        const float sX = origin.x + contentW - sSz.x;
        const float sY = origin.y + (fs - sSz.y) * 0.5f;
        if (sX > origin.x + titleSz.x + 16.f)
            dl->AddText(font, fs * 0.90f, ImVec2(sX, sY), ToU32(TextMuted()), subtitle);
    }

    ImGui::Dummy(ImVec2(0, fs + (compact ? 6.f : 10.f)));
}

// ═══════════════════════════════════════════════════════════════════════
// Cards
// ═══════════════════════════════════════════════════════════════════════

void BeginCard(const char* id, float width, bool autoY) {
    const Layout L = Layout::Current();
    const ShellPalette p = Palette();
    float w = width;
    if (w > 0.f) {
        const float avail = ImGui::GetContentRegionAvail().x;
        if (w > avail) w = (std::max)(1.f, avail);
    }

    g_cardStack = {};
    // Card lifts above bg; neutral rim (not accent-colored)
    g_cardStack.pushColor(ImGuiCol_ChildBg, p.card);
    g_cardStack.pushColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.07f));
    g_cardStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_cardStack.pushVar(ImGuiStyleVar_WindowPadding, ImVec2(L.cardPad, L.cardPad));
    g_cardStack.pushVar(ImGuiStyleVar_ItemSpacing,
        Config::menu_compact ? ImVec2(6.f, 4.f) : ImVec2(8.f, 6.f));

    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (autoY) flags |= ImGuiChildFlags_AutoResizeY;

    ImGui::BeginChild(id, ImVec2(w, 0), flags, ImGuiWindowFlags_None);
    g_cardOpen = true;
    ImGui::PushItemWidth(-1.f);
    g_cardHasItemWidth = true;
}

void EndCard() {
    if (g_cardHasItemWidth) { ImGui::PopItemWidth(); g_cardHasItemWidth = false; }
    if (g_cardOpen) { ImGui::EndChild(); g_cardOpen = false; }
    g_cardStack.pop();
}

void BeginStrip(const char* id) {
    const Layout L = Layout::Current();
    const ShellPalette p = Palette();
    g_stripStack = {};
    g_stripStack.pushColor(ImGuiCol_ChildBg, p.card);
    g_stripStack.pushColor(ImGuiCol_Border, p.border);
    g_stripStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_stripStack.pushVar(ImGuiStyleVar_WindowPadding,
        ImVec2(L.compact ? 8.f : 10.f, L.compact ? 6.f : 8.f));
    ImGui::BeginChild(id, ImVec2(-1.f, 0.f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_None);
    g_stripOpen = true;
}

void EndStrip() {
    if (g_stripOpen) { ImGui::EndChild(); g_stripOpen = false; }
    g_stripStack.pop();
}

// ═══════════════════════════════════════════════════════════════════════
// Form
// ═══════════════════════════════════════════════════════════════════════

bool Slider(const char* label, const char* id, float* v, float vmin, float vmax, const char* fmt) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.f);
    return ImGui::SliderFloat(id, v, vmin, vmax, fmt);
}

bool Combo(const char* label, const char* id, int* cur, const char* const items[], int count) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.f);
    return ImGui::Combo(id, cur, items, count);
}

void Tip(const char* text) {
    if (text && text[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

bool CheckboxTip(const char* label, bool* v, const char* tip) {
    const bool changed = ImGui::Checkbox(label, v);
    if (tip) Tip(tip);
    return changed;
}

bool ColorQuadRow(const char* idPrefix, float colors[16]) {
    const ImGuiColorEditFlags fl =
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaOpaque;
    const float gap = 4.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w0 = floorf((std::max)(0.f, avail - gap * 3.f) * 0.25f);
    const float w3 = (std::max)(1.f, avail - w0 * 3.f - gap * 3.f);
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine(0, gap);
        const float w = (i == 3) ? w3 : (std::max)(1.f, w0);
        ImGui::PushID(idPrefix);
        ImGui::PushID(i);
        ImGui::PushItemWidth(w);
        changed |= ImGui::ColorEdit4("##c", colors + i * 4, fl);
        ImGui::PopItemWidth();
        ImGui::PopID();
        ImGui::PopID();
    }
    if (changed) {
        for (int i = 0; i < 4; ++i)
            colors[i * 4 + 3] = 1.f;
    }
    return changed;
}

bool FeatureToggle(const char* label, bool* v, const char* tip) {
    const bool changed = ImGui::Checkbox(label, v);
    if (ImGui::IsItemHovered()) {
        if (tip && tip[0])
            ImGui::SetTooltip("%s\nRight-click for settings.", tip);
        else
            ImGui::SetTooltip("Right-click for settings.");
    }
    return changed;
}

void PopupTitle(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, TextBright());
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    SoftSeparator();
}

// ═══════════════════════════════════════════════════════════════════════
// Navigation + hover animation
// ═══════════════════════════════════════════════════════════════════════

bool NavIcon(const char* label, const char* iconUtf8, bool selected, const ImVec2& size, int index) {
    const int idx = (std::clamp)(index, 0, 7);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));

    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "##nav_%s", label ? label : "x");
    const bool pressed = ImGui::Button(idBuf, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dt = ImGui::GetIO().DeltaTime;

    // Drive hover/select toward targets
    const float hoverT = ImGui::IsItemHovered() ? 1.f : 0.f;
    const float selT   = selected ? 1.f : 0.f;
    g_anim.navHover[idx]  = Approach(g_anim.navHover[idx],  hoverT, 18.f, dt);
    g_anim.navSelect[idx] = Approach(g_anim.navSelect[idx], selT,   16.f, dt);

    const float h = g_anim.navHover[idx];
    const float s = g_anim.navSelect[idx];

    // Select: left accent rail + soft white plate (not full accent wash)
    if (s > 0.01f || h > 0.01f) {
        const ImVec4 ac = Config::menu_accent;
        const int ar = (int)(ac.x * 255.f);
        const int ag = (int)(ac.y * 255.f);
        const int ab = (int)(ac.z * 255.f);
        const int aSel = (int)(22.f * s);   // subtle plate
        const int aHov = (int)(12.f * h * (1.f - s));
        if (aSel > 0)
            dl->AddRectFilled(
                ImVec2(min.x + 1.f, min.y + 1.f),
                ImVec2(max.x - 1.f, max.y - 1.f),
                IM_COL32(255, 255, 255, aSel), 3.f);
        if (aHov > 0)
            dl->AddRectFilled(
                ImVec2(min.x + 1.f, min.y + 1.f),
                ImVec2(max.x - 1.f, max.y - 1.f),
                IM_COL32(255, 255, 255, aHov), 3.f);
        if (s > 0.05f) {
            // Accent rail on left
            dl->AddRectFilled(
                ImVec2(min.x + 1.f, min.y + 5.f),
                ImVec2(min.x + 3.f, max.y - 5.f),
                IM_COL32(ar, ag, ab, (int)(220.f * s)), 1.5f);
        }
    }

    // Icon: select = accent, hover = bright white, idle = muted gray
    ImVec4 iconVec;
    if (s > 0.5f) {
        iconVec = WithA(Accent(), 1.f);
    } else if (h > 0.01f) {
        iconVec = WithA(TextBright(), 0.95f);
    } else {
        iconVec = WithA(TextMuted(), 0.72f);
    }
    const ImU32 iconCol = ToU32(iconVec);

    if (iconUtf8 && iconUtf8[0] && g_MenuIconFont) {
        const float iconSz = Config::menu_compact ? 14.f : 15.f;
        const ImVec2 isz = g_MenuIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, iconUtf8);
        const float cx = floorf((min.x + max.x) * 0.5f - isz.x * 0.5f);
        const float cy = floorf((min.y + max.y) * 0.5f - isz.y * 0.5f);
        dl->AddText(g_MenuIconFont, iconSz, ImVec2(cx, cy), iconCol, iconUtf8);
    }

    if (label && label[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", label);

    return pressed;
}

bool NavButton(const char* label, const char* iconUtf8, bool selected, const ImVec2& size) {
    if (size.x > 0.f && size.x <= 80.f && iconUtf8 && iconUtf8[0])
        return NavIcon(label, iconUtf8, selected, size, 0);

    const float round = 10.f;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, round);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button,
        selected ? WithA(Accent(), 0.20f) : ImVec4(1.f, 1.f, 1.f, 0.03f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        selected ? WithA(Accent(), 0.28f) : ImVec4(1.f, 1.f, 1.f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithA(Accent(), 0.34f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));

    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "##nav_%s", label ? label : "x");
    const bool pressed = ImGui::Button(idBuf, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(
            ImVec2(min.x + 1.f, min.y + 8.f),
            ImVec2(min.x + 3.5f, max.y - 8.f),
            AccentU32(1.f), 2.f);
    }

    const ImU32 col = selected || ImGui::IsItemHovered()
        ? ToU32(TextBright()) : ToU32(TextMuted());
    // Accent only when selected — hover uses bright text (contrast, not monochrome)
    const ImU32 iconCol = selected ? AccentU32(1.f)
        : (ImGui::IsItemHovered() ? ToU32(TextBright()) : ToU32(WithA(TextMuted(), 0.75f)));

    float x = min.x + 12.f;
    const float midY = (min.y + max.y) * 0.5f;
    if (iconUtf8 && iconUtf8[0] && g_MenuIconFont) {
        const float iconSz = 13.f;
        const ImVec2 isz = g_MenuIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, iconUtf8);
        dl->AddText(g_MenuIconFont, iconSz, ImVec2(x, midY - isz.y * 0.5f), iconCol, iconUtf8);
        x += isz.x + 8.f;
    }
    if (label && label[0]) {
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
        dl->AddText(font, fs, ImVec2(x, midY - tsz.y * 0.5f), col, label);
    }
    return pressed;
}

bool NavButton(const char* label, bool selected, const ImVec2& size) {
    return NavButton(label, nullptr, selected, size);
}

void SubNav(const char* const* labels, int count, int* selected) {
    if (!labels || count <= 0 || !selected) return;

    const Layout L = Layout::Current();
    const float gap = 0.f;
    const float h = L.subNavH;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float dt = ImGui::GetIO().DeltaTime;

    const ImVec2 trackMin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Flat sub-tabs: baseline rule + single 1.5px accent underline (no end caps)
    const float baselineY = trackMin.y + h - 0.5f;
    dl->AddLine(
        ImVec2(trackMin.x, baselineY),
        ImVec2(trackMin.x + avail, baselineY),
        IM_COL32(255, 255, 255, 18), 1.f);

    const float baseW = floorf(avail / (float)count);
    const float lastW = (std::max)(1.f, avail - baseW * (count - 1));

    float targetX = trackMin.x;
    float targetW = baseW;
    {
        float acc = 0.f;
        for (int i = 0; i < count; ++i) {
            const float w = (i == count - 1) ? lastW : baseW;
            if (i == *selected) {
                const float inset = 10.f;
                targetX = trackMin.x + acc + inset;
                targetW = (std::max)(8.f, w - inset * 2.f);
            }
            acc += w;
        }
    }

    if (!g_anim.subInit || g_anim.subCount != count) {
        g_anim.subPillX = targetX;
        g_anim.subPillW = targetW;
        g_anim.subInit = true;
        g_anim.subCount = count;
    } else {
        g_anim.subPillX = Approach(g_anim.subPillX, targetX, 18.f, dt);
        g_anim.subPillW = Approach(g_anim.subPillW, targetW, 18.f, dt);
    }

    const float ulY = baselineY - 0.5f;
    dl->AddRectFilled(
        ImVec2(g_anim.subPillX, ulY),
        ImVec2(g_anim.subPillX + g_anim.subPillW, ulY + 1.5f),
        AccentU32(0.95f));

    ImGui::SetCursorScreenPos(trackMin);
    ImGui::BeginGroup();
    float used = 0.f;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0, gap);
        const float w = (i == count - 1) ? lastW : baseW;
        used += w;

        const bool sel = (*selected == i);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.035f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.055f));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? TextBright() : TextMuted());

        char idBuf[64];
        std::snprintf(idBuf, sizeof(idBuf), "%s##sub_%d", labels[i], i);
        if (ImGui::Button(idBuf, ImVec2(w, h)))
            *selected = i;

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(trackMin.x, trackMin.y + h + (Config::menu_compact ? 6.f : 8.f)));
    ImGui::Dummy(ImVec2(0, 0));
}

int ButtonRow(const char* const* labels, int count, float height) {
    if (!labels || count <= 0) return -1;
    const float gap = 6.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float usable = (std::max)(0.f, avail - gap * (count - 1));
    const float baseW = floorf(usable / (float)count);
    float used = 0.f;
    int pressed = -1;
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0, gap);
        const float w = (i == count - 1) ? (std::max)(1.f, usable - used) : baseW;
        used += w;
        if (ImGui::Button(labels[i], ImVec2(w, height)))
            pressed = i;
    }
    return pressed;
}

void SplitRow(float gap, float* leftW, float* rightW) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float g = (gap > 0.f) ? gap : 8.f;
    const float left = floorf((std::max)(0.f, avail - g) * 0.5f);
    if (leftW)  *leftW  = left;
    if (rightW) *rightW = (std::max)(1.f, avail - left - g);
}

// ═══════════════════════════════════════════════════════════════════════
// Shell — rounded inset panels
// ═══════════════════════════════════════════════════════════════════════

void BeginSidebar(const Layout& L, float height) {
    const ShellPalette p = Palette();
    g_sideStack = {};
    g_sideStack.pushColor(ImGuiCol_ChildBg, p.side);
    // Darker rail, neutral edge — separates from content without hue
    g_sideStack.pushColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.05f));
    g_sideStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_sideStack.pushVar(ImGuiStyleVar_WindowPadding,
        L.compact ? ImVec2(6.f, 8.f) : ImVec2(8.f, 10.f));
    g_sideStack.pushVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(0.f, L.compact ? 4.f : 5.f));

    ImGui::BeginChild("##sidebar", ImVec2(L.sidebar, height),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar);
    g_sideOpen = true;
}

void EndSidebar() {
    if (g_sideOpen) { ImGui::EndChild(); g_sideOpen = false; }
    g_sideStack.pop();
}

void SidebarBrandMark() {
    // Neutral plate + accent letter — not a solid accent square
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float box = Config::menu_compact ? 18.f : 20.f;
    const float x = floorf(p.x + (ImGui::GetContentRegionAvail().x - box) * 0.5f);
    const float y = floorf(p.y + 2.f);

    dl->AddRectFilled(
        ImVec2(x, y), ImVec2(x + box, y + box),
        IM_COL32(255, 255, 255, 12), 3.f);
    dl->AddRect(
        ImVec2(x, y), ImVec2(x + box, y + box),
        IM_COL32(255, 255, 255, 28), 3.f, 0, 1.f);
    // Tiny accent corner
    const ImVec4 ac = Config::menu_accent;
    dl->AddRectFilled(
        ImVec2(x + 1.f, y + 1.f),
        ImVec2(x + 4.f, y + box - 1.f),
        IM_COL32((int)(ac.x * 255.f), (int)(ac.y * 255.f), (int)(ac.z * 255.f), 180), 2.f);

    ImFont* font = ImGui::GetFont();
    const float fs = box * 0.55f;
    const char* ch = "L";
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, ch);
    dl->AddText(font, fs,
        ImVec2(x + (box - ts.x) * 0.5f + 1.f, y + (box - ts.y) * 0.5f - 0.5f),
        ToU32(TextBright()), ch);

    ImGui::Dummy(ImVec2(0, box + (Config::menu_compact ? 10.f : 14.f)));
}

void BeginContent(const Layout& L, float contentW, float contentH) {
    const ShellPalette p = Palette();
    g_contentStack = {};
    g_contentStack.pushVar(ImGuiStyleVar_WindowPadding, ImVec2(L.contentPad, L.contentPad));
    g_contentStack.pushVar(ImGuiStyleVar_ChildRounding, L.childRound);
    g_contentStack.pushColor(ImGuiCol_ChildBg, WithA(p.bg, 0.0f)); // transparent — shell shows through
    g_contentStack.pushColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    ImGui::BeginChild("##content", ImVec2(contentW, contentH),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_None);
    g_contentOpen = true;
}

void EndContent() {
    if (g_contentOpen) { ImGui::EndChild(); g_contentOpen = false; }
    g_contentStack.pop();
}

void BeginHeader(const Layout&, float) {}
void EndHeader() {}
void HeaderBrand(const char*, const char*, const char*) {}
void HeaderRightHint(const char*) {}

} // namespace MenuUI
