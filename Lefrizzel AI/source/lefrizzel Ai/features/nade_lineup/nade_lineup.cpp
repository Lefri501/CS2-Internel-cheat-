#include "nade_lineup.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../bones/bones.h"
#include "../gamemode/gamemode.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/datatypes/schema/ISchemaClass/ISchemaClass.h"
#include "../../../../external/imgui/imgui.h"
#include "../../../../external/json/json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>

#include "../../utils/console/console.h"
#include "../../keybinds/keybinds.h"
#include "../w2s/w2s.h"
#include "../../menu/menu.h"
#include "../visuals/assets/weapon_icons.hpp"
#include "../notify/notify.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"

#include <cfloat>

namespace NadeLineup {
namespace {

constexpr float kAimDrawDist = 350.f;   // world aim ball locked to stand+saved angles
constexpr float kCircleRadius = 14.f;   // visual stand pad
constexpr float kAimExactR = 4.5f;      // feet lock for full aim reticle
constexpr float kAimPreviewR = 12.f;    // on-pad preview (dim) before exact lock
constexpr float kEyeLift = 64.f;
constexpr float kUnitsToM = 0.0254f;    // Source units → meters

std::vector<Lineup> g_lineups;
// Index into g_lineups — never store raw Lineup* (vector realloc invalidates pointers)
int g_currentIdx = -1;
bool g_loaded = false;
bool g_captureKeyWasDown = false;
char g_mapCache[128] = {};
// Indices of lineups for current map (avoids scanning all maps every frame)
std::vector<int> g_mapIndices;
char g_mapIndicesKey[128] = {};

// Armed capture: freeze stand feet on arm; refresh aim each frame until throw.
// Motion: recency timestamps — whole-session sticky false-tagged stand after
// run-to-spot / early hop. Jumpthrow still needs ~500ms window (bind releases space).
struct CaptureSession {
	bool armed = false;
	Vector_t pos{};           // stand feet (arm snapshot)
	QAngle_t ang{};           // live aim — updated every armed frame
	NadeKind kind = NadeKind::Any;
	char name[64]{};
	ULONGLONG armMs = 0;
	bool attackHeld = false;
	bool sawAttack = false;      // M1/M2 while holding a grenade (not menu click)
	bool jumpWhileAtk = false;   // space/rise while M1/M2 held
	bool runWhileAtk = false;
	bool duckWhileAtk = false;
	ULONGLONG lastJumpMs = 0;
	ULONGLONG lastRunMs = 0;
	ULONGLONG lastDuckMs = 0;
	ULONGLONG attackDownMs = 0;  // when real attack press started (0 = none)
};
CaptureSession g_cap{};
constexpr ULONGLONG kCaptureTimeoutMs = 20000;
// How long before release a jump/run/duck still counts (jumpthrow bind lag).
constexpr ULONGLONG kMotionRecentMs = 550;
// Ignore residual LMB from "Arm Capture" button / F6 chord.
constexpr ULONGLONG kArmIgnoreAtkMs = 280;
// Click vs pin-hold: need hold this long before release counts as throw.
constexpr ULONGLONG kMinAtkHoldMs = 70;

void InvalidateMapIndex() {
	g_mapIndices.clear();
	g_mapIndicesKey[0] = '\0';
}

std::filesystem::path LineupFolder() {
	std::filesystem::path folder;
	char* userProfile = nullptr;
	size_t len = 0;
	if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile && len > 0) {
		folder = userProfile;
		free(userProfile);
		folder /= "Documents";
		folder /= "Lefrizzel AI";
		folder /= "NadeLineups";
	} else {
		folder = "Lefrizzel AI";
		folder /= "NadeLineups";
	}
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	return folder;
}

// Legacy typo path (Nade.ineups) — migrate so old captures still load.
std::filesystem::path LegacyLineupFolder() {
	std::filesystem::path folder;
	char* userProfile = nullptr;
	size_t len = 0;
	if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile && len > 0) {
		folder = userProfile;
		free(userProfile);
		folder /= "Documents";
		folder /= "Lefrizzel AI";
		folder /= "Nade.ineups";
	} else {
		folder = "Lefrizzel AI";
		folder /= "Nade.ineups";
	}
	return folder;
}

std::filesystem::path LineupFilePath() {
	return LineupFolder() / "lineups.json";
}

std::filesystem::path LegacyLineupFilePath() {
	return LegacyLineupFolder() / "lineups.json";
}

void AngleVectors(const QAngle_t& ang, Vector_t& forward) {
	const float pitch = ang.x * (3.14159265f / 180.f);
	const float yaw = ang.y * (3.14159265f / 180.f);
	const float cp = std::cos(pitch);
	const float sp = std::sin(pitch);
	const float cy = std::cos(yaw);
	const float sy = std::sin(yaw);
	forward = { cp * cy, cp * sy, -sp };
}

// Aim marker is ALWAYS stand feet + eye lift + saved aim angles * dist.
// Never current camera / current eye — so it stays where you captured it.
Vector_t AimWorldFromStand(const Vector_t& stand, const QAngle_t& ang, float dist) {
	Vector_t fwd{};
	AngleVectors(ang, fwd);
	return {
		stand.x + fwd.x * dist,
		stand.y + fwd.y * dist,
		stand.z + kEyeLift + fwd.z * dist
	};
}

bool IsFiniteVec(const Vector_t& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool IsFiniteAng(const QAngle_t& a) {
	return std::isfinite(a.x) && std::isfinite(a.y);
}

bool GetLocalViewAngles(QAngle_t& out) {
	if (!Input::GetViewAngles || !Input::viewAngleContext)
		return false;
	const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
	if (!viewPtr || !Mem::IsReadable(reinterpret_cast<void*>(viewPtr), sizeof(Vector_t)))
		return false;
	Vector_t v{};
	__try {
		v = *reinterpret_cast<const Vector_t*>(viewPtr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!std::isfinite(v.x) || !std::isfinite(v.y))
		return false;
	out = { v.x, v.y, 0.f };
	return true;
}

const char* SehClassName(CEntityInstance* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	SchemaClassInfoData_t* cls = nullptr;
	__try {
		ent->dump_class_info(&cls);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (cls && cls->szName && Mem::IsReadable(cls->szName, 2) && cls->szName[0])
		return cls->szName;
	return nullptr;
}

const char* SehDesignerName(CEntityInstance* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	CEntityIdentity* id = nullptr;
	if (!Mem::ReadField(ent, 0x10, id) || !id || !Mem::Valid(id, 0x28)) {
		__try {
			id = ent->m_pEntityIdentity();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	if (!id || !Mem::Valid(id, 0x28))
		return nullptr;
	const char* p = nullptr;
	if (!Mem::ReadField(id, 0x20, p) || !p) {
		__try {
			p = id->m_designerName();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}
	if (!p || !Mem::IsReadable(p, 2) || !p[0])
		return nullptr;
	return p;
}

NadeKind ClassifyHeld(C_CSWeaponBase* wep) {
	if (!wep)
		return NadeKind::Any;
	const char* a = SehClassName(reinterpret_cast<CEntityInstance*>(wep));
	const char* b = SehDesignerName(reinterpret_cast<CEntityInstance*>(wep));
	a = a ? a : "";
	b = b ? b : "";
	if (std::strstr(a, "SmokeGrenade") || std::strstr(b, "smokegrenade"))
		return NadeKind::Smoke;
	if (std::strstr(a, "Molotov") || std::strstr(a, "Incendiary")
		|| std::strstr(b, "molotov") || std::strstr(b, "incgrenade") || std::strstr(b, "incendiary"))
		return NadeKind::Molly;
	if (std::strstr(a, "HEGrenade") || std::strstr(b, "hegrenade"))
		return NadeKind::HE;
	if (std::strstr(a, "Flashbang") || std::strstr(b, "flashbang"))
		return NadeKind::Flash;
	if (std::strstr(a, "Decoy") || std::strstr(b, "decoy"))
		return NadeKind::Decoy;
	return NadeKind::Any;
}

const char* StripMapPrefix(const char* s) {
	if (!s || !s[0])
		return s;
	// de_mirage / cs_office / gd_xxx → bare name for fuzzy match
	if (s[0] && s[1] && s[2] == '_' &&
		((s[0] == 'd' || s[0] == 'D') || (s[0] == 'c' || s[0] == 'C') || (s[0] == 'g' || s[0] == 'G')))
		return s + 3;
	return s;
}

bool MapsEqual(const std::string& a, const char* b) {
	if (!b || !b[0])
		return a.empty();
	if (a.empty())
		return false;
	const char* baseB = b;
	for (const char* p = b; *p; ++p) {
		if (*p == '/' || *p == '\\')
			baseB = p + 1;
	}
	const char* baseA = a.c_str();
	for (const char* p = a.c_str(); *p; ++p) {
		if (*p == '/' || *p == '\\')
			baseA = p + 1;
	}
	if (_stricmp(baseA, baseB) == 0)
		return true;
	// engine sometimes returns "mirage", packs use "de_mirage"
	return _stricmp(StripMapPrefix(baseA), StripMapPrefix(baseB)) == 0;
}

void RebuildMapIndex() {
	g_mapIndices.clear();
	if (!g_mapCache[0]) {
		g_mapIndicesKey[0] = '\0';
		return;
	}
	std::snprintf(g_mapIndicesKey, sizeof(g_mapIndicesKey), "%s", g_mapCache);
	g_mapIndices.reserve(64);
	for (int i = 0; i < static_cast<int>(g_lineups.size()); ++i) {
		if (!g_lineups[i].map.empty() && MapsEqual(g_lineups[i].map, g_mapCache))
			g_mapIndices.push_back(i);
	}
}

void EnsureMapIndex() {
	if (!g_mapCache[0]) {
		InvalidateMapIndex();
		return;
	}
	if (g_mapIndicesKey[0] && _stricmp(g_mapIndicesKey, g_mapCache) == 0)
		return;
	RebuildMapIndex();
}

bool KindMatches(NadeKind want, NadeKind held) {
	if (want == NadeKind::Any)
		return true;
	if (held == NadeKind::Any)
		return !Config::nade_lineup_only_held;
	return want == held;
}

ImU32 ColA(const ImVec4& c, float a) {
	const int aa = static_cast<int>(std::clamp(c.w * a * 255.f, 0.f, 255.f));
	return IM_COL32(
		static_cast<int>(c.x * 255.f),
		static_cast<int>(c.y * 255.f),
		static_cast<int>(c.z * 255.f),
		aa);
}

ImU32 ColRGB(int r, int g, int b, float a) {
	return IM_COL32(r, g, b, static_cast<int>(std::clamp(a * 255.f, 0.f, 255.f)));
}

bool WorldToScreen2(const ViewMatrix& vm, const Vector_t& w, ImVec2& out) {
	Vector_t s{};
	if (!vm.WorldToScreen(w, s))
		return false;
	out = ImVec2{ s.x, s.y };
	return true;
}

// Project to screen, or screen-edge when off-frustum / behind (view-space, no flip).
bool ProjectOrEdge(const ViewMatrix& vm, const Vector_t& world, ImVec2& out, bool& onScreen,
	const Vector_t& eye, const QAngle_t& viewAng)
{
	(void)vm;
	float ox = 0.f, oy = 0.f;
	if (!W2S::ProjectOrEdge(world, ox, oy, onScreen, 28.f, eye, viewAng))
		return false;
	out = ImVec2{ ox, oy };
	return true;
}

void DrawOffscreenHint(ImDrawList* dl, const ImVec2& edge, const ImVec2& center,
	float alpha, const ImVec4& col, const char* name, float dist)
{
	if (!dl || alpha < 0.05f)
		return;
	float dx = edge.x - center.x;
	float dy = edge.y - center.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len > 0.001f) { dx /= len; dy /= len; }
	else { dx = 0.f; dy = -1.f; }

	const float px = -dy, py = dx;
	const float tip = 11.f;
	const float base = 6.5f;
	const ImVec2 t0{ edge.x + dx * tip, edge.y + dy * tip };
	const ImVec2 t1{ edge.x - dx * 2.5f + px * base, edge.y - dy * 2.5f + py * base };
	const ImVec2 t2{ edge.x - dx * 2.5f - px * base, edge.y - dy * 2.5f - py * base };
	const ImU32 fill = ColA(col, alpha * 0.92f);
	const ImU32 ink = ColRGB(0, 0, 0, alpha * 0.60f);
	// Soft glow under arrow
	dl->AddTriangleFilled(
		ImVec2(t0.x + dx * 2.f, t0.y + dy * 2.f),
		ImVec2(t1.x - px * 2.f, t1.y - py * 2.f),
		ImVec2(t2.x + px * 2.f, t2.y + py * 2.f),
		ColA(col, alpha * 0.22f));
	dl->AddTriangleFilled(t0, t1, t2, fill);
	dl->AddTriangle(t0, t1, t2, ink, 1.15f);

	char line[64]{};
	const float meters = dist * kUnitsToM;
	if (name && name[0])
		std::snprintf(line, sizeof(line), "%s  ·  %.0fm", name, meters);
	else
		std::snprintf(line, sizeof(line), "%.0fm", meters);
	const ImVec2 ts = ImGui::CalcTextSize(line);
	const float padX = 5.f, padY = 2.f;
	ImVec2 tp{ edge.x - dx * 18.f - ts.x * 0.5f, edge.y - dy * 18.f - ts.y * 0.5f };
	const ImVec2 mn{ floorf(tp.x - padX), floorf(tp.y - padY) };
	const ImVec2 mx{ mn.x + ts.x + padX * 2.f, mn.y + ts.y + padY * 2.f };
	dl->AddRectFilled(mn, mx, ColRGB(8, 10, 14, alpha * 0.82f), 4.f);
	dl->AddRect(mn, mx, ColA(col, alpha * 0.45f), 4.f, 0, 1.0f);
	dl->AddText(ImVec2(mn.x + padX + 1.f, mn.y + padY + 1.f), ColRGB(0, 0, 0, alpha * 0.65f), line);
	dl->AddText(ImVec2(mn.x + padX, mn.y + padY), ColA(col, alpha), line);
}

// Ground ring (soft fill + dual stroke). Returns false if nothing projected.
bool DrawStandPad(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
	float radius, ImU32 ringCol, ImU32 fillCol, int segs = 36, bool active = false)
{
	if (!dl || !vm.viewMatrix || radius <= 1.f)
		return false;
	if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z))
		return false;

	ImVec2 pts[64];
	int n = 0;
	const int maxSegs = (std::min)(segs, 63);
	for (int i = 0; i <= maxSegs; ++i) {
		const float a = (static_cast<float>(i) / maxSegs) * 6.2831853f;
		const Vector_t w{
			center.x + std::cos(a) * radius,
			center.y + std::sin(a) * radius,
			center.z + 1.5f
		};
		ImVec2 s{};
		if (!WorldToScreen2(vm, w, s))
			continue;
		if (!std::isfinite(s.x) || !std::isfinite(s.y))
			continue;
		// Partial W2S (behind camera) can produce huge coords — skip fill if wild
		if (s.x < -4000.f || s.x > 8000.f || s.y < -4000.f || s.y > 8000.f)
			continue;
		if (n < 64)
			pts[n++] = s;
	}
	if (n < 3)
		return false;

	// Only fill when enough of the circle projected (partial poly looks broken)
	const bool fillOk = n >= (maxSegs * 3) / 4;
	if (fillOk && (fillCol >> IM_COL32_A_SHIFT) > 0)
		dl->AddConvexPolyFilled(pts, n, fillCol);

	// Outer soft + main ring
	const float thick = active ? 1.65f : 1.2f;
	for (int i = 0; i + 1 < n; ++i)
		dl->AddLine(pts[i], pts[i + 1], ringCol, thick);
	if (n >= 2)
		dl->AddLine(pts[n - 1], pts[0], ringCol, thick);

	// Center crosshair tick (helps feet alignment)
	ImVec2 mid{};
	for (int i = 0; i < n; ++i) {
		mid.x += pts[i].x;
		mid.y += pts[i].y;
	}
	const float inv = 1.f / static_cast<float>(n);
	mid.x *= inv;
	mid.y *= inv;
	const float tick = active ? 5.5f : 4.f;
	const ImU32 tickCol = ringCol;
	dl->AddLine(ImVec2(mid.x - tick, mid.y), ImVec2(mid.x + tick, mid.y), tickCol, 1.0f);
	dl->AddLine(ImVec2(mid.x, mid.y - tick), ImVec2(mid.x, mid.y + tick), tickCol, 1.0f);
	dl->AddCircleFilled(mid, active ? 1.6f : 1.2f, tickCol, 8);

	return true;
}

void DrawFeetCross(ImDrawList* dl, const ImVec2& c, float size, ImU32 col, ImU32 outline) {
	(void)outline;
	const float s = size > 1.f ? size : 2.5f;
	dl->AddCircleFilled(c, s + 1.2f, ColRGB(0, 0, 0, 0.45f), 10);
	dl->AddCircleFilled(c, s, col, 10);
	dl->AddCircleFilled(c, s * 0.35f, IM_COL32(255, 255, 255, 220), 8);
}

static const char* KindWeaponKey(NadeKind k) {
	switch (k) {
	case NadeKind::HE: return "hegrenade";
	case NadeKind::Flash: return "flashbang";
	case NadeKind::Smoke: return "smokegrenade";
	case NadeKind::Molly: return "molotov";
	case NadeKind::Decoy: return "decoy";
	default: return nullptr;
	}
}

// Utility glyph on soft disc — stand widget.
void DrawKindIcon(ImDrawList* dl, const ImVec2& c, NadeKind kind, float alpha, float pxSize,
	const ImVec4& accent, bool locked = false)
{
	if (!dl || alpha < 0.05f || !g_WeaponIconFont || pxSize < 6.f)
		return;
	const char* key = KindWeaponKey(kind);
	if (!key)
		return;
	auto it = weapon_icons::icon_table.find(key);
	if (it == weapon_icons::icon_table.end() || it->second.empty())
		return;
	const char* glyph = it->second.c_str();
	const float discR = pxSize * 0.70f;
	const float pulse = locked
		? (0.85f + 0.15f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f))
		: 1.f;

	// Soft glow
	dl->AddCircleFilled(c, discR + 5.f, ColA(accent, alpha * 0.18f * pulse), 28);
	dl->AddCircleFilled(c, discR + 1.8f, ColRGB(0, 0, 0, alpha * 0.50f), 28);
	dl->AddCircleFilled(c, discR, ColRGB(10, 12, 16, alpha * 0.94f), 28);
	dl->AddCircle(c, discR, ColA(accent, alpha * 0.85f * pulse), 28, locked ? 1.55f : 1.2f);

	const ImVec2 isz = g_WeaponIconFont->CalcTextSizeA(pxSize, FLT_MAX, 0.f, glyph);
	const float ix = floorf(c.x - isz.x * 0.5f);
	const float iy = floorf(c.y - isz.y * 0.55f);
	const int a = static_cast<int>(std::clamp(alpha * 255.f, 0.f, 255.f));
	dl->AddText(g_WeaponIconFont, pxSize, ImVec2(ix + 1.f, iy + 1.f), IM_COL32(0, 0, 0, (a * 180) / 255), glyph);
	dl->AddText(g_WeaponIconFont, pxSize, ImVec2(ix, iy), IM_COL32(250, 250, 252, a), glyph);
}

// Aim marker: reticle + throw chip. locked = full; preview = dim.
void DrawAimReticle(ImDrawList* dl, const ImVec2& c, float a, const ImVec4& accent, bool locked,
	const char* throwLabel, const char* kindLabel)
{
	if (a < 0.04f)
		return;
	const float pulse = locked
		? (0.80f + 0.20f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.6f))
		: 1.f;
	const float r = locked ? 4.0f : 2.6f;
	const float arm = locked ? 9.f : 6.f;
	const float gap = locked ? 3.2f : 2.4f;
	const ImU32 ink = ColRGB(0, 0, 0, a * 0.55f);
	const ImU32 ring = ColA(accent, a * 0.92f * pulse);
	const ImU32 core = ColRGB(255, 255, 255, a * 0.98f);

	// Soft glow
	dl->AddCircleFilled(c, r + 6.f, ColA(accent, a * 0.16f * pulse), 24);
	// Crosshair arms
	dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x - gap, c.y), ink, 2.2f);
	dl->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + arm, c.y), ink, 2.2f);
	dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y - gap), ink, 2.2f);
	dl->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + arm), ink, 2.2f);
	dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x - gap, c.y), ring, 1.25f);
	dl->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + arm, c.y), ring, 1.25f);
	dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y - gap), ring, 1.25f);
	dl->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + arm), ring, 1.25f);
	// Core disc
	dl->AddCircleFilled(c, r + 1.4f, ink, 18);
	dl->AddCircleFilled(c, r, ring, 18);
	dl->AddCircleFilled(c, r * 0.38f, core, 12);

	// Chip: "Stand+Jump · Smoke"
	char chip[48]{};
	if (throwLabel && throwLabel[0] && kindLabel && kindLabel[0] && locked)
		std::snprintf(chip, sizeof(chip), "%s  ·  %s", throwLabel, kindLabel);
	else if (throwLabel && throwLabel[0])
		std::snprintf(chip, sizeof(chip), "%s", throwLabel);
	else if (kindLabel && kindLabel[0])
		std::snprintf(chip, sizeof(chip), "%s", kindLabel);
	if (!chip[0])
		return;

	const ImVec2 ts = ImGui::CalcTextSize(chip);
	const float padX = 6.f, padY = 2.5f;
	const ImVec2 min{ floorf(c.x - ts.x * 0.5f - padX), floorf(c.y + r + 8.f) };
	const ImVec2 max{ min.x + ts.x + padX * 2.f, min.y + ts.y + padY * 2.f };
	dl->AddRectFilled(min, max, ColRGB(8, 10, 14, a * 0.88f), 4.f);
	dl->AddRect(min, max, ColA(accent, a * 0.55f), 4.f, 0, 1.0f);
	dl->AddRectFilled(ImVec2(min.x, min.y + 1.5f), ImVec2(min.x + 2.3f, max.y - 1.5f),
		ColA(accent, a), 2.f);
	dl->AddText(ImVec2(min.x + padX + 1.f, min.y + padY + 1.f), ColRGB(0, 0, 0, a * 0.55f), chip);
	dl->AddText(ImVec2(min.x + padX, min.y + padY), ColA(accent, a), chip);
}

void DrawLabelChip(ImDrawList* dl, const ImVec2& anchor, const char* title,
	const char* meta, float alpha, const ImVec4& accent, bool active)
{
	if (!title || !title[0] || alpha < 0.04f)
		return;

	const ImVec2 titleSz = ImGui::CalcTextSize(title);
	const ImVec2 metaSz = (meta && meta[0]) ? ImGui::CalcTextSize(meta) : ImVec2{ 0.f, 0.f };
	const float padX = 8.f;
	const float padY = 4.f;
	const float gap = (meta && meta[0]) ? 2.f : 0.f;
	const float w = (std::max)(titleSz.x, metaSz.x) + padX * 2.f + 3.f;
	const float h = titleSz.y + (metaSz.y > 0.f ? metaSz.y + gap : 0.f) + padY * 2.f;

	const ImVec2 min{ floorf(anchor.x - w * 0.5f), floorf(anchor.y - h - 16.f) };
	const ImVec2 max{ min.x + w, min.y + h };
	const float round = 5.f;

	dl->AddRectFilled(min, max, ColRGB(8, 10, 14, alpha * 0.88f), round);
	dl->AddRect(min, max,
		active ? ColA(accent, alpha * 0.75f) : ColRGB(255, 255, 255, alpha * 0.08f),
		round, 0, active ? 1.15f : 1.0f);
	// Left accent bar
	dl->AddRectFilled(ImVec2(min.x, min.y + 2.f), ImVec2(min.x + 2.5f, max.y - 2.f),
		ColA(accent, alpha * (active ? 1.f : 0.70f)), 2.f);

	const float tx = floorf(min.x + padX);
	const float ty = floorf(min.y + padY);
	dl->AddText(ImVec2(tx + 1.f, ty + 1.f), ColRGB(0, 0, 0, alpha * 0.55f), title);
	dl->AddText(ImVec2(tx, ty), ColRGB(240, 242, 246, alpha), title);
	if (meta && meta[0]) {
		const float my = ty + titleSz.y + gap;
		dl->AddText(ImVec2(tx + 1.f, my + 1.f), ColRGB(0, 0, 0, alpha * 0.45f), meta);
		dl->AddText(ImVec2(tx, my), ColA(accent, alpha * 0.90f), meta);
	}
}

void DrawGuideLine(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col, bool locked) {
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len < 14.f)
		return;
	const float inv = 1.f / len;
	const float trim = locked ? 16.f : 12.f;
	const ImVec2 p0{ a.x + dx * (trim * inv), a.y + dy * (trim * inv) };
	const ImVec2 p1{ a.x + dx * ((len - trim) * inv), a.y + dy * ((len - trim) * inv) };
	// Dark understroke + color
	dl->AddLine(p0, p1, IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) / 2), locked ? 2.2f : 1.6f);
	dl->AddLine(p0, p1, col, locked ? 1.35f : 1.0f);
}

void SyncMapCache() {
	GameMode::EnsureMap();
	const char* m = GameMode::BaseMap();
	if (m && m[0])
		std::snprintf(g_mapCache, sizeof(g_mapCache), "%s", m);
	else
		g_mapCache[0] = '\0';
}

C_CSPlayerPawn* GetLocalSeh() {
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (!local || !Mem::ValidEntity(local))
		return nullptr;
	return local;
}

C_CSWeaponBase* GetWeaponSeh(C_CSPlayerPawn* local) {
	if (!local)
		return nullptr;
	C_CSWeaponBase* wep = nullptr;
	__try {
		wep = local->GetActiveWeapon();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	if (!wep || !Mem::ValidEntity(wep))
		return nullptr;
	return wep;
}

void CopyMapNameSeh(const char* mapName, char* out, size_t outSz) {
	if (!out || outSz == 0)
		return;
	out[0] = '\0';
	if (!mapName || !Mem::IsReadable(mapName, 1))
		return;
	char tmp[128]{};
	__try {
		if (mapName[0] && mapName[0] > 32) {
			std::snprintf(tmp, sizeof(tmp), "%s", mapName);
			for (char* p = tmp; *p; ++p) {
				if (*p == '.') { *p = '\0'; break; }
			}
			const char* base = tmp;
			for (char* p = tmp; *p; ++p) {
				if (*p == '/' || *p == '\\')
					base = p + 1;
			}
			std::snprintf(out, outSz, "%s", base);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out[0] = '\0';
	}
}

} // namespace

const char* KindName(NadeKind k) {
	switch (k) {
	case NadeKind::HE: return "HE";
	case NadeKind::Flash: return "Flash";
	case NadeKind::Smoke: return "Smoke";
	case NadeKind::Molly: return "Molly";
	case NadeKind::Decoy: return "Decoy";
	default: return "Any";
	}
}

const char* ThrowName(ThrowType t) {
	switch (t) {
	case ThrowType::Walk: return "Walk";
	case ThrowType::Run: return "Run";
	case ThrowType::Jump: return "Stand+Jump";
	case ThrowType::Crouch: return "Crouch";
	case ThrowType::RunJump: return "Run+Jump";
	case ThrowType::Stand:
	default: return "Stand";
	}
}

const std::vector<Lineup>& All() { return g_lineups; }
const Lineup* Current() {
	if (g_currentIdx < 0 || g_currentIdx >= static_cast<int>(g_lineups.size()))
		return nullptr;
	return &g_lineups[static_cast<size_t>(g_currentIdx)];
}
const char* CurrentMap() {
	SyncMapCache();
	return g_mapCache;
}

bool IsCurrentMap(const Lineup& L) {
	SyncMapCache();
	if (!g_mapCache[0] || L.map.empty())
		return false;
	return MapsEqual(L.map, g_mapCache);
}

int CountCurrentMap() {
	SyncMapCache();
	if (!g_mapCache[0])
		return 0;
	EnsureMapIndex();
	return static_cast<int>(g_mapIndices.size());
}

void Init() {
	if (!g_loaded)
		Load();
}

void Shutdown() {
	g_currentIdx = -1;
	g_cap = {};
	g_lineups.clear();
	g_loaded = false;
	InvalidateMapIndex();
	g_captureKeyWasDown = false;
}

void OnLevelInit(const char* mapName) {
	g_currentIdx = -1;
	g_cap = {};
	CopyMapNameSeh(mapName, g_mapCache, sizeof(g_mapCache));
	InvalidateMapIndex();
	if (!g_loaded)
		Load();
}

// Sample pawn + keys once per armed frame.
// Refresh aim every frame. Motion uses recency + while-attack latches (not whole-session).
static void SampleCaptureMotion(C_CSPlayerPawn* local) {
	if (!local || !g_cap.armed)
		return;

	// Live aim while armed — arm-only freeze saved wrong angles after re-aim.
	QAngle_t liveAng{};
	if (GetLocalViewAngles(liveAng) && IsFiniteAng(liveAng))
		g_cap.ang = liveAng;

	std::uint32_t flags = 0;
	Vector_t vel{};
	__try {
		flags = local->m_fFlags();
		vel = local->m_vecAbsVelocity();
		if (!IsFiniteVec(vel))
			vel = local->m_vecVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		flags = 0;
		vel = Vector_t{};
	}

	const ULONGLONG now = GetTickCount64();
	const float speed2d = (std::isfinite(vel.x) && std::isfinite(vel.y))
		? std::sqrt(vel.x * vel.x + vel.y * vel.y) : 0.f;
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool ducking = (flags & (FL_DUCKING | FL_ANIMDUCKING)) != 0
		|| (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0
		|| (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
	const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	// Jump: space or rising only — bare airborne latched whole sessions as Jump.
	const bool rising = std::isfinite(vel.z) && vel.z > 40.f;
	const bool jumpNow = spaceDown || rising;
	// Jumpthrow apex: airborne while actually pinning attack (not post-throw).
	const bool airWhileAtk = !onGround && g_cap.attackHeld && g_cap.sawAttack;

	if (speed2d > 135.f) {
		g_cap.lastRunMs = now;
		if (g_cap.attackHeld && g_cap.sawAttack)
			g_cap.runWhileAtk = true;
	}
	if (ducking) {
		g_cap.lastDuckMs = now;
		if (g_cap.attackHeld && g_cap.sawAttack)
			g_cap.duckWhileAtk = true;
	}
	if (jumpNow || airWhileAtk) {
		g_cap.lastJumpMs = now;
		if (g_cap.attackHeld && g_cap.sawAttack)
			g_cap.jumpWhileAtk = true;
	}
}

static bool RecentMs(ULONGLONG stamp, ULONGLONG now, ULONGLONG window) {
	return stamp != 0 && now >= stamp && (now - stamp) <= window;
}

static ThrowType DetectThrowStyle(C_CSPlayerPawn* local, const CaptureSession& cap) {
	if (!local)
		return ThrowType::Stand;

	std::uint32_t flags = 0;
	Vector_t vel{};
	__try {
		flags = local->m_fFlags();
		vel = local->m_vecAbsVelocity();
		if (!IsFiniteVec(vel))
			vel = local->m_vecVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		flags = 0;
		vel = Vector_t{};
	}

	const ULONGLONG now = GetTickCount64();
	const float speed2d = (std::isfinite(vel.x) && std::isfinite(vel.y))
		? std::sqrt(vel.x * vel.x + vel.y * vel.y) : 0.f;
	const bool onGround = (flags & FL_ONGROUND) != 0;
	const bool duckingNow = (flags & (FL_DUCKING | FL_ANIMDUCKING)) != 0;
	const bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	const bool shiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0
		|| (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
	const bool rising = std::isfinite(vel.z) && vel.z > 40.f;

	// Prefer while-attack latches (real throw motion). Recent stamps only
	// for bind lag after release — ignore bare airborne from walk-to-spot hop.
	const bool jumping = cap.jumpWhileAtk
		|| RecentMs(cap.lastJumpMs, now, kMotionRecentMs)
		|| spaceDown || rising;
	const bool running = cap.runWhileAtk
		|| RecentMs(cap.lastRunMs, now, kMotionRecentMs)
		|| (speed2d > 135.f && !shiftDown && onGround);
	const bool ducking = cap.duckWhileAtk
		|| RecentMs(cap.lastDuckMs, now, kMotionRecentMs)
		|| duckingNow;

	if (jumping && running)
		return ThrowType::RunJump;
	if (jumping)
		return ThrowType::Jump;
	if (ducking)
		return ThrowType::Crouch;
	if (shiftDown || (speed2d > 25.f && speed2d <= 135.f && onGround))
		return ThrowType::Walk;
	if (running || speed2d > 135.f)
		return ThrowType::Run;
	return ThrowType::Stand;
}

bool IsCapturing() { return g_cap.armed; }

void CancelCapture() {
	if (!g_cap.armed)
		return;
	g_cap = {};
	Notify::Warn("Lineup capture", "Cancelled");
}

bool ArmCapture(const char* name, NadeKind kindOverride) {
	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return false;

	Vector_t pos{};
	if (!Bones::GetOrigin(local, pos) || !Bones::IsValidPos(pos))
		return false;
	if (std::fabs(pos.x) < 1.f && std::fabs(pos.y) < 1.f && std::fabs(pos.z) < 1.f)
		return false;

	QAngle_t ang{};
	if (!GetLocalViewAngles(ang) || !IsFiniteAng(ang))
		return false;

	if (!GameMode::EnsureMap()) {
		Notify::Error("Lineup capture", "Could not read map name");
		return false;
	}
	SyncMapCache();
	if (!g_mapCache[0]) {
		Notify::Error("Lineup capture", "Map name empty");
		return false;
	}

	g_cap = {};
	g_cap.armed = true;
	g_cap.pos = pos; // feet locked at arm (lineup stand pad)
	g_cap.ang = ang; // aim refreshed every SampleCaptureMotion frame
	g_cap.armMs = GetTickCount64();
	g_cap.attackHeld = false;
	g_cap.sawAttack = false;
	g_cap.jumpWhileAtk = false;
	g_cap.runWhileAtk = false;
	g_cap.duckWhileAtk = false;
	g_cap.lastJumpMs = 0;
	g_cap.lastRunMs = 0;
	g_cap.lastDuckMs = 0;
	g_cap.attackDownMs = 0;
	std::snprintf(g_cap.name, sizeof(g_cap.name), "%s",
		(name && name[0]) ? name : "Lineup");
	// Do not sample motion yet — ignore residual LMB from Arm button / keybind.

	if (kindOverride != NadeKind::Any)
		g_cap.kind = kindOverride;
	else
		g_cap.kind = ClassifyHeld(GetWeaponSeh(local));

	Notify::Info("CAPTURING", "Throw the nade — Stand+Jump/Run+Jump/Walk auto-detected. Press capture again to cancel.");
	Con::Ok("NadeLineup armed capture \"%s\" kind=%s — waiting for throw",
		g_cap.name, KindName(g_cap.kind));
	return true;
}

bool Capture(const char* name, ThrowType throwType, NadeKind kindOverride) {
	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return false;

	Vector_t pos{};
	if (!Bones::GetOrigin(local, pos) || !Bones::IsValidPos(pos))
		return false;
	if (std::fabs(pos.x) < 1.f && std::fabs(pos.y) < 1.f && std::fabs(pos.z) < 1.f)
		return false;

	QAngle_t ang{};
	if (!GetLocalViewAngles(ang) || !IsFiniteAng(ang))
		return false;

	Lineup L{};
	L.name = (name && name[0]) ? name : "Lineup";
	if (!GameMode::EnsureMap()) {
		Con::Error("NadeLineup capture failed — could not read map name");
		return false;
	}
	SyncMapCache();
	const char* map = g_mapCache[0] ? g_mapCache : GameMode::BaseMap();
	if (!map || !map[0]) {
		Con::Error("NadeLineup capture failed — map name empty after EnsureMap");
		return false;
	}
	L.map = map;
	L.throwType = throwType;
	L.pos = pos;
	L.aimAngles = ang;
	L.target = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);

	if (kindOverride != NadeKind::Any)
		L.kind = kindOverride;
	else
		L.kind = ClassifyHeld(GetWeaponSeh(local));

	g_lineups.push_back(std::move(L));
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();
	Con::Ok("NadeLineup captured \"%s\" map=%s kind=%s throw=%s ang=%.1f/%.1f",
		g_lineups.back().name.c_str(),
		g_lineups.back().map.c_str(),
		KindName(g_lineups.back().kind),
		ThrowName(g_lineups.back().throwType),
		g_lineups.back().aimAngles.x, g_lineups.back().aimAngles.y);
	char toast[96]{};
	std::snprintf(toast, sizeof(toast), "%s · %s",
		g_lineups.back().name.c_str(), ThrowName(g_lineups.back().throwType));
	Notify::Success("Lineup saved", toast);
	return true;
}

static bool FinishArmedCapture(C_CSPlayerPawn* local) {
	if (!g_cap.armed || !local)
		return false;

	// Refresh map if arm happened mid-load / empty cache
	SyncMapCache();
	if (!g_mapCache[0]) {
		GameMode::EnsureMap();
		SyncMapCache();
	}
	if (!g_mapCache[0]) {
		Notify::Error("Lineup capture", "Map name empty");
		g_cap = {};
		return false;
	}
	if (!IsFiniteVec(g_cap.pos) || !IsFiniteAng(g_cap.ang)
		|| (std::fabs(g_cap.pos.x) < 1.f && std::fabs(g_cap.pos.y) < 1.f && std::fabs(g_cap.pos.z) < 1.f)) {
		Notify::Error("Lineup capture", "Invalid stand / angles");
		g_cap = {};
		return false;
	}

	// Last sample: refresh aim + motion window at mouse release
	SampleCaptureMotion(local);
	const ThrowType style = DetectThrowStyle(local, g_cap);
	Lineup L{};
	L.name = g_cap.name[0] ? g_cap.name : "Lineup";
	L.map = g_mapCache;
	L.throwType = style;
	L.pos = g_cap.pos; // stand feet from arm
	// Prefer live aim at throw (Sample already wrote g_cap.ang)
	L.aimAngles = g_cap.ang;
	if (!IsFiniteAng(L.aimAngles)) {
		QAngle_t a{};
		if (GetLocalViewAngles(a) && IsFiniteAng(a))
			L.aimAngles = a;
	}
	L.target = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);
	L.kind = g_cap.kind;
	if (L.kind == NadeKind::Any)
		L.kind = ClassifyHeld(GetWeaponSeh(local));

	g_lineups.push_back(std::move(L));
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();

	char msg[96]{};
	std::snprintf(msg, sizeof(msg), "%s · %s · %s",
		g_lineups.back().name.c_str(),
		KindName(g_lineups.back().kind),
		ThrowName(g_lineups.back().throwType));
	Notify::Success("Lineup saved", msg);
	Con::Ok("NadeLineup auto-captured \"%s\" throw=%s",
		g_lineups.back().name.c_str(), ThrowName(g_lineups.back().throwType));
	g_cap = {};
	return true;
}

bool RemoveAt(int index) {
	if (index < 0 || index >= static_cast<int>(g_lineups.size()))
		return false;
	g_lineups.erase(g_lineups.begin() + index);
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();
	return true;
}

void ClearCurrentMap() {
	SyncMapCache();
	g_lineups.erase(std::remove_if(g_lineups.begin(), g_lineups.end(),
		[](const Lineup& L) { return MapsEqual(L.map, g_mapCache); }),
		g_lineups.end());
	g_currentIdx = -1;
	InvalidateMapIndex();
	Save();
}

bool Save() {
	nlohmann::json j = nlohmann::json::array();
	for (const auto& L : g_lineups) {
		j.push_back({
			{ "name", L.name },
			{ "map", L.map },
			{ "kind", static_cast<int>(L.kind) },
			{ "throw", static_cast<int>(L.throwType) },
			{ "pos", { L.pos.x, L.pos.y, L.pos.z } },
			{ "ang", { L.aimAngles.x, L.aimAngles.y, L.aimAngles.z } },
			{ "target", { L.target.x, L.target.y, L.target.z } }
		});
	}
	const auto path = LineupFilePath();
	std::ofstream out(path);
	if (!out)
		return false;
	out << j.dump(2);
	return true;
}

// LefrizzelAi kind: Any=0 HE=1 Flash=2 Smoke=3 Molly=4 Decoy=5
// CS2-DMA Type: Flash=0 Smoke=1 HE=2 Molly=3
static NadeKind KindFromDmaType(int t) {
	switch (t) {
	case 0: return NadeKind::Flash;
	case 1: return NadeKind::Smoke;
	case 2: return NadeKind::HE;
	case 3: return NadeKind::Molly;
	default: return NadeKind::Any;
	}
}

static NadeKind KindFromWeaponString(const std::string& s) {
	std::string l = s;
	for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (l.find("smoke") != std::string::npos) return NadeKind::Smoke;
	if (l.find("flash") != std::string::npos) return NadeKind::Flash;
	if (l.find("molotov") != std::string::npos || l.find("incen") != std::string::npos
		|| l.find("incgrenade") != std::string::npos) return NadeKind::Molly;
	if (l.find("hegrenade") != std::string::npos || l == "he" || l.find("weapon_he") != std::string::npos)
		return NadeKind::HE;
	if (l.find("decoy") != std::string::npos) return NadeKind::Decoy;
	return NadeKind::Any;
}

static ThrowType ClampThrow(int v) {
	if (v < 0 || v >= static_cast<int>(ThrowType::Count))
		return ThrowType::Stand;
	return static_cast<ThrowType>(v);
}

static ThrowType ThrowFromDmaStyle(int style) {
	// DMA: 0 Stand, 1 Run, 2 Jump, 3 Crouch, 4 Run+Jump
	switch (style) {
	case 1: return ThrowType::Run;
	case 2: return ThrowType::Jump;
	case 3: return ThrowType::Crouch;
	case 4: return ThrowType::RunJump;
	case 0:
	default: return ThrowType::Stand;
	}
}

static ThrowType ThrowFromString(std::string tt) {
	for (char& c : tt)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	if (tt.find("RUN") != std::string::npos && tt.find("JUMP") != std::string::npos)
		return ThrowType::RunJump;
	// Stand+Jump / StandJump / Jumpthrow → Jump (stand jumpthrow)
	if (tt.find("JUMP") != std::string::npos)
		return ThrowType::Jump;
	if (tt.find("CROUCH") != std::string::npos || tt.find("DUCK") != std::string::npos
		|| tt.find("SNEAK") != std::string::npos)
		return ThrowType::Crouch;
	if (tt.find("WALK") != std::string::npos || tt.find("SHIFT") != std::string::npos)
		return ThrowType::Walk;
	if (tt.find("RUN") != std::string::npos)
		return ThrowType::Run;
	return ThrowType::Stand;
}

// DMA Chinese names encode style (Style field is often 0). Hex UTF-8 — no /utf-8 needed.
// 跳=\xE8\xB7\xB3 跑=\xE8\xB7\x91 蹲=\xE8\xB9\xB2 静=\xE9\x9D\x99
// 走=\xE8\xB5\xB0 步=\xE6\xAD\xA5 台=\xE5\x8F\xB0 点=\xE7\x82\xB9
// 投=\xE6\x8A\x95 双=\xE5\x8F\x8C 键=\xE9\x94\xAE
static bool ContainsBytes(const std::string& s, const char* bytes) {
	return s.find(bytes) != std::string::npos;
}

static ThrowType ThrowFromName(const std::string& name) {
	if (name.empty())
		return ThrowType::Stand;

	// Prefer instruction after '|' (中路|W跳)
	std::string instr = name;
	const size_t pipe = name.find_last_of('|');
	if (pipe != std::string::npos && pipe + 1 < name.size())
		instr = name.substr(pipe + 1);

	constexpr const char* kJump = "\xE8\xB7\xB3";
	constexpr const char* kRun = "\xE8\xB7\x91";
	constexpr const char* kCrouch = "\xE8\xB9\xB2";
	constexpr const char* kQuiet = "\xE9\x9D\x99";
	constexpr const char* kWalkZ = "\xE8\xB5\xB0";
	constexpr const char* kStep = "\xE6\xAD\xA5";
	constexpr const char* kPlat = "\xE5\x8F\xB0";
	constexpr const char* kPoint = "\xE7\x82\xB9";
	constexpr const char* kThrowCh = "\xE6\x8A\x95";
	constexpr const char* kDouble = "\xE5\x8F\x8C";
	constexpr const char* kKey = "\xE9\x94\xAE";

	// Strip location false-positives 跳台 / 跳点
	std::string cn = instr;
	const std::string jumpPlat = std::string(kJump) + kPlat;
	const std::string jumpPoint = std::string(kJump) + kPoint;
	for (;;) {
		size_t p = cn.find(jumpPlat);
		if (p == std::string::npos) break;
		cn.erase(p, jumpPlat.size());
	}
	for (;;) {
		size_t p = cn.find(jumpPoint);
		if (p == std::string::npos) break;
		cn.erase(p, jumpPoint.size());
	}

	std::string en = name;
	for (char& c : en)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	// English pack suffixes / UI labels first (order matters).
	// "stand+jump" is Jump, not RunJump. "runjump" / "run+jump" is RunJump.
	if (en.find("runjump") != std::string::npos
		|| en.find("run+jump") != std::string::npos
		|| en.find("run + jump") != std::string::npos
		|| en.find("run-jump") != std::string::npos)
		return ThrowType::RunJump;
	// Trailing " - Run" / " - Jump" from our packs (avoid place names with "jump")
	const size_t dash = en.rfind(" - ");
	if (dash != std::string::npos) {
		const std::string tail = en.substr(dash + 3);
		if (tail == "runjump" || tail.find("run+jump") != std::string::npos)
			return ThrowType::RunJump;
		if (tail == "jump" || tail == "stand+jump" || tail == "jumpthrow")
			return ThrowType::Jump;
		if (tail == "crouch" || tail == "duck")
			return ThrowType::Crouch;
		if (tail == "run" || tail == "running")
			return ThrowType::Run;
		if (tail == "walk" || tail == "shift")
			return ThrowType::Walk;
		if (tail == "stand" || tail == "normal")
			return ThrowType::Stand;
	}

	const bool wJump = (instr.find('W') != std::string::npos || instr.find('w') != std::string::npos)
		&& ContainsBytes(instr, kJump);
	const bool runJumpCn = ContainsBytes(cn, kRun) && ContainsBytes(cn, kJump);
	const bool jumpCn = ContainsBytes(cn, kJump)
		|| (ContainsBytes(instr, kJump) && ContainsBytes(instr, kThrowCh))
		|| (ContainsBytes(instr, kDouble) && ContainsBytes(instr, kKey));
	const bool runCn = ContainsBytes(cn, kRun);
	const bool crouchCn = ContainsBytes(cn, kCrouch);
	const bool walkCn = (ContainsBytes(instr, kQuiet) && ContainsBytes(instr, kWalkZ))
		|| (ContainsBytes(instr, kQuiet) && ContainsBytes(instr, kStep));

	// English freeform (user renames / sapphyrus)
	const bool runEn = en.find("running") != std::string::npos
		|| en.find("w+") != std::string::npos
		|| (en.find("run") != std::string::npos
			&& en.find("runjump") == std::string::npos
			&& en.find("run+jump") == std::string::npos);
	const bool jumpEn = en.find("jumpthrow") != std::string::npos
		|| en.find("stand+jump") != std::string::npos
		|| en.find("jump") != std::string::npos;
	const bool crouchEn = en.find("crouch") != std::string::npos || en.find("duck") != std::string::npos;
	const bool walkEn = en.find("walk") != std::string::npos || en.find("silent") != std::string::npos;

	if (wJump || runJumpCn || (runEn && jumpEn && en.find("stand+jump") == std::string::npos))
		return ThrowType::RunJump;
	if (jumpCn || jumpEn)
		return ThrowType::Jump;
	if (crouchCn || crouchEn)
		return ThrowType::Crouch;
	if (runCn || runEn)
		return ThrowType::Run;
	if (walkCn || walkEn)
		return ThrowType::Walk;
	return ThrowType::Stand;
}

// DMA Style field is often 0 while Chinese name encodes jump/run.
// English packs we wrote set both Style + " - Jump" suffix — trust Style when
// non-Stand; use name only to fill Stand under-tags or resolve ambiguity.
static ThrowType ResolveThrow(int stored, const std::string& name) {
	const ThrowType fromStyle = ThrowFromDmaStyle(stored);
	const ThrowType fromName = ThrowFromName(name);
	// Explicit non-Stand Style always wins (packs fixed Style field)
	if (fromStyle != ThrowType::Stand)
		return fromStyle;
	// Style says Stand — allow name to upgrade (DMA under-tag / Chinese)
	if (fromName != ThrowType::Stand)
		return fromName;
	return ThrowType::Stand;
}

static bool PushLineupIfValid(Lineup& L) {
	if (!IsFiniteVec(L.pos) || !IsFiniteAng(L.aimAngles))
		return false;
	// Reject zero / near-zero feet (bad JSON / failed parse)
	if (std::fabs(L.pos.x) < 1.f && std::fabs(L.pos.y) < 1.f && std::fabs(L.pos.z) < 1.f)
		return false;
	if (L.map.empty())
		return false;
	L.target = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);
	g_lineups.push_back(std::move(L));
	return true;
}

// Safe float from JSON number or numeric string (avoids .get throw on bad packs).
static float JsonFloat(const nlohmann::json& v, float def = 0.f) {
	try {
		if (v.is_number_float()) return v.get<float>();
		if (v.is_number_integer()) return static_cast<float>(v.get<std::int64_t>());
		if (v.is_string()) return std::stof(v.get<std::string>());
	} catch (...) {}
	return def;
}

// LefrizzelAi native: { name,map,kind,throw,pos,ang }
// throw is ThrowType enum (0 Stand, 1 Jump, 2 Walk, 3 Run, 4 Crouch, 5 RunJump)
// — NOT DMA Style ints. Prefer stored throw; name only if throw missing/Stand.
static void ParseTempleEntry(const nlohmann::json& e) {
	if (!e.is_object())
		return;
	Lineup L{};
	L.name = e.value("name", "Lineup");
	L.map = e.value("map", "");
	{
		const int k = e.value("kind", 0);
		L.kind = static_cast<NadeKind>(std::clamp(k, 0, 5));
	}
	{
		const int thr = e.value("throw", 0);
		const ThrowType stored = ClampThrow(thr);
		const ThrowType fromName = ThrowFromName(L.name);
		// Own captures: trust throw field. Name upgrade only when throw==Stand.
		L.throwType = (stored != ThrowType::Stand) ? stored
			: (fromName != ThrowType::Stand ? fromName : ThrowType::Stand);
	}
	if (e.contains("pos") && e["pos"].is_array() && e["pos"].size() >= 3) {
		L.pos.x = JsonFloat(e["pos"][0]);
		L.pos.y = JsonFloat(e["pos"][1]);
		L.pos.z = JsonFloat(e["pos"][2]);
	}
	if (e.contains("ang") && e["ang"].is_array() && e["ang"].size() >= 2) {
		L.aimAngles.x = JsonFloat(e["ang"][0]);
		L.aimAngles.y = JsonFloat(e["ang"][1]);
		L.aimAngles.z = (e["ang"].size() >= 3) ? JsonFloat(e["ang"][2]) : 0.f;
	}
	PushLineupIfValid(L);
}

// CS2-DMA grenade-helper: { "infos":[ { Name,Style,Type,Position[],Angle[] } ] }
static void ParseDmaInfos(const nlohmann::json& root, const std::string& mapHint) {
	if (!root.contains("infos") || !root["infos"].is_array())
		return;
	for (const auto& e : root["infos"]) {
		if (!e.is_object())
			continue;
		Lineup L{};
		L.name = e.value("Name", e.value("name", "Lineup"));
		// Prefer entry map field if present; else filename hint
		L.map = e.value("Map", e.value("map", mapHint));
		if (L.map.empty())
			L.map = mapHint;
		L.kind = KindFromDmaType(e.value("Type", 1));
		// Style is DMA int 0..4 — ResolveThrow maps once (do NOT pre-map ThrowType)
		L.throwType = ResolveThrow(e.value("Style", 0), L.name);
		if (e.contains("Position") && e["Position"].is_array() && e["Position"].size() >= 3) {
			L.pos.x = JsonFloat(e["Position"][0]);
			L.pos.y = JsonFloat(e["Position"][1]);
			L.pos.z = JsonFloat(e["Position"][2]);
		}
		if (e.contains("Angle") && e["Angle"].is_array() && e["Angle"].size() >= 2) {
			L.aimAngles.x = JsonFloat(e["Angle"][0]);
			L.aimAngles.y = JsonFloat(e["Angle"][1]);
			L.aimAngles.z = 0.f;
		}
		PushLineupIfValid(L);
	}
}

// sapphyrus / AimTux style (often CS:GO coords — may be off on remade maps):
// { "csgo_grenades":[ { map,name,grenade,throwType,x,y,z,pitch,yaw } ] }
static void ParseSapphyrus(const nlohmann::json& root) {
	const char* key = root.contains("csgo_grenades") ? "csgo_grenades"
		: (root.contains("grenades") ? "grenades" : nullptr);
	if (!key || !root[key].is_array())
		return;
	for (const auto& e : root[key]) {
		if (!e.is_object())
			continue;
		Lineup L{};
		L.name = e.value("name", "Lineup");
		L.map = e.value("map", "");
		L.kind = KindFromWeaponString(e.value("grenade", e.value("weapon", "")));
		// Sapphyrus throwType is a string — map to ThrowType, then name can only
		// upgrade Stand (same policy as Temple).
		{
			const ThrowType fromStr = ThrowFromString(
				e.value("throwType", e.value("throw_type", "NORMAL")));
			const ThrowType fromName = ThrowFromName(L.name);
			L.throwType = (fromStr != ThrowType::Stand) ? fromStr
				: (fromName != ThrowType::Stand ? fromName : ThrowType::Stand);
		}
		try {
			L.pos.x = std::stof(e.value("x", "0"));
			L.pos.y = std::stof(e.value("y", "0"));
			L.pos.z = std::stof(e.value("z", "0"));
			L.aimAngles.x = std::stof(e.value("pitch", "0"));
			L.aimAngles.y = std::stof(e.value("yaw", "0"));
			L.aimAngles.z = 0.f;
		} catch (...) {
			continue;
		}
		PushLineupIfValid(L);
	}
}

static void IngestJsonFile(const std::filesystem::path& path, const std::string& mapHint) {
	std::ifstream in(path);
	if (!in)
		return;
	nlohmann::json j;
	try {
		in >> j;
	} catch (...) {
		return;
	}
	// packs/: accept DMA / sapphyrus / Temple. Main lineups.json is Temple array.
	if (j.is_array()) {
		for (const auto& e : j)
			ParseTempleEntry(e);
		return;
	}
	if (!j.is_object())
		return;
	if (j.contains("infos"))
		ParseDmaInfos(j, mapHint);
	else if (j.contains("csgo_grenades") || j.contains("grenades"))
		ParseSapphyrus(j);
	else if (j.contains("name") || j.contains("pos"))
		ParseTempleEntry(j);
}

static std::string MapHintFromFilename(const std::filesystem::path& path) {
	std::string stem = path.stem().string();
	// de_mirage.json / de_mirage_lineups.json → de_mirage
	if (stem.rfind("de_", 0) == 0 || stem.rfind("cs_", 0) == 0) {
		const auto us = stem.find('_', 3);
		if (us != std::string::npos && us > 3) {
			// keep full de_mirage even if extra suffix after second token? prefer whole stem if starts de_
		}
		// strip common suffixes
		const char* suffixes[] = { "_lineups", "_grenades", "_helper", "-lineups" };
		for (const char* s : suffixes) {
			const size_t n = std::strlen(s);
			if (stem.size() > n && _stricmp(stem.c_str() + stem.size() - n, s) == 0) {
				stem.resize(stem.size() - n);
				break;
			}
		}
		return stem;
	}
	return stem;
}

bool Load() {
	g_lineups.clear();
	g_currentIdx = -1;
	g_loaded = true;
	InvalidateMapIndex();

	auto path = LineupFilePath();
	bool fromLegacy = false;
	if (!std::filesystem::exists(path)) {
		const auto legacy = LegacyLineupFilePath();
		if (std::filesystem::exists(legacy)) {
			path = legacy;
			fromLegacy = true;
		}
	}

	size_t mainCount = g_lineups.size();
	if (std::filesystem::exists(path))
		IngestJsonFile(path, "");
	mainCount = g_lineups.size();

	// Drop community packs next to lineups.json:
	//   Documents/Lefrizzel AI/NadeLineups/packs/de_mirage.json
	const auto packs = LineupFolder() / "packs";
	std::error_code ec;
	if (std::filesystem::is_directory(packs, ec)) {
		for (const auto& ent : std::filesystem::directory_iterator(packs, ec)) {
			if (ec) break;
			if (!ent.is_regular_file())
				continue;
			const auto ext = ent.path().extension().string();
			if (_stricmp(ext.c_str(), ".json") != 0)
				continue;
			if (std::filesystem::equivalent(ent.path(), path, ec))
				continue;
			// Skip Temple-format pack copies when lineups.json already loaded (avoids 2x merge)
			if (mainCount > 0) {
				std::ifstream peek(ent.path());
				if (peek) {
					nlohmann::json pj;
					try { peek >> pj; } catch (...) { continue; }
					if (pj.is_array())
						continue; // temple duplicate of main file
					if (pj.is_object() && pj.contains("infos"))
						ParseDmaInfos(pj, MapHintFromFilename(ent.path()));
					else if (pj.is_object() && (pj.contains("csgo_grenades") || pj.contains("grenades")))
						ParseSapphyrus(pj);
					continue;
				}
			}
			IngestJsonFile(ent.path(), MapHintFromFilename(ent.path()));
		}
	}

	// Collapse near-identical stand+angle copies (main file + packs overlap)
	{
		std::vector<Lineup> uniq;
		uniq.reserve(g_lineups.size());
		constexpr float kPosEps = 8.f;
		constexpr float kAngEps = 1.5f;
		for (auto& L : g_lineups) {
			bool dup = false;
			for (const auto& U : uniq) {
				if (_stricmp(L.map.c_str(), U.map.c_str()) != 0)
					continue;
				if (L.kind != U.kind)
					continue;
				if (L.throwType != U.throwType)
					continue;
				const float dx = L.pos.x - U.pos.x;
				const float dy = L.pos.y - U.pos.y;
				const float dz = L.pos.z - U.pos.z;
				if (dx * dx + dy * dy + dz * dz > kPosEps * kPosEps)
					continue;
				if (std::fabs(L.aimAngles.x - U.aimAngles.x) > kAngEps)
					continue;
				if (std::fabs(L.aimAngles.y - U.aimAngles.y) > kAngEps)
					continue;
				dup = true;
				break;
			}
			if (!dup)
				uniq.push_back(std::move(L));
		}
		g_lineups.swap(uniq);
	}

	InvalidateMapIndex();
	if (!g_lineups.empty() && fromLegacy)
		Save();
	Con::Ok("NadeLineup loaded %zu entries", g_lineups.size());
	return true;
}

void Update() {
	if (!Config::nade_lineup) {
		if (g_cap.armed)
			g_cap = {};
		return;
	}
	// Never reload every frame on empty list (disk thrash). Load once if not loaded.
	if (!g_loaded)
		Load();
	SyncMapCache();
	if (!g_mapCache[0]) {
		GameMode::EnsureMap();
		SyncMapCache();
	}

	// Capture key: arm session (or cancel if already armed)
	const bool keyDown = Config::nade_lineup_capture && keybind.isActive(Config::nade_lineup_capture);
	if (keyDown && !g_captureKeyWasDown) {
		if (g_cap.armed)
			CancelCapture();
		else {
			ArmCapture(Config::nade_lineup_capture_name,
				static_cast<NadeKind>(std::clamp(Config::nade_lineup_capture_kind, 0, 5)));
		}
	}
	g_captureKeyWasDown = keyDown;

	// Armed: wait for real grenade pin + release. Sample every frame (aim + motion).
	if (g_cap.armed) {
		const ULONGLONG now = GetTickCount64();
		if (now - g_cap.armMs > kCaptureTimeoutMs) {
			g_cap = {};
			Notify::Warn("Lineup capture", "Timed out");
		} else if (C_CSPlayerPawn* local = GetLocalSeh()) {
			// Ignore residual LMB from Arm button / capture keybind chord
			const bool armGrace = (now - g_cap.armMs) < kArmIgnoreAtkMs;
			const bool wantMouse = ImGui::GetCurrentContext()
				&& ImGui::GetIO().WantCaptureMouse;
			const bool atkRaw = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			const NadeKind heldNow = ClassifyHeld(GetWeaponSeh(local));
			const bool holdingNade = heldNow != NadeKind::Any;

			// Only count pin while holding a grenade (not menu click / gun fire)
			const bool atk = atkRaw && holdingNade && !armGrace && !wantMouse;

			if (atk) {
				g_cap.sawAttack = true;
				if (!g_cap.attackHeld) {
					g_cap.attackHeld = true;
					g_cap.attackDownMs = now;
				}
			}
			// Sample AFTER attackHeld so while-atk latches fire this frame
			SampleCaptureMotion(local);

			if (!atkRaw && g_cap.attackHeld) {
				const ULONGLONG holdMs = (g_cap.attackDownMs != 0 && now >= g_cap.attackDownMs)
					? (now - g_cap.attackDownMs) : 0ull;
				g_cap.attackHeld = false;
				g_cap.attackDownMs = 0;
				// Require: saw grenade pin, min hold (not click-through), no menu focus
				if (g_cap.sawAttack && holdMs >= kMinAtkHoldMs && !wantMouse)
					FinishArmedCapture(local);
				// Short residual click — drop attack latch, stay armed
			} else if (!atkRaw) {
				// Released without valid pin — clear latch only
				g_cap.attackHeld = false;
				g_cap.attackDownMs = 0;
			}
		}
	}

	g_currentIdx = -1;
	if (!g_mapCache[0])
		return;

	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return;

	Vector_t origin{};
	if (!Bones::GetOrigin(local, origin) || !Bones::IsValidPos(origin))
		return;

	NadeKind held = NadeKind::Any;
	if (Config::nade_lineup_only_held) {
		held = ClassifyHeld(GetWeaponSeh(local));
		if (held == NadeKind::Any)
			return; // not holding a nade
	}

	EnsureMapIndex();
	// Clamp select radius so bad configs don't scan half the map every frame
	const float selectR = std::clamp(Config::nade_lineup_select_dist, 50.f, 2000.f);
	float bestDist = selectR;
	int bestIdx = -1;
	for (int idx : g_mapIndices) {
		if (idx < 0 || idx >= static_cast<int>(g_lineups.size()))
			continue;
		const Lineup& L = g_lineups[static_cast<size_t>(idx)];
		if (!KindMatches(L.kind, held))
			continue;
		const float dx = origin.x - L.pos.x;
		const float dy = origin.y - L.pos.y;
		const float dz = origin.z - L.pos.z;
		const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (d < bestDist) {
			bestDist = d;
			bestIdx = idx;
		}
	}
	g_currentIdx = bestIdx;
}

void Draw(const ViewMatrix& vm) {
	if (!Config::nade_lineup)
		return;

	// Capturing banner — show even with empty list / no matrix yet
	if (g_cap.armed) {
		ImDrawList* hud = ImGui::GetForegroundDrawList();
		if (hud) {
			const ImVec2 ds = ImGui::GetIO().DisplaySize;
			const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.5f);
			const ULONGLONG leftMs = (g_cap.armMs + kCaptureTimeoutMs > GetTickCount64())
				? (g_cap.armMs + kCaptureTimeoutMs - GetTickCount64()) : 0ull;
			const float leftSec = static_cast<float>(leftMs) * 0.001f;
			char line[160]{};
			std::snprintf(line, sizeof(line),
				"CAPTURING  %s  ·  %s  ·  throw to save  ·  %.0fs",
				g_cap.name[0] ? g_cap.name : "Lineup",
				KindName(g_cap.kind), leftSec);
			const ImVec2 ts = ImGui::CalcTextSize(line);
			const ImVec2 min{ floorf(ds.x * 0.5f - ts.x * 0.5f - 16.f), 26.f };
			const ImVec2 max{ min.x + ts.x + 32.f, min.y + ts.y + 16.f };
			hud->AddRectFilled(min, max, IM_COL32(8, 10, 14, 225), 7.f);
			hud->AddRect(min, max, IM_COL32(50, 230, 130, static_cast<int>(160 + pulse * 90)), 7.f, 0, 1.5f);
			hud->AddRectFilled(ImVec2(min.x, min.y + 2.f), ImVec2(min.x + 3.f, max.y - 2.f),
				IM_COL32(50, 230, 130, 255), 2.f);
			hud->AddText(ImVec2(min.x + 16.f, min.y + 8.f), IM_COL32(90, 255, 165, 255), line);
			if (vm.viewMatrix && IsFiniteVec(g_cap.pos)) {
				Vector_t s{};
				if (vm.WorldToScreen(g_cap.pos, s)) {
					const ImVec2 sp{ s.x, s.y };
					const float pr = 9.f + pulse * 3.f;
					hud->AddCircle(sp, pr + 4.f, IM_COL32(50, 230, 130, 70), 24, 2.0f);
					hud->AddCircle(sp, pr, IM_COL32(50, 230, 130, 210), 24, 1.6f);
					hud->AddCircleFilled(sp, 2.4f, IM_COL32(50, 230, 130, 240), 10);
					// Aim preview from frozen capture angles
					const Vector_t aimW = AimWorldFromStand(g_cap.pos, g_cap.ang, kAimDrawDist);
					Vector_t as{};
					if (vm.WorldToScreen(aimW, as)) {
						hud->AddLine(sp, ImVec2(as.x, as.y), IM_COL32(50, 230, 130, 90), 1.2f);
						hud->AddCircleFilled(ImVec2(as.x, as.y), 3.f, IM_COL32(50, 230, 130, 200), 10);
					}
				}
			}
		}
	}

	if (!vm.viewMatrix)
		return;
	if (!g_loaded)
		Load();
	if (g_lineups.empty())
		return;
	SyncMapCache();
	if (!g_mapCache[0]) {
		GameMode::EnsureMap();
		SyncMapCache();
	}
	if (!g_mapCache[0])
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	C_CSPlayerPawn* local = GetLocalSeh();
	if (!local)
		return;

	Vector_t origin{};
	if (!Bones::GetOrigin(local, origin) || !Bones::IsValidPos(origin))
		return;

	Vector_t eye = Bones::GetEyePos(local);
	if (!Bones::IsValidPos(eye))
		eye = origin;
	QAngle_t viewAng{};
	if (!GetLocalViewAngles(viewAng))
		viewAng = {};

	NadeKind held = NadeKind::Any;
	if (Config::nade_lineup_only_held) {
		held = ClassifyHeld(GetWeaponSeh(local));
		if (held == NadeKind::Any)
			return;
	}

	EnsureMapIndex();
	if (g_mapIndices.empty())
		return;

	const float standDist = std::clamp(Config::nade_lineup_stand_dist, 50.f, 2500.f);
	// Perf: ProjectOrEdge per marker. Full pad only on nearest few.
	// Looking away → edge arrow (not drop).
	constexpr int kMaxDraw = 24;
	constexpr int kMaxDetailed = 3;
	constexpr int kMaxLabels = 5;

	struct Cand {
		int idx;
		float distSq;
	};
	Cand cands[64];
	int nCand = 0;
	const float standDistSq = standDist * standDist;

	for (int idx : g_mapIndices) {
		if (idx < 0 || idx >= static_cast<int>(g_lineups.size()))
			continue;
		const Lineup& L = g_lineups[static_cast<size_t>(idx)];
		if (!KindMatches(L.kind, held))
			continue;
		if (!IsFiniteVec(L.pos) || !IsFiniteAng(L.aimAngles))
			continue;
		const float dx = origin.x - L.pos.x;
		const float dy = origin.y - L.pos.y;
		const float dz = origin.z - L.pos.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > standDistSq)
			continue;
		if (nCand < 64) {
			cands[nCand++] = { idx, d2 };
		} else {
			int worst = 0;
			for (int i = 1; i < nCand; ++i) {
				if (cands[i].distSq > cands[worst].distSq)
					worst = i;
			}
			if (d2 < cands[worst].distSq)
				cands[worst] = { idx, d2 };
		}
	}
	if (nCand == 0)
		return;

	// Partial selection sort — nearest first
	const int drawN = (std::min)(nCand, kMaxDraw);
	for (int i = 0; i < drawN; ++i) {
		int best = i;
		for (int j = i + 1; j < nCand; ++j) {
			if (cands[j].distSq < cands[best].distSq)
				best = j;
		}
		if (best != i) {
			const Cand tmp = cands[i];
			cands[i] = cands[best];
			cands[best] = tmp;
		}
	}

	const ImVec4 standCol = Config::nade_lineup_color;
	const ImVec4 aimCol = Config::nade_lineup_aim_color;
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const ImVec2 screenCenter{ disp.x * 0.5f, disp.y * 0.5f };

	// Config aim_dist: how close feet must be for full aim reticle (clamped)
	const float aimLockR = std::clamp(Config::nade_lineup_aim_dist, kAimExactR, 40.f);
	const float aimLockR2 = aimLockR * aimLockR;
	const float aimPreviewR2 = kAimPreviewR * kAimPreviewR;
	const float padR2 = kCircleRadius * kCircleRadius;

	for (int rank = 0; rank < drawN; ++rank) {
		const int li = cands[rank].idx;
		if (li < 0 || li >= static_cast<int>(g_lineups.size()))
			continue;
		const Lineup& L = g_lineups[static_cast<size_t>(li)];
		const float dist = std::sqrt(cands[rank].distSq);
		const bool isCurrent = (g_currentIdx == li);
		// XY only — pad highlight vs feet lock for aim UI
		const float fdx = origin.x - L.pos.x;
		const float fdy = origin.y - L.pos.y;
		const float fxy2 = fdx * fdx + fdy * fdy;
		const bool onPad = fxy2 <= padR2;
		const bool onExact = fxy2 <= aimLockR2;
		const bool onPreview = fxy2 <= aimPreviewR2;
		const bool detailed = rank < kMaxDetailed || onPad || isCurrent;

		const float rawA = 1.f - (dist / standDist);
		float standA = std::clamp(rawA * 1.55f, 0.f, 1.f);
		if (isCurrent)
			standA = (std::min)(1.f, standA + 0.18f);
		if (onExact)
			standA = (std::min)(1.f, standA + 0.12f);

		ImVec2 sp{};
		bool onScreen = false;
		if (!ProjectOrEdge(vm, L.pos, sp, onScreen, eye, viewAng))
			continue;

		// Looking away / feet under camera — edge arrow
		if (!onScreen) {
			DrawOffscreenHint(dl, sp, screenCenter, standA * 0.92f,
				isCurrent || onExact ? aimCol : standCol,
				(rank < kMaxLabels || isCurrent || onPad) ? L.name.c_str() : nullptr,
				dist);
		} else {
			const ImVec4 iconAccent = onExact || isCurrent ? aimCol : standCol;
			const bool hasIcon = L.kind != NadeKind::Any;

			if (detailed) {
				const ImU32 ring = ColA(standCol, standA * (onPad ? 0.95f : (hasIcon ? 0.55f : 0.78f)));
				const ImU32 fill = ColA(standCol, standA * (onExact ? 0.16f : (onPad ? 0.10f : 0.05f)));
				DrawStandPad(dl, vm, L.pos, kCircleRadius, ring, fill, onPad ? 18 : 12, onExact || isCurrent);
			} else if (!hasIcon) {
				dl->AddCircle(sp, 5.5f, ColA(standCol, standA * 0.70f), 14, 1.15f);
			}

			if (hasIcon) {
				const float iconPx = detailed ? (onExact ? 24.f : (onPad ? 21.f : 18.f)) : 16.f;
				DrawKindIcon(dl, sp, L.kind, standA * (detailed ? 0.98f : 0.80f),
					iconPx, iconAccent, onExact || isCurrent);
			} else {
				DrawFeetCross(dl, sp, onExact ? 3.2f : 2.4f,
					ColA(iconAccent, standA),
					ColRGB(0, 0, 0, standA * 0.4f));
			}

			if (rank < kMaxLabels || onPad || isCurrent) {
				char meta[56]{};
				const float meters = dist * kUnitsToM;
				if (meters >= 1.f)
					std::snprintf(meta, sizeof(meta), "%s  ·  %.0fm", ThrowName(L.throwType), meters);
				else
					std::snprintf(meta, sizeof(meta), "%s", ThrowName(L.throwType));
				DrawLabelChip(dl, sp, L.name.c_str(), meta, standA,
					iconAccent, isCurrent || onExact);
			}
		}

		// Aim: full reticle on exact lock; dim preview when on pad
		if (!onPreview)
			continue;

		const Vector_t aimWorld = AimWorldFromStand(L.pos, L.aimAngles, kAimDrawDist);
		ImVec2 screenAim{};
		bool aimOn = false;
		if (!ProjectOrEdge(vm, aimWorld, screenAim, aimOn, eye, viewAng) || !aimOn)
			continue;

		const float aimA = onExact ? 1.f : 0.42f;
		DrawAimReticle(dl, screenAim, aimA, Config::nade_lineup_aim_color, onExact,
			ThrowName(L.throwType), KindName(L.kind));
		if (onScreen)
			DrawGuideLine(dl, sp, screenAim, ColA(aimCol, onExact ? 0.42f : 0.18f), onExact);
	}
}

} // namespace NadeLineup
