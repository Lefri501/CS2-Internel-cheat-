#include "notify.h"

#include "../../../../external/imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Notify {
namespace {

constexpr int   kMaxToasts     = 6;
constexpr float kDefaultLife   = 3.2f;
constexpr float kAnimIn        = 0.22f;
constexpr float kAnimOut       = 0.28f;
constexpr float kToastW        = 280.f;
constexpr float kPadX          = 14.f;
constexpr float kPadY          = 11.f;
constexpr float kGap           = 8.f;
constexpr float kMargin        = 16.f;

struct Toast {
	std::string title;
	std::string message;
	Type type = Type::Info;
	float born = 0.f;
	float life = kDefaultLife;
};

std::vector<Toast> g_toasts;

ImU32 AccentFor(Type t) {
	switch (t) {
	case Type::Success: return IM_COL32(80, 210, 140, 240);
	case Type::Warn:    return IM_COL32(255, 180, 70, 240);
	case Type::Error:   return IM_COL32(255, 85, 95, 240);
	case Type::Info:
	default:            return IM_COL32(150, 115, 255, 240);
	}
}

const char* GlyphFor(Type t) {
	switch (t) {
	case Type::Success: return "+";
	case Type::Warn:    return "!";
	case Type::Error:   return "x";
	case Type::Info:
	default:            return "i";
	}
}

float EaseOutCubic(float t) {
	t = std::clamp(t, 0.f, 1.f);
	const float u = 1.f - t;
	return 1.f - u * u * u;
}

float ToastAlpha(const Toast& toast, float now) {
	const float age = now - toast.born;
	if (age < 0.f) return 0.f;
	if (age < kAnimIn)
		return EaseOutCubic(age / kAnimIn);
	if (age > toast.life - kAnimOut) {
		const float t = (toast.life - age) / kAnimOut;
		return EaseOutCubic(std::clamp(t, 0.f, 1.f));
	}
	return 1.f;
}

float ToastSlide(const Toast& toast, float now) {
	const float age = now - toast.born;
	if (age < kAnimIn)
		return (1.f - EaseOutCubic(age / kAnimIn)) * 28.f;
	if (age > toast.life - kAnimOut) {
		const float t = 1.f - std::clamp((toast.life - age) / kAnimOut, 0.f, 1.f);
		return EaseOutCubic(t) * 18.f;
	}
	return 0.f;
}

float MeasureHeight(const Toast& toast) {
	const float titleH = ImGui::CalcTextSize(toast.title.c_str()).y;
	float h = kPadY * 2.f + titleH;
	if (!toast.message.empty()) {
		ImGui::PushTextWrapPos(kToastW - kPadX * 2.f - 28.f);
		const ImVec2 ms = ImGui::CalcTextSize(toast.message.c_str(), nullptr, false,
			kToastW - kPadX * 2.f - 28.f);
		ImGui::PopTextWrapPos();
		h += 4.f + ms.y;
	}
	return h;
}

} // namespace

void Push(const char* title, const char* message, Type type, float durationSec) {
	if (!title || !title[0])
		return;

	Toast t;
	t.title = title;
	if (message && message[0])
		t.message = message;
	t.type = type;
	t.born = static_cast<float>(ImGui::GetTime());
	t.life = durationSec > 0.1f ? durationSec : kDefaultLife;

	g_toasts.push_back(std::move(t));
	while (static_cast<int>(g_toasts.size()) > kMaxToasts)
		g_toasts.erase(g_toasts.begin());
}

void Render() {
	if (g_toasts.empty())
		return;

	const float now = static_cast<float>(ImGui::GetTime());
	g_toasts.erase(
		std::remove_if(g_toasts.begin(), g_toasts.end(),
			[now](const Toast& t) { return (now - t.born) >= t.life; }),
		g_toasts.end());
	if (g_toasts.empty())
		return;

	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const ImVec2 ds = ImGui::GetIO().DisplaySize;

	float stackY = ds.y - kMargin;
	for (int i = static_cast<int>(g_toasts.size()) - 1; i >= 0; --i) {
		const Toast& toast = g_toasts[static_cast<size_t>(i)];
		const float a = ToastAlpha(toast, now);
		if (a <= 0.01f)
			continue;

		const float h = MeasureHeight(toast);
		const float slide = ToastSlide(toast, now);
		const float x = ds.x - kMargin - kToastW + slide;
		const float y = stackY - h;
		stackY = y - kGap;

		const int alpha = static_cast<int>(a * 255.f);
		const ImU32 accent = AccentFor(toast.type);
		const int accentA = static_cast<int>(((accent >> IM_COL32_A_SHIFT) & 0xFF) * a);

		auto withA = [a](ImU32 c) -> ImU32 {
			const int ca = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
			return (c & ~IM_COL32_A_MASK) | ((ca & 0xFF) << IM_COL32_A_SHIFT);
		};

		const ImVec2 p0(x, y);
		const ImVec2 p1(x + kToastW, y + h);
		const float r = 8.f;

		// Glass body
		dl->AddRectFilled(p0, p1, withA(IM_COL32(14, 14, 18, 210)), r);
		dl->AddRect(p0, p1, withA(IM_COL32(255, 255, 255, 18)), r, 0, 1.f);
		// Accent strip
		dl->AddRectFilled(
			ImVec2(p0.x, p0.y + 4.f),
			ImVec2(p0.x + 2.5f, p1.y - 4.f),
			(accent & ~IM_COL32_A_MASK) | ((accentA & 0xFF) << IM_COL32_A_SHIFT), 2.f);
		// Top gloss
		dl->AddRectFilledMultiColor(
			ImVec2(p0.x + 1.f, p0.y + 1.f),
			ImVec2(p1.x - 1.f, p0.y + 16.f),
			withA(IM_COL32(255, 255, 255, 20)),
			withA(IM_COL32(255, 255, 255, 20)),
			withA(IM_COL32(255, 255, 255, 0)),
			withA(IM_COL32(255, 255, 255, 0)));

		// Type badge
		const float badgeR = 9.f;
		const ImVec2 badge(p0.x + kPadX + 10.f, p0.y + kPadY + 8.f);
		dl->AddCircleFilled(badge, badgeR,
			withA(IM_COL32(
				((accent >> IM_COL32_R_SHIFT) & 0xFF),
				((accent >> IM_COL32_G_SHIFT) & 0xFF),
				((accent >> IM_COL32_B_SHIFT) & 0xFF),
				55)), 16);
		dl->AddCircle(badge, badgeR,
			(accent & ~IM_COL32_A_MASK) | ((accentA & 0xFF) << IM_COL32_A_SHIFT), 16, 1.2f);
		const char* glyph = GlyphFor(toast.type);
		const ImVec2 gs = ImGui::CalcTextSize(glyph);
		dl->AddText(ImVec2(badge.x - gs.x * 0.5f, badge.y - gs.y * 0.5f),
			withA(IM_COL32(245, 245, 255, 255)), glyph);

		const float textX = p0.x + kPadX + 28.f;
		float textY = p0.y + kPadY;
		dl->AddText(ImVec2(textX, textY), withA(IM_COL32(245, 245, 255, 255)), toast.title.c_str());
		textY += ImGui::CalcTextSize(toast.title.c_str()).y + 3.f;

		if (!toast.message.empty()) {
			const float wrapW = kToastW - kPadX - 28.f - kPadX;
			dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
				ImVec2(textX, textY), withA(IM_COL32(175, 175, 190, 230)),
				toast.message.c_str(), nullptr, wrapW);
		}

		// Lifetime progress hairline
		const float lifeT = std::clamp((now - toast.born) / toast.life, 0.f, 1.f);
		const float barW = (kToastW - 8.f) * (1.f - lifeT);
		if (barW > 1.f) {
			dl->AddRectFilled(
				ImVec2(p0.x + 4.f, p1.y - 2.5f),
				ImVec2(p0.x + 4.f + barW, p1.y - 1.f),
				(accent & ~IM_COL32_A_MASK) | ((accentA & 0xFF) << IM_COL32_A_SHIFT), 1.f);
		}
	}
}

} // namespace Notify
