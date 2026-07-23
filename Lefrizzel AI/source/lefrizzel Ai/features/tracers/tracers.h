#pragma once

// Local bullet tracers: eye → server bullet_impact (same event as hitmarker pin).
// IDA client: event "bullet_impact" fields x/y/z float, userid = shooter.

#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "../../utils/math/vector/vector.h"

namespace Tracers {

enum Style : int {
	STYLE_BEAM = 0,   // 2-layer body + impact flare
	STYLE_LASER = 1,  // thin white needle + tip
	STYLE_GLOW = 2,   // fat soft haze (2 lines)
	STYLE_DASHED = 3, // ≤10 animated dashes
	STYLE_ENERGY = 4, // dual rail + traveling node
	STYLE_COUNT
};

// Fire: TraceLine land + draw. Impact snaps end to server hit.
void NoteFire(const Vector_t& eye, const Vector_t& dir);
void OnImpact(const Vector_t& end);
void Draw(const ViewMatrix& vm);
void Clear();

} // namespace Tracers
