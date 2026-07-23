#pragma once
#include "../../utils/math/vector/vector.h"
#include <cstdint>

// Engine + local movement prediction.
// IDA-verified 2026-07-22 (client.dll.i64 session f01b8369, imagebase 0x180000000):
//   ProcessMovement              0x180A1B1C0  (moveSvc*, CUserCmd*)  free fn
//   RunCommand_Context           0x180A1CB90  free fn (moveSvc*, cmd*)
//                                internally: vfunc[46] SetPredictionCommand →
//                                tickbase++ → gv setup → ProcessMovement →
//                                vfunc[47] Reset → gv restore. Canonical path.
//   pPrediction getter           0x180B916B0  → &unk_1823A4140 (RVA 0x23A4140)
//     pattern: 48 8D 05 ? ? ? ? C3 CC...40 53 56 41 54  (unique, 1 hit)
//     CreateInterface name: Source2ClientPrediction001 (xref sub_1800D59F0)
//     CPrediction ctor 0x180B6A0B0 zeros +0x34 (BYTE) and +0xF0 (WORD).
//     in_prediction=+0x34 / first_prediction=+0xF0 are inferred from ctor —
//     verify against reader before adjusting.
//   ProcessSubTickInput          0x180B03E40  (flWhen < 1.0, max 32 steps)
//   ModernSubtickJumpCheck       0x180882870
//   CreateNewSubtickMoveStep     0x1804E23F0  (E8 from CreateMove fill)
//   MovementServices_CheckJumpButton 0x180B0D130
//   WriteSubtickFromEntry        0x180C8E7E0
//   ForceButtonsDown             0x180A11230
//   SetupMove                    0x180D54460
// GlobalVars off_18208FD60 (RVA 0x208FD60):
//   curtime+0x30 frametime+0x34 tickcount+0x44 field20+0x50 threadId+0x58
// MS vfuncs: 46=SetPredictionCommand(cmd), 47=Reset(). vfunc 32 was previously
// treated as "RunCommand" — this is WRONG. Real RunCommand is the free fn
// above. vfunc 32 kept as fallback only when free fn resolution fails.
// Dump ModernJump: CCSPlayer_MovementServices+0x6C8, m_flLastLandedFrac +0x24
//
// KNOWN BROKEN (do not use — patterns hit schema-registration fns):
//   QueueForceSubtickMove pattern → sub_180A0A890 (schema desc for
//     CPlayer_MovementServices fields). NOT a runtime queue.
//   ProcessForceSubtickMoves pattern → sub_180A15A90 (schema desc registrar
//     for m_arrForceSubtickMoveWhen). NOT a runtime drain.
//   Force-subtick API (Queue/Process/Flush) resolves as disabled at Init.

class CUserCmd;
class C_CSPlayerPawn;

namespace Pred {

constexpr float kTickInterval = 1.f / 64.f;
constexpr float kGravity = 800.f;
constexpr float kSvAirAccelerate = 12.f;
constexpr float kSvAccelerate = 5.5f;
constexpr float kSvFriction = 5.2f;
constexpr float kSvStopSpeed = 80.f;
constexpr float kAirWishSpeed = 30.f;
constexpr float kMaxMove = 450.f;

// Dump RVA fallbacks (client.dll base + rva) — build 14169
constexpr std::uintptr_t kRvaGlobalVars = 0x208FD60;
constexpr std::uintptr_t kRvaPrediction = 0x23A4140;

// CPrediction field offsets (1.1.5 + dump-stable)
constexpr std::uintptr_t kPredOffInPrediction = 0x34;
constexpr std::uintptr_t kPredOffFirstPrediction = 0xF0;

// Movement services vfuncs (IDA RunCommand uses *a1+368 / +376)
constexpr std::size_t kVfuncSetPredictionCommand = 46;
constexpr std::size_t kVfuncResetPredictionCommand = 47;
constexpr std::size_t kVfuncRunCommand = 32;

// ModernJump dump offsets
constexpr std::uintptr_t kModernJumpOff = 0x6C8;
constexpr std::uintptr_t kLastLandedFracOff = 0x24;
constexpr std::uintptr_t kLastLandedTickOff = 0x20;

struct MoveState {
	Vector_t origin{};
	Vector_t velocity{};
	bool onGround = false;
	float surfaceFriction = 1.f;
	float maxSpeed = 250.f;
};

// Snapshot after last successful engine RunCommand / ProcessMovement / local fallback
struct PredictedState {
	Vector_t origin{};
	Vector_t absOrigin{};
	Vector_t velocity{};
	Vector_t absVelocity{};
	Vector_t eye{};
	std::uint32_t flags = 0;       // post-sim (same as postFlags)
	std::uint32_t preFlags = 0;
	std::uint32_t postFlags = 0;
	int tickBase = 0;
	float curtime = 0.f;
	float frametime = kTickInterval;
	float landFrac = 0.f;          // m_ModernJump.m_flLastLandedFrac post-sim
	bool hasLandFrac = false;
	bool onGround = false;
	bool valid = false;
	bool fromEngine = false;
	bool usedRunCommand = false;
};

struct EngineAddrs {
	void* pPrediction = nullptr;           // CPrediction*
	void* runCommand = nullptr;            // RunCommand_Context free fn (preferred)
	void* processMovement = nullptr;
	void* processSubtickInput = nullptr;
	void* modernSubtickJumpCheck = nullptr;
	void* createNewSubtickMoveStep = nullptr;
	void* checkJumpButton = nullptr;
	void* writeSubtickFromEntry = nullptr;
	void* forceButtonsDown = nullptr;
	void* queueForceSubtickMove = nullptr;
	void* processForceSubtickMoves = nullptr;
	void* setupMove = nullptr;
	void* globalVars = nullptr;            // CGlobalVarsBase*
	bool resolved = false;
};

bool Init();
const EngineAddrs& Engines();

// QueueForceSubtickMove — DEAD (schema registrar only). Use AimCommon::StampCmdAngles.
bool QueueForceSubtick(float pitch, float yaw);
// ProcessForceSubtickMoves — DEAD. No-op when ptr null.
bool ProcessForceSubtick(CUserCmd* cmd);

// WriteSubtickFromEntry — engine protobuf writer for input history (silent stamp).
bool StampInputHistory(
	void* historyEntryPb,
	int renderTick,
	float renderFrac,
	int playerTick,
	float playerFrac,
	const QAngle_t& angles,
	const Vector_t& shootPos);

// ---- CreateMove prediction (UC official-server desync fix + IDA) ----
// UC thread 748659: incomplete restore + wrong first_prediction + Update() = online lag.
// IDA free RunCommand 0x180A1CB90: SetPredCmd → tickbase++ → gv → ProcessMovement → Reset → gv.
// Start: backup full pawn (origin/vel/baseVel/flags/ground/stamina/ModernJump/buttons/tickbase)
//        first_prediction=true, strip attack/jump, free RunCommand, capture post, full restore.
// End: do NOT re-sample live flags (would overwrite post with pre-sim). No CPrediction::Update.
// Never engine2 RunPrediction.
bool Start(CUserCmd* cmd);
void End();
// Map unload / death — drop cached postFlags / pawn
void Invalidate();
bool Active();
const PredictedState& Last();

// True when last Start used free RunCommand / ProcessMovement (not local math).
bool LastFromEngine();

// Optional: call ProcessForceSubtickMoves after feature queue (silent angles).
bool FlushForceSubticks();

// Prefer predicted state when valid (engine or local), else live pawn.
Vector_t Velocity(C_CSPlayerPawn* pawn);
std::uint32_t Flags(C_CSPlayerPawn* pawn);
Vector_t Origin(C_CSPlayerPawn* pawn);
Vector_t Eye(C_CSPlayerPawn* pawn);
float LandFrac(C_CSPlayerPawn* pawn);
bool OnGround(C_CSPlayerPawn* pawn);

// ---- Local Source-style math (bhop / autostrafer) ----
void Accelerate(
	Vector_t& vel,
	const Vector_t& wishdir,
	float wishspeed,
	float accel,
	float frametime,
	float maxspeed,
	float surfaceFriction = 1.f);

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

MoveState SimulateTick(
	const MoveState& in,
	float yawDeg,
	float forwardMove,
	float sideMove,
	float frametime = kTickInterval);

float VelocityYawDeg(const Vector_t& vel);
float IdealStrafeDeltaDeg(float speed2d, float wishspeed = kAirWishSpeed);

bool PredictLandingFrac(
	const Vector_t& origin,
	const Vector_t& mins,
	const Vector_t& vel,
	float groundZ,
	float& outFrac,
	float gravity = kGravity,
	float frametime = kTickInterval,
	float window = 32.f);

// Hull-trace land this tick (uneven ground / stairs / ramps).
// True when airborne falling and TraceHull hits floor within one tick.
// outFrac = subtick when contact happens (for IN_JUMP press).
bool PredictLandThisTick(
	C_CSPlayerPawn* pawn,
	const Vector_t& vel,
	float& outFrac,
	float frametime = kTickInterval);

// Straight-down hull: distance to floor under feet (0 = standing on floor).
// Used with free-fall timing for high drops / stairs when multi-step hull is noisy.
bool ProbeFloorDistance(C_CSPlayerPawn* pawn, float& outDist, float maxDist = 128.f);

float PredictSpeedAfterStrafe(
	const Vector_t& vel,
	float yawDeg,
	float sideSign,
	float frametime = kTickInterval,
	float surfaceFriction = 1.f);

float PredictSpeedAfterWishMoves(
	const Vector_t& vel,
	float viewYawDeg,
	float wishYawDeg,
	float frametime = kTickInterval,
	float surfaceFriction = 1.f);

} // namespace Pred
