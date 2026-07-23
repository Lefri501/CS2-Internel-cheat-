#define NOMINMAX
#include "hitlog.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../notify/notify.h"
#include "../../../../external/imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace HitLog {
namespace {

Entry g_log[kMax]{};
int g_write = 0;

// Session totals (reset on Clear / map change)
int g_sessHits = 0;
int g_sessHeads = 0;
int g_sessKills = 0;
int g_sessDmg = 0;

const char* HitgroupName(int hg)
{
	switch (hg) {
	case 1: return "HEAD";
	case 2: return "CHEST";
	case 3: return "STOMACH";
	case 4: return "L-ARM";
	case 5: return "R-ARM";
	case 6: return "L-LEG";
	case 7: return "R-LEG";
	case 8: return "NECK";
	default: return "BODY";
	}
}

float Now()
{
	return static_cast<float>(ImGui::GetTime());
}

float EaseOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	const float u = 1.f - t;
	return 1.f - u * u * u;
}

ImU32 ToU32(ImVec4 c)
{
	return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 WithA(ImVec4 c, float a)
{
	c.w = std::clamp(c.w * a, 0.f, 1.f);
	return ToU32(c);
}

ImU32 WithA(ImU32 c, float a)
{
	const int aa = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.f, 1.f));
	return (c & ~IM_COL32(0, 0, 0, 255)) | (static_cast<ImU32>(aa) << IM_COL32_A_SHIFT);
}

float Rounding()
{
	return std::clamp((std::max)(Config::menu_rounding, 2.f), 2.f, 6.f);
}

ImVec4 PlateBg()
{
	ImVec4 c = Config::menu_child_bg;
	if (c.w < 0.90f)
		c.w = 0.96f;
	// Slightly darker plate for log readability
	c.x *= 0.92f;
	c.y *= 0.92f;
	c.z *= 0.94f;
	return c;
}

ImVec4 TextMain()
{
	ImVec4 t = Config::menu_text;
	if ((0.2126f * t.x + 0.7152f * t.y + 0.0722f * t.z) < 0.72f)
		t = ImVec4(0.93f, 0.94f, 0.96f, 1.f);
	t.w = 1.f;
	return t;
}

ImVec4 TextMuted()
{
	ImVec4 t = TextMain();
	t.w = 0.55f;
	return t;
}

void ClipName(char* buf, size_t bufSize, float maxW)
{
	if (!buf || bufSize < 4 || maxW <= 0.f)
		return;
	if (ImGui::CalcTextSize(buf).x <= maxW)
		return;
	while (std::strlen(buf) > 0) {
		const size_t len = std::strlen(buf);
		if (len + 3 >= bufSize)
			break;
		char tmp[256];
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

ImVec2 ClampPos(ImVec2 pos, float panelW, float panelH)
{
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	pos.x = std::clamp(pos.x, 4.f, (std::max)(4.f, ds.x - panelW - 4.f));
	pos.y = std::clamp(pos.y, 4.f, (std::max)(4.f, ds.y - panelH - 4.f));
	return pos;
}

bool IsAutoPos(ImVec2 cfg)
{
	return cfg.x < 0.f && cfg.y < 0.f;
}

} // namespace

void Clear()
{
	for (int i = 0; i < kMax; ++i)
		g_log[i] = Entry{};
	g_write = 0;
	g_sessHits = 0;
	g_sessHeads = 0;
	g_sessKills = 0;
	g_sessDmg = 0;
}

void Push(const char* name, int damage, int hitgroup, bool head, bool kill, int healthLeft)
{
	if (!Config::hitlog || damage <= 0)
		return;

	Entry& e = g_log[g_write];
	g_write = (g_write + 1) % kMax;
	e.active = true;
	e.damage = damage;
	e.hitgroup = hitgroup;
	e.healthLeft = (healthLeft >= 0) ? healthLeft : -1;
	e.head = head;
	e.kill = kill;
	e.born = Now();
	e.name[0] = 0;
	if (name && name[0])
		std::snprintf(e.name, sizeof(e.name), "%s", name);
	else
		std::snprintf(e.name, sizeof(e.name), "player");

	++g_sessHits;
	g_sessDmg += damage;
	if (head)
		++g_sessHeads;
	if (kill)
		++g_sessKills;

	if (Config::hitlog_console) {
		const char* hg = HitgroupName(hitgroup);
		// Distinct from engine spam: fixed brand prefix + color by outcome
		// KILL=red, HEAD=gold, body=cyan
		int cr = 80, cg = 220, cb = 255; // body cyan
		if (kill) {
			cr = 255; cg = 70; cb = 90;
		} else if (head) {
			cr = 255; cg = 210; cb = 60;
		}

		// Channel tag = (Lefrizzel AI) via Notify brand log.
		// Body: HIT  name  -DMG  TAG  …
		if (kill)
			Notify::ConsoleColorPrintf(cr, cg, cb, 255,
				"HIT  %s  -%d  %s  | KILL", e.name, damage, hg);
		else if (e.healthLeft >= 0)
			Notify::ConsoleColorPrintf(cr, cg, cb, 255,
				"HIT  %s  -%d  %s  | %dhp left", e.name, damage, hg, e.healthLeft);
		else
			Notify::ConsoleColorPrintf(cr, cg, cb, 255,
				"HIT  %s  -%d  %s", e.name, damage, hg);
	}
}

void Draw()
{
	if (!Config::hitlog)
		return;

	const float life = std::clamp(Config::hitlog_duration, 1.f, 12.f);
	const float now = Now();
	const ImGuiIO& io = ImGui::GetIO();
	const bool menuOpen = g_bMenuOpen;

	// Collect live rows (newest first)
	struct Row {
		Entry* e = nullptr;
		float age = 0.f;
		float alpha = 0.f;
		float slide = 0.f;
	};
	Row rows[kMax]{};
	int nRows = 0;
	const float slideIn = 0.12f;
	const float fadeOut = 0.30f;
	const int maxVis = std::clamp(Config::hitlog_max_rows, 4, kMax);

	for (int n = 0; n < kMax; ++n) {
		const int i = (g_write - 1 - n + kMax * 2) % kMax;
		Entry& e = g_log[i];
		if (!e.active)
			continue;
		const float age = now - e.born;
		if (age >= life) {
			e.active = false;
			continue;
		}
		Row& row = rows[nRows++];
		row.e = &e;
		row.age = age;
		const float enterT = EaseOut(std::clamp(age / slideIn, 0.f, 1.f));
		row.slide = enterT;
		float a = 1.f;
		if (age > life - fadeOut) {
			const float t = (age - (life - fadeOut)) / fadeOut;
			a = 1.f - EaseOut(t);
		}
		a *= enterT;
		row.alpha = std::clamp(a, 0.f, 1.f);
		if (nRows >= maxVis)
			break;
	}

	// Always show panel when menu open (empty state + drag), else only with rows
	if (nRows <= 0 && !menuOpen)
		return;

	const float fs = ImGui::GetFontSize();
	const float padX = 10.f;
	const float padY = 8.f;
	const float headerH = 18.f;
	const float rowH = fs + 10.f;
	const float rowGap = 2.f;
	const float statsH = Config::hitlog_show_stats ? (fs + 8.f) : 0.f;
	const float panelW = std::clamp(Config::hitlog_width, 200.f, 420.f);
	const int drawRows = (nRows > 0) ? nRows : (menuOpen ? 1 : 0);
	const float bodyH = (drawRows > 0)
		? (float)drawRows * rowH + (float)(drawRows - 1) * rowGap
		: 0.f;
	const float sepH = 5.f;
	const float panelH = padY + headerH + sepH + bodyH
		+ (statsH > 0.f ? sepH + statsH : 0.f) + padY;

	ImVec2 pos = Config::hitlog_pos;
	if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || IsAutoPos(pos))
		pos = ImVec2(16.f, io.DisplaySize.y * 0.30f);
	pos = ClampPos(pos, panelW, panelH);

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

	ImGui::Begin("##hitlog_panel", nullptr, flags);
	{
		ImGui::InvisibleButton("##hl_hit", ImVec2(panelW, panelH));
		const bool hovered = ImGui::IsItemHovered();

		// Drag (menu only)
		if (menuOpen) {
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
				Config::hitlog_pos.x += io.MouseDelta.x;
				Config::hitlog_pos.y += io.MouseDelta.y;
				Config::hitlog_pos = ClampPos(Config::hitlog_pos, panelW, panelH);
				ImGui::SetWindowPos(Config::hitlog_pos);
			} else {
				ImVec2 p = ImGui::GetWindowPos();
				if (!std::isfinite(p.x) || !std::isfinite(p.y))
					p = Config::hitlog_pos;
				Config::hitlog_pos = ClampPos(p, panelW, panelH);
				if (Config::hitlog_pos.x != p.x || Config::hitlog_pos.y != p.y)
					ImGui::SetWindowPos(Config::hitlog_pos);
			}
		}

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 a = ImGui::GetWindowPos();
		const ImVec2 b(a.x + panelW, a.y + panelH);
		const float r = Rounding();
		const ImVec4 plate = PlateBg();
		const ImVec4 text = TextMain();
		const ImVec4 muted = TextMuted();

		// Panel chrome
		dl->AddRectFilled(ImVec2(a.x + 1.f, a.y + 2.f), ImVec2(b.x + 1.f, b.y + 3.f),
			IM_COL32(0, 0, 0, 70), r);
		dl->AddRectFilled(a, b, ToU32(plate), r);
		dl->AddRect(a, b, IM_COL32(255, 255, 255, 18), r, 0, 1.f);
		dl->AddRect(
			ImVec2(a.x + 1.f, a.y + 1.f),
			ImVec2(b.x - 1.f, b.y - 1.f),
			IM_COL32(0, 0, 0, 36),
			(std::max)(0.f, r - 1.f), 0, 1.f);

		// Header
		const float titleY = a.y + padY;
		dl->AddText(ImVec2(a.x + padX, titleY), ToU32(text), "HIT LOG");
		if (menuOpen && hovered) {
			const char* tip = "drag";
			const ImVec2 tipSz = ImGui::CalcTextSize(tip);
			dl->AddText(
				ImVec2(b.x - padX - tipSz.x, titleY),
				ToU32(muted), tip);
		} else if (g_sessHits > 0) {
			char cnt[16];
			std::snprintf(cnt, sizeof(cnt), "%d", g_sessHits);
			const ImVec2 csz = ImGui::CalcTextSize(cnt);
			dl->AddText(ImVec2(b.x - padX - csz.x, titleY), ToU32(muted), cnt);
		}

		// Header sep
		const float sepY = a.y + padY + headerH + 1.f;
		dl->AddLine(
			ImVec2(a.x + padX, sepY),
			ImVec2(b.x - padX, sepY),
			IM_COL32(255, 255, 255, 14), 1.f);

		float y = sepY + 4.f;

		if (nRows <= 0 && menuOpen) {
			const char* empty = "waiting for hits...";
			const ImVec2 es = ImGui::CalcTextSize(empty);
			dl->AddText(
				ImVec2(a.x + (panelW - es.x) * 0.5f, y + (rowH - es.y) * 0.5f),
				ToU32(muted), empty);
			y += rowH;
		}

		const float railW = 2.f;

		for (int i = 0; i < nRows; ++i) {
			const Row& row = rows[i];
			const Entry& e = *row.e;
			const float alpha = row.alpha;
			if (alpha < 0.02f) {
				y += rowH + rowGap;
				continue;
			}

			const float xOff = (1.f - row.slide) * -10.f;
			const float ry = y;
			const float rx0 = a.x + padX + xOff;
			const float rx1 = b.x - padX + xOff;

			ImVec4 accent = Config::hitlog_color;
			if (e.kill)
				accent = Config::hitlog_kill_color;
			else if (e.head)
				accent = Config::hitlog_head_color;
			if (accent.x + accent.y + accent.z < 0.12f)
				accent = Config::menu_accent;

			// Row plate (subtle)
			dl->AddRectFilled(
				ImVec2(rx0, ry),
				ImVec2(rx1, ry + rowH),
				IM_COL32(255, 255, 255, static_cast<int>(8.f * alpha)),
				2.f);

			// Accent rail
			dl->AddRectFilled(
				ImVec2(rx0, ry + 3.f),
				ImVec2(rx0 + railW, ry + rowH - 3.f),
				WithA(accent, alpha * 0.95f), 1.f);

			// Tag
			const char* tag = e.kill ? "KILL" : (e.head ? "HEAD" : HitgroupName(e.hitgroup));
			const ImVec2 tagSz = ImGui::CalcTextSize(tag);
			const float textY = ry + (rowH - tagSz.y) * 0.5f;
			const float tagX = rx0 + railW + 8.f;
			dl->AddText(ImVec2(tagX, textY), WithA(accent, alpha), tag);

			// Damage (right)
			char dmgBuf[16];
			std::snprintf(dmgBuf, sizeof(dmgBuf), "-%d", e.damage);
			const ImVec2 dmgSz = ImGui::CalcTextSize(dmgBuf);
			float rightX = rx1 - dmgSz.x;

			// Optional remaining HP after dmg
			char hpBuf[16]{};
			ImVec2 hpSz{};
			if (Config::hitlog_show_hp && e.healthLeft >= 0 && !e.kill) {
				std::snprintf(hpBuf, sizeof(hpBuf), "%d", e.healthLeft);
				hpSz = ImGui::CalcTextSize(hpBuf);
				rightX -= (hpSz.x + 8.f);
			}

			// Name between tag and right cluster
			char nameLine[72];
			std::snprintf(nameLine, sizeof(nameLine), "%s", e.name);
			const float nameX = tagX + tagSz.x + 8.f;
			const float nameMaxW = (std::max)(0.f, rightX - nameX - 8.f);
			ClipName(nameLine, sizeof(nameLine), nameMaxW);
			const ImVec2 nsz = ImGui::CalcTextSize(nameLine);
			dl->AddText(
				ImVec2(nameX, ry + (rowH - nsz.y) * 0.5f),
				WithA(text, alpha), nameLine);

			if (hpBuf[0]) {
				dl->AddText(
					ImVec2(rightX, ry + (rowH - hpSz.y) * 0.5f),
					WithA(muted, alpha), hpBuf);
				rightX += hpSz.x + 8.f;
			}

			dl->AddText(
				ImVec2(rx1 - dmgSz.x, ry + (rowH - dmgSz.y) * 0.5f),
				WithA(accent, alpha), dmgBuf);

			// Life underline
			const float lifeFrac = 1.f - std::clamp(row.age / life, 0.f, 1.f);
			const float barY = ry + rowH - 1.5f;
			const float barX0 = rx0 + 2.f;
			const float barX1 = rx1 - 2.f;
			const float fullW = barX1 - barX0;
			dl->AddRectFilled(
				ImVec2(barX0, barY),
				ImVec2(barX1, barY + 1.f),
				IM_COL32(255, 255, 255, static_cast<int>(14.f * alpha)), 0.f);
			const float barW = fullW * lifeFrac;
			if (barW > 0.5f) {
				dl->AddRectFilled(
					ImVec2(barX0, barY),
					ImVec2(barX0 + barW, barY + 1.5f),
					WithA(accent, alpha * 0.90f), 0.f);
			}

			y += rowH + rowGap;
		}

		// Session stats footer
		if (Config::hitlog_show_stats && (g_sessHits > 0 || menuOpen)) {
			const float footY = b.y - padY - statsH;
			dl->AddLine(
				ImVec2(a.x + padX, footY - 3.f),
				ImVec2(b.x - padX, footY - 3.f),
				IM_COL32(255, 255, 255, 12), 1.f);

			char foot[96];
			std::snprintf(foot, sizeof(foot), "H %d  HS %d  K %d  DMG %d",
				g_sessHits, g_sessHeads, g_sessKills, g_sessDmg);
			const ImVec2 fsz = ImGui::CalcTextSize(foot);
			dl->AddText(
				ImVec2(a.x + padX, footY + (statsH - fsz.y) * 0.5f),
				ToU32(muted), foot);
		}

		// Right-click settings (menu)
		if (menuOpen && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("##hitlog_widget_settings");

		if (ImGui::BeginPopup("##hitlog_widget_settings")) {
			ImGui::TextUnformatted("HIT LOG");
			ImGui::Separator();
			ImGui::Checkbox("Show HP left", &Config::hitlog_show_hp);
			ImGui::Checkbox("Session stats", &Config::hitlog_show_stats);
			ImGui::Checkbox("Game console", &Config::hitlog_console);
			ImGui::SetNextItemWidth(160.f);
			ImGui::SliderFloat("Duration", &Config::hitlog_duration, 1.f, 12.f, "%.1f s");
			ImGui::SetNextItemWidth(160.f);
			ImGui::SliderFloat("Width", &Config::hitlog_width, 200.f, 380.f, "%.0f");
			ImGui::SetNextItemWidth(160.f);
			ImGui::SliderInt("Max rows", &Config::hitlog_max_rows, 4, 16);
			ImGui::ColorEdit4("Body", (float*)&Config::hitlog_color, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Head", (float*)&Config::hitlog_head_color, ImGuiColorEditFlags_NoInputs);
			ImGui::SameLine();
			ImGui::ColorEdit4("Kill", (float*)&Config::hitlog_kill_color, ImGuiColorEditFlags_NoInputs);
			ImGui::Dummy(ImVec2(0, 4.f));
			if (ImGui::Button("Reset position", ImVec2(-1.f, 0.f)))
				Config::hitlog_pos = ImVec2(-1.f, -1.f);
			if (ImGui::Button("Clear log", ImVec2(-1.f, 0.f)))
				Clear();
			ImGui::EndPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

} // namespace HitLog
