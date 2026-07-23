#pragma once

#include "../../utils/math/vector/vector.h"
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include <cstdint>
#include <string>
#include <vector>

// UC 9xth grenade lineup helper (stand circle + aim marker).
// Stand = abs origin (GetAbsOrigin / scene+0x330). Aim = view angles → eye+fwd*8192.
// Patterns: patterns.hpp GetAbsOrigin, GetViewAngles (already wired via Bones/Input).
namespace NadeLineup {

enum class ThrowType : int {
	Stand = 0,   // was Normal — keep value for old JSON
	Jump = 1,    // was Jumpthrow
	Walk = 2,
	Run = 3,
	Crouch = 4,
	RunJump = 5,
	// Back-compat aliases
	Normal = Stand,
	Jumpthrow = Jump,
	Count = 6
};

enum class NadeKind : int {
	Any = 0,
	HE,
	Flash,
	Smoke,
	Molly,
	Decoy
};

struct Lineup {
	std::string name;
	std::string map;          // base map name (de_mirage, …)
	NadeKind kind = NadeKind::Any;
	ThrowType throwType = ThrowType::Normal;
	Vector_t pos{};           // stand feet
	QAngle_t aimAngles{};
	Vector_t target{};        // eye + forward * 8192 (aim marker world)
};

void Init();
void Shutdown();
void OnLevelInit(const char* mapName);
void Update(); // CreateMove / Present — capture key + nearest target
void Draw(const ViewMatrix& vm);

// Menu / hotkey — ArmCapture waits for throw; auto-detects Jump/Run/Walk/Crouch
bool ArmCapture(const char* name, NadeKind kindOverride = NadeKind::Any);
void CancelCapture();
bool IsCapturing();
bool Capture(const char* name, ThrowType throwType, NadeKind kindOverride = NadeKind::Any);
bool RemoveAt(int index);
void ClearCurrentMap();
bool Save();
bool Load();

const std::vector<Lineup>& All();
const Lineup* Current();
const char* CurrentMap();
bool IsCurrentMap(const Lineup& L);
int CountCurrentMap();
const char* KindName(NadeKind k);
const char* ThrowName(ThrowType t);

} // namespace NadeLineup
