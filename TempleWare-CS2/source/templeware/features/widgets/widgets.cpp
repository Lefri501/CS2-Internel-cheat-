#include "widgets.h"
#include "steam_avatar.h"

#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../visuals/visuals.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../../external/imgui/imgui.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <d3d11.h>

extern ID3D11Device* pDevice;

namespace Widgets {
namespace {

void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 accent) {
	const float r = 7.f;
	dl->AddRectFilled(a, b, IM_COL32(14, 14, 18, 210), r);
	dl->AddRect(a, b, IM_COL32(255, 255, 255, 18), r, 0, 1.f);
	dl->AddRectFilled(ImVec2(a.x, a.y + 3.f), ImVec2(a.x + 2.5f, b.y - 3.f), accent, 2.f);
	dl->AddRectFilledMultiColor(
		ImVec2(a.x + 1.f, a.y + 1.f),
		ImVec2(b.x - 1.f, a.y + 12.f),
		IM_COL32(255, 255, 255, 16), IM_COL32(255, 255, 255, 16),
		IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
}

ImVec4 U32ToVec4(ImU32 c) {
	return ImVec4(
		((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f);
}

ImU32 ColU32(const ImVec4& c) {
	return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 ColU32A(const ImVec4& c, float aMul) {
	ImVec4 t = c;
	t.w = std::clamp(c.w * aMul, 0.f, 1.f);
	return ImGui::ColorConvertFloat4ToU32(t);
}

constexpr ImGuiColorEditFlags kWidgetColFlags =
	ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
	| ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

void WidgetColorEdit(const char* label, ImVec4* col) {
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	const float btn = ImGui::GetFrameHeight();
	const float right = ImGui::GetWindowContentRegionMax().x;
	ImGui::SameLine(right - btn);
	char id[64];
	std::snprintf(id, sizeof(id), "##wcol_%s", label);
	ImGui::ColorEdit4(id, (float*)col, kWidgetColFlags);
}

// Right-click settings — transparent glossy glass matching widget panels
bool BeginWidgetPopup(const char* id, const char* title, ImU32 accent, float width = 196.f) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 10.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 7.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);

	const ImVec4 accentV = U32ToVec4(accent);
	// Match DrawPanel fill (14,14,18,210) — translucent glass, not solid
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(14.f / 255.f, 14.f / 255.f, 18.f / 255.f, 210.f / 255.f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.07f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.94f, 0.98f, 1.f));
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 1.f, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.f, 1.f, 1.f, 0.05f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.22f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.35f));
	ImGui::PushStyleColor(ImGuiCol_CheckMark, accentV);
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accentV.x, accentV.y, accentV.z, 0.28f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.40f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.50f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.05f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.28f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.40f));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, accentV);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(accentV.x, accentV.y, accentV.z, 1.f));

	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, 480.f));
	if (!ImGui::BeginPopup(id, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::PopStyleColor(16);
		ImGui::PopStyleVar(8);
		return false;
	}

	ImGui::GetStateStorage()->SetInt(ImGui::GetID("##wpop_accent"), (int)accent);

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(
		((accent >> IM_COL32_R_SHIFT) & 0xFF) / 255.f,
		((accent >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
		((accent >> IM_COL32_B_SHIFT) & 0xFF) / 255.f,
		0.95f));
	ImGui::TextUnformatted(title);
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0.f, 1.f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0.f, 2.f));
	return true;
}

void EndWidgetPopup() {
	const ImU32 accent = (ImU32)ImGui::GetStateStorage()->GetInt(ImGui::GetID("##wpop_accent"),
		(int)IM_COL32(130, 110, 255, 230));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetWindowPos();
	const ImVec2 sz = ImGui::GetWindowSize();
	const ImVec2 b(a.x + sz.x, a.y + sz.y);
	const float r = 7.f;
	// Glossy chrome to match widget DrawPanel (drawn over edges, under interactive widgets ok for border)
	dl->AddRect(a, b, IM_COL32(255, 255, 255, 18), r, 0, 1.f);
	dl->AddRectFilled(ImVec2(a.x, a.y + 3.f), ImVec2(a.x + 2.5f, b.y - 3.f), accent, 2.f);
	dl->AddRectFilledMultiColor(
		ImVec2(a.x + 1.f, a.y + 1.f),
		ImVec2(b.x - 1.f, a.y + 14.f),
		IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 18),
		IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));

	ImGui::EndPopup();
	ImGui::PopStyleColor(16);
	ImGui::PopStyleVar(8);
}

bool WidgetResetButton(bool withSeparator = true) {
	if (withSeparator) {
		ImGui::Dummy(ImVec2(0.f, 2.f));
		ImGui::Separator();
		ImGui::Dummy(ImVec2(0.f, 2.f));
	}
	const float w = ImGui::GetContentRegionAvail().x;
	return ImGui::Button("Reset position", ImVec2(w, 0.f));
}

void DrawProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, ImU32 fill, ImU32 bg) {
	t = std::clamp(t, 0.f, 1.f);
	const float r = 3.f;
	dl->AddRectFilled(a, b, bg, r);
	if (t > 0.01f) {
		const float w = (b.x - a.x) * t;
		dl->AddRectFilled(a, ImVec2(a.x + w, b.y), fill, r);
		if (w > 4.f)
			dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + w, a.y + 1.5f),
				IM_COL32(255, 255, 255, 40), 0.f);
	}
}

const char* ModeTag(int mode) {
	switch (mode) {
	case 0: return "A";
	case 1: return "H";
	case 2: return "T";
	default: return "?";
	}
}

float EstimateBombDamage(float distUnits, int armor) {
	constexpr float kDmg = 500.f;
	constexpr float kRadius = 1750.f;
	if (distUnits >= kRadius || distUnits < 0.f)
		return 0.f;
	float dmg = kDmg - distUnits * (kDmg / kRadius);
	if (dmg < 0.f)
		dmg = 0.f;
	if (armor > 0) {
		const float newDmg = dmg * 0.5f;
		float armorNeeded = (dmg - newDmg) * 0.5f;
		if (armorNeeded > static_cast<float>(armor)) {
			armorNeeded = static_cast<float>(armor) * 2.f;
			dmg = dmg - armorNeeded;
		} else {
			dmg = newDmg;
		}
		if (dmg < 0.f)
			dmg = 0.f;
	}
	return dmg;
}

ImVec2 ClampPanelPos(ImVec2 pos, float panelW, float panelH) {
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	pos.x = std::clamp(pos.x, 4.f, (std::max)(4.f, ds.x - panelW - 4.f));
	pos.y = std::clamp(pos.y, 4.f, (std::max)(4.f, ds.y - panelH - 4.f));
	return pos;
}

bool IsAutoPos(ImVec2 cfg) {
	// Sentinel from defaults / reset; both axes negative = auto place
	return cfg.x < 0.f && cfg.y < 0.f;
}

bool IsFinitePos(ImVec2 p) {
	return std::isfinite(p.x) && std::isfinite(p.y);
}

ImVec2 ResolvePos(ImVec2 cfg, float panelW, float panelH, ImVec2 fallback) {
	ImVec2 pos = cfg;
	if (!IsFinitePos(pos) || IsAutoPos(pos))
		pos = fallback;
	return ClampPanelPos(pos, panelW, panelH);
}

void HandleWidgetDrag(ImVec2& cfgPos, float panelW, float panelH, bool menuOpen) {
	if (!menuOpen)
		return;
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		cfgPos.x += ImGui::GetIO().MouseDelta.x;
		cfgPos.y += ImGui::GetIO().MouseDelta.y;
		cfgPos = ClampPanelPos(cfgPos, panelW, panelH);
		ImGui::SetWindowPos(cfgPos);
	} else {
		ImVec2 p = ImGui::GetWindowPos();
		if (!IsFinitePos(p))
			p = cfgPos;
		cfgPos = ClampPanelPos(p, panelW, panelH);
		if (cfgPos.x != p.x || cfgPos.y != p.y)
			ImGui::SetWindowPos(cfgPos);
	}
}

void ClipTextToWidth(char* buf, size_t bufSize, float maxW) {
	if (!buf || bufSize < 4 || maxW <= 0.f)
		return;
	if (ImGui::CalcTextSize(buf).x <= maxW)
		return;

	// Shrink until base text fits with room for "..."
	while (std::strlen(buf) > 0) {
		const size_t len = std::strlen(buf);
		char tmp[256];
		if (len + 3 >= sizeof(tmp) || len + 3 >= bufSize)
			break;
		std::memcpy(tmp, buf, len);
		tmp[len] = '.';
		tmp[len + 1] = '.';
		tmp[len + 2] = '.';
		tmp[len + 3] = '\0';
		if (ImGui::CalcTextSize(tmp).x <= maxW) {
			std::memcpy(buf, tmp, len + 4);
			return;
		}
		buf[len - 1] = '\0';
	}
	if (bufSize >= 4) {
		buf[0] = '.'; buf[1] = '.'; buf[2] = '.'; buf[3] = '\0';
	}
}

void RenderKeybindList(bool menuOpen) {
	if (!Config::widget_keybinds && !menuOpen)
		return;

	KeybindSnapshot snaps[8]{};
	const int nAll = keybind.listSnapshots(snaps, 8);
	if (nAll <= 0)
		return;

	bool anyActive = false;
	for (int i = 0; i < nAll; ++i) {
		if (snaps[i].active) {
			anyActive = true;
			break;
		}
	}

	// In-game hide rules (ignored while menu is open — always preview)
	if (!menuOpen) {
		if (Config::widget_keybinds_only_when_active && !anyActive)
			return;
	}

	// Build visible rows
	KeybindSnapshot rows[8]{};
	int n = 0;
	const bool showAll = menuOpen || Config::widget_keybinds_show_all;
	for (int i = 0; i < nAll; ++i) {
		if (!showAll && !snaps[i].active)
			continue;
		rows[n++] = snaps[i];
	}
	if (n <= 0) {
		// Menu open + show-all off + nothing active: still show empty shell for drag/settings
		if (!menuOpen)
			return;
	}

	const float rowH = 20.f;
	const float padX = 10.f;
	const float padY = 8.f;
	const float headerH = 18.f;
	const float panelW = 178.f;
	const int drawRows = (n > 0) ? n : 1;
	const float panelH = padY + headerH + 4.f + drawRows * rowH + padY;

	ImVec2 pos = ResolvePos(Config::widget_keybinds_pos, panelW, panelH, ImVec2(14.f, 58.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_keybinds", nullptr, flags);
	{
		ImGui::InvisibleButton("##kb_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_keybinds_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);
		const ImU32 accent = ColU32(Config::widget_keybinds_accent);
		DrawPanel(dl, a, b, accent);

		dl->AddText(ImVec2(a.x + padX + 2.f, a.y + padY - 1.f),
			ColU32A(Config::widget_keybinds_accent, 0.75f), "KEYBINDS");

		float y = a.y + padY + headerH;
		if (n <= 0) {
			dl->AddText(ImVec2(a.x + padX, y + 2.f),
				IM_COL32(140, 140, 155, 200), "No active binds");
		} else {
			for (int i = 0; i < n; ++i) {
				const auto& s = rows[i];
				const bool on = s.active;
				const ImU32 nameCol = on ? IM_COL32(245, 245, 250, 255) : IM_COL32(150, 150, 160, 200);
				const ImU32 keyBg = on ? ColU32A(Config::widget_keybinds_accent, 0.28f) : IM_COL32(255, 255, 255, 12);
				const ImU32 keyCol = on ? ColU32A(Config::widget_keybinds_accent, 1.1f) : IM_COL32(170, 170, 180, 210);
				const ImU32 dot = on ? IM_COL32(90, 220, 130, 255) : IM_COL32(70, 70, 80, 200);

				char keyBuf[48];
				char keyName[24];
				Keybinds::formatKeyName(s.key, keyName, sizeof(keyName));
				const char* mt = ModeTag(s.mode);
				if (s.mode == 0)
					std::snprintf(keyBuf, sizeof(keyBuf), "Always");
				else
					std::snprintf(keyBuf, sizeof(keyBuf), "%s · %s", keyName, mt);

				const ImVec2 ksz = ImGui::CalcTextSize(keyBuf);
				const float kx = b.x - padX - ksz.x;
				const float nameX = a.x + padX + 12.f;
				const float maxNameW = (std::max)(20.f, kx - 8.f - nameX);

				char nameBuf[64];
				std::snprintf(nameBuf, sizeof(nameBuf), "%s", s.name);
				ClipTextToWidth(nameBuf, sizeof(nameBuf), maxNameW);

				dl->AddCircleFilled(ImVec2(a.x + padX + 4.f, y + rowH * 0.5f), 3.f, dot, 10);
				dl->AddText(ImVec2(nameX, y + 2.f), nameCol, nameBuf);
				dl->AddRectFilled(ImVec2(kx - 5.f, y + 1.f), ImVec2(kx + ksz.x + 5.f, y + rowH - 3.f),
					keyBg, 4.f);
				dl->AddText(ImVec2(kx, y + 2.f), keyCol, keyBuf);
				y += rowH;
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##kb_widget_settings");
		if (BeginWidgetPopup("##kb_widget_settings", "KEYBINDS", accent)) {
			ImGui::Checkbox("Only when active", &Config::widget_keybinds_only_when_active);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Hide in-game until a bind is held/toggled.\n"
					"Menu always shows the widget.");
			ImGui::Checkbox("Show all binds", &Config::widget_keybinds_show_all);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"ON = every bind.\n"
					"OFF = active binds only.");
			WidgetColorEdit("Accent", &Config::widget_keybinds_accent);
			if (WidgetResetButton())
				Config::widget_keybinds_pos = ImVec2(-1.f, -1.f);
			EndWidgetPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

void RenderBombWidget(bool menuOpen) {
	if (!Config::widget_bomb && !menuOpen)
		return;

	const PlantedBombInfo& bomb = g_plantedBomb;
	const bool live = bomb.active;
	if (!live && !menuOpen)
		return;

	const float padX = 12.f;
	const float padY = 10.f;
	const float panelW = 200.f;
	float panelH = padY + 18.f + 8.f + 16.f + 22.f + padY;
	if (live && Config::widget_bomb_show_defuse && bomb.defusing && bomb.defuseLeft >= 0.f)
		panelH += 28.f;
	if (live && Config::widget_bomb_show_damage)
		panelH += 20.f;
	else if (!live)
		panelH += 8.f;
	if (menuOpen)
		panelH += 14.f;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImVec2 pos = ResolvePos(Config::widget_bomb_pos, panelW, panelH,
		ImVec2(ds.x * 0.5f - panelW * 0.5f, ds.y - panelH - 48.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_bomb", nullptr, flags);
	{
		ImGui::InvisibleButton("##bomb_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_bomb_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);

		const float blow = live ? bomb.blowLeft : -1.f;
		const bool urgent = blow >= 0.f && blow <= 10.f;
		const ImU32 accent = live
			? (urgent ? ColU32(Config::widget_bomb_urgent) : ColU32(Config::widget_bomb_accent))
			: ColU32A(Config::widget_bomb_accent, 0.55f);
		DrawPanel(dl, a, b, accent);

		char title[48];
		if (live) {
			const char siteCh = (bomb.site == 0) ? 'A' : (bomb.site == 1) ? 'B' : '?';
			std::snprintf(title, sizeof(title), "BOMB  ·  SITE %c", siteCh);
		} else {
			std::snprintf(title, sizeof(title), "BOMB");
		}
		dl->AddText(ImVec2(a.x + padX + 2.f, a.y + padY - 1.f),
			ColU32A(urgent ? Config::widget_bomb_urgent : Config::widget_bomb_accent, 1.05f), title);

		if (menuOpen) {
			const char* hint = "drag";
			const ImVec2 hs = ImGui::CalcTextSize(hint);
			dl->AddText(ImVec2(b.x - padX - hs.x, a.y + padY),
				IM_COL32(120, 120, 140, 180), hint);
		}

		auto bombSettingsPopup = [&]() {
			if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				ImGui::OpenPopup("##bomb_widget_settings");
			if (BeginWidgetPopup("##bomb_widget_settings", "BOMB", accent)) {
				WidgetColorEdit("Accent", &Config::widget_bomb_accent);
				WidgetColorEdit("Urgent", &Config::widget_bomb_urgent);
				ImGui::Checkbox("Show damage", &Config::widget_bomb_show_damage);
				ImGui::Checkbox("Show defuse", &Config::widget_bomb_show_defuse);
				if (WidgetResetButton())
					Config::widget_bomb_pos = ImVec2(-1.f, -1.f);
				EndWidgetPopup();
			}
		};

		float y = a.y + padY + 20.f;

		if (!live) {
			dl->AddText(ImVec2(a.x + padX, y),
				IM_COL32(160, 155, 150, 200), "No bomb planted");
			bombSettingsPopup();
			ImGui::End();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(2);
			return;
		}

		const float fuseFull = 40.f;
		const float fuseT = (blow >= 0.f) ? std::clamp(blow / fuseFull, 0.f, 1.f) : 0.f;
		const ImU32 fill = urgent
			? ColU32(Config::widget_bomb_urgent)
			: ColU32(Config::widget_bomb_accent);
		DrawProgressBar(dl, ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 7.f),
			fuseT, fill, IM_COL32(255, 255, 255, 18));
		y += 14.f;

		char blowLine[48];
		if (blow >= 0.f)
			std::snprintf(blowLine, sizeof(blowLine), "Explodes  %.1fs", blow);
		else
			std::snprintf(blowLine, sizeof(blowLine), "Explodes  —");
		dl->AddText(ImVec2(a.x + padX, y), IM_COL32(240, 240, 245, 255), blowLine);
		y += 20.f;

		if (Config::widget_bomb_show_defuse && bomb.defusing && bomb.defuseLeft >= 0.f) {
			const float defT = std::clamp(bomb.defuseLeft / 10.f, 0.f, 1.f);
			const bool canMake = blow < 0.f || bomb.defuseLeft <= blow + 0.05f;
			const ImU32 defCol = canMake ? IM_COL32(80, 200, 120, 230) : ColU32(Config::widget_bomb_urgent);
			DrawProgressBar(dl, ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 6.f),
				defT, defCol, IM_COL32(255, 255, 255, 18));
			y += 12.f;
			char defLine[48];
			std::snprintf(defLine, sizeof(defLine), "Defuse    %.1fs%s",
				bomb.defuseLeft, canMake ? "" : "  !");
			dl->AddText(ImVec2(a.x + padX, y), defCol, defLine);
			y += 18.f;
		}

		if (Config::widget_bomb_show_damage) {
			float dmg = 0.f;
			int hp = 100;
			bool haveDmg = false;
			if (cached_local.active) {
				const float dx = bomb.position.x - cached_local.position.x;
				const float dy = bomb.position.y - cached_local.position.y;
				const float dz = bomb.position.z - cached_local.position.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				hp = cached_local.alive && cached_local.health > 0 ? cached_local.health : 100;
				const int armor = cached_local.alive ? cached_local.armor : 0;
				dmg = EstimateBombDamage(dist, armor);
				haveDmg = cached_local.alive;
			}

			char dmgLine[48];
			const bool lethal = haveDmg && dmg + 0.5f >= static_cast<float>(hp);
			if (!haveDmg)
				std::snprintf(dmgLine, sizeof(dmgLine), "Damage    —");
			else if (dmg < 1.f)
				std::snprintf(dmgLine, sizeof(dmgLine), "Damage    safe");
			else if (lethal)
				std::snprintf(dmgLine, sizeof(dmgLine), "Damage    ~%.0f  LETHAL", dmg);
			else
				std::snprintf(dmgLine, sizeof(dmgLine), "Damage    ~%.0f HP", dmg);

			dl->AddText(ImVec2(a.x + padX, y),
				lethal ? ColU32(Config::widget_bomb_urgent) : IM_COL32(210, 210, 220, 255), dmgLine);
		}

		bombSettingsPopup();
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

struct SpecEntry {
	char name[64]{};
	std::uint64_t steamId = 0;
};

// CS2 ObserverMode_t — only these mean actively watching a player.
constexpr std::uint8_t kObsInEye = 2;
constexpr std::uint8_t kObsChase = 3;

void AddLocalTarget(int* targets, int& nTargets, int maxTargets, const CBaseHandle& h) {
	if (!h.valid() || nTargets >= maxTargets)
		return;
	const int idx = h.index();
	for (int i = 0; i < nTargets; ++i) {
		if (targets[i] == idx)
			return;
	}
	targets[nTargets++] = idx;
}

CPlayer_ObserverServices* ReadObserverServices(C_CSPlayerPawn* pawn) {
	if (!pawn || !Mem::ValidEntity(pawn))
		return nullptr;
	CPlayer_ObserverServices* obs = nullptr;
	__try { obs = pawn->m_pObserverServices(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { obs = nullptr; }
	if (!obs || !Mem::Valid(obs, 0x50))
		return nullptr;
	return obs;
}

bool IsActivelyWatchingLocal(CPlayer_ObserverServices* obs, const int* localTargets, int nTargets) {
	if (!obs || nTargets <= 0)
		return false;
	std::uint8_t mode = 0;
	CBaseHandle target{};
	__try {
		mode = obs->m_iObserverMode();
		target = obs->m_hObserverTarget();
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

	// Stale OBS_MODE_NONE / deathcam / roaming must not keep old targets after round restart.
	if (mode != kObsInEye && mode != kObsChase)
		return false;
	if (!target.valid())
		return false;

	const int tIdx = target.index();
	for (int t = 0; t < nTargets; ++t) {
		if (localTargets[t] == tIdx)
			return true;
	}
	return false;
}

bool ControllerPawnAlive(CCSPlayerController* ctrl) {
	if (!ctrl)
		return false;
	bool alive = false;
	__try { alive = ctrl->m_bPawnIsAlive(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return alive;
}

int CollectSpectators(SpecEntry* out, int maxOut, int* totalOut) {
	if (totalOut)
		*totalOut = 0;
	if (!out || maxOut <= 0)
		return 0;
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return 0;
	if (!H::oGetLocalPlayer)
		return 0;

	C_CSPlayerPawn* localPawn = nullptr;
	__try { localPawn = H::oGetLocalPlayer(0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	if (!localPawn || !Mem::ValidEntity(localPawn))
		return 0;

	int localTargets[4]{};
	int nTargets = 0;
	AddLocalTarget(localTargets, nTargets, 4, localPawn->handle());

	bool localAlive = false;
	__try {
		localAlive = localPawn->m_iHealth() > 0 && localPawn->m_lifeState() == 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { localAlive = false; }

	const CBaseHandle hCtrl = localPawn->m_hController();
	if (hCtrl.valid()) {
		auto* ctrl = I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl);
		if (ctrl && Mem::ValidEntity(ctrl)) {
			if (localAlive) {
				AddLocalTarget(localTargets, nTargets, 4, ctrl->m_hPlayerPawn());
				AddLocalTarget(localTargets, nTargets, 4, ctrl->m_hPawn());
			} else {
				AddLocalTarget(localTargets, nTargets, 4, ctrl->m_hObserverPawn());
				AddLocalTarget(localTargets, nTargets, 4, ctrl->m_hPawn());
			}
		}
	}
	if (nTargets <= 0)
		return 0;

	const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	int count = 0;
	int total = 0;
	for (int i = 1; i <= nMax; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity))
			continue;

		SchemaClassInfoData_t* cls = nullptr;
		__try { Entity->dump_class_info(&cls); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
			continue;
		if (HASH(cls->szName) != HASH("CCSPlayerController"))
			continue;

		auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
		bool isLocal = false;
		__try { isLocal = Controller->IsLocalPlayer(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (isLocal)
			continue;

		if (ControllerPawnAlive(Controller))
			continue;

		CBaseHandle hObs = Controller->m_hObserverPawn();
		if (!hObs.valid())
			hObs = Controller->m_hPawn();
		if (!hObs.valid())
			continue;

		auto* obsPawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hObs);
		CPlayer_ObserverServices* obs = ReadObserverServices(obsPawn);
		if (!IsActivelyWatchingLocal(obs, localTargets, nTargets))
			continue;

		++total;
		if (count >= maxOut)
			continue;

		SpecEntry& e = out[count];
		e.steamId = 0;
		e.name[0] = '\0';
		__try { e.steamId = Controller->m_steamID(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { e.steamId = 0; }
		if (!Controller->ReadSanitizedName(e.name, sizeof(e.name)) || !e.name[0])
			std::snprintf(e.name, sizeof(e.name), "Player");
		++count;
	}
	if (totalOut)
		*totalOut = total;
	return count;
}

void DrawLetterAvatar(ImDrawList* dl, ImVec2 center, float radius, const char* name) {
	dl->AddCircleFilled(center, radius, IM_COL32(40, 42, 50, 180), 20);
	char letter[2] = { '?', '\0' };
	if (name && name[0]) {
		char c = name[0];
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
		letter[0] = c;
	}
	const ImVec2 ts = ImGui::CalcTextSize(letter);
	dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
		IM_COL32(180, 185, 195, 200), letter);
}

void RenderSpectatorList(bool menuOpen) {
	if (!Config::widget_spectators && !menuOpen)
		return;

	SpecEntry specs[16]{};
	int total = 0;
	int maxShow = Config::widget_spectators_max;
	if (maxShow < 1) maxShow = 1;
	if (maxShow > 16) maxShow = 16;
	const int n = CollectSpectators(specs, maxShow, &total);
	if (n <= 0 && !menuOpen)
		return;

	const float rowH = 22.f;
	const float padX = 8.f;
	const float padY = 6.f;
	const float headerH = 14.f;
	const float avatarR = 7.f;
	const bool showAvatars = Config::widget_spectators_show_avatars;
	const float panelW = 168.f;
	const int drawRows = (n > 0) ? n : 1;
	const float panelH = padY + headerH + 4.f + drawRows * rowH + padY;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImVec2 pos = ResolvePos(Config::widget_spectators_pos, panelW, panelH,
		ImVec2(ds.x - panelW - 12.f, 48.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav
		| ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (!menuOpen)
		flags |= ImGuiWindowFlags_NoInputs;

	ImGui::Begin("##widget_spectators", nullptr, flags);
	{
		ImGui::InvisibleButton("##spec_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_spectators_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);
		const ImU32 accent = ColU32(Config::widget_spectators_accent);
		const float r = 6.f;
		dl->AddRectFilled(a, b, IM_COL32(10, 10, 14, 130), r);
		dl->AddRect(a, b, IM_COL32(255, 255, 255, 12), r, 0, 1.f);
		dl->AddRectFilled(ImVec2(a.x, a.y + 2.f), ImVec2(a.x + 2.f, b.y - 2.f), accent, 2.f);

		char header[28];
		if (total > n)
			std::snprintf(header, sizeof(header), "spectators  %d+", n);
		else if (n > 0)
			std::snprintf(header, sizeof(header), "spectators  %d", n);
		else
			std::snprintf(header, sizeof(header), "spectators");
		dl->AddText(ImVec2(a.x + padX + 2.f, a.y + padY - 1.f),
			ColU32A(Config::widget_spectators_accent, 0.7f), header);

		float y = a.y + padY + headerH;
		if (n <= 0) {
			dl->AddText(ImVec2(a.x + padX, y + 2.f),
				IM_COL32(120, 125, 135, 130), "none");
		} else {
			for (int i = 0; i < n; ++i) {
				const SpecEntry& s = specs[i];
				const float cy = y + rowH * 0.5f;
				float textX = a.x + padX;

				if (showAvatars) {
					const ImVec2 av(a.x + padX + avatarR, cy);
					const ImTextureID tex = SteamAvatar::Get(s.steamId, pDevice);
					if (tex != ImTextureID_Invalid) {
						dl->AddImageRounded(ImTextureRef(tex),
							ImVec2(av.x - avatarR, av.y - avatarR),
							ImVec2(av.x + avatarR, av.y + avatarR),
							ImVec2(0, 0), ImVec2(1, 1),
							IM_COL32(255, 255, 255, 200), avatarR);
					} else {
						DrawLetterAvatar(dl, av, avatarR, s.name);
					}
					textX = a.x + padX + avatarR * 2.f + 7.f;
				}

				const float maxNameW = b.x - padX - textX;
				char clipped[64];
				std::snprintf(clipped, sizeof(clipped), "%s", s.name);
				ClipTextToWidth(clipped, sizeof(clipped), maxNameW);
				dl->AddText(ImVec2(textX, cy - ImGui::GetFontSize() * 0.5f),
					IM_COL32(200, 205, 215, 190), clipped);
				y += rowH;
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##spec_widget_settings");
		if (BeginWidgetPopup("##spec_widget_settings", "SPECTATORS", accent)) {
			WidgetColorEdit("Accent", &Config::widget_spectators_accent);
			ImGui::Checkbox("Show avatars", &Config::widget_spectators_show_avatars);
			ImGui::TextUnformatted("Max shown");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::SliderInt("##spec_max", &Config::widget_spectators_max, 1, 16);
			if (WidgetResetButton())
				Config::widget_spectators_pos = ImVec2(-1.f, -1.f);
			EndWidgetPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

} // namespace

void Render() {
	const bool menuOpen = g_bMenuOpen;
	RenderKeybindList(menuOpen);
	RenderBombWidget(menuOpen);
	RenderSpectatorList(menuOpen);
}

} // namespace Widgets
