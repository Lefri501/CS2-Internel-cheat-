#define NOMINMAX
#include "prediction.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"
#include <algorithm>
#include <cmath>

namespace Pred {
namespace {

EngineAddrs g_eng{};
bool g_triedInit = false;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.f;
constexpr float kRad2Deg = 180.f / kPi;
constexpr float kMinSubtickFrac = 0.001f;
constexpr float kMaxSubtickFrac = 0.995f;

float Length2D(const Vector_t& v)
{
	return std::sqrt(v.x * v.x + v.y * v.y);
}

void Normalize2D(Vector_t& v)
{
	const float len = Length2D(v);
	if (len > 0.0001f) {
		v.x /= len;
		v.y /= len;
	} else {
		v.x = 0.f;
		v.y = 0.f;
	}
}

void AngleVectorsYaw(float yawDeg, Vector_t& forward, Vector_t& right)
{
	const float yaw = yawDeg * kDeg2Rad;
	const float sy = std::sin(yaw);
	const float cy = std::cos(yaw);
	forward = { cy, sy, 0.f };
	right = { -sy, cy, 0.f }; // Source pitch=0: right = (-sin yaw, cos yaw)
}

void* ResolveRipCall(std::uint8_t* hit)
{
	if (!hit || hit[0] != 0xE8)
		return nullptr;
	return M::GetAbsoluteAddress(hit, 1, 0);
}

void* CallPredictionGetter(void* fn)
{
	if (!fn)
		return nullptr;
	using FnGet = void* (*)();
	__try {
		return reinterpret_cast<FnGet>(fn)();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

} // namespace

bool Init()
{
	if (g_triedInit)
		return g_eng.resolved;
	g_triedInit = true;

	// PPREDICTION: 48 8D 05 ? ? ? ? C3 ... → lea rax, [g_Prediction]; ret
	if (auto* p = M::FindPattern("client",
		"48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 40 53 56 41 54")) {
		g_eng.pPrediction = CallPredictionGetter(p);
	}

	// RUNCOMMAND_CONTEXT pattern from dump is build-specific; resolve ProcessMovement call site instead.
	if (auto* hit = M::FindPattern("client",
		"E8 ? ? ? ? 48 8B 06 48 8B CE FF 90 ? ? ? ? 48 85 DB")) {
		g_eng.processMovement = ResolveRipCall(hit);
		// Call sits inside CPrediction_RunCommand (IDA a2bd4708 @ 0x180A1C740).
		g_eng.runCommand = hit; // call-site marker; full fn start not required for sim
	}

	g_eng.processSubtickInput = M::FindPattern("client",
		"89 54 24 10 48 89 4C 24 08 53 56 57 48 83 EC 70");

	g_eng.modernSubtickJumpCheck = M::FindPattern("client",
		"48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 40 48 8B EA 48 8B D9 48 8B 49 08 BA 02 00 00 00");

	if (auto* createHit = M::FindPattern("client",
		"E8 ? ? ? ? 48 8B D0 48 8B CE E8 ? ? ? ? 48 8B C8"))
		g_eng.createNewSubtickMoveStep = ResolveRipCall(createHit);

	g_eng.resolved =
		g_eng.processMovement != nullptr
		|| g_eng.processSubtickInput != nullptr
		|| g_eng.createNewSubtickMoveStep != nullptr
		|| g_eng.pPrediction != nullptr;

	Con::Info(
		"Pred: pPred=%p runCmd=%p procMove=%p subtickIn=%p jumpChk=%p createStep=%p",
		g_eng.pPrediction, g_eng.runCommand, g_eng.processMovement,
		g_eng.processSubtickInput, g_eng.modernSubtickJumpCheck,
		g_eng.createNewSubtickMoveStep);

	return g_eng.resolved;
}

const EngineAddrs& Engines()
{
	if (!g_triedInit)
		Init();
	return g_eng;
}

void AirAccelerate(
	Vector_t& vel,
	const Vector_t& wishdir,
	float wishspeed,
	float accel,
	float frametime,
	float surfaceFriction)
{
	if (wishspeed <= 0.f)
		return;

	const float currentspeed = vel.x * wishdir.x + vel.y * wishdir.y;
	const float addspeed = wishspeed - currentspeed;
	if (addspeed <= 0.f)
		return;

	float accelspeed = accel * wishspeed * frametime * surfaceFriction;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	vel.x += accelspeed * wishdir.x;
	vel.y += accelspeed * wishdir.y;
}

void Friction(
	Vector_t& vel,
	bool onGround,
	float friction,
	float stopspeed,
	float frametime,
	float surfaceFriction)
{
	if (!onGround)
		return;

	const float speed = Length2D(vel);
	if (speed < 0.1f)
		return;

	float control = (speed < stopspeed) ? stopspeed : speed;
	float drop = control * friction * surfaceFriction * frametime;
	float newspeed = speed - drop;
	if (newspeed < 0.f)
		newspeed = 0.f;
	if (newspeed != speed) {
		newspeed /= speed;
		vel.x *= newspeed;
		vel.y *= newspeed;
	}
}

void ApplyGravity(Vector_t& vel, float gravity, float frametime)
{
	vel.z -= gravity * frametime;
}

MoveState SimulateTick(
	const MoveState& in,
	float yawDeg,
	float forwardMove,
	float sideMove,
	float frametime)
{
	MoveState out = in;
	Friction(out.velocity, out.onGround, kSvFriction, kSvStopSpeed, frametime, out.surfaceFriction);

	Vector_t forward{}, right{};
	AngleVectorsYaw(yawDeg, forward, right);

	Vector_t wishvel{
		forward.x * forwardMove + right.x * sideMove,
		forward.y * forwardMove + right.y * sideMove,
		0.f
	};

	float wishspeed = Length2D(wishvel);
	Vector_t wishdir = wishvel;
	Normalize2D(wishdir);

	if (wishspeed > out.maxSpeed && wishspeed > 0.0001f) {
		const float scale = out.maxSpeed / wishspeed;
		wishvel.x *= scale;
		wishvel.y *= scale;
		wishspeed = out.maxSpeed;
	}

	if (out.onGround) {
		AirAccelerate(out.velocity, wishdir, wishspeed, kSvAccelerate, frametime, out.surfaceFriction);
	} else {
		const float airWish = (wishspeed > kAirWishSpeed) ? kAirWishSpeed : wishspeed;
		AirAccelerate(out.velocity, wishdir, airWish, kSvAirAccelerate, frametime, out.surfaceFriction);
		ApplyGravity(out.velocity, kGravity, frametime);
	}

	out.origin.x += out.velocity.x * frametime;
	out.origin.y += out.velocity.y * frametime;
	out.origin.z += out.velocity.z * frametime;
	return out;
}

float VelocityYawDeg(const Vector_t& vel)
{
	return std::atan2(vel.y, vel.x) * kRad2Deg;
}

float IdealStrafeDeltaDeg(float speed2d, float wishspeed)
{
	if (speed2d < 1.f)
		return 90.f;
	const float ratio = wishspeed / speed2d;
	if (ratio >= 1.f)
		return 90.f;
	// atan(wish/speed) — optimal circle-strafe offset from velocity yaw
	return std::atan(ratio) * kRad2Deg;
}

bool PredictLandingFrac(
	const Vector_t& origin,
	const Vector_t& mins,
	const Vector_t& vel,
	float groundZ,
	float& outFrac,
	float gravity,
	float frametime,
	float window)
{
	if (vel.z >= 0.f)
		return false;

	const float bottom = origin.z + mins.z;
	const float predictedBottom =
		bottom + vel.z * frametime
		- 0.5f * gravity * frametime * frametime;

	if (bottom < groundZ - window)
		return false;
	if (predictedBottom > groundZ + window)
		return false;

	const float travel = bottom - predictedBottom;
	if (travel <= 0.0001f)
		return false;

	outFrac = std::clamp(
		(bottom - groundZ) / travel,
		kMinSubtickFrac,
		kMaxSubtickFrac);
	return true;
}

float PredictSpeedAfterStrafe(
	const Vector_t& vel,
	float yawDeg,
	float sideSign,
	float frametime,
	float surfaceFriction)
{
	MoveState st{};
	st.velocity = vel;
	st.onGround = false;
	st.surfaceFriction = surfaceFriction;
	st.maxSpeed = 260.f;

	const MoveState out = SimulateTick(st, yawDeg, 0.f, sideSign * kMaxMove, frametime);
	return Length2D(out.velocity);
}

} // namespace Pred
