#include "keybinds.h"
#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

#include "../../../external/imgui/imgui.h"
#include "../config/config.h"

namespace {

// Own edge detection — GetAsyncKeyState's 0x1 bit is global and gets eaten by
// other callers (and can false-trigger after the click that started listening).
bool g_keyDown[256] = {};
bool g_keyPressed[256] = {}; // rising edge this frame
bool g_keyStatesReady = false;

bool RawKeyDown(int vk) {
	if (vk <= 0 || vk >= 256)
		return false;
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void UpdateKeyStates() {
	for (int i = 1; i < 256; ++i) {
		const bool down = RawKeyDown(i);
		// First sample: seed state only (no synthetic press from inject/menu open)
		g_keyPressed[i] = g_keyStatesReady && down && !g_keyDown[i];
		g_keyDown[i] = down;
	}
	g_keyStatesReady = true;
}

bool IsMouseVk(int key) {
	return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON
		|| key == VK_XBUTTON1 || key == VK_XBUTTON2;
}

} // namespace

Keybind::Keybind(bool& featureFlag, int k, KeyMode m)
	: feature(featureFlag)
	, key(k)
	, mode(static_cast<int>(m))
	, toggled(false)
	, isListening(false)
	, waitRelease(false) {}

bool Keybind::isActive() const {
	if (!feature)
		return false;

	switch (static_cast<KeyMode>(mode)) {
	case KeyMode::Always:
		return true;
	case KeyMode::Hold:
		// Unbound hold = never active (not "always")
		if (key <= 0 || key >= 256)
			return false;
		// Live high-bit — tracks mouse/keyboard every CreateMove tick
		return RawKeyDown(key);
	case KeyMode::Toggle:
		// Need a bound key; stale latch with key=0 is treated as off
		if (key <= 0 || key >= 256)
			return false;
		return toggled;
	default:
		return false;
	}
}

Keybinds::Keybinds() {
	// Default: hold mouse4 (aimbot), mouse5 (autofire), toggle MMB (thirdperson)
	keybinds.emplace_back(Keybind(Config::aimbot, VK_XBUTTON1, KeyMode::Hold));
	keybinds.emplace_back(Keybind(Config::autofire, VK_XBUTTON2, KeyMode::Hold));
	keybinds.emplace_back(Keybind(Config::triggerbot, VK_MENU, KeyMode::Hold));
	// AW key: Always unbound = always on when AF/TR Autowall checkbox on
	keybinds.emplace_back(Keybind(Config::autowall, 0, KeyMode::Always));
	keybinds.emplace_back(Keybind(Config::thirdperson, VK_MBUTTON, KeyMode::Toggle));
	keybinds.emplace_back(Keybind(Config::nade_lineup_capture, VK_F6, KeyMode::Hold));
	keybinds.emplace_back(Keybind(Config::knifebot, 0, KeyMode::Always));
	// Always when unbound — Hold+key0 never fires (edgebug "doesn't work")
	keybinds.emplace_back(Keybind(Config::jumpbug, 0, KeyMode::Always));
	keybinds.emplace_back(Keybind(Config::edgebug, 0, KeyMode::Always));
	keybinds.emplace_back(Keybind(Config::edgejump, 0, KeyMode::Always));
}

Keybind* Keybinds::find(bool& feature) {
	for (auto& entry : keybinds) {
		if (&entry.feature == &feature)
			return &entry;
	}
	return nullptr;
}

const Keybind* Keybinds::find(bool& feature) const {
	for (const auto& entry : keybinds) {
		if (&entry.feature == &feature)
			return &entry;
	}
	return nullptr;
}

bool Keybinds::isActive(bool& feature) const {
	const Keybind* kb = find(feature);
	return kb ? kb->isActive() : feature;
}

int Keybinds::getKey(bool& feature) const {
	const Keybind* kb = find(feature);
	return kb ? kb->key : 0;
}

int Keybinds::getMode(bool& feature) const {
	const Keybind* kb = find(feature);
	return kb ? kb->mode : static_cast<int>(KeyMode::Always);
}

void Keybinds::setKey(bool& feature, int key) {
	if (Keybind* kb = find(feature)) {
		kb->key = key;
		// Rebind / config load must not inherit a stale Toggle latch
		kb->toggled = false;
		kb->isListening = false;
		kb->waitRelease = false;
	}
}

void Keybinds::setMode(bool& feature, int mode) {
	if (Keybind* kb = find(feature)) {
		if (mode < static_cast<int>(KeyMode::Always) || mode > static_cast<int>(KeyMode::Toggle))
			mode = static_cast<int>(KeyMode::Hold);
		kb->mode = mode;
		// Always clear toggle latch on mode change / config load
		kb->toggled = false;
	}
}

void Keybinds::resetToDefaults() {
	// Match Keybinds() ctor + Config defaults
	setKey(Config::aimbot, VK_XBUTTON1);
	setMode(Config::aimbot, static_cast<int>(KeyMode::Hold));
	setKey(Config::autofire, VK_XBUTTON2);
	setMode(Config::autofire, static_cast<int>(KeyMode::Hold));
	setKey(Config::triggerbot, VK_MENU);
	setMode(Config::triggerbot, static_cast<int>(KeyMode::Hold));
	setKey(Config::autowall, 0);
	setMode(Config::autowall, static_cast<int>(KeyMode::Always));
	setKey(Config::thirdperson, VK_MBUTTON);
	setMode(Config::thirdperson, static_cast<int>(KeyMode::Toggle));
	setKey(Config::nade_lineup_capture, VK_F6);
	setMode(Config::nade_lineup_capture, static_cast<int>(KeyMode::Hold));
	setKey(Config::knifebot, 0);
	setMode(Config::knifebot, static_cast<int>(KeyMode::Always));
	setKey(Config::jumpbug, 0);
	setMode(Config::jumpbug, static_cast<int>(KeyMode::Always));
	setKey(Config::edgebug, 0);
	setMode(Config::edgebug, static_cast<int>(KeyMode::Always));
	setKey(Config::edgejump, 0);
	setMode(Config::edgejump, static_cast<int>(KeyMode::Always));
}

void Keybinds::clearAllToggles() {
	for (Keybind& k : keybinds) {
		k.toggled = false;
		k.isListening = false;
		k.waitRelease = false;
	}
}

bool Keybinds::isBlockedKey(int key) {
	// Only keys that break menu / can't be unbound cleanly.
	// LMB/RMB are allowed — waitRelease + edge detect avoids binding the open-click.
	switch (key) {
	case VK_INSERT: // menu toggle
	case VK_ESCAPE: // cancel rebind
		return true;
	default:
		return false;
	}
}

bool Keybinds::anyKeyDown() {
	// Prefer cached frame state when available (same as edge scan)
	if (g_keyStatesReady) {
		for (int i = 1; i < 256; ++i) {
			if (g_keyDown[i])
				return true;
		}
		return false;
	}
	for (int i = 1; i < 256; ++i) {
		if (RawKeyDown(i))
			return true;
	}
	return false;
}

int Keybinds::firstKeyPressed() {
	// Mouse first so LMB/RMB win over keyboard if both edge in same frame
	static const int kMouseOrder[] = {
		VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
	};
	for (int vk : kMouseOrder) {
		if (g_keyPressed[vk] && !isBlockedKey(vk))
			return vk;
	}
	for (int i = 1; i < 256; ++i) {
		if (IsMouseVk(i))
			continue;
		if (g_keyPressed[i] && !isBlockedKey(i))
			return i;
	}
	return 0;
}

void Keybinds::keyName(int key, char* out, size_t outSize) {
	formatKeyName(key, out, outSize);
}

void Keybinds::formatKeyName(int key, char* out, size_t outSize) {
	if (!out || outSize == 0)
		return;
	if (key == 0) {
		strcpy_s(out, outSize, "None");
		return;
	}

	switch (key) {
	case VK_INSERT:   strcpy_s(out, outSize, "INS"); return;
	case VK_DELETE:   strcpy_s(out, outSize, "DEL"); return;
	case VK_HOME:     strcpy_s(out, outSize, "HOME"); return;
	case VK_END:      strcpy_s(out, outSize, "END"); return;
	case VK_PRIOR:    strcpy_s(out, outSize, "PGUP"); return;
	case VK_NEXT:     strcpy_s(out, outSize, "PGDN"); return;
	case VK_LEFT:     strcpy_s(out, outSize, "LEFT"); return;
	case VK_RIGHT:    strcpy_s(out, outSize, "RIGHT"); return;
	case VK_UP:       strcpy_s(out, outSize, "UP"); return;
	case VK_DOWN:     strcpy_s(out, outSize, "DOWN"); return;
	case VK_SPACE:    strcpy_s(out, outSize, "SPACE"); return;
	case VK_TAB:      strcpy_s(out, outSize, "TAB"); return;
	case VK_SHIFT:    strcpy_s(out, outSize, "SHIFT"); return;
	case VK_LSHIFT:   strcpy_s(out, outSize, "LSHIFT"); return;
	case VK_RSHIFT:   strcpy_s(out, outSize, "RSHIFT"); return;
	case VK_CONTROL:  strcpy_s(out, outSize, "CTRL"); return;
	case VK_LCONTROL: strcpy_s(out, outSize, "LCTRL"); return;
	case VK_RCONTROL: strcpy_s(out, outSize, "RCTRL"); return;
	case VK_MENU:     strcpy_s(out, outSize, "ALT"); return;
	case VK_LMENU:    strcpy_s(out, outSize, "LALT"); return;
	case VK_RMENU:    strcpy_s(out, outSize, "RALT"); return;
	case VK_CAPITAL:  strcpy_s(out, outSize, "CAPS"); return;
	case VK_LBUTTON:  strcpy_s(out, outSize, "M1"); return;
	case VK_RBUTTON:  strcpy_s(out, outSize, "M2"); return;
	case VK_MBUTTON:  strcpy_s(out, outSize, "M3"); return;
	case VK_XBUTTON1: strcpy_s(out, outSize, "M4"); return;
	case VK_XBUTTON2: strcpy_s(out, outSize, "M5"); return;
	default:
		break;
	}

	if (key >= VK_F1 && key <= VK_F24) {
		sprintf_s(out, outSize, "F%d", key - VK_F1 + 1);
		return;
	}
	if (key >= 'A' && key <= 'Z') {
		sprintf_s(out, outSize, "%c", key);
		return;
	}
	if (key >= '0' && key <= '9') {
		sprintf_s(out, outSize, "%c", key);
		return;
	}
	if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
		sprintf_s(out, outSize, "NP%d", key - VK_NUMPAD0);
		return;
	}

	UINT scan = MapVirtualKeyA((UINT)key, MAPVK_VK_TO_VSC);
	if (scan) {
		LONG lParam = (LONG)(scan << 16);
		if (GetKeyNameTextA(lParam, out, (int)outSize) > 0)
			return;
	}
	sprintf_s(out, outSize, "0x%02X", key);
}

bool Keybinds::isListening() const {
	for (const auto& kb : keybinds) {
		if (kb.isListening)
			return true;
	}
	return false;
}

void Keybinds::pollInputs(bool menuOpen) {
	// One edge sample per frame — menuButton reads the same edges for rebind
	UpdateKeyStates();

	if (menuOpen || isListening())
		return;

	for (Keybind& k : keybinds) {
		// Feature off → drop Toggle latch so re-enable doesn't inherit "on"
		if (!k.feature) {
			k.toggled = false;
			continue;
		}
		if (static_cast<KeyMode>(k.mode) != KeyMode::Toggle)
			continue;
		if (k.key <= 0 || k.key >= 256)
			continue;
		if (g_keyPressed[k.key])
			k.toggled = !k.toggled;
	}
}

// Clamp requested width to remaining space on the current line.
// Never return 0 — ImGui Button(w=0) auto-sizes and easily clips out of cards.
static float FitItemWidth(float want)
{
	const float avail = ImGui::GetContentRegionAvail().x;
	if (avail < 1.f)
		return 1.f;
	if (want < 0.f)
		return avail;
	if (want > avail)
		return avail;
	if (want < 1.f)
		return avail;
	return want;
}

void Keybinds::menuMode(bool& feature, float width) {
	Keybind* kb = find(feature);
	if (!kb) {
		ImGui::TextDisabled("N/A");
		return;
	}

	ImGui::PushID(kb);
	const char* modes[] = { "Always", "Hold", "Toggle" };
	ImGui::SetNextItemWidth(FitItemWidth(width));
	if (ImGui::Combo("##mode", &kb->mode, modes, IM_ARRAYSIZE(modes))) {
		if (static_cast<KeyMode>(kb->mode) != KeyMode::Toggle)
			kb->toggled = false;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Always on, hold key, or toggle with key.");
	ImGui::PopID();
}

void Keybinds::menuButton(bool& feature, float width) {
	Keybind* kb = find(feature);
	if (!kb) {
		ImGui::TextDisabled("N/A");
		return;
	}

	ImGui::PushID(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(kb) ^ 0xB1u));

	// Critical: btnW must never be 0 (auto-size overflows SameLine / tight cards)
	const float btnW = FitItemWidth(width);

	const bool needKey = static_cast<KeyMode>(kb->mode) != KeyMode::Always;
	if (!needKey) {
		ImGui::BeginDisabled();
		ImGui::Button("Always", ImVec2(btnW, 0));
		ImGui::EndDisabled();
		ImGui::PopID();
		return;
	}

	char name[32];
	keyName(kb->key, name, sizeof(name));

	if (!kb->isListening) {
		char label[48];
		sprintf_s(label, "[ %s ]", name);

		if (ImGui::Button(label, ImVec2(btnW, 0))) {
			// LMB started rebind — wait until all keys released before capturing
			kb->isListening = true;
			kb->waitRelease = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Click to set key. Right-click to clear.");

		// Clear bind (not while listening — RMB is a valid bind key)
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			kb->key = 0;
			kb->isListening = false;
			kb->waitRelease = false;
			kb->toggled = false;
		}
	} else {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.38f, 0.98f, 0.55f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.38f, 0.98f, 0.70f));
		ImGui::Button("...", ImVec2(btnW, 0));
		ImGui::PopStyleColor(2);

		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Press a key. Esc cancels.");

		if (kb->waitRelease) {
			// Edges from pollInputs already advanced while keys held; stay until quiet
			if (!anyKeyDown())
				kb->waitRelease = false;
		} else {
			// Esc cancel — use edge so hold doesn't spam
			if (g_keyPressed[VK_ESCAPE]) {
				kb->isListening = false;
				kb->waitRelease = false;
			} else {
				const int pressed = firstKeyPressed();
				if (pressed != 0) {
					kb->key = pressed;
					kb->isListening = false;
					kb->waitRelease = false;
					kb->toggled = false;
				}
			}
		}
	}

	ImGui::PopID();
}

int Keybinds::listSnapshots(KeybindSnapshot* out, int maxOut) const {
	if (!out || maxOut <= 0)
		return 0;
	int n = 0;
	for (const auto& kb : keybinds) {
		if (n >= maxOut)
			break;
		KeybindSnapshot& s = out[n++];
		s.key = kb.key;
		s.mode = kb.mode;
		// Nade Capture host stays true; gate visibility on master lineup toggle
		// Autowall host stays true; show when AF or TR Autowall checkbox on
		if (&kb.feature == &Config::nade_lineup_capture)
			s.enabled = kb.feature && Config::nade_lineup;
		else if (&kb.feature == &Config::autowall)
			s.enabled = kb.feature
				&& (Config::autofire_autowall || Config::trigger_autowall);
		else
			s.enabled = kb.feature;
		s.active = kb.isActive();
		if (&kb.feature == &Config::aimbot)
			s.name = "Aimbot";
		else if (&kb.feature == &Config::autofire)
			s.name = "Autofire";
		else if (&kb.feature == &Config::triggerbot)
			s.name = "Triggerbot";
		else if (&kb.feature == &Config::autowall)
			s.name = "Autowall";
		else if (&kb.feature == &Config::thirdperson)
			s.name = "Third Person";
		else if (&kb.feature == &Config::nade_lineup_capture)
			s.name = "Nade Capture";
		else if (&kb.feature == &Config::knifebot)
			s.name = "Knife Bot";
		else if (&kb.feature == &Config::jumpbug)
			s.name = "Jumpbug";
		else if (&kb.feature == &Config::edgebug)
			s.name = "Edgebug";
		else if (&kb.feature == &Config::edgejump)
			s.name = "Edgejump";
		else
			s.name = "Bind";
	}
	return n;
}

Keybinds keybind;
