#define NOMINMAX
#include "w2s.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"

#include "../../../../external/imgui/imgui.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>

namespace W2S {
namespace {

viewmatrix_t g_live{};
bool g_hasLive = false;
viewmatrix_t* g_matrixBase = nullptr; // unk_1823A9340 — float[16] per view slot
uintptr_t g_fnScreenTransform = 0;
bool g_inited = false;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.f;
constexpr float kRad2Deg = 180.f / kPi;
constexpr float kNearW = 0.001f;

uintptr_t ScanClient(const char* tag, const char* pat) {
	uintptr_t a = M::patternScan("client", pat);
	if (!a)
		a = M::patternScan("client.dll", pat);
	if (a)
		Con::Ok("W2S %s @ 0x%p", tag, (void*)a);
	else
		Con::OffsetMiss(tag);
	return a;
}

bool SehCopyMatrix(const viewmatrix_t* src, viewmatrix_t* dst) {
	if (!src || !dst || !Mem::IsReadable(src, sizeof(viewmatrix_t)))
		return false;
	__try {
		std::memcpy(dst, src, sizeof(viewmatrix_t));
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// IDA sub_180BB2AD0 — row0=x, row1=y, row3=w (row-major float[16]).
bool ProjectClip(const viewmatrix_t* m, const Vector_t& position,
	float& clipX, float& clipY, float& clipW)
{
	if (!m)
		return false;
	clipX = m->matrix[0][0] * position.x + m->matrix[0][1] * position.y
		+ m->matrix[0][2] * position.z + m->matrix[0][3];
	clipY = m->matrix[1][0] * position.x + m->matrix[1][1] * position.y
		+ m->matrix[1][2] * position.z + m->matrix[1][3];
	clipW = m->matrix[3][0] * position.x + m->matrix[3][1] * position.y
		+ m->matrix[3][2] * position.z + m->matrix[3][3];
	return std::isfinite(clipX) && std::isfinite(clipY) && std::isfinite(clipW);
}

bool Project(const viewmatrix_t* m, const Vector_t& position, Vector_t& out, bool allowBehind) {
	float cx = 0.f, cy = 0.f, cw = 0.f;
	if (!ProjectClip(m, position, cx, cy, cw))
		return false;

	if (!allowBehind && cw < kNearW)
		return false;
	if (std::fabs(cw) < kNearW)
		return false;

	const float invW = 1.0f / cw;
	const ImVec2 wS = ImGui::GetIO().DisplaySize;
	if (wS.x <= 1.f || wS.y <= 1.f)
		return false;
	const float centerX = wS.x * 0.5f;
	const float centerY = wS.y * 0.5f;
	out.x = centerX + (cx * invW * centerX);
	out.y = centerY - (cy * invW * centerY);
	return std::isfinite(out.x) && std::isfinite(out.y);
}

float NormalizeYawDeg(float yaw) {
	while (yaw > 180.f) yaw -= 360.f;
	while (yaw < -180.f) yaw += 360.f;
	return yaw;
}

// IDA ScreenTransform (0xBB2BB0): returns 0 in front, 1 behind.
// Out = already perspective-divided NDC (x,y); behind still writes huge NDC — ignore.
bool SehScreenTransform(const Vector_t& world, Vector_t& ndcOut, bool& behind) {
	behind = true;
	ndcOut = Vector_t{ 0.f, 0.f, 0.f };
	if (!g_fnScreenTransform)
		return false;
	__try {
		using Fn = std::int64_t(__fastcall*)(const Vector_t*, Vector_t*);
		Vector_t in = world;
		Vector_t out{};
		const std::int64_t ret = reinterpret_cast<Fn>(g_fnScreenTransform)(&in, &out);
		behind = (ret != 0);
		ndcOut = out;
		return std::isfinite(out.x) && std::isfinite(out.y);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void ClampDirToEdge(float dx, float dy, float margin, float& outX, float& outY) {
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	const float cx = ds.x * 0.5f;
	const float cy = ds.y * 0.5f;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len < 1e-6f) {
		dx = 0.f;
		dy = -1.f;
	} else {
		dx /= len;
		dy /= len;
	}
	const float maxHX = (std::max)(cx - margin, 1.f);
	const float maxHY = (std::max)(cy - margin, 1.f);
	float t = 1e9f;
	if (std::fabs(dx) > 1e-6f)
		t = (std::min)(t, maxHX / std::fabs(dx));
	if (std::fabs(dy) > 1e-6f)
		t = (std::min)(t, maxHY / std::fabs(dy));
	if (!(t < 1e8f))
		t = maxHX;
	outX = cx + dx * t;
	outY = cy + dy * t;
	const float minX = margin, maxX = ds.x - margin;
	const float minY = margin, maxY = ds.y - margin;
	if (outX < minX) outX = minX;
	if (outX > maxX) outX = maxX;
	if (outY < minY) outY = minY;
	if (outY > maxY) outY = maxY;
}

void YawPitchEdge(const Vector_t& world, const Vector_t& eye, const QAngle_t& viewAng,
	float margin, float& outX, float& outY)
{
	const float dxw = world.x - eye.x;
	const float dyw = world.y - eye.y;
	const float dzw = world.z - eye.z;
	const float horiz = std::sqrt(dxw * dxw + dyw * dyw);

	float screenDx = 0.f;
	float screenDy = -1.f;
	if (horiz > 1e-3f) {
		const float targetYaw = std::atan2(dyw, dxw) * kRad2Deg;
		const float dyaw = NormalizeYawDeg(targetYaw - viewAng.y) * kDeg2Rad;
		// CS2: +yaw left → negative screen X
		screenDx = -std::sin(dyaw);
		screenDy = -std::cos(dyaw);
	}
	if (horiz > 1e-3f || std::fabs(dzw) > 1e-3f) {
		const float targetPitch = -std::atan2(dzw, (std::max)(horiz, 1e-3f)) * kRad2Deg;
		const float dpitch = NormalizeYawDeg(targetPitch - viewAng.x) * kDeg2Rad;
		screenDy += std::sin(dpitch) * 0.65f;
	}
	ClampDirToEdge(screenDx, screenDy, margin, outX, outY);
}

} // namespace

bool Init() {
	if (g_inited)
		return g_fnScreenTransform != 0 || g_matrixBase != nullptr;
	g_inited = true;

	// Dump ScreenTransform @ RVA 0xBB2BB0 — unique synth (not the 3 false hits).
	g_fnScreenTransform = ScanClient("ScreenTransform",
		"48 89 74 24 10 57 48 83 EC 30 48 83 3D ? ? ? ? 00 48 8B FA 48 8B F1 74");
	if (!g_fnScreenTransform) {
		// Fallback: first match of shorter pattern that is the real one (calls BB2AD0)
		g_fnScreenTransform = ScanClient("ScreenTransform",
			"48 89 74 24 10 57 48 83 EC 30 48 83 3D");
	}

	// Dump pViewMatrix → unk_1823A9340 (64-byte slots). Do NOT hook misnamed
	// GetMatrixForView (dump RVA 0x1666C0 is a 3-arg FOV helper — was poisoning g_live).
	if (uintptr_t site = ScanClient("pViewMatrix", "48 8D 0D ? ? ? ? 48 C1 E0 06")) {
		const uintptr_t abs = M::getAbsoluteAddress(site, 3, 0);
		if (abs)
			g_matrixBase = reinterpret_cast<viewmatrix_t*>(abs);
	}

	g_hasLive = false;
	return true;
}

void OnMatrixCaptured(const viewmatrix_t* worldToProjection) {
	// Kept for API compat — only accept if it looks like a real projection row.
	if (!worldToProjection || !Mem::IsReadable(worldToProjection, sizeof(viewmatrix_t)))
		return;
	float w00 = 0.f, w33 = 0.f;
	__try {
		w00 = worldToProjection->matrix[0][0];
		w33 = worldToProjection->matrix[3][3];
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (!std::isfinite(w00) || !std::isfinite(w33))
		return;
	// Reject near-zero / identity-looking junk from the old wrong hook
	if (std::fabs(w00) < 1e-6f && std::fabs(w33) < 1e-6f)
		return;
	if (SehCopyMatrix(worldToProjection, &g_live))
		g_hasLive = true;
}

const viewmatrix_t* Matrix() {
	if (g_hasLive)
		return &g_live;
	// Slot 0 — ScreenTransform picks active slot; game path preferred in WorldToScreen.
	return g_matrixBase;
}

bool HasLiveMatrix() {
	return g_hasLive || g_matrixBase != nullptr || g_fnScreenTransform != 0;
}

uintptr_t GetMatrixForViewAddr() {
	// Intentionally 0 — dump "GetMatrixForView" is not a matrix capture.
	return 0;
}

bool ScreenTransform(const Vector_t& world, Vector_t& clipOut) {
	bool behind = true;
	if (!SehScreenTransform(world, clipOut, behind))
		return false;
	return !behind;
}

bool WorldToScreen(const Vector_t& world, Vector_t& screen) {
	const ImVec2 wS = ImGui::GetIO().DisplaySize;
	if (wS.x <= 1.f || wS.y <= 1.f)
		return false;

	// Prefer IDA ScreenTransform (active view slot + correct divide).
	bool behind = true;
	Vector_t ndc{};
	if (SehScreenTransform(world, ndc, behind) && !behind) {
		screen.x = (wS.x * 0.5f) + (ndc.x * wS.x * 0.5f);
		screen.y = (wS.y * 0.5f) - (ndc.y * wS.y * 0.5f);
		return std::isfinite(screen.x) && std::isfinite(screen.y);
	}

	if (const viewmatrix_t* m = Matrix()) {
		if (Project(m, world, screen, false))
			return true;
	}
	return false;
}

bool ProjectOrEdge(const Vector_t& world, float& outX, float& outY, bool& onScreen,
	float margin, const Vector_t& eye, const QAngle_t& viewAng)
{
	onScreen = false;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x < 2.f || ds.y < 2.f)
		return false;
	const float minX = margin, maxX = ds.x - margin;
	const float minY = margin, maxY = ds.y - margin;
	const float cx = ds.x * 0.5f;
	const float cy = ds.y * 0.5f;

	// 1) Game ScreenTransform — in front: use NDC (on-screen or clamp to edge).
	bool behind = true;
	Vector_t ndc{};
	if (SehScreenTransform(world, ndc, behind) && !behind) {
		const float sx = cx + ndc.x * cx;
		const float sy = cy - ndc.y * cy;
		if (sx >= minX && sx <= maxX && sy >= minY && sy <= maxY) {
			outX = sx;
			outY = sy;
			onScreen = true;
			return true;
		}
		// In front but off frustum — project direction to edge (not yaw flip).
		ClampDirToEdge(sx - cx, sy - cy, margin, outX, outY);
		return true;
	}

	// 2) Matrix path (same rule) if ScreenTransform miss
	const viewmatrix_t* m = Matrix();
	float clipX = 0.f, clipY = 0.f, clipW = 0.f;
	if (m && ProjectClip(m, world, clipX, clipY, clipW) && clipW >= kNearW) {
		const float invW = 1.0f / clipW;
		const float sx = cx + clipX * invW * cx;
		const float sy = cy - clipY * invW * cy;
		if (sx >= minX && sx <= maxX && sy >= minY && sy <= maxY) {
			outX = sx;
			outY = sy;
			onScreen = true;
			return true;
		}
		ClampDirToEdge(sx - cx, sy - cy, margin, outX, outY);
		return true;
	}

	// 3) Behind camera — yaw/pitch compass only
	YawPitchEdge(world, eye, viewAng, margin, outX, outY);
	return true;
}

} // namespace W2S
