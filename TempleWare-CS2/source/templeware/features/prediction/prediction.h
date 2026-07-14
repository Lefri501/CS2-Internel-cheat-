#pragma once
#include "../../utils/math/vector/vector.h"
#include <cstdint>

// Local movement prediction for bhop / autostrafer.
// Engine addresses resolved from cs2_patterns_raw.txt + IDA a2bd4708:
//   PPREDICTION          → PPREDICTION_Get @ 0x180B91280 → &g_Prediction
//   RUNCOMMAND_CONTEXT   → CPrediction_RunCommand @ 0x180A1C740
//   PROCESSMOVEMENT      → ProcessMovement @ 0x180A1AD70 (call site in RunCommand)
//   PROCESSSUBTICKINPUT  → ProcessSubtickInput @ 0x180B039F0
//   MODERNSUBTICKJUMPCHECK → ModernSubtickJumpCheck @ 0x180882650
//   CREATENEWSUBTICKMOVESTEP → CreateNewSubtickMoveStep (E8 rip)
// RUNPREDICTION / SETUPMOVE patterns in dump are stale on this build.

namespace Pred {

constexpr float kTickInterval = 1.f / 64.f;
constexpr float kGravity = 800.f;
constexpr float kSvAirAccelerate = 12.f;
constexpr float kSvAccelerate = 5.5f;
constexpr float kSvFriction = 5.2f;
constexpr float kSvStopSpeed = 80.f;
constexpr float kAirWishSpeed = 30.f;   // Source air wishspeed clamp
constexpr float kMaxMove = 450.f;

struct MoveState {
	Vector_t origin{};
	Vector_t velocity{};
	bool onGround = false;
	float surfaceFriction = 1.f;
	float maxSpeed = 250.f;
};

struct EngineAddrs {
	void* pPrediction = nullptr;          // g_Prediction*
	void* runCommand = nullptr;
	void* processMovement = nullptr;
	void* processSubtickInput = nullptr;
	void* modernSubtickJumpCheck = nullptr;
	void* createNewSubtickMoveStep = nullptr;
	bool resolved = false;
};

bool Init();
const EngineAddrs& Engines();

void AirAccelerate(
	Vector_t& vel,
	const Vector_t& wishdir,
	float wishspeed,
	float accel,
	float frametime,
	float surfaceFriction = 1.f);

void Friction(
	Vector_t& vel,
	bool onGround,
	float friction,
	float stopspeed,
	float frametime,
	float surfaceFriction = 1.f);

void ApplyGravity(Vector_t& vel, float gravity, float frametime);

// One tick of 2D air/ground accel from yaw + forward/side (Source-style).
MoveState SimulateTick(
	const MoveState& in,
	float yawDeg,
	float forwardMove,
	float sideMove,
	float frametime = kTickInterval);

float VelocityYawDeg(const Vector_t& vel);
float IdealStrafeDeltaDeg(float speed2d, float wishspeed = kAirWishSpeed);

// Gravity landing fraction within a tick (subtick jump when).
bool PredictLandingFrac(
	const Vector_t& origin,
	const Vector_t& mins,
	const Vector_t& vel,
	float groundZ,
	float& outFrac,
	float gravity = kGravity,
	float frametime = kTickInterval,
	float window = 32.f);

// Predicted 2D speed after applying a proposed air strafe this tick.
float PredictSpeedAfterStrafe(
	const Vector_t& vel,
	float yawDeg,
	float sideSign, // -1 left, +1 right
	float frametime = kTickInterval,
	float surfaceFriction = 1.f);

} // namespace Pred
