#pragma once
#include "../../utils/math/vector/vector.h"
#include <cstdint>
#include <cstddef>

namespace Trace {

// CGameTrace layout verified IDA client TraceShape fill (sub_180986B80) + InitTraceInfo (0x1816403D0):
//   +0x00 surface / contents ptr
//   +0x08 hit_entity
//   +0x10 hitbox (often default "invalid_hitbox" singleton — prefer entity+capsule)
//   +0x78 startpos (vec3)
//   +0x84 endpos (vec3)
//   +0x90 plane normal (vec3)
//   +0xAC fraction (float)  — DidHit: fraction < 1.0
//   +0xBB startsolid (byte) — DidHit: || startsolid
//   +0xBA ray type copy
// Engine multi-hit stride is 0xC0; InitTraceInfo writes through +0xBB.
// Keep headroom so fill helpers never overrun the stack object.
struct CGameTrace {
	alignas(16) std::uint8_t raw[0x200]{};

	void* surface() const {
		return *reinterpret_cast<void* const*>(raw + 0x00);
	}
	void* hit_entity() const {
		return *reinterpret_cast<void* const*>(raw + 0x08);
	}
	void* hit_box() const {
		return *reinterpret_cast<void* const*>(raw + 0x10);
	}
	// CHitBox offsets (when real, not invalid singleton): +0x38 group, +0x48 index
	int hitgroup() const {
		const auto* hb = reinterpret_cast<const std::uint8_t*>(hit_box());
		if (!hb)
			return -1;
		const int g = *reinterpret_cast<const int*>(hb + 0x38);
		// invalid_hitbox singleton keeps group 0 and name "invalid_hitbox"
		if (g == 0) {
			// still allow group 0 only if entity hit and fraction < 1
			if (!hit_entity() || fraction() >= 1.f)
				return -1;
		}
		return g;
	}
	int hitbox_index() const {
		const auto* hb = reinterpret_cast<const std::uint8_t*>(hit_box());
		if (!hb)
			return -1;
		const int idx = static_cast<int>(*reinterpret_cast<const std::uint16_t*>(hb + 0x48));
		// invalid singleton uses 0xFFFF at nearby fields
		if (idx < 0 || idx > 64)
			return -1;
		return idx;
	}
	Vector_t startpos() const {
		const float* f = reinterpret_cast<const float*>(raw + 0x78);
		return Vector_t{ f[0], f[1], f[2] };
	}
	Vector_t endpos() const {
		const float* f = reinterpret_cast<const float*>(raw + 0x84);
		return Vector_t{ f[0], f[1], f[2] };
	}
	Vector_t normal() const {
		const float* f = reinterpret_cast<const float*>(raw + 0x90);
		return Vector_t{ f[0], f[1], f[2] };
	}
	float fraction() const {
		return *reinterpret_cast<const float*>(raw + 0xAC);
	}
	bool startsolid() const {
		// IDA TraceShape return: *(float*)(tr+172) < 1.0 || *(BYTE*)(tr+187)
		return raw[0xBB] != 0;
	}
	bool allsolid() const {
		return raw[0xBA] != 0 && raw[0xBB] != 0;
	}
};

// FireBullet = 0x1C3001. Vis adds BLOCKLOS|OPAQUE|WINDOW|GRATE (0xCA) so
// static world that isn't tagged SOLID alone still occludes.
constexpr std::uint64_t kMaskVis      = 0x1C300Full; // 0x1C3001 | 0xE (win/grate/aux-ish) | keep solid
constexpr std::uint64_t kMaskShot     = 0x1C3001ull;
constexpr std::uint64_t kMaskShotPen  = 0x1C300Bull;
constexpr std::uint64_t kMaskGrenade  = 0x2009ull;
constexpr std::uint64_t kMaskWorld    = 0x1C300Full;

// Layer 4 = FireBullet. a5=0 → rewrite runs (entity list prep).
constexpr int kFilterLayerVis = 4;
constexpr std::uint16_t kFilterA5Vis = 0;
constexpr int kFilterLayerGrenade = 4;
constexpr std::uint16_t kFilterA5Grenade = 7;

bool Init();
bool Ready();

// Line trace via TraceShape (primary) or wrapper. Returns true if call succeeded.
bool TraceLine(const Vector_t& start, const Vector_t& end, void* skip, CGameTrace& out,
               std::uint64_t mask = kMaskVis, int layer = kFilterLayerVis,
               std::uint16_t a5 = kFilterA5Vis);

// Hull (mins/maxs) trace — same TraceShape path with typed ray.
bool TraceHull(const Vector_t& start, const Vector_t& end,
               const Vector_t& mins, const Vector_t& maxs,
               void* skip, CGameTrace& out,
               std::uint64_t mask = kMaskVis, int layer = kFilterLayerVis,
               std::uint16_t a5 = kFilterA5Vis);

// Visible iff first hit entity == target, or clear path (no hit).
bool IsVisible(const Vector_t& start, const Vector_t& end, void* skip = nullptr,
               void* target = nullptr, std::uint64_t mask = kMaskVis,
               bool penetrate_players = false);

bool IsAnyVisible(const Vector_t& start, const Vector_t* ends, int count, void* skip,
                  void* target = nullptr, std::uint64_t mask = kMaskVis,
                  bool penetrate_players = false);

bool IsBodyVisible(const Vector_t& eye, const Vector_t* bones, int boneCount,
                   void* skip, void* target, std::uint64_t mask = kMaskVis);

bool IsWorldVisible(const Vector_t& eye, const Vector_t* bones, int boneCount,
                    void* skip, std::uint64_t mask = kMaskVis);

// IDA TraceShape return predicate
bool DidHit(const CGameTrace& tr);

// Surface data for hit (GetSurfaceData) — pen/hardness. Returns false if unavailable.
bool GetHitSurfaceData(const CGameTrace& tr, float& outPenetration, float& outHardness);

// Raw game pointers (debug / advanced)
void* GetTraceWorld();
void* GetTraceShapeFn();
void* GetInitFilterFn();

} // namespace Trace
