#pragma once
#include <cstdint>
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "../../../templeware/utils/schema/schema.h"
#include "../C_CSWeaponBase/C_CSWeaponBase.h"

class CMaterial2
{
public:
	virtual const char* GetName() = 0;
	virtual const char* GetShareName() = 0;
};

struct MaterialKeyVar_t
{
	std::uint64_t uKey;
	const char* szName;

	MaterialKeyVar_t(std::uint64_t uKey, const char* szName) :
		uKey(uKey), szName(szName) { }

	MaterialKeyVar_t(const char* szName, bool bShouldFindKey = false) :
		szName(szName)
	{
		uKey = bShouldFindKey ? FindKey(szName) : 0x0;
	}

	std::uint64_t FindKey(const char* szName)
	{
		using fn = std::uint64_t(__fastcall*)(const char*, unsigned int, int);
		static auto find = reinterpret_cast<fn>(M::patternScan("particles", ("48 89 5C 24 ? 57 48 81 EC ? ? ? ? 33 C0")));
		return find(szName, 0x12, 0x31415926);
	}
};

class CObjectInfo
{
	MEM_PAD(0xB0);
	int nId;
};

class CSceneAnimatableObject {
public:
	// scenesystem CSceneAnimatableObject::m_hOwner @ +0xC0
	CBaseHandle Owner() const {
		if (!this)
			return CBaseHandle();
		return *reinterpret_cast<const CBaseHandle*>(
			reinterpret_cast<const std::uintptr_t>(this) + 0x0C0);
	}
};

struct Color {
	std::uint8_t r = 0U, g = 0U, b = 0U, a = 0U;
};

// scenesystem c_mesh_draw_primitive — 0x68 bytes (SDK 2026-07-13)
//   +0x18 scene animatable, +0x20 material
//   +0x50 tint RGBA, +0x54 alpha scale (distance fade)
class CMeshData
{
public:
	static constexpr std::size_t kStride = 0x68;

	CMeshData* At(int index) noexcept {
		return reinterpret_cast<CMeshData*>(
			reinterpret_cast<std::uintptr_t>(this)
			+ static_cast<std::uintptr_t>(index) * kStride);
	}

	std::uint8_t pad0[0x18];
	CSceneAnimatableObject* pSceneAnimatableObject; // 0x18
	CMaterial2* pMaterial;                         // 0x20
	std::uint8_t pad1[0x28];                       // 0x28 → 0x50
	Color color;                                   // 0x50
	float alphaScale;                              // 0x54 — force 1.0 for XQZ range
	std::uint8_t pad2[0x0E];                       // 0x58 → 0x66
	// 0x62/0x64 render flags live in pad2; leave untouched unless needed
};

static_assert(offsetof(CMeshData, pSceneAnimatableObject) == 0x18, "anim obj");
static_assert(offsetof(CMeshData, pMaterial) == 0x20, "material");
static_assert(offsetof(CMeshData, color) == 0x50, "tint color");
static_assert(offsetof(CMeshData, alphaScale) == 0x54, "alpha scale");
