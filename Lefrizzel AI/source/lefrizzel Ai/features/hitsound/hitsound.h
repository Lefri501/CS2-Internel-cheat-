#pragma once

#include <cstddef>

namespace Hitsound {
	// Fixed scan folder for custom .wav files (created on first inject via Install).
	constexpr const char* kFolder =
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\sounds\\hitsounds";

	// Hooks::init → Install: create_directories(hitsounds) + scan *.wav
	void Install();
	void Shutdown();

	// Rescan folder for *.wav (also re-creates folder if missing)
	void RefreshList();

	// Dropdown helpers
	int Count();
	const char* NameAt(int index);          // basename e.g. "hit1.wav"
	int IndexOf(const char* fileName);      // -1 if missing

	// Play selected (or head/kill override). Safe from game-event thread.
	void Play(bool head, bool kill);

	// Preview currently selected normal sound (menu Test button)
	void PreviewSelected();
}
