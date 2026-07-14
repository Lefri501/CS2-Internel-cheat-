#pragma once
#include <cstdint>
#include <d3d11.h>
#include "../../../../external/imgui/imgui.h"

namespace SteamAvatar {

// Cached Steam avatar SRV for ImGui. Invalid until Steam resolves the image.
ImTextureID Get(std::uint64_t steamId, ID3D11Device* device);
void ClearCache();

} // namespace SteamAvatar
