#pragma once

// Floating damage log panel (player_hurt via Hitmarker).
// Cards + optional session stats; drag when menu open.

namespace HitLog {

struct Entry {
	char name[64]{};
	int damage = 0;
	int hitgroup = 0;
	int healthLeft = -1; // victim HP after hit; -1 = unknown
	bool head = false;
	bool kill = false;
	float born = 0.f;
	bool active = false;
};

// healthLeft: remaining HP after hit (-1 if unknown).
void Push(const char* name, int damage, int hitgroup, bool head, bool kill, int healthLeft = -1);
void Draw();
void Clear();

constexpr int kMax = 24;

} // namespace HitLog
