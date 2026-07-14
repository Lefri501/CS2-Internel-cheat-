#include "vector.h"

#include <cmath>

void QAngle_t::ToDirections(Vector_t* pvecForward, Vector_t* pvecRight, Vector_t* pvecUp) const
{
	constexpr float kDeg2Rad = 3.14159265358979323846f / 180.f;

	const float pitch = this->x * kDeg2Rad;
	const float yaw = this->y * kDeg2Rad;
	const float roll = this->z * kDeg2Rad;

	const float sp = std::sinf(pitch);
	const float cp = std::cosf(pitch);
	const float sy = std::sinf(yaw);
	const float cy = std::cosf(yaw);
	const float sr = std::sinf(roll);
	const float cr = std::cosf(roll);

	if (pvecForward) {
		pvecForward->x = cp * cy;
		pvecForward->y = cp * sy;
		pvecForward->z = -sp;
	}

	if (pvecRight) {
		pvecRight->x = -1.f * sr * sp * cy + -1.f * cr * -sy;
		pvecRight->y = -1.f * sr * sp * sy + -1.f * cr * cy;
		pvecRight->z = -1.f * sr * cp;
	}

	if (pvecUp) {
		pvecUp->x = cr * sp * cy + -sr * -sy;
		pvecUp->y = cr * sp * sy + -sr * cy;
		pvecUp->z = cr * cp;
	}
}
