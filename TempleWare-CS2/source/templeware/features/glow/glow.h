#pragma once
#include <cstdint>
#include "../../../../external/imgui/imgui.h"
#include "../../utils/memory/memsafe/memsafe.h"

// Andromeda-style CS2 glow ESP:
//   player  → DrawGlow hook mutates CGlowProperty
//   world   → force m_Glow on weapons/bomb/projectiles each cache tick

namespace Glow {

struct ColorRgba {
	std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

// CGlowProperty layout (client) — fields used by Andromeda
struct CGlowProperty {
	static constexpr std::size_t kMinSize = 0x58;

	[[nodiscard]] bool ok() const {
		return Mem::IsReadable(this, kMinSize);
	}

	void* owner() const {
		if (!ok())
			return nullptr;
		return Mem::ReadFieldOr<void*>(this, 0x18, nullptr);
	}
	int& glow_type() {
		static int s_fb = 0;
		if (!ok()) return s_fb;
		return *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(this) + 0x30);
	}
	int& glow_range() {
		static int s_fb = 0;
		if (!ok()) return s_fb;
		return *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(this) + 0x38);
	}
	int& glow_range_min() {
		static int s_fb = 0;
		if (!ok()) return s_fb;
		return *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(this) + 0x3C);
	}
	ColorRgba& color_override() {
		static ColorRgba s_fb{};
		if (!ok()) return s_fb;
		return *reinterpret_cast<ColorRgba*>(reinterpret_cast<std::uint8_t*>(this) + 0x40);
	}
	bool& glowing() {
		static bool s_fb = false;
		if (!ok()) return s_fb;
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(this) + 0x51);
	}
};

CGlowProperty* ModelGlow(void* baseModelEntity);
ColorRgba FromImVec4(const ImVec4& c);
void* __fastcall OnDrawGlow(CGlowProperty* glow);
void ApplyWorld(void* baseModelEntity, const ImVec4& color, bool enable);

} // namespace Glow
