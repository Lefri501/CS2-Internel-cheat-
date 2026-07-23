#define NOMINMAX
#include "tracers.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../w2s/w2s.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../../external/imgui/imgui.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Tracers {
namespace {

constexpr int kMax = 64;

struct Tracer {
	Vector_t start{};
	Vector_t end{};
	std::uint64_t bornMs = 0;
	float life = 0.f;
	bool active = false;
	bool provisional = false; // NoteFire stub — replaced by OnImpact
};

struct LastFire {
	Vector_t eye{};
	Vector_t dir{};
	std::uint64_t timeMs = 0;
	bool valid = false;
	int provisionalIdx = -1;
	bool impactDone = false; // one tracer per shot (no double push)
};

Tracer g_list[kMax]{};
int g_write = 0;
LastFire g_fire{};

// Guns only — knife/nade/taser/c4 never spawn tracers
bool LocalHoldingGun()
{
	C_CSPlayerPawn* lp = H::SafeLocalPlayer();
	if (!lp)
		return false;
	C_CSWeaponBase* w = nullptr;
	__try { w = lp->GetActiveWeapon(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!w || !Mem::ValidEntity(w))
		return false;
	__try {
		if (w->IsNonGunWeapon())
			return false;
		// Need bullets
		if (w->m_iClip1() <= 0 && w->m_iClip1() != -1)
			return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

void KillProvisional()
{
	if (g_fire.provisionalIdx < 0 || g_fire.provisionalIdx >= kMax)
		return;
	Tracer& p = g_list[g_fire.provisionalIdx];
	if (p.active && p.provisional)
		p.active = false;
	g_fire.provisionalIdx = -1;
}

// Wall clock — safe on FireEvent thread (ImGui time can be 0 / wrong context)
std::uint64_t NowMs()
{
	return GetTickCount64();
}

float AgeSec(std::uint64_t bornMs)
{
	if (!bornMs)
		return 0.f;
	const std::uint64_t n = NowMs();
	if (n < bornMs)
		return 0.f;
	return static_cast<float>(n - bornMs) * 0.001f;
}

ImU32 ColSolid(ImVec4 c, float a)
{
	return IM_COL32(
		static_cast<int>(std::clamp(c.x, 0.f, 1.f) * 255.f),
		static_cast<int>(std::clamp(c.y, 0.f, 1.f) * 255.f),
		static_cast<int>(std::clamp(c.z, 0.f, 1.f) * 255.f),
		static_cast<int>(std::clamp(a, 0.f, 1.f) * 255.f));
}

ImVec4 Hot(ImVec4 c, float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return ImVec4(
		c.x + (1.f - c.x) * t,
		c.y + (1.f - c.y) * t,
		c.z + (1.f - c.z) * t,
		c.w);
}

Vector_t FallbackEye()
{
	C_CSPlayerPawn* lp = H::SafeLocalPlayer();
	if (!lp || !Mem::ValidEntity(lp))
		return {};
	Vector_t e = Bones::GetShootPos(lp);
	return Bones::IsValidPos(e) ? e : Vector_t{};
}

// Project world → screen. Prefer live W2S; allow slightly behind via clip.
bool Project(const ViewMatrix& vm, const Vector_t& w, ImVec2& out, float* outW = nullptr)
{
	Vector_t s{};
	// Live path first
	if (W2S::WorldToScreen(w, s) && std::isfinite(s.x) && std::isfinite(s.y)) {
		out = ImVec2(s.x, s.y);
		if (outW)
			*outW = 1.f;
		return true;
	}
	if (vm.WorldToScreen(w, s) && std::isfinite(s.x) && std::isfinite(s.y)) {
		out = ImVec2(s.x, s.y);
		if (outW)
			*outW = 1.f;
		return true;
	}
	// Manual clip coords for near-plane lerp
	float nx = 0.f, ny = 0.f, ww = 0.f;
	if (!vm.WorldToClip(w, nx, ny, ww))
		return false;
	if (outW)
		*outW = ww;
	// Only accept if in front
	if (ww <= 0.001f)
		return false;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	const float inv = 1.f / ww;
	out.x = ds.x * 0.5f + nx * inv * ds.x * 0.5f;
	out.y = ds.y * 0.5f - ny * inv * ds.y * 0.5f;
	return std::isfinite(out.x) && std::isfinite(out.y);
}

// Clip segment so both ends project (start often at eye → behind near plane).
bool ProjectSegment(
	const ViewMatrix& vm,
	Vector_t a, Vector_t b,
	ImVec2& sa, ImVec2& sb)
{
	float wa = 0.f, wb = 0.f;
	bool okA = Project(vm, a, sa, &wa);
	bool okB = Project(vm, b, sb, &wb);

	if (okA && okB)
		return true;

	// Near-plane clip: lerp world points until w > eps
	float nxa, nya, nwa;
	float nxb, nyb, nwb;
	const bool clipA = vm.WorldToClip(a, nxa, nya, nwa);
	const bool clipB = vm.WorldToClip(b, nxb, nyb, nwb);
	if (!clipA || !clipB)
		return okA && okB;

	constexpr float kEps = 0.05f;
	// If A behind, slide toward B
	if (nwa <= kEps && nwb > kEps) {
		const float t = (kEps - nwa) / (nwb - nwa);
		a.x = a.x + (b.x - a.x) * t;
		a.y = a.y + (b.y - a.y) * t;
		a.z = a.z + (b.z - a.z) * t;
		okA = Project(vm, a, sa, &wa);
	}
	// If B behind, slide toward A
	if (nwb <= kEps && nwa > kEps) {
		const float t = (kEps - nwb) / (nwa - nwb);
		b.x = b.x + (a.x - b.x) * t;
		b.y = b.y + (a.y - b.y) * t;
		b.z = b.z + (a.z - b.z) * t;
		okB = Project(vm, b, sb, &wb);
	}

	// Both behind / failed: fall back to screen center → end
	if (!okA && okB) {
		const ImVec2 ds = ImGui::GetIO().DisplaySize;
		sa = ImVec2(ds.x * 0.5f, ds.y * 0.5f);
		okA = true;
	}
	if (!okB && okA) {
		// can't draw without end
		return false;
	}
	return okA && okB;
}

// Per-style draw — keep primitive count tiny (thick multi-pass = FPS death).
// Caps: ≤3 lines + ≤2 circles. Dashed ≤10 segments. Energy ≤6 segs.
void DrawBeamLine(
	ImDrawList* dl,
	ImVec2 a, ImVec2 b,
	ImVec4 col,
	float alpha,
	float thickness,
	int style)
{
	if (alpha < 0.02f)
		return;

	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len < 0.5f)
		return;

	const float th = std::clamp(thickness, 0.5f, 6.f);
	const ImVec4 white(1.f, 1.f, 1.f, 1.f);
	const ImVec4 core = Hot(col, 0.65f);
	const float ux = dx / len;
	const float uy = dy / len;
	// Perp for double-rail styles
	const float px = -uy;
	const float py = ux;

	switch (style) {
	case STYLE_LASER: {
		// Thin hot needle — no wide bloom (looks nothing like Beam/Glow)
		const float coreTh = (std::max)(1.f, th * 0.55f);
		dl->AddLine(a, b, ColSolid(col, alpha * 0.35f), coreTh + 1.5f);
		dl->AddLine(a, b, ColSolid(white, alpha * 0.95f), coreTh);
		// Impact pin only (8-seg circle is cheap)
		dl->AddCircleFilled(b, coreTh * 1.6f, ColSolid(col, alpha * 0.85f), 8);
		dl->AddCircleFilled(b, coreTh * 0.55f, ColSolid(white, alpha), 6);
		break;
	}
	case STYLE_GLOW: {
		// Soft fat haze + dim core — max 2 lines (old: 4 thick = heavy)
		const float outer = (std::min)(th * 4.5f, 14.f);
		dl->AddLine(a, b, ColSolid(col, alpha * 0.22f), outer);
		dl->AddLine(a, b, ColSolid(core, alpha * 0.55f), (std::max)(1.2f, th * 0.7f));
		break;
	}
	case STYLE_DASHED: {
		// Fixed period from length so long rays stay ≤10 dashes (old: 2 lines * hundreds)
		constexpr int kMaxDash = 10;
		const float period = (std::max)(28.f, len / static_cast<float>(kMaxDash));
		const float dash = period * 0.58f;
		const float anim = std::fmod(static_cast<float>(NowMs()) * 0.06f, period);
		const ImU32 c = ColSolid(col, alpha * 0.95f);
		const float dth = (std::max)(1.5f, th * 1.15f);
		int n = 0;
		for (float t0 = -anim; t0 < len && n < kMaxDash; t0 += period, ++n) {
			const float s0 = (std::max)(0.f, t0);
			const float s1 = (std::min)(t0 + dash, len);
			if (s1 <= s0)
				continue;
			dl->AddLine(
				ImVec2(a.x + ux * s0, a.y + uy * s0),
				ImVec2(a.x + ux * s1, a.y + uy * s1),
				c, dth);
		}
		// Small land tick
		dl->AddCircleFilled(b, dth * 0.9f, ColSolid(core, alpha * 0.9f), 6);
		break;
	}
	case STYLE_ENERGY: {
		// Dual rail + traveling node — fixed 6 segs max (old: len/18 * 2 lines)
		constexpr int kSegs = 6;
		const float rail = th * 0.9f;
		const float off = (std::max)(2.5f, th * 1.1f);
		const ImU32 railCol = ColSolid(col, alpha * 0.55f);
		// Two parallel rails
		dl->AddLine(
			ImVec2(a.x + px * off, a.y + py * off),
			ImVec2(b.x + px * off, b.y + py * off),
			railCol, rail);
		dl->AddLine(
			ImVec2(a.x - px * off, a.y - py * off),
			ImVec2(b.x - px * off, b.y - py * off),
			railCol, rail);
		// Center pulse segments (few)
		const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(NowMs()) * 0.012f);
		for (int i = 0; i < kSegs; ++i) {
			if ((i & 1) == 0)
				continue; // every other = dashed energy look
			const float t0 = (float)i / (float)kSegs;
			const float t1 = (float)(i + 1) / (float)kSegs;
			dl->AddLine(
				ImVec2(a.x + dx * t0, a.y + dy * t0),
				ImVec2(a.x + dx * t1, a.y + dy * t1),
				ColSolid(core, alpha * pulse), th * 1.3f);
		}
		// Traveling head
		const float ht = std::fmod(static_cast<float>(NowMs()) * 0.0014f, 1.f);
		const ImVec2 head(a.x + dx * ht, a.y + dy * ht);
		dl->AddCircleFilled(head, th * 1.8f, ColSolid(col, alpha * 0.75f), 8);
		dl->AddCircleFilled(b, th * 1.1f, ColSolid(white, alpha * 0.85f), 6);
		break;
	}
	case STYLE_BEAM:
	default: {
		// Classic 2-layer beam + impact flare (3 prims) — distinct medium body
		const float outer = (std::min)(th * 2.8f, 10.f);
		dl->AddLine(a, b, ColSolid(col, alpha * 0.40f), outer);
		dl->AddLine(a, b, ColSolid(core, alpha * 0.95f), (std::max)(1.2f, th));
		dl->AddCircleFilled(b, th * 1.5f, ColSolid(core, alpha * 0.9f), 8);
		dl->AddCircleFilled(b, th * 0.55f, ColSolid(white, alpha), 6);
		break;
	}
	}
}

} // namespace

void Clear()
{
	for (int i = 0; i < kMax; ++i)
		g_list[i] = Tracer{};
	g_write = 0;
	g_fire = LastFire{};
}

float LifeCfg()
{
	return std::clamp(Config::tracers_duration, 0.5f, 8.f);
}

// Land point along fire ray — TraceLine wall hit, else far fallback.
Vector_t ResolveLand(const Vector_t& eye, const Vector_t& nd)
{
	constexpr float kMaxRange = 8192.f;
	const Vector_t farEnd{
		eye.x + nd.x * kMaxRange,
		eye.y + nd.y * kMaxRange,
		eye.z + nd.z * kMaxRange
	};

	C_CSPlayerPawn* lp = H::SafeLocalPlayer();
	Trace::CGameTrace tr{};
	if (Trace::Ready() && Trace::TraceLine(eye, farEnd, lp, tr, Trace::kMaskShot)) {
		const float frac = tr.fraction();
		if (std::isfinite(frac) && frac >= 0.f && frac < 1.f && !tr.startsolid()) {
			const Vector_t hit = tr.endpos();
			if (Bones::IsValidPos(hit))
				return hit;
			// frac * ray if endpos bad
			return Vector_t{
				eye.x + (farEnd.x - eye.x) * frac,
				eye.y + (farEnd.y - eye.y) * frac,
				eye.z + (farEnd.z - eye.z) * frac
			};
		}
	}
	// No hit: still draw long beam so feature never goes invisible
	return farEnd;
}

int PushTracer(Vector_t start, Vector_t end, bool provisional)
{
	Vector_t d{
		end.x - start.x,
		end.y - start.y,
		end.z - start.z
	};
	float len = d.Length();
	if (!std::isfinite(len))
		return -1;
	if (len < 0.5f) {
		start.z += 4.f;
		d.z = end.z - start.z;
		len = d.Length();
	}
	if (!(len > 0.5f) || len > 16384.f)
		return -1;

	const float inv = 1.f / len;
	d.x *= inv; d.y *= inv; d.z *= inv;
	const float push = (std::min)(32.f, len * 0.06f);
	start.x += d.x * push;
	start.y += d.y * push;
	start.z += d.z * push;

	const int idx = g_write;
	Tracer& t = g_list[idx];
	g_write = (g_write + 1) % kMax;
	t.start = start;
	t.end = end;
	t.bornMs = NowMs();
	t.life = LifeCfg();
	t.active = true;
	t.provisional = provisional;
	return idx;
}

void NoteFire(const Vector_t& eye, const Vector_t& dir)
{
	if (!Config::tracers)
		return;
	// Knife / nade / taser: never
	if (!LocalHoldingGun()) {
		g_fire.valid = false;
		KillProvisional();
		return;
	}
	if (!Bones::IsValidPos(eye))
		return;
	float len = dir.Length();
	if (!(len > 1e-4f) || !std::isfinite(len))
		return;
	const Vector_t nd{ dir.x / len, dir.y / len, dir.z / len };
	const std::uint64_t now = NowMs();

	// Aim stamps every CM while M1 held — one beam per real shot (~2.5 ticks)
	if (g_fire.timeMs && (now - g_fire.timeMs) < 40ull) {
		// Refresh pin only (same shot)
		g_fire.eye = eye;
		g_fire.dir = nd;
		g_fire.valid = true;
		return;
	}

	// New shot: kill any leftover provisional so we never stack two beams
	KillProvisional();

	g_fire.eye = eye;
	g_fire.dir = nd;
	g_fire.timeMs = now;
	g_fire.valid = true;
	g_fire.impactDone = false;

	// One provisional: TraceLine land now; OnImpact snaps end (no second tracer)
	const Vector_t land = ResolveLand(eye, nd);
	g_fire.provisionalIdx = PushTracer(eye, land, true);
}

void OnImpact(const Vector_t& end)
{
	if (!Config::tracers)
		return;
	if (!std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(end.z))
		return;
	if (std::fabs(end.x) < 1.f && std::fabs(end.y) < 1.f && std::fabs(end.z) < 1.f)
		return;

	const std::uint64_t now = NowMs();
	// Must be tied to a gun NoteFire — rejects knife-only impacts / other clients
	if (!g_fire.valid || (now - g_fire.timeMs) > 800ull)
		return;

	// One impact per shot: multi-pellet / multi-surface only refine first beam
	if (g_fire.impactDone)
		return;

	// Prefer snap provisional (TraceLine beam already up)
	if (g_fire.provisionalIdx >= 0 && g_fire.provisionalIdx < kMax) {
		Tracer& p = g_list[g_fire.provisionalIdx];
		if (p.active && p.provisional) {
			p.end = end;
			p.provisional = false;
			p.life = LifeCfg();
			g_fire.provisionalIdx = -1;
			g_fire.impactDone = true;
			return;
		}
	}

	// Provisional already finalized or missing — only push if we never drew this shot
	if (g_fire.provisionalIdx < 0 && !g_fire.impactDone) {
		Vector_t start = Bones::IsValidPos(g_fire.eye) ? g_fire.eye : FallbackEye();
		if (!Bones::IsValidPos(start)) {
			start = Vector_t{
				end.x - g_fire.dir.x * 64.f,
				end.y - g_fire.dir.y * 64.f,
				end.z - g_fire.dir.z * 64.f
			};
		}
		PushTracer(start, end, false);
	}

	g_fire.provisionalIdx = -1;
	g_fire.impactDone = true;
}

void Draw(const ViewMatrix& vm)
{
	if (!Config::tracers)
		return;

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const bool haveMatrix = vm.viewMatrix || W2S::HasLiveMatrix() || W2S::Matrix();
	if (!haveMatrix)
		return;

	const float lifeCfg = LifeCfg();
	const float thick = std::clamp(Config::tracers_thickness, 0.5f, 8.f);
	const int style = std::clamp(Config::tracers_style, 0, STYLE_COUNT - 1);
	const ImVec4 col = Config::tracers_color;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	const ImVec2 center(ds.x * 0.5f, ds.y * 0.5f);

	for (int i = 0; i < kMax; ++i) {
		Tracer& t = g_list[i];
		if (!t.active)
			continue;

		const float life = (t.life > 0.05f) ? t.life : lifeCfg;
		const float age = AgeSec(t.bornMs);
		if (age >= life) {
			t.active = false;
			continue;
		}

		// Hold solid longer, soft fade only last 30%
		float alpha = 1.f;
		const float fadeStart = life * 0.70f;
		if (age > fadeStart) {
			const float u = (age - fadeStart) / (std::max)(0.05f, life - fadeStart);
			alpha = 1.f - u * u;
		}
		alpha = std::clamp(alpha, 0.f, 1.f) * std::clamp(col.w, 0.15f, 1.f);
		if (alpha < 0.03f)
			continue;

		ImVec2 sa{}, sb{};
		if (!ProjectSegment(vm, t.start, t.end, sa, sb)) {
			Vector_t se{}, ss{};
			const bool okE = W2S::WorldToScreen(t.end, se);
			const bool okS = W2S::WorldToScreen(t.start, ss);
			if (okE && okS) {
				sa = ImVec2(ss.x, ss.y);
				sb = ImVec2(se.x, se.y);
			} else if (okE) {
				sa = center;
				sb = ImVec2(se.x, se.y);
			} else {
				continue;
			}
		}

		DrawBeamLine(dl, sa, sb, col, alpha, thick, style);
	}
}

} // namespace Tracers
