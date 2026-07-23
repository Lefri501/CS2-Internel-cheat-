#pragma once
#include <cstdint>
#include "../../../../external/imgui/imgui.h"
#include "../../utils/memory/memsafe/memsafe.h"

// CS2 engine glow ESP (independent of chams mesh tint):
//   player  → DrawGlow + GetGlowColor hooks mutate CGlowProperty / scene float4
//   world   → force m_Glow on weapons/bomb/projectiles each cache tick
//
// IDA (client.dll):
//   GetGlowColor        0x180B499F0 — override → float4 used by ManageGlow
//   ManageGlowSceneObject 0x180B1B2B0 — writes scene object colour @ +0xD0
//   DrawGlow getter     0x180B49B30 — glow_type @ +0x30
// Chams only touch CMeshData::color during DrawArray; they must never drive glow.

namespace Glow {

struct ColorRgba {
	std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

// CGlowProperty layout (client) — fields used by engine glow path
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
	// IDA ManageGlow: secondary scene @ +0x20, primary scene @ +0x28
	void* scene_object_alt() const {
		if (!ok())
			return nullptr;
		return Mem::ReadFieldOr<void*>(this, 0x20, nullptr);
	}
	// IDA B04B30 stores ManageGlow primary scene object here
	void* scene_object() const {
		if (!ok())
			return nullptr;
		return Mem::ReadFieldOr<void*>(this, 0x28, nullptr);
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
	// m_bFlashing @ +0x44 — multiplies GetGlowColor RGB if set
	bool& flashing() {
		static bool s_fb = false;
		if (!ok()) return s_fb;
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(this) + 0x44);
	}
	// m_fGlowColor @ +0x8 — float RGB used by scene glow (independent of mesh/chams tint)
	float* glow_color_f3() {
		if (!ok()) return nullptr;
		return reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(this) + 0x8);
	}
	bool& glowing() {
		static bool s_fb = false;
		if (!ok()) return s_fb;
		return *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(this) + 0x51);
	}
};

CGlowProperty* ModelGlow(void* baseModelEntity);
ColorRgba FromImVec4(const ImVec4& c);

// True when scene animatable is an engine glow outline object for this entity
// (or carries ManageGlow magic @ +0xC4). Chams must never replace these draws.
[[nodiscard]] bool IsEngineGlowSceneObject(void* baseModelEntity, void* sceneAnimatable);
// Magic-only check (no entity) — IDA ManageGlow writes dword -1396705927 @ scene+196.
[[nodiscard]] bool IsEngineGlowSceneObject(void* sceneAnimatable);

// Write Config glow colour → override + m_fGlowColor (never chams colours).
// Alpha is honoured (semi-transparent outlines).
void ApplyGlowColor(CGlowProperty* glow, const ColorRgba& col);

// Stop engine backface path (entity+0xE38 / ManageGlow a8) treating glow as mesh-linked.
void IsolateFromMeshTint(void* baseModelEntity);

// DrawGlow getter path — enable/disable + stamp property colours.
void* __fastcall OnDrawGlow(CGlowProperty* glow);

// IDA B499F0: force float4 so chams/mesh pre-fill cannot bleed into glow.
// Returns true when outRgba was overwritten with Config glow colours.
bool ForceSceneColor(CGlowProperty* glow, float* outRgba);

// IDA B1B2B0: if GetGlowColor just forced a colour this call, write it into color4.
// Returns true when color4 was overwritten.
bool ForceManageColor(float* color4);

// IDA B04B30 — full glow apply (pre-fill → GetGlowColor → ManageGlow).
// Stamp Config colours before original; force scene-object float4 after.
std::int64_t OnApplyGlowScene(CGlowProperty* glow, void* sceneNode,
	std::int64_t(__fastcall* original)(CGlowProperty*, void*));

// Per-frame stamp from player cache (keeps colours sticky when DrawGlow is sparse).
void ApplyPlayer(void* pawn, bool visible);

void ApplyWorld(void* baseModelEntity, const ImVec4& color, bool enable);

// Resolve GlowObjectManager + OnGlowTypeChanged + ManageGlowSceneObject
bool Init();
void* GlowManager(); // GlowObjectManager* (may be null)

} // namespace Glow
