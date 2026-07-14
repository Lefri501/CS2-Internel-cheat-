#pragma once
#include <vector>
#include <cstddef>

enum class KeyMode : int {
	Always = 0,
	Hold = 1,
	Toggle = 2
};

struct Keybind {
	bool& feature;   // master checkbox (Config::aimbot etc.)
	int   key;
	int   mode;      // KeyMode
	bool  toggled;   // runtime state for Toggle mode
	bool  isListening;
	bool  waitRelease;

	Keybind(bool& featureFlag, int k = 0, KeyMode m = KeyMode::Toggle);
	bool isActive() const;
};

struct KeybindSnapshot {
	const char* name = "";
	int key = 0;
	int mode = 0;
	bool active = false;
	bool enabled = false;
};

class Keybinds {
public:
	Keybinds();

	// Call every frame (even when menu closed). Safe while listening/menu open.
	void pollInputs(bool menuOpen);

	// ImGui: rebind key only
	void menuButton(bool& feature);

	// ImGui: Always / Hold / Toggle combo for this feature
	void menuMode(bool& feature);

	bool isActive(bool& feature) const;
	bool isListening() const;

	// persist key + mode
	int  getKey(bool& feature) const;
	int  getMode(bool& feature) const;
	void setKey(bool& feature, int key);
	void setMode(bool& feature, int mode);
	// Sync live keybinds to defaults (used by Config::ResetToDefaults / Load)
	void resetToDefaults();
	// Drop Toggle latches without changing keys (config load safety)
	void clearAllToggles();

	// Overlay / widgets
	int listSnapshots(KeybindSnapshot* out, int maxOut) const;
	static void formatKeyName(int key, char* out, size_t outSize);

private:
	std::vector<Keybind> keybinds;

	Keybind* find(bool& feature);
	const Keybind* find(bool& feature) const;

	static bool anyKeyDown();
	static int  firstKeyPressed();
	static void keyName(int key, char* out, size_t outSize);
	static bool isBlockedKey(int key);
};

extern Keybinds keybind;
