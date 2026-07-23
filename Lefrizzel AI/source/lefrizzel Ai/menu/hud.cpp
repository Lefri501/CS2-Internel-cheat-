#include "hud.h"
#include "../../../external/imgui/imgui.h"
#include "../config/config.h"
#include "../hooks/hooks.h"
#include "../keybinds/keybinds.h"
#include "../features/widgets/widgets.h"
#include "../features/notify/notify.h"
#include "../features/visuals/scope/scope.h"
#include "../utils/console/console.h"
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <DirectXMath.h>
#include <Windows.h>

Hud::Hud() {
}

float CalculateFovRadius(float fovDegrees, float screenWidth, float screenHeight, float gameVerticalFOV) {
    float fovRadians = fovDegrees * (DirectX::XM_PI / 180.0f);
    float screenRadius = std::tan(fovRadians / 2.0f) * (screenHeight / 2.0f) / std::tan(gameVerticalFOV * (DirectX::XM_PI / 180.0f) / 2.0f);
    static float flScalingMultiplier = 2.5f;
    return screenRadius * flScalingMultiplier;
}

void RenderFovCircle(ImDrawList* drawList, float fov, ImVec2 screenCenter, float screenWidth, float screenHeight,
    float thickness, const ImVec4& col)
{
    if (fov <= 0.01f)
        return;
    float radius = CalculateFovRadius(fov, screenWidth, screenHeight, H::g_flActiveFov);
    if (radius < 1.f)
        return;
    const uint32_t color = ImGui::ColorConvertFloat4ToU32(col);
    drawList->AddCircle(screenCenter, radius, color, 64, thickness);
}

static void DrawWatermark(ImDrawList* dl) {
    if (!dl)
        return;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();
    if (!font)
        return;
    const float fs = ImGui::GetFontSize();
    if (fs <= 0.f || io.DisplaySize.x < 32.f || io.DisplaySize.y < 32.f)
        return;

    const float fps = io.Framerate > 0.f ? io.Framerate : 0.f;
    const float frameMs = (fps > 0.f) ? (1000.f / fps) : 0.f;

    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);
    char timeBuf[16]{};
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M", &localTime);

    char fpsBuf[24]{};
    char msBuf[24]{};
    std::snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", fps);
    std::snprintf(msBuf, sizeof(msBuf), "%.1f ms", frameMs);

    const float op = (std::clamp)(Config::menu_opacity, 0.55f, 1.f);
    const int bgA = (int)(std::clamp(200.f * op + 30.f, 200.f, 240.f));
    const ImU32 brand = IM_COL32(220, 222, 228, 255);
    const ImU32 muted = IM_COL32(140, 144, 152, 230);
    const ImU32 sepBar = IM_COL32(255, 255, 255, 28);
    const ImU32 plate = IM_COL32(16, 17, 20, bgA);
    const ImU32 border = IM_COL32(255, 255, 255, 16);

    const char* brandTxt = "Lefrizzel";
    const ImVec2 sBrand = font->CalcTextSizeA(fs, FLT_MAX, 0.f, brandTxt);
    const ImVec2 sFps = font->CalcTextSizeA(fs, FLT_MAX, 0.f, fpsBuf);
    const ImVec2 sMs = font->CalcTextSizeA(fs, FLT_MAX, 0.f, msBuf);
    const ImVec2 sTm = font->CalcTextSizeA(fs, FLT_MAX, 0.f, timeBuf);

    const float padX = 10.f;
    const float padY = 5.f;
    const float gap = 10.f;
    const float contentW = sBrand.x + gap + sFps.x + gap + sMs.x + gap + sTm.x;
    const float boxW = contentW + padX * 2.f;
    const float boxH = fs + padY * 2.f;
    const float rounding = 3.f;

    const ImVec2 pos(io.DisplaySize.x - boxW - 14.f, 10.f);
    const ImVec2 boxMax(pos.x + boxW, pos.y + boxH);

    dl->AddRectFilled(
        ImVec2(pos.x, pos.y + 1.f),
        ImVec2(boxMax.x, boxMax.y + 2.f),
        IM_COL32(0, 0, 0, 50), rounding);
    dl->AddRectFilled(pos, boxMax, plate, rounding);
    dl->AddRect(pos, boxMax, border, rounding, 0, 1.f);

    const float ty = pos.y + (boxH - fs) * 0.5f - 0.5f;
    float tx = pos.x + padX;

    dl->AddText(font, fs, ImVec2(tx, ty), brand, brandTxt);
    tx += sBrand.x + gap;

    auto tickBar = [&](float x) {
        const float cy = pos.y + boxH * 0.5f;
        const float h = boxH * 0.36f;
        const float bx = x - gap * 0.5f;
        dl->AddLine(
            ImVec2(bx, cy - h * 0.5f),
            ImVec2(bx, cy + h * 0.5f),
            sepBar, 1.f);
    };

    tickBar(tx);
    dl->AddText(font, fs, ImVec2(tx, ty), muted, fpsBuf);
    tx += sFps.x + gap;
    tickBar(tx);
    dl->AddText(font, fs, ImVec2(tx, ty), muted, msBuf);
    tx += sMs.x + gap;
    tickBar(tx);
    dl->AddText(font, fs, ImVec2(tx, ty), muted, timeBuf);
}

void Hud::render() {
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList)
        return;

    if (Config::watermark) {
        __try { DrawWatermark(drawList); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.watermark", GetExceptionCode()); }
    }

    {
        const ImVec2 Center = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        const float sw = ImGui::GetIO().DisplaySize.x;
        const float sh = ImGui::GetIO().DisplaySize.y;
        // FOV rings only when parent feature is enabled (not just the circle toggle)
        if (Config::fov_circle && Config::aimbot) {
            __try {
                RenderFovCircle(drawList, Config::aimbot_fov, Center, sw, sh, 1.25f, Config::fovCircleColor);
            } __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.fov.aim", GetExceptionCode()); }
        }
        if (Config::fov_circle_autofire && Config::autofire) {
            __try {
                RenderFovCircle(drawList, Config::autofire_fov, Center, sw, sh, 1.25f, Config::fovCircleColorAf);
            } __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.fov.af", GetExceptionCode()); }
        }
        if (Config::fov_circle_magnet && Config::triggerbot && Config::trigger_magnet) {
            __try {
                RenderFovCircle(drawList, Config::trigger_magnet_fov, Center, sw, sh, 1.25f, Config::fovCircleColorMagnet);
            } __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.fov.mag", GetExceptionCode()); }
        }
    }

    __try { Widgets::Render(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.widgets", GetExceptionCode()); }
    __try { Scope::ApplyHideViewmodel(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.scopeHide", GetExceptionCode()); }
    __try { Scope::DrawLines(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.scope", GetExceptionCode()); }
    __try { Notify::Render(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Hud.notify", GetExceptionCode()); }
}
