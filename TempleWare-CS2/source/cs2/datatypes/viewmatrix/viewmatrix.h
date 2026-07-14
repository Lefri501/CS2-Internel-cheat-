#pragma once

struct viewmatrix_t {
	float* operator[ ](int index) {
		return matrix[index];
	}
	float matrix[4][4];
};