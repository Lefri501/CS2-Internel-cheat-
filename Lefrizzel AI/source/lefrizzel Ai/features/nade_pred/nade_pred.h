#pragma once
#include "../../utils/math/vector/vector.h"
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include <vector>
#include <cstdint>

namespace NadePred {

enum class NadeType : int {
	Unknown = 0,
	HE,
	Flash,
	Smoke,
	Molly,
	Decoy
};

struct Path {
	NadeType type = NadeType::Unknown;
	std::vector<Vector_t> points;
	std::vector<Vector_t> hits; // bounce positions for markers
	Vector_t origin{}; // current projectile / effect pos (for warning icon)
	Vector_t land{};             // rest / pop point (HE/flash may boom mid-air)
	float radius = 0.f;
	float time_left = -1.f; // seconds remaining for smoke/fire/decoy; -1 = none
	float flight_sec = 0.f; // sim duration to land/boom (seconds)
	bool local_preview = false;
	bool own_projectile = false; // path color only
	bool show_warn = false;      // flying or live effect — draw warn badge
	bool effect_active = false;  // detonated smoke / burning inferno
	bool detonated = false;      // fuse/impact boom (not a ground rest for HE/flash)
	int ent_index = 0;           // entity slot (instant drop when gone)
	// C_Inferno live footprint (hull draw — not origin circle)
	Vector_t fire_pos[64]{};
	int fire_count = 0;
	float fire_half_width = 0.f;
};

void Update();
void Draw(const ViewMatrix& vm);
const std::vector<Path>& Paths();
void Reset(); // clear effect timers (LevelInit / map change / round)
void OnGameEvent(void* gameEvent); // round_start etc — wipe cross-round timer bleed

} // namespace NadePred
