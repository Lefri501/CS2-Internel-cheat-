#pragma once

#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "../../utils/math/vector/vector.h"

// Live W2S via IDA ScreenTransform (0xBB2BB0) + pViewMatrix slot array.
// Dump "GetMatrixForView" is a FOV helper — do not hook for matrix capture.

namespace W2S {

bool Init();

// Optional external matrix feed (validated). Prefer ScreenTransform.
void OnMatrixCaptured(const viewmatrix_t* worldToProjection);

// Prefer ScreenTransform (active view); else pViewMatrix / last capture.
bool WorldToScreen(const Vector_t& world, Vector_t& screen);

const viewmatrix_t* Matrix();
bool HasLiveMatrix();

// Game ScreenTransform. Returns true if in front (IDA return == 0).
bool ScreenTransform(const Vector_t& world, Vector_t& clipOut);

// Always 0 — misnamed dump symbol; kept for link compat.
uintptr_t GetMatrixForViewAddr();

// OOF / edge icons: in-front → NDC (on-screen or clamp to edge);
// behind → yaw/pitch compass (never use behind NDC — IDA scales by 1e5).
bool ProjectOrEdge(const Vector_t& world, float& outX, float& outY, bool& onScreen,
	float margin, const Vector_t& eye, const QAngle_t& viewAng);

} // namespace W2S
