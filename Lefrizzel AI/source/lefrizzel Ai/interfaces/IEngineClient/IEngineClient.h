#pragma once

// used: callvfunc
#include "..\..\utils\memory\vfunc\vfunc.h"
#include "..\..\utils\math\vector\vector.h"
#include <type_traits>
#include <cstring>
#include <cmath>
#include <Windows.h>

// NetClientInfo — Source2EngineToClient (UC Pa1nt4r: LocalData.ShootPosition).
// Layouts differ slightly across builds; we probe LocalData pointer slots + vector offsets.
class c_local_data
{
public:
	// Common layouts: shoot at +0x0 / +0x4 / +0x8 / +0xC
	char pad_0000[0x40];
};

class c_networked_client_info
{
public:
	char pad_0000[0x4];              // 0x0000
	int32_t m_nRenderTick;           // 0x0004
	float m_flRenderTickFraction;    // 0x0008
	int32_t m_nPlayerTickCount;      // 0x000C
	float m_flPlayerTickFraction;    // 0x0010
	char pad_0014[0x4];              // 0x0014
	// LocalData* commonly @ 0x18 or 0x20 — scanned in get_local_shoot_position
	char pad_0018[0x30];             // 0x0018..
}; // Size: 0x48

class IEngineClient
{
public:
	int maxClients()
	{
		return M::vfunc<int, 35U>(this);
	}

	bool in_game()
	{
		return M::vfunc<bool, 39U>(this);
	}

	bool connected()
	{
		return M::vfunc<bool, 40U>(this);
	}

	int get_local_player() {
		int nIndex = -1;
		M::vfunc<void, 54U>(this, std::ref(nIndex), 0);
		return nIndex + 1;
	}

	static bool info_looks_filled(const c_networked_client_info& out)
	{
		if (out.m_nRenderTick > 0 && out.m_nRenderTick < 0x7FFFFFFF)
			return true;
		const auto* base = reinterpret_cast<const std::uint8_t*>(&out);
		for (int off = 0x14; off <= 0x28; off += 8) {
			void* p = nullptr;
			std::memcpy(&p, base + off, sizeof(p));
			if (p && reinterpret_cast<std::uintptr_t>(p) > 0x10000ull)
				return true;
		}
		return false;
	}

	// Safe fill — never return pointer to stack
	bool get_networked_client_info(c_networked_client_info& out)
	{
		std::memset(&out, 0, sizeof(out));
		// Common indices across builds (compile-time CallVFunc)
		__try {
			M::CallVFunc<void, 178U>(this, &out);
			if (info_looks_filled(out))
				return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		std::memset(&out, 0, sizeof(out));
		__try {
			M::CallVFunc<void, 179U>(this, &out, 1);
			if (info_looks_filled(out))
				return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		std::memset(&out, 0, sizeof(out));
		__try {
			M::CallVFunc<void, 177U>(this, &out);
			if (info_looks_filled(out))
				return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		return false;
	}

	static bool valid_shoot_vec(const Vector_t& sp)
	{
		if (!std::isfinite(sp.x) || !std::isfinite(sp.y) || !std::isfinite(sp.z))
			return false;
		if (sp.x == 0.f && sp.y == 0.f && sp.z == 0.f)
			return false;
		if (std::fabs(sp.x) > 16384.f || std::fabs(sp.y) > 16384.f || std::fabs(sp.z) > 16384.f)
			return false;
		return true;
	}

	// Engine local shoot origin (post-prediction). Prefer over abs+view / m_vOldOrigin.
	bool get_local_shoot_position(Vector_t& out)
	{
		out = Vector_t{};
		c_networked_client_info info{};
		if (!get_networked_client_info(info))
			return false;

		const auto* base = reinterpret_cast<const std::uint8_t*>(&info);
		// Scan LocalData* candidates then Vector candidates inside pointed block
		for (int poff = 0x14; poff <= 0x28; poff += 8) {
			void* ld = nullptr;
			std::memcpy(&ld, base + poff, sizeof(ld));
			if (!ld || reinterpret_cast<std::uintptr_t>(ld) < 0x10000ull)
				continue;
			const auto* ldb = reinterpret_cast<const std::uint8_t*>(ld);
			for (int voff = 0; voff <= 0x18; voff += 4) {
				Vector_t sp{};
				std::memcpy(&sp, ldb + voff, sizeof(sp));
				if (valid_shoot_vec(sp)) {
					out = sp;
					return true;
				}
			}
		}
		return false;
	}

	void get_screen_size(int& width, int& height)
	{
		return M::CallVFunc<void, 60U>(this, width, height);
	}

	// CEngineClient slots 64/65 (sdk dump) — full path vs short name (de_mirage)
	const char* get_level_name()
	{
		return M::vfunc<const char*, 64U>(this);
	}

	const char* get_level_name_short()
	{
		return M::vfunc<const char*, 65U>(this);
	}
public:
	inline bool valid() {
		return connected() && in_game();
	}
};
