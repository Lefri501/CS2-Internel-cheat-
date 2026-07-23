#pragma once

namespace GameMode {

// True when mode treats teammates as valid targets
// (Deathmatch, workshop aim maps, single-team lobbies).
bool IsFfa();

// Effective team filter for aimbot / ESP / glow / chams.
// Auto on  → FFA=false, team modes=true
// Auto off → userPref
bool WantTeamCheck(bool userPref);

// Human-readable mode label for menu
const char* ModeLabel();

// Call once per frame (cheap when cached)
void Tick();

// Apply resolved map name + rescan mode (do not pass junk pointers)
void OnLevelInit(const char* mapName);

// Resolve map via engine if LevelInit was missed (mid-inject).
// Returns true when a non-empty base map name is available.
bool EnsureMap();

// Current map string (may be empty before first LevelInit / EnsureMap)
const char* MapName();
// Strip path / workshop prefix → de_mirage
const char* BaseMap();

} // namespace GameMode
