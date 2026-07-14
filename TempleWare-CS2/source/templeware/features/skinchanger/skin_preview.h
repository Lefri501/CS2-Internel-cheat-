#pragma once
#include <cstdint>
#include <string>
#include <d3d11.h>
#include "../../../../external/imgui/imgui.h"

namespace SkinPreview {

void Init(ID3D11Device* device);
void Shutdown();

// panorama path helpers
std::string PaintPath(const char* simpleName, const char* kitToken);
std::string ModelPath(const char* simpleName);

// Load (or cache) texture for a GAME vtex_c path. Returns invalid if missing.
ImTextureID Get(const std::string& path);

// Convenience: paint kit preview (kitToken empty/Vanilla → model base image)
ImTextureID GetPaint(const char* simpleName, const char* kitToken, int paintKitId);

// Draw fitted image in available width / fixed height. Returns true if drawn.
bool DrawPanel(ImTextureID tex, float maxH = 140.f);

// Hover tooltip with image
void DrawHover(ImTextureID tex, float size = 160.f);

} // namespace SkinPreview
