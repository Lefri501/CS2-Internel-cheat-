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

// Capture map name from LevelInit (a2 often const char*)
void OnLevelInit(const char* mapName);

} // namespace GameMode
