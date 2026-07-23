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

// Sample ~4 dominant colours from the skin preview image into outRgba[16] (4×RGBA).
// Used when CPaintKit color0..3 are filler (most skins — pattern is in the texture).
bool SamplePaintPalette(const char* simpleName, const char* kitToken, int paintKitId, float outRgba[16]);

// True when the last Get/GetPaint path is still downloading / decoding.
bool PreviewPending();

// Draw fitted image in available width / fixed height. Returns true if drawn.
bool DrawPanel(ImTextureID tex, float maxH = 140.f);

// Hover tooltip with image
void DrawHover(ImTextureID tex, float size = 160.f);

} // namespace SkinPreview
