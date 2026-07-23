#pragma once

#include <cstdint>

// engine2.dll patterns from cs2 sdk dump (Patterns/patterns.hpp namespace engine2).
// Resolves NetworkGameClient, level name, in-game/connected, build, window size,
// PVS, and CNetworkGameClient::RunPrediction for cheat reliability.

namespace Engine2 {

bool Init();

// CNetworkGameClient* (may be null in menu)
void* NetworkGameClient();

// Signon state int @ NGC+0x230 (IDA GetLevelName). Ready when >= 2; full play ~6+.
int SignonState();

bool IsConnected();
bool IsInGame();
// Prefer over fragile EngineClient vfuncs when mid-inject / iface stale.
bool Ready(); // connected && signon usable && in map

const char* LevelNameShort(); // de_mirage
const char* LevelName();      // maps/de_mirage or full

int BuildNumber();

bool GetWindowSize(int& outW, int& outH);

void* PvsManager();

// CNetworkGameClient::RunPrediction(this, reason) — engine client-side predict.
// Safe no-op if unresolved / not in game. Call under SEH from Pred.
bool RunPrediction(unsigned reason = 0);

// Voice: true if engine thinks slot is speaking (when pattern resolves).
bool IsHearingClient(int slot);

} // namespace Engine2
