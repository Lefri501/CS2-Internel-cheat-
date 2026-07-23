#include "sound_esp.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/math/vector/vector.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../bones/bones.h"
#include "../gamemode/gamemode.h"
#include "../w2s/w2s.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../interfaces/CUserCmd/CUserCmd.h" // FL_ONGROUND
#include "../../../../external/imgui/imgui.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// CS2: mp_footsteps_serverside — client does NOT compute enemy footsteps.
// Sound ESP uses live pawn velocity + FL_ONGROUND (reliable on MM client).

namespace SoundEsp {
namespace {

constexpr int kMaxMarks = 48;
constexpr int kMaxTrack = 64;
constexpr int kRingSegs = 28;
constexpr float kRingStartR = 12.f;
constexpr float kRingEndR = 48.f;
constexpr float kGroundLift = 2.f;
constexpr float kMinSpeed = 85.f;      // units/s — walk+
constexpr float kRunSpeed = 160.f;     // faster ring cadence
constexpr float kMinInterval = 0.28f;  // run
constexpr float kMaxInterval = 0.55f;  // walk

struct Mark {
	bool active = false;
	float born = 0.f;
	float life = 1.4f;
	Vector_t pos{};
};

struct Track {
	int handleIdx = -1;
	float lastRing = -100.f;
	float lastX = 0.f;
	float lastY = 0.f;
};

Mark g_marks[kMaxMarks]{};
int g_write = 0;
Track g_track[kMaxTrack]{};

float Now() {
	// Prefer ImGui clock when frame open; else QPC ms
	if (ImGui::GetCurrentContext())
		return static_cast<float>(ImGui::GetTime());
	static LARGE_INTEGER s_freq{}, s_start{};
	static bool s_init = false;
	if (!s_init) {
		QueryPerformanceFrequency(&s_freq);
		QueryPerformanceCounter(&s_start);
		s_init = true;
	}
	LARGE_INTEGER c{};
	QueryPerformanceCounter(&c);
	return static_cast<float>(
		static_cast<double>(c.QuadPart - s_start.QuadPart) / static_cast<double>(s_freq.QuadPart));
}

bool ValidPos(const Vector_t& p) {
	if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
		return false;
	if (p.x == 0.f && p.y == 0.f && p.z == 0.f)
		return false;
	if (std::fabs(p.x) > 16384.f || std::fabs(p.y) > 16384.f || std::fabs(p.z) > 16384.f)
		return false;
	return true;
}

C_CSPlayerPawn* LocalPawn() {
	return H::SafeLocalPlayer();
}

Track* FindTrack(int handleIdx) {
	for (int i = 0; i < kMaxTrack; ++i) {
		if (g_track[i].handleIdx == handleIdx)
			return &g_track[i];
	}
	// free slot
	for (int i = 0; i < kMaxTrack; ++i) {
		if (g_track[i].handleIdx < 0)
			return &g_track[i];
	}
	// overwrite oldest-looking
	return &g_track[handleIdx % kMaxTrack];
}

void PushGroundRing(Vector_t pos) {
	if (!ValidPos(pos))
		return;
	pos.z += kGroundLift;

	const float now = Now();
	for (int i = 0; i < kMaxMarks; ++i) {
		Mark& m = g_marks[i];
		if (!m.active)
			continue;
		const float dx = m.pos.x - pos.x;
		const float dy = m.pos.y - pos.y;
		if (dx * dx + dy * dy < 100.f && (now - m.born) < 0.15f) {
			m.born = now;
			m.pos = pos;
			return;
		}
	}

	Mark& m = g_marks[g_write];
	g_write = (g_write + 1) % kMaxMarks;
	m.active = true;
	m.born = now;
	m.life = std::clamp(Config::sound_esp_duration, 0.4f, 4.f);
	m.pos = pos;
}

bool ResolvePawn(const CBaseHandle& h, C_CSPlayerPawn** out) {
	*out = nullptr;
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return false;
	auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(h);
	if (!pawn || !Mem::ValidEntity(pawn))
		return false;
	*out = pawn;
	return true;
}

// Sample enemy walkers → ground rings
void SampleWalkers() {
	if (!Config::sound_esp)
		return;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return;

	C_CSPlayerPawn* lp = LocalPawn();
	if (!lp || !Mem::ValidEntity(lp))
		return;

	int myTeam = 0;
	__try { myTeam = static_cast<int>(lp->m_iTeamNum()); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }

	const float now = Now();
	// Same team filter as player ESP (auto team check + manual pref)
	const bool doTeamCheck = GameMode::WantTeamCheck(Config::teamCheck);

	// Controllers 1..64 — same set ESP can draw (alive, enemy when team check on)
	for (int i = 1; i <= 64; ++i) {
		void* raw = nullptr;
		__try { raw = I::GameEntity->Instance->Get(i); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!raw || !Mem::ValidEntity(raw))
			continue;

		auto* ctrl = reinterpret_cast<CCSPlayerController*>(raw);
		bool isLocal = false;
		__try { isLocal = ctrl->IsLocalPlayer(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (isLocal)
			continue;

		bool alive = false;
		__try { alive = ctrl->m_bPawnIsAlive(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!alive)
			continue;

		CBaseHandle hPawn{};
		__try { hPawn = ctrl->m_hPlayerPawn(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!hPawn.valid())
			continue;

		C_CSPlayerPawn* pawn = nullptr;
		if (!ResolvePawn(hPawn, &pawn) || !pawn)
			continue;
		if (!pawn->IsBasePlayer())
			continue;

		int team = 0;
		std::uint32_t flags = 0;
		Vector_t vel{};
		Vector_t origin{};
		__try {
			team = static_cast<int>(pawn->m_iTeamNum());
			flags = pawn->m_fFlags();
			vel = pawn->m_vecAbsVelocity();
			if (CGameSceneNode* node = pawn->m_pGameSceneNode())
				origin = node->m_vecAbsOrigin();
			else
				origin = pawn->m_vOldOrigin();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}

		if (doTeamCheck && myTeam >= 2 && team == myTeam)
			continue;
		if ((flags & FL_ONGROUND) == 0)
			continue; // air — no footstep ring

		const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);
		if (speed2d < kMinSpeed)
			continue; // standing / crouch idle

		if (!ValidPos(origin))
			continue;

		// Cadence from speed (faster run → more rings)
		const float t = std::clamp((speed2d - kMinSpeed) / (kRunSpeed - kMinSpeed + 1.f), 0.f, 1.f);
		const float interval = kMaxInterval + (kMinInterval - kMaxInterval) * t;

		const int hIdx = static_cast<int>(hPawn.index());
		Track* tr = FindTrack(hIdx);
		if (!tr)
			continue;
		if (tr->handleIdx != hIdx) {
			tr->handleIdx = hIdx;
			tr->lastRing = -100.f;
		}

		// Also require actual movement since last ring
		const float mdx = origin.x - tr->lastX;
		const float mdy = origin.y - tr->lastY;
		const float moved = std::sqrt(mdx * mdx + mdy * mdy);

		if ((now - tr->lastRing) < interval)
			continue;
		if (tr->lastRing > 0.f && moved < 8.f)
			continue;

		tr->lastRing = now;
		tr->lastX = origin.x;
		tr->lastY = origin.y;
		PushGroundRing(origin);
	}
}

void DrawGroundRing(ImDrawList* dl, const Vector_t& center, float radius, ImU32 col, ImU32 outline, float thick) {
	if (!dl || radius < 1.f)
		return;

	// Center must project — else skip whole ring
	Vector_t cScr{};
	if (!W2S::WorldToScreen(center, cScr))
		return;

	ImVec2 pts[kRingSegs]{};
	int n = 0;
	for (int i = 0; i < kRingSegs; ++i) {
		const float a = (6.28318530718f * static_cast<float>(i)) / static_cast<float>(kRingSegs);
		Vector_t w{
			center.x + std::cos(a) * radius,
			center.y + std::sin(a) * radius,
			center.z
		};
		Vector_t s{};
		if (!W2S::WorldToScreen(w, s)) {
			// keep ring continuous: drop bad points but need enough
			continue;
		}
		pts[n++] = ImVec2(s.x, s.y);
	}

	if (n >= 6) {
		if (outline)
			dl->AddPolyline(pts, n, outline, ImDrawFlags_Closed, thick + 1.8f);
		dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, thick);
		return;
	}

	// Fallback: screen-space ellipse from distance (when ring clips frustum)
	const float dist = radius; // rough
	float scrR = 18.f + dist * 0.15f;
	// Scale by how large center "feels" — clamp
	scrR = std::clamp(scrR, 10.f, 90.f);
	// Use horizontal distance from a known world offset if possible
	Vector_t edge = center;
	edge.x += radius;
	Vector_t eScr{};
	if (W2S::WorldToScreen(edge, eScr)) {
		const float dx = eScr.x - cScr.x;
		const float dy = eScr.y - cScr.y;
		scrR = std::sqrt(dx * dx + dy * dy);
		scrR = std::clamp(scrR, 6.f, 120.f);
	}
	const ImVec2 c(cScr.x, cScr.y);
	if (outline)
		dl->AddCircle(c, scrR + 1.f, outline, 24, thick + 1.5f);
	dl->AddCircle(c, scrR, col, 24, thick);
}

} // namespace

void Install() {
	g_write = 0;
	for (int i = 0; i < kMaxMarks; ++i)
		g_marks[i] = Mark{};
	for (int i = 0; i < kMaxTrack; ++i)
		g_track[i] = Track{};
	// No client footstep hook — serverside footsteps never call it for enemies.
	Con::Ok("SoundEsp: velocity ground rings (serverside footsteps)");
}

void OnGameEvent(void* /*gameEvent*/) {
}

void Draw() {
	if (!Config::sound_esp)
		return;

	// Sample every draw frame (Present) — creates rings for walking enemies
	__try { SampleWalkers(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const float now = Now();
	const ImVec4 base = Config::sound_esp_color;

	for (int i = 0; i < kMaxMarks; ++i) {
		Mark& m = g_marks[i];
		if (!m.active)
			continue;
		const float age = now - m.born;
		if (age >= m.life || age < 0.f) {
			m.active = false;
			continue;
		}

		const float u = age / m.life;
		const float fade = 1.f - u;
		const float scale = std::clamp(Config::sound_esp_ring_size, 0.5f, 3.f);
		const float radius = (kRingStartR + (kRingEndR - kRingStartR) * u) * scale;

		const int a = static_cast<int>(std::clamp(base.w * fade, 0.f, 1.f) * 255.f);
		if (a < 12)
			continue;

		const ImU32 col = IM_COL32(
			static_cast<int>(std::clamp(base.x, 0.f, 1.f) * 255.f),
			static_cast<int>(std::clamp(base.y, 0.f, 1.f) * 255.f),
			static_cast<int>(std::clamp(base.z, 0.f, 1.f) * 255.f),
			a);
		const ImU32 outline = IM_COL32(0, 0, 0, (a * 3) / 4);

		DrawGroundRing(dl, m.pos, radius, col, outline, 2.4f);
		if (u < 0.5f) {
			const int a2 = a / 2;
			const ImU32 col2 = IM_COL32(
				static_cast<int>(std::clamp(base.x, 0.f, 1.f) * 255.f),
				static_cast<int>(std::clamp(base.y, 0.f, 1.f) * 255.f),
				static_cast<int>(std::clamp(base.z, 0.f, 1.f) * 255.f),
				a2);
			DrawGroundRing(dl, m.pos, radius * 0.5f, col2, IM_COL32(0, 0, 0, a2), 1.5f);
		}
	}
}

void Clear() {
	for (int i = 0; i < kMaxMarks; ++i)
		g_marks[i] = Mark{};
	g_write = 0;
	for (int i = 0; i < kMaxTrack; ++i)
		g_track[i] = Track{};
}

} // namespace SoundEsp
