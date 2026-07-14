#pragma once
#include <algorithm>

namespace Math {
	template <typename T>
	T clamp(T value, T min, T max) {
		return std::min(std::max(value, min), max);
	}
}