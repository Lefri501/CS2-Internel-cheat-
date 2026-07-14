#include "hud.h"
#include "../../../external/imgui/imgui.h"
#include "../config/config.h"
#include "../hooks/hooks.h"
#include "../features/widgets/widgets.h"
#include "../features/notify/notify.h"
#include "../features/visuals/scope/scope.h"
#include <ctime>
#include <string>
#include <sstream>
#include <DirectXMath.h>

Hud::Hud() {
}

float CalculateFovRadius(float fovDegrees, float screenWidth, float screenHeight, float gameVerticalFOV) {
    float fovRadians = fovDegrees * (DirectX::XM_PI / 180.0f);
    float screenRadius = std::tan(fovRadians / 2.0f) * (screenHeight / 2.0f) / std::tan(gameVerticalFOV * (DirectX::XM_PI / 180.0f) / 2.0f);
    static float flScalingMultiplier = 2.5f;
    return screenRadius * flScalingMultiplier;
}

void RenderFovCircle(ImDrawList* drawList, float fov, ImVec2 screenCenter, float screenWidth, float screenHeight, float thickness) {
    float radius = CalculateFovRadius(fov, screenWidth, screenHeight, H::g_flActiveFov);
    uint32_t color = ImGui::ColorConvertFloat4ToU32(Config::fovCircleColor);
    drawList->AddCircle(screenCenter, radius, color, 100, thickness);
}

void Hud::render() {
    std::time_t now = std::time(nullptr);
    std::tm localTime;
    localtime_s(&localTime, &now);
    char timeBuffer[9];
    std::strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &localTime);

    float fps = ImGui::GetIO().Framerate;
    std::ostringstream fpsStream;
    fpsStream << static_cast<int>(fps) << " FPS";

    std::string watermarkText = "TempleWare  |  " + fpsStream.str() + "  |  " + timeBuffer;

    ImVec2 textSize = ImGui::CalcTextSize(watermarkText.c_str());
    float padX = 12.0f;
    float padY = 7.0f;
    ImVec2 pos = ImVec2(14, 14);
    ImVec2 rectMax = ImVec2(pos.x + textSize.x + padX * 2, pos.y + textSize.y + padY * 2);
    float rounding = 8.0f;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    // Glass fill
    drawList->AddRectFilled(pos, rectMax, IM_COL32(18, 16, 28, 175), rounding);
    // Soft border
    drawList->AddRect(pos, rectMax, IM_COL32(160, 110, 255, 90), rounding, 0, 1.2f);
    // Accent bar left
    drawList->AddRectFilled(
        ImVec2(pos.x, pos.y + 4.f),
        ImVec2(pos.x + 3.f, rectMax.y - 4.f),
        IM_COL32(160, 110, 255, 220), 2.f);
    // Top gloss
    drawList->AddRectFilledMultiColor(
        ImVec2(pos.x + 1.f, pos.y + 1.f),
        ImVec2(rectMax.x - 1.f, pos.y + 14.f),
        IM_COL32(255, 255, 255, 22),
        IM_COL32(255, 255, 255, 22),
        IM_COL32(255, 255, 255, 0),
        IM_COL32(255, 255, 255, 0));

    drawList->AddText(ImVec2(pos.x + padX, pos.y + padY), IM_COL32(245, 245, 255, 255), watermarkText.c_str());

    if (Config::fov_circle) {
        ImVec2 Center = ImVec2(ImGui::GetIO().DisplaySize.x / 2.f, ImGui::GetIO().DisplaySize.y / 2.f);
        RenderFovCircle(drawList, Config::aimbot_fov, Center, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y, 1.f);
    }

    Widgets::Render();
    Scope::DrawLines();
    Notify::Render();
}
