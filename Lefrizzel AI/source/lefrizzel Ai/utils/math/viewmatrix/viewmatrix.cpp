#include "viewmatrix.h"

#include "../../../features/w2s/w2s.h"
#include "../../../../../external/imgui/imgui.h"
#include <cmath>

bool ViewMatrix::WorldToScreen(const Vector_t& position, Vector_t& out) const {
	// Prefer live GetMatrixForView capture / ScreenTransform when available
	if (W2S::HasLiveMatrix() || W2S::Matrix()) {
		if (W2S::WorldToScreen(position, out))
			return true;
	}

	const viewmatrix_t* m = viewMatrix ? viewMatrix : W2S::Matrix();
	if (!m)
		return false;

	const float w = m->matrix[3][0] * position.x + m->matrix[3][1] * position.y + m->matrix[3][2] * position.z + m->matrix[3][3];
	if (w <= 0.001f)
		return false;

	const float invW = 1.0f / w;
	const ImVec2 wS = ImGui::GetIO().DisplaySize;
	if (wS.x <= 1.f || wS.y <= 1.f)
		return false;

	const float centerX = wS.x * 0.5f;
	const float centerY = wS.y * 0.5f;

	out.x = centerX + ((m->matrix[0][0] * position.x + m->matrix[0][1] * position.y + m->matrix[0][2] * position.z + m->matrix[0][3]) * invW * centerX);
	out.y = centerY - ((m->matrix[1][0] * position.x + m->matrix[1][1] * position.y + m->matrix[1][2] * position.z + m->matrix[1][3]) * invW * centerY);
	return true;
}

bool ViewMatrix::WorldToClip(const Vector_t& position, float& numX, float& numY, float& w) const {
	const viewmatrix_t* m = viewMatrix ? viewMatrix : W2S::Matrix();
	if (!m)
		return false;

	w = m->matrix[3][0] * position.x + m->matrix[3][1] * position.y + m->matrix[3][2] * position.z + m->matrix[3][3];
	numX = m->matrix[0][0] * position.x + m->matrix[0][1] * position.y + m->matrix[0][2] * position.z + m->matrix[0][3];
	numY = m->matrix[1][0] * position.x + m->matrix[1][1] * position.y + m->matrix[1][2] * position.z + m->matrix[1][3];

	return std::isfinite(w) && std::isfinite(numX) && std::isfinite(numY);
}