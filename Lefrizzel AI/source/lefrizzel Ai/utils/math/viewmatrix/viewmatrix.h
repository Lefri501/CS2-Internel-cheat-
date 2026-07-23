#pragma once

#include "../../../../cs2/datatypes/viewmatrix/viewmatrix.h"
#include "../vector/vector.h"

class ViewMatrix {
public:
	ViewMatrix() : viewMatrix(nullptr) {}
	bool WorldToScreen(const Vector_t& position, Vector_t& out) const;

	// Homogeneous projection: numX/numY are the pre-divide screen numerators,
	// w is clip-space depth (>0 in front of camera). Screen =
	//   center.x + numX / w * center.x , center.y - numY / w * center.y
	// Exposed so ring/path segments can be clipped at the near plane instead
	// of dropped whole (fixes fragmented circles when standing on an effect).
	bool WorldToClip(const Vector_t& position, float& numX, float& numY, float& w) const;

	viewmatrix_t* viewMatrix;
};