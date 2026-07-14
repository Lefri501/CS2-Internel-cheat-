#pragma once
#include <cstdint>
#include <vector>
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "..\..\..\cs2\entity\C_CSPlayerPawn\C_CSPlayerPawn.h"
#include "..\..\..\cs2\entity\C_BaseEntity\C_BaseEntity.h"

class LocalPlayerCached {
public:
	int handle = 0;
	int health = 0;
	int armor = 0;
	int team = 0;
	int lastTeam = 0; // retained after death for team filter
	bool alive = false;
	bool active = false;
	Vector_t position{};

	void reset() {
		position = Vector_t();
		team = 0;
		health = 0;
		armor = 0;
		handle = 0;
		alive = false;
		active = false;
		// lastTeam intentionally kept
	}
};

enum PlayerType_t : int {
	none = -1,
	enemy = 0,
	team = 1,
};

struct PlayerCache {
	CBaseHandle handle{};
	PlayerType_t type = none;
	int health = 0;
	int maxHealth = 100;
	int armor = 0;
	int team_num = 0;
	Vector_t position{};
	Vector_t viewOffset{};
	char name[128]{};
	std::uint64_t steamId = 0;
	char weapon_name[64]{};
	char weapon_key[48]{}; // icon lookup key: ak47 / m4a1_silencer

	// Flags
	bool flashed = false;
	bool bomb = false;      // carrying / planting C4
	bool scoped = false;
	bool reloading = false;
	bool defusing = false;

	// Line-of-sight from local eye (true if no wall / hit is this pawn)
	bool visible = true;
};

enum WorldEspKind : int {
	WORLD_WEAPON = 0,
	WORLD_BOMB,
	WORLD_SMOKE,
	WORLD_MOLOTOV,
	WORLD_HE,
	WORLD_FLASH,
	WORLD_DECOY,
};

struct WorldCache {
	WorldEspKind kind = WORLD_WEAPON;
	Vector_t position{};
	char label[64]{};
	char weapon_key[48]{}; // for WORLD_WEAPON icon draw
	float timer = -1.f; // bomb/defuse seconds remaining; <0 = none
	float radius = 0.f; // smoke cloud / molly fire ring (units)
	int bomb_site = -1; // 0=A, 1=B
	bool defusing = false;
	bool effect_active = false; // smoke popped / inferno burning
	bool use_badge = false;     // cool warn-style icon (smoke/molly)
};

// Shared planted-C4 state for bomb HUD widget (independent of world ESP draw)
struct PlantedBombInfo {
	bool active = false;
	int site = -1;           // 0=A, 1=B
	float blowLeft = -1.f;   // seconds to explosion
	float defuseLeft = -1.f; // seconds to finish defuse; <0 if not defusing
	bool defusing = false;
	Vector_t position{};
};
extern PlantedBombInfo g_plantedBomb;

namespace Esp {
	class Visuals {
	public:
		void init();
		void esp();
	private:
		bool ensureViewMatrix();
		void drawPlayers();
		void drawWorld();
		ViewMatrix viewMatrix;
	};
	void cache();
	void ResetWorldFxTimers(); // clear smoke/fire/decoy expiry (LevelInit)
}

extern LocalPlayerCached cached_local;
extern std::vector<PlayerCache> cached_players;
extern std::vector<WorldCache> cached_world;
