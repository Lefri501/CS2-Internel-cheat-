#pragma once

// Panorama UI bridge (panorama.dll + client matchmaking).
// - Auto-accept: Lobby ReadyUp on match-found
// - Custom HUD API: RunFrame ticks + RunScript / DispatchEvent for later panels

namespace Panorama {

	void Install();

	// Captured from CUIEngine::RunFrame hook (null until first UI tick).
	void* UIEngine();

	// client FindPanel by id (e.g. "CSGOHud", "CSGOPopupManager", "PopupManager").
	void* FindPanel(const char* id);

	// Compile+run JS in a panel context. panel=null → CSGOHud.
	bool RunScript(const char* js);
	bool RunScript(void* panel, const char* js);

	// $.DispatchEvent("<name>") via RunScript on CSGOHud.
	bool DispatchEvent(const char* eventName);

	// Custom HUD panels — register a per-UI-frame callback (id unique).
	using HudTickFn = void(*)();
	bool RegisterHudTick(const char* id, HudTickFn fn);
	void UnregisterHudTick(const char* id);

	// Queue auto-accept attempts (MatchFound hook + RunFrame retries).
	void RequestAutoAccept();
}
