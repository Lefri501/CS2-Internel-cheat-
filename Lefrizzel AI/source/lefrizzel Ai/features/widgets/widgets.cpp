#include "widgets.h"
#include "steam_avatar.h"

#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/console/console.h"
#include "../visuals/visuals.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../utils/math/vector/vector.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../../external/imgui/imgui.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <Windows.h>
#include <d3d11.h>

extern ID3D11Device* pDevice;

namespace Widgets {
namespace {

float PanelRounding() {
	return std::clamp(Config::menu_rounding, 2.f, 5.f);
}

ImU32 ColU32(const ImVec4& c) {
	return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 ColU32A(const ImVec4& c, float aMul) {
	ImVec4 t = c;
	t.w = std::clamp(c.w * aMul, 0.f, 1.f);
	return ImGui::ColorConvertFloat4ToU32(t);
}

ImVec4 U32ToVec4(ImU32 c) {
	return ImVec4(
		((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.f,
		((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f);
}

// Flat panel: fill + border. No accent rail.
void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 /*accent*/) {
	const float h = b.y - a.y;
	const float r = (std::min)(PanelRounding(), h * 0.5f);

	dl->AddRectFilled(ImVec2(a.x + 1.f, a.y + 2.f), ImVec2(b.x + 1.f, b.y + 2.f),
		IM_COL32(0, 0, 0, 70), r);
	dl->AddRectFilled(a, b, IM_COL32(18, 19, 22, 242), r);
	dl->AddRect(a, b, IM_COL32(255, 255, 255, 16), r, 0, 1.f);
}

void DrawHeaderSep(ImDrawList* dl, ImVec2 a, ImVec2 b, float padX, float y) {
	dl->AddLine(
		ImVec2(a.x + padX, y),
		ImVec2(b.x - padX, y),
		IM_COL32(255, 255, 255, 12), 1.f);
}

constexpr ImGuiColorEditFlags kWidgetColFlags =
	ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
	| ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview;

void WidgetColorEdit(const char* label, ImVec4* col) {
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	// ImGui 1.92: avoid obsolete GetWindowContentRegionMax
	const float btn = ImGui::GetFrameHeight();
	ImGui::SameLine();
	const float remain = ImGui::GetContentRegionAvail().x;
	if (remain > btn)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remain - btn);
	char id[64];
	std::snprintf(id, sizeof(id), "##wcol_%s", label);
	ImGui::ColorEdit4(id, (float*)col, kWidgetColFlags);
}

bool BeginWidgetPopup(const char* id, const char* title, ImU32 accent, float width = 196.f) {
	const float r = PanelRounding();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, r);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 5.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, r);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);

	const ImVec4 accentV = U32ToVec4(accent);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.082f, 0.09f, 0.98f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.07f));
	ImGui::PushStyleColor(ImGuiCol_Text, Config::menu_text);
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 1.f, 0.07f));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.28f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.18f));
	ImGui::PushStyleColor(ImGuiCol_CheckMark, accentV);
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accentV.x, accentV.y, accentV.z, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentV.x, accentV.y, accentV.z, 0.18f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentV.x, accentV.y, accentV.z, 0.24f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 0.04f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.08f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, accentV);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(accentV.x, accentV.y, accentV.z, 1.f));

	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, 480.f));
	if (!ImGui::BeginPopup(id, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::PopStyleColor(16);
		ImGui::PopStyleVar(8);
		return false;
	}

	ImGui::TextUnformatted(title);
	ImGui::Dummy(ImVec2(0.f, 1.f));
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0.f, 3.f));
	return true;
}

void EndWidgetPopup() {
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
	const float r = 1.5f;
	dl->AddRectFilled(a, b, bg, r);
	if (t > 0.01f) {
		const float w = (b.x - a.x) * t;
		dl->AddRectFilled(a, ImVec2(a.x + w, b.y), fill, r);
	}
}

const char* ModeTagShort(int mode) {
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
	if (!Config::widget_keybinds)
		return;

	// 16: aim/af/tr/aw/tp/nade/knife/jb/eb + room (was 8 → edgebug dropped)
	constexpr int kMaxBinds = 16;
	KeybindSnapshot snaps[kMaxBinds]{};
	const int nAll = keybind.listSnapshots(snaps, kMaxBinds);
	if (nAll <= 0)
		return;

	// Only features that are actually enabled in config
	KeybindSnapshot enabled[kMaxBinds]{};
	int nEnabled = 0;
	for (int i = 0; i < nAll; ++i) {
		if (!snaps[i].enabled)
			continue;
		enabled[nEnabled++] = snaps[i];
	}
	if (nEnabled <= 0)
		return;

	bool anyActive = false;
	for (int i = 0; i < nEnabled; ++i) {
		if (enabled[i].active) {
			anyActive = true;
			break;
		}
	}

	if (!menuOpen) {
		if (Config::widget_keybinds_only_when_active && !anyActive)
			return;
	}

	KeybindSnapshot rows[kMaxBinds]{};
	int n = 0;
	// Menu + in-game: respect "show all" only among enabled binds
	const bool showAll = Config::widget_keybinds_show_all || menuOpen;
	for (int i = 0; i < nEnabled; ++i) {
		if (!showAll && !enabled[i].active)
			continue;
		rows[n++] = enabled[i];
	}
	if (n <= 0)
		return;

	const float padX = 10.f;
	const float padY = 8.f;
	const float headerH = 16.f;
	const float rowH = 20.f;
	const float rowGap = 1.f;
	const float panelW = 180.f;
	const int drawRows = (n > 0) ? n : 1;
	const float bodyH = (float)drawRows * rowH + (float)(drawRows - 1) * rowGap;
	const float panelH = padY + headerH + 5.f + bodyH + padY;

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

		const float titleX = a.x + padX;
		const float titleY = a.y + padY;
		dl->AddText(ImVec2(titleX, titleY), ColU32A(Config::menu_text_muted, 0.90f), "Keybinds");

		if (anyActive) {
			int activeN = 0;
			for (int i = 0; i < n; ++i)
				if (rows[i].active) ++activeN;
			char badge[16];
			std::snprintf(badge, sizeof(badge), "%d", activeN);
			const ImVec2 bsz = ImGui::CalcTextSize(badge);
			dl->AddText(ImVec2(b.x - padX - bsz.x, titleY),
				ColU32(Config::menu_text), badge);
		}

		DrawHeaderSep(dl, a, b, padX, a.y + padY + headerH);

		float y = a.y + padY + headerH + 3.f;
		if (n <= 0) {
			dl->AddText(ImVec2(a.x + padX, y + 2.f),
				ColU32A(Config::menu_text_muted, 0.70f), "No active binds");
		} else {
			for (int i = 0; i < n; ++i) {
				const auto& s = rows[i];
				const bool on = s.active;

				const ImVec2 rowA(a.x + padX, y);
				const ImVec2 rowB(b.x - padX, y + rowH);

				const float nameX = rowA.x;
				const ImU32 nameCol = on
					? ColU32(Config::menu_text)
					: ColU32A(Config::menu_text_muted, 0.85f);

				char keyName[24];
				Keybinds::formatKeyName(s.key, keyName, sizeof(keyName));
				char keyBuf[32];
				if (s.mode == 0)
					std::snprintf(keyBuf, sizeof(keyBuf), "Always");
				else
					std::snprintf(keyBuf, sizeof(keyBuf), "%s", keyName);

				const ImVec2 ksz = ImGui::CalcTextSize(keyBuf);
				const float keyX = rowB.x - ksz.x;
				const float keyY = y + (rowH - ksz.y) * 0.5f;

				const char* mt = ModeTagShort(s.mode);
				const ImVec2 msz = ImGui::CalcTextSize(mt);
				const float modeX = keyX - 6.f - msz.x;
				const float modeY = y + (rowH - msz.y) * 0.5f;

				const float maxNameW = (std::max)(24.f, modeX - 6.f - nameX);
				char nameBuf[64];
				std::snprintf(nameBuf, sizeof(nameBuf), "%s", s.name ? s.name : "");
				ClipTextToWidth(nameBuf, sizeof(nameBuf), maxNameW);

				const ImVec2 nsz = ImGui::CalcTextSize(nameBuf);
				dl->AddText(ImVec2(nameX, y + (rowH - nsz.y) * 0.5f), nameCol, nameBuf);

				const ImU32 modeCol = on
					? ColU32A(Config::widget_keybinds_accent, 0.85f)
					: ColU32A(Config::menu_text_muted, 0.65f);
				dl->AddText(ImVec2(modeX, modeY), modeCol, mt);

				const ImU32 keyCol = on
					? ColU32(Config::menu_text)
					: ColU32A(Config::menu_text_muted, 0.90f);
				dl->AddText(ImVec2(keyX, keyY), keyCol, keyBuf);

				y += rowH + rowGap;
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##kb_widget_settings");
		if (BeginWidgetPopup("##kb_widget_settings", "Keybinds", accent)) {
			ImGui::Checkbox("Only when active", &Config::widget_keybinds_only_when_active);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Hide list until a keybind is active.");
			ImGui::Checkbox("Show all binds", &Config::widget_keybinds_show_all);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Show every bind, not only active ones.");
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
	if (!Config::widget_bomb)
		return;

	const PlantedBombInfo& bomb = g_plantedBomb;
	const bool live = bomb.active;
	if (!live && !menuOpen)
		return;

	const float padX = 10.f;
	const float padY = 8.f;
	const float panelW = 184.f;
	float panelH = padY + 14.f + 5.f + 14.f + 16.f + padY;
	if (live && Config::widget_bomb_show_defuse && !bomb.defused && bomb.defusing && bomb.defuseLeft >= 0.f)
		panelH += 22.f;
	if (live && Config::widget_bomb_show_damage && !bomb.defused)
		panelH += 14.f;
	else if (!live)
		panelH += 4.f;
	if (menuOpen)
		panelH += 8.f;

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
		const bool defused = live && bomb.defused;
		const bool urgent = !defused && blow >= 0.f && blow <= 10.f;
		const ImU32 accent = live
			? (defused ? IM_COL32(80, 200, 120, 230)
				: (urgent ? ColU32(Config::widget_bomb_urgent) : ColU32(Config::widget_bomb_accent)))
			: ColU32A(Config::widget_bomb_accent, 0.55f);
		DrawPanel(dl, a, b, accent);

		char title[48];
		if (live) {
			const char siteCh = (bomb.site == 0) ? 'A' : (bomb.site == 1) ? 'B' : '?';
			std::snprintf(title, sizeof(title), "Bomb  %c", siteCh);
		} else {
			std::snprintf(title, sizeof(title), "Bomb");
		}
		dl->AddText(ImVec2(a.x + padX, a.y + padY),
			live
				? ColU32A(defused ? ImVec4(0.40f, 0.75f, 0.50f, 1.f)
					: (urgent ? Config::widget_bomb_urgent : Config::menu_text), 1.f)
				: ColU32A(Config::menu_text_muted, 0.90f), title);

		auto bombSettingsPopup = [&]() {
			if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				ImGui::OpenPopup("##bomb_widget_settings");
			if (BeginWidgetPopup("##bomb_widget_settings", "Bomb", accent)) {
				WidgetColorEdit("Accent", &Config::widget_bomb_accent);
				WidgetColorEdit("Urgent", &Config::widget_bomb_urgent);
				ImGui::Checkbox("Show damage", &Config::widget_bomb_show_damage);
				ImGui::Checkbox("Show defuse", &Config::widget_bomb_show_defuse);
				if (WidgetResetButton())
					Config::widget_bomb_pos = ImVec2(-1.f, -1.f);
				EndWidgetPopup();
			}
		};

		float y = a.y + padY + 16.f;
		const float textX = a.x + padX;

		if (!live) {
			DrawHeaderSep(dl, a, b, padX, a.y + padY + 14.f);
			dl->AddText(ImVec2(textX, y + 2.f),
				ColU32A(Config::menu_text_muted, 0.70f), "Not planted");
			bombSettingsPopup();
			ImGui::End();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(2);
			return;
		}

		DrawHeaderSep(dl, a, b, padX, a.y + padY + 14.f);

		const float fuseFull = 40.f;
		const float fuseT = (blow >= 0.f) ? std::clamp(blow / fuseFull, 0.f, 1.f) : 0.f;
		const ImU32 fill = defused
			? IM_COL32(80, 200, 120, 230)
			: (urgent ? ColU32(Config::widget_bomb_urgent) : ColU32(Config::widget_bomb_accent));
		DrawProgressBar(dl, ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 3.f),
			fuseT, fill, IM_COL32(255, 255, 255, 12));
		y += 10.f;

		char blowLine[48];
		if (defused) {
			if (blow >= 0.f)
				std::snprintf(blowLine, sizeof(blowLine), "Defused  %.1fs", blow);
			else
				std::snprintf(blowLine, sizeof(blowLine), "Defused");
		} else if (blow >= 0.f) {
			std::snprintf(blowLine, sizeof(blowLine), "%.1fs", blow);
		} else {
			std::snprintf(blowLine, sizeof(blowLine), "—");
		}
		dl->AddText(ImVec2(textX, y),
			defused ? IM_COL32(80, 200, 120, 255) : IM_COL32(220, 222, 228, 255), blowLine);
		y += 14.f;

		if (Config::widget_bomb_show_defuse && !defused && bomb.defusing && bomb.defuseLeft >= 0.f) {
			const float defT = std::clamp(bomb.defuseLeft / 10.f, 0.f, 1.f);
			const bool canMake = blow < 0.f || bomb.defuseLeft <= blow + 0.05f;
			const ImU32 defCol = canMake ? IM_COL32(80, 200, 120, 230) : ColU32(Config::widget_bomb_urgent);
			DrawProgressBar(dl, ImVec2(a.x + padX, y), ImVec2(b.x - padX, y + 2.f),
				defT, defCol, IM_COL32(255, 255, 255, 12));
			y += 8.f;
			char defLine[48];
			std::snprintf(defLine, sizeof(defLine), "Defuse %.1fs%s",
				bomb.defuseLeft, canMake ? "" : " !");
			dl->AddText(ImVec2(textX, y), defCol, defLine);
			y += 14.f;
		}

		if (Config::widget_bomb_show_damage && !defused) {
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
				std::snprintf(dmgLine, sizeof(dmgLine), "Dmg  —");
			else if (dmg < 1.f)
				std::snprintf(dmgLine, sizeof(dmgLine), "Dmg  safe");
			else if (lethal)
				std::snprintf(dmgLine, sizeof(dmgLine), "Dmg  lethal");
			else
				std::snprintf(dmgLine, sizeof(dmgLine), "Dmg  ~%.0f", dmg);

			dl->AddText(ImVec2(textX, y),
				lethal ? ColU32(Config::widget_bomb_urgent) : IM_COL32(170, 172, 180, 255), dmgLine);
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
	C_CSPlayerPawn* localPawn = H::SafeLocalPlayer();
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

	const int nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
	// Controllers live in low slots — no need to walk full entity list
	const int nMax = (nMaxRaw > 128) ? 128 : nMaxRaw;
	int count = 0;
	int total = 0;
	for (int i = 1; i <= nMax; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity))
			continue;

		// Designer first — skip dump_class on props/weapons
		bool isCtrl = false;
		__try {
			CEntityIdentity* id = nullptr;
			if (!Mem::ReadField(Entity, 0x10, id) || !id || !Mem::Valid(id, 0x28))
				id = Entity->m_pEntityIdentity();
			if (id && Mem::Valid(id, 0x28)) {
				const char* designer = nullptr;
				if (!Mem::ReadField(id, 0x20, designer) || !designer)
					designer = id->m_designerName();
				if (designer && Mem::IsReadable(designer, 2) && designer[0]) {
					if (std::strcmp(designer, "cs_player_controller") == 0
						|| std::strstr(designer, "player_controller") != nullptr)
						isCtrl = true;
					else
						continue;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!isCtrl) {
			SchemaClassInfoData_t* cls = nullptr;
			__try { Entity->dump_class_info(&cls); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
				continue;
			if (HASH(cls->szName) != HASH("CCSPlayerController"))
				continue;
		}

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

void DrawLetterAvatar(ImDrawList* dl, ImVec2 center, float radius, const char* name, ImU32 /*accent*/) {
	dl->AddCircleFilled(center, radius, IM_COL32(36, 38, 44, 240), 12);
	char letter[2] = { '?', '\0' };
	if (name && name[0]) {
		char c = name[0];
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
		letter[0] = c;
	}
	const ImVec2 ts = ImGui::CalcTextSize(letter);
	dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
		ColU32A(Config::menu_text_muted, 0.95f), letter);
}

void RenderSpectatorList(bool menuOpen) {
	if (!Config::widget_spectators)
		return;

	static SpecEntry s_specs[16]{};
	static int s_n = 0;
	static int s_total = 0;
	static float s_nextRefresh = 0.f;

	int maxShow = Config::widget_spectators_max;
	if (maxShow < 1) maxShow = 1;
	if (maxShow > 16) maxShow = 16;

	const float now = static_cast<float>(ImGui::GetTime());
	// Throttle entity walk (~8 Hz); refresh immediately when menu opens for drag UX
	if (menuOpen || now >= s_nextRefresh) {
		s_nextRefresh = now + 0.12f;
		s_n = CollectSpectators(s_specs, maxShow, &s_total);
	}
	const int n = s_n;
	const int total = s_total;
	if (n <= 0 && !menuOpen)
		return;

	const float padX = 10.f;
	const float padY = 8.f;
	const float headerH = 16.f;
	const float rowH = 22.f;
	const float rowGap = 1.f;
	const float avatarR = 7.f;
	const bool showAvatars = Config::widget_spectators_show_avatars;
	const float panelW = 180.f;
	const int drawRows = (n > 0) ? n : 1;
	const float bodyH = (float)drawRows * rowH + (float)(drawRows - 1) * rowGap;
	const float panelH = padY + headerH + 5.f + bodyH + padY;

	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImVec2 pos = ResolvePos(Config::widget_spectators_pos, panelW, panelH,
		ImVec2(ds.x - panelW - 14.f, 58.f));

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
		DrawPanel(dl, a, b, accent);

		const float titleX = a.x + padX;
		const float titleY = a.y + padY;
		dl->AddText(ImVec2(titleX, titleY),
			ColU32A(Config::menu_text_muted, 0.90f), "Spectators");

		{
			char badge[16];
			if (total > n)
				std::snprintf(badge, sizeof(badge), "%d+", total);
			else
				std::snprintf(badge, sizeof(badge), "%d", total);
			const ImVec2 bsz = ImGui::CalcTextSize(badge);
			dl->AddText(ImVec2(b.x - padX - bsz.x, titleY),
				total > 0
					? ColU32(Config::menu_text)
					: ColU32A(Config::menu_text_muted, 0.70f),
				badge);
		}

		DrawHeaderSep(dl, a, b, padX, a.y + padY + headerH);

		float y = a.y + padY + headerH + 3.f;
		if (n <= 0) {
			dl->AddText(ImVec2(a.x + padX, y + (rowH - ImGui::GetFontSize()) * 0.5f),
				ColU32A(Config::menu_text_muted, 0.70f), "None");
		} else {
			for (int i = 0; i < n; ++i) {
				const SpecEntry& s = s_specs[i];
				const ImVec2 rowA(a.x + padX, y);
				const ImVec2 rowB(b.x - padX, y + rowH);
				const float cy = y + rowH * 0.5f;

				float textX = rowA.x;

				if (showAvatars) {
					const ImVec2 av(rowA.x + avatarR, cy);
					const ImTextureID tex = SteamAvatar::Get(s.steamId, pDevice);
					if (tex != ImTextureID_Invalid) {
						dl->AddImageRounded(ImTextureRef(tex),
							ImVec2(av.x - avatarR, av.y - avatarR),
							ImVec2(av.x + avatarR, av.y + avatarR),
							ImVec2(0, 0), ImVec2(1, 1),
							IM_COL32(255, 255, 255, 230), avatarR);
					} else {
						DrawLetterAvatar(dl, av, avatarR, s.name, accent);
					}
					textX = av.x + avatarR + 6.f;
				}

				const float maxNameW = (std::max)(24.f, rowB.x - textX);
				char clipped[64];
				std::snprintf(clipped, sizeof(clipped), "%s", s.name);
				ClipTextToWidth(clipped, sizeof(clipped), maxNameW);
				const ImVec2 nsz = ImGui::CalcTextSize(clipped);
				dl->AddText(ImVec2(textX, cy - nsz.y * 0.5f),
					ColU32(Config::menu_text), clipped);

				y += rowH + rowGap;
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##spec_widget_settings");
		if (BeginWidgetPopup("##spec_widget_settings", "Spectators", accent)) {
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

void RenderRadarWidget(bool menuOpen) {
	if (!Config::widget_radar)
		return;

	const bool square = Config::widget_radar_shape == 1;
	const float diameter = std::clamp(Config::widget_radar_size, 90.f, 280.f);
	const float half = diameter * 0.5f;
	const float pad = 6.f;
	const float panelW = diameter + pad * 2.f;
	const float panelH = diameter + pad * 2.f;
	ImVec2 pos = ResolvePos(Config::widget_radar_pos, panelW, panelH,
		ImVec2(ImGui::GetIO().DisplaySize.x - panelW - 18.f, 52.f));

	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
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

	ImGui::Begin("##widget_radar", nullptr, flags);
	{
		ImGui::InvisibleButton("##radar_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();
		HandleWidgetDrag(Config::widget_radar_pos, panelW, panelH, menuOpen);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 win = ImGui::GetWindowPos();
		const ImVec2 center(win.x + panelW * 0.5f, win.y + panelH * 0.5f);
		const ImU32 accent = ColU32(Config::widget_radar_accent);
		const float rOuter = half;
		const float round = square ? 4.f : 0.f;
		const ImVec2 a(center.x - rOuter, center.y - rOuter);
		const ImVec2 b(center.x + rOuter, center.y + rOuter);

		const ImU32 shadow = IM_COL32(0, 0, 0, 70);
		if (square)
			dl->AddRectFilled(ImVec2(a.x + 1.f, a.y + 2.f), ImVec2(b.x + 1.f, b.y + 2.f), shadow, round);
		else
			dl->AddCircleFilled(ImVec2(center.x + 1.f, center.y + 2.f), rOuter, shadow, 48);

		const ImU32 plate = IM_COL32(18, 19, 22, 242);
		const ImU32 border = IM_COL32(255, 255, 255, 16);
		const ImU32 grid = IM_COL32(255, 255, 255, 16);
		const ImU32 gridSoft = IM_COL32(255, 255, 255, 10);

		if (square) {
			dl->AddRectFilled(a, b, plate, round);
			dl->AddRect(a, b, border, round, 0, 1.f);
		} else {
			dl->AddCircleFilled(center, rOuter, plate, 48);
			dl->AddCircle(center, rOuter, border, 48, 1.f);
		}

		const float inset = 5.f;
		const float usable = rOuter - inset;
		if (square) {
			dl->AddLine(ImVec2(center.x, a.y + inset), ImVec2(center.x, b.y - inset), grid, 1.f);
			dl->AddLine(ImVec2(a.x + inset, center.y), ImVec2(b.x - inset, center.y), grid, 1.f);
		} else {
			dl->AddCircle(center, usable * 0.5f, gridSoft, 32, 1.f);
			dl->AddLine(ImVec2(center.x, center.y - usable), ImVec2(center.x, center.y + usable), grid, 1.f);
			dl->AddLine(ImVec2(center.x - usable, center.y), ImVec2(center.x + usable, center.y), grid, 1.f);
		}

		{
			const float s = 4.5f;
			const ImVec2 tip(center.x, center.y - s);
			const ImVec2 bl(center.x - s * 0.7f, center.y + s * 0.5f);
			const ImVec2 br(center.x + s * 0.7f, center.y + s * 0.5f);
			dl->AddTriangleFilled(tip, bl, br, accent);
		}

		constexpr float kRange = 2200.f;
		const float scale = usable / kRange;
		const float maxR = usable - 1.f;
		const Vector_t localPos = cached_local.position;

		float yawRad = 0.f;
		if (Input::GetViewAngles && Input::viewAngleContext) {
			const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
			if (viewPtr) {
				__try {
					const Vector_t va = *reinterpret_cast<const Vector_t*>(viewPtr);
					constexpr float kDeg2Rad = 3.14159265358979323846f / 180.f;
					yawRad = va.y * kDeg2Rad;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					yawRad = 0.f;
				}
			}
		}
		const float cosY = std::cos(yawRad);
		const float sinY = std::sin(yawRad);

		for (const auto& p : cached_players) {
			if (p.health < 1)
				continue;
			const float wdx = p.position.x - localPos.x;
			const float wdy = p.position.y - localPos.y;
			const float fwd = wdx * cosY + wdy * sinY;
			const float right = wdx * sinY - wdy * cosY;
			float dx = right * scale;
			float dy = -fwd * scale;

			if (square) {
				// Clamp into square bounds
				dx = std::clamp(dx, -maxR, maxR);
				dy = std::clamp(dy, -maxR, maxR);
			} else {
				const float dist = std::sqrt(dx * dx + dy * dy);
				if (dist > maxR && dist > 0.001f) {
					const float t = maxR / dist;
					dx *= t;
					dy *= t;
				}
			}

			const ImVec2 pt(center.x + dx, center.y + dy);
			const bool isEnemy = (p.type == enemy);
			const ImU32 fill = isEnemy
				? IM_COL32(230, 80, 80, 245)
				: IM_COL32(100, 170, 240, 235);

			if (square) {
				const float hs = 2.4f;
				dl->AddRectFilled(ImVec2(pt.x - hs, pt.y - hs),
					ImVec2(pt.x + hs, pt.y + hs), fill, 1.f);
				if (!p.visible && isEnemy)
					dl->AddRect(ImVec2(pt.x - hs - 1.f, pt.y - hs - 1.f),
						ImVec2(pt.x + hs + 1.f, pt.y + hs + 1.f),
						IM_COL32(255, 255, 255, 40), 1.f, 0, 1.f);
			} else {
				dl->AddCircleFilled(pt, 2.4f, fill, 10);
				if (!p.visible && isEnemy)
					dl->AddCircle(pt, 3.6f, IM_COL32(255, 255, 255, 35), 10, 1.f);
			}
		}

		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##radar_widget_settings");
		if (BeginWidgetPopup("##radar_widget_settings", "RADAR", accent, 210.f)) {
			ImGui::TextUnformatted("Shape");
			ImGui::SetNextItemWidth(-1.f);
			const char* shapes[] = { "Circle", "Square" };
			int shape = Config::widget_radar_shape;
			if (ImGui::Combo("##rshape", &shape, shapes, IM_ARRAYSIZE(shapes)))
				Config::widget_radar_shape = shape;

			ImGui::TextUnformatted("Size");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::SliderFloat("##rsz", &Config::widget_radar_size, 90.f, 280.f, "%.0f");
			WidgetColorEdit("Accent", &Config::widget_radar_accent);
			if (WidgetResetButton())
				Config::widget_radar_pos = ImVec2(-1.f, -1.f);
			EndWidgetPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

} // namespace

void Render() {
	const bool menuOpen = g_bMenuOpen;
	__try { RenderKeybindList(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.keybinds", GetExceptionCode()); }
	__try { RenderBombWidget(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.bomb", GetExceptionCode()); }
	__try { RenderSpectatorList(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.specs", GetExceptionCode()); }
	__try { RenderRadarWidget(menuOpen); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("Widget.radar", GetExceptionCode()); }
}

} // namespace Widgets
