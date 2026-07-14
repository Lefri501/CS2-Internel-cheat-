#pragma once
#include "../../module/module.h"

#include <limits>
#include <cmath>


struct QAngle_t;
struct Matrix3x4_t;

struct Vector2D_t {
	constexpr Vector2D_t(const float x = 0.0f, const float y = 0.0f) :
		x(x), y(y) {
	}

	[[nodiscard]] bool IsZero() const {
		return (this->x == 0.0f && this->y == 0.0f);
	}

	float x = 0.0f, y = 0.0f;
};

struct Vector_t {
public:
	constexpr Vector_t(const float x = 0.0f, const float y = 0.0f, const float z = 0.0f) :
		x(x), y(y), z(z) {
	}

	constexpr Vector_t(const float* arrVector) :
		x(arrVector[0]), y(arrVector[1]), z(arrVector[2]) {
	}

	constexpr Vector_t(const Vector2D_t& vecBase2D) :
		x(vecBase2D.x), y(vecBase2D.y) {
	}


	[[nodiscard]] float& operator[](const int nIndex) {
		return reinterpret_cast<float*>(this)[nIndex];
	}

	[[nodiscard]] const float& operator[](const int nIndex) const {
		return reinterpret_cast<const float*>(this)[nIndex];
	}

	constexpr Vector_t& operator=(const Vector_t& vecBase) {
		this->x = vecBase.x;
		this->y = vecBase.y;
		this->z = vecBase.z;
		return *this;
	}

	constexpr Vector_t& operator=(const Vector2D_t& vecBase2D) {
		this->x = vecBase2D.x;
		this->y = vecBase2D.y;
		this->z = 0.0f;
		return *this;
	}

	constexpr Vector_t& operator+=(const Vector_t& vecBase) {
		this->x += vecBase.x;
		this->y += vecBase.y;
		this->z += vecBase.z;
		return *this;
	}

	constexpr Vector_t& operator-=(const Vector_t& vecBase) {
		this->x -= vecBase.x;
		this->y -= vecBase.y;
		this->z -= vecBase.z;
		return *this;
	}

	constexpr Vector_t& operator*=(const Vector_t& vecBase) {
		this->x *= vecBase.x;
		this->y *= vecBase.y;
		this->z *= vecBase.z;
		return *this;
	}

	constexpr Vector_t& operator/=(const Vector_t& vecBase) {
		this->x /= vecBase.x;
		this->y /= vecBase.y;
		this->z /= vecBase.z;
		return *this;
	}

	constexpr Vector_t& operator+=(const float flAdd) {
		this->x += flAdd;
		this->y += flAdd;
		this->z += flAdd;
		return *this;
	}

	constexpr Vector_t& operator-=(const float flSubtract) {
		this->x -= flSubtract;
		this->y -= flSubtract;
		this->z -= flSubtract;
		return *this;
	}

	constexpr Vector_t& operator*=(const float flMultiply) {
		this->x *= flMultiply;
		this->y *= flMultiply;
		this->z *= flMultiply;
		return *this;
	}

	constexpr Vector_t& operator/=(const float flDivide) {
		this->x /= flDivide;
		this->y /= flDivide;
		this->z /= flDivide;
		return *this;
	}

	constexpr Vector_t& operator-() {
		this->x = -this->x;
		this->y = -this->y;
		this->z = -this->z;
		return *this;
	}

	constexpr Vector_t operator-() const {
		return { -this->x, -this->y, -this->z };
	}

	Vector_t operator+(const Vector_t& vecAdd) const {
		return { this->x + vecAdd.x, this->y + vecAdd.y, this->z + vecAdd.z };
	}

	Vector_t operator-(const Vector_t& vecSubtract) const {
		return { this->x - vecSubtract.x, this->y - vecSubtract.y, this->z - vecSubtract.z };
	}

	Vector_t operator*(const Vector_t& vecMultiply) const {
		return { this->x * vecMultiply.x, this->y * vecMultiply.y, this->z * vecMultiply.z };
	}

	Vector_t operator/(const Vector_t& vecDivide) const {
		return { this->x / vecDivide.x, this->y / vecDivide.y, this->z / vecDivide.z };
	}

	Vector_t operator+(const float flAdd) const {
		return { this->x + flAdd, this->y + flAdd, this->z + flAdd };
	}

	Vector_t operator-(const float flSubtract) const {
		return { this->x - flSubtract, this->y - flSubtract, this->z - flSubtract };
	}

	Vector_t operator*(const float flMultiply) const {
		return { this->x * flMultiply, this->y * flMultiply, this->z * flMultiply };
	}

	Vector_t operator/(const float flDivide) const {
		return { this->x / flDivide, this->y / flDivide, this->z / flDivide };
	}
	Vector_t Normalize() const {
		float len = Length();
		if (len != 0)
			return { x / len, y / len, z / len };
		return { 0, 0, 0 };
	}
	float Length() const {
		return std::sqrt(x * x + y * y + z * z);
	}
	float Distance(const Vector_t& other) const {
		return std::sqrt((x - other.x) * (x - other.x) +
						 (y - other.y) * (y - other.y) +
						 (z - other.z) * (z - other.z));
	}

	float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Vector4D_t {
	constexpr Vector4D_t(const float x = 0.0f, const float y = 0.0f, const float z = 0.0f, const float w = 0.0f) :
		x(x), y(y), z(z), w(w) {
	}

	float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

struct alignas(16) VectorAligned_t : Vector_t {
	VectorAligned_t() = default;

	explicit VectorAligned_t(const Vector_t& vecBase) {
		this->x = vecBase.x;
		this->y = vecBase.y;
		this->z = vecBase.z;
		this->w = 0.0f;
	}

	constexpr VectorAligned_t& operator=(const Vector_t& vecBase) {
		this->x = vecBase.x;
		this->y = vecBase.y;
		this->z = vecBase.z;
		this->w = 0.0f;
		return *this;
	}

	float w = 0.0f;
};

struct Matrix3x4_t;

struct QAngle_t {
	constexpr QAngle_t(float x = 0.f, float y = 0.f, float z = 0.f) :
		x(x), y(y), z(z) {
	}

	constexpr QAngle_t(const float* arrAngles) :
		x(arrAngles[0]), y(arrAngles[1]), z(arrAngles[2]) {
	}

#pragma region qangle_array_operators

	[[nodiscard]] float& operator[](const int nIndex) {
		return reinterpret_cast<float*>(this)[nIndex];
	}

	[[nodiscard]] const float& operator[](const int nIndex) const {
		return reinterpret_cast<const float*>(this)[nIndex];
	}

#pragma endregion

#pragma region qangle_relational_operators

	bool operator==(const QAngle_t& angBase) const {
		return this->IsEqual(angBase);
	}

	bool operator!=(const QAngle_t& angBase) const {
		return !this->IsEqual(angBase);
	}

#pragma endregion

#pragma region qangle_assignment_operators

	constexpr QAngle_t& operator=(const QAngle_t& angBase) {
		this->x = angBase.x;
		this->y = angBase.y;
		this->z = angBase.z;
		return *this;
	}

#pragma endregion

#pragma region qangle_arithmetic_assignment_operators

	constexpr QAngle_t& operator+=(const QAngle_t& angBase) {
		this->x += angBase.x;
		this->y += angBase.y;
		this->z += angBase.z;
		return *this;
	}

	constexpr QAngle_t& operator-=(const QAngle_t& angBase) {
		this->x -= angBase.x;
		this->y -= angBase.y;
		this->z -= angBase.z;
		return *this;
	}

	constexpr QAngle_t& operator*=(const QAngle_t& angBase) {
		this->x *= angBase.x;
		this->y *= angBase.y;
		this->z *= angBase.z;
		return *this;
	}

	constexpr QAngle_t& operator/=(const QAngle_t& angBase) {
		this->x /= angBase.x;
		this->y /= angBase.y;
		this->z /= angBase.z;
		return *this;
	}

	constexpr QAngle_t& operator+=(const float flAdd) {
		this->x += flAdd;
		this->y += flAdd;
		this->z += flAdd;
		return *this;
	}

	constexpr QAngle_t& operator-=(const float flSubtract) {
		this->x -= flSubtract;
		this->y -= flSubtract;
		this->z -= flSubtract;
		return *this;
	}

	constexpr QAngle_t& operator*=(const float flMultiply) {
		this->x *= flMultiply;
		this->y *= flMultiply;
		this->z *= flMultiply;
		return *this;
	}

	constexpr QAngle_t& operator/=(const float flDivide) {
		this->x /= flDivide;
		this->y /= flDivide;
		this->z /= flDivide;
		return *this;
	}

#pragma endregion

#pragma region qangle_arithmetic_unary_operators

	constexpr QAngle_t& operator-() {
		this->x = -this->x;
		this->y = -this->y;
		this->z = -this->z;
		return *this;
	}

	constexpr QAngle_t operator-() const {
		return { -this->x, -this->y, -this->z };
	}

#pragma endregion

#pragma region qangle_arithmetic_ternary_operators

	constexpr QAngle_t operator+(const QAngle_t& angAdd) const {
		return { this->x + angAdd.x, this->y + angAdd.y, this->z + angAdd.z };
	}

	constexpr QAngle_t operator-(const QAngle_t& angSubtract) const {
		return { this->x - angSubtract.x, this->y - angSubtract.y, this->z - angSubtract.z };
	}

	constexpr QAngle_t operator*(const QAngle_t& angMultiply) const {
		return { this->x * angMultiply.x, this->y * angMultiply.y, this->z * angMultiply.z };
	}

	constexpr QAngle_t operator/(const QAngle_t& angDivide) const {
		return { this->x / angDivide.x, this->y / angDivide.y, this->z / angDivide.z };
	}

	constexpr QAngle_t operator+(const float flAdd) const {
		return { this->x + flAdd, this->y + flAdd, this->z + flAdd };
	}

	constexpr QAngle_t operator-(const float flSubtract) const {
		return { this->x - flSubtract, this->y - flSubtract, this->z - flSubtract };
	}

	constexpr QAngle_t operator*(const float flMultiply) const {
		return { this->x * flMultiply, this->y * flMultiply, this->z * flMultiply };
	}

	constexpr QAngle_t operator/(const float flDivide) const {
		return { this->x / flDivide, this->y / flDivide, this->z / flDivide };
	}

#pragma endregion

	[[nodiscard]] bool IsValid() const {
		return (std::isfinite(this->x) && std::isfinite(this->y) && std::isfinite(this->z));
	}

	[[nodiscard]] bool IsEqual(const QAngle_t& angEqual, const float flErrorMargin = std::numeric_limits<float>::epsilon()) const {
		return (std::fabsf(this->x - angEqual.x) < flErrorMargin && std::fabsf(this->y - angEqual.y) < flErrorMargin && std::fabsf(this->z - angEqual.z) < flErrorMargin);
	}

	[[nodiscard]] bool IsZero() const {
		return (this->x == 0.0f && this->y == 0.0f && this->z == 0.0f);
	}

	/// @returns: length of hypotenuse
	[[nodiscard]] float Length2D() const {
		return std::sqrtf(x * x + y * y);
	}

	QAngle_t& Normalize() {
		this->x = std::remainderf(this->x, 360.f);
		this->y = std::remainderf(this->y, 360.f);
		this->z = std::remainderf(this->z, 360.f);
		return *this;
	}

	void ToDirections(Vector_t* pvecForward, Vector_t* pvecRight = nullptr, Vector_t* pvecUp = nullptr) const;

	[[nodiscard]] Matrix3x4_t ToMatrix(const Vector_t& vecOrigin = {}) const;

public:
	float x = 0.0f, y = 0.0f, z = 0.0f;
};