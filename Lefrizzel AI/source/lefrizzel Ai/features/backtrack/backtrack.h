#pragma once

// Lag compensation: AF/TR fire stamp + skeleton draw.
// Records: m_flSimulationTime @ 0x3B8, GlobalVars tickcount, head/skeleton slots.

#include "../../utils/math/vector/vector.h"
#include "../../utils/math/viewmatrix/viewmatrix.h"
#include "../bones/bones.h"
#include <cstdint>

class CUserCmd;
class C_CSPlayerPawn;

namespace Backtrack {

constexpr int kMaxRecords = 64;
constexpr int kMaxTracks = 64;

constexpr std::uintptr_t kSimTimeOff = 0x3B8;
constexpr std::uintptr_t kAnimTimeOff = 0x3B4;
constexpr std::uintptr_t kGvCurtimeOff = 0x30;
constexpr std::uintptr_t kGvTickcountOff = 0x44;
constexpr float kTickInterval = 1.f / 64.f;

struct Record {
	Vector_t origin{};
	Vector_t head{};
	Vector_t slots[Bones::S_COUNT]{};
	bool slotOk[Bones::S_COUNT]{};
	float simTime = 0.f;
	float captureCur = 0.f;
	// Lag-comp stamp: prefer sim-derived tick (enemy pose time).
	// captureRenderTick = client render clock at sample (age / clamp).
	int tickcount = 0;
	int captureRenderTick = 0;
	float tickFrac = 0.f;
	std::uint64_t wallMs = 0;
	bool valid = false;
};

bool WantRecords();
float WindowSec(float preferMs = 0.f);

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* local);

bool BestHead(
	C_CSPlayerPawn* target,
	std::uint32_t handle,
	const Vector_t& eye,
	const QAngle_t& view,
	float maxFov,
	Vector_t& outPoint,
	float& outFov,
	float* outSimTime = nullptr,
	float preferMs = 0.f,
	int* outTick = nullptr,
	float* outTickFrac = nullptr);

// Lag multipoint under crosshair — uses Config::trigger_hitboxes when set,
// else head/neck/chest/stomach/pelvis. outHb = Config::AimHitbox index.
bool BestUnderCrosshair(
	const Vector_t& eye,
	const QAngle_t& view,
	float maxFov,
	Vector_t& outPoint,
	float& outFov,
	float& outSimTime,
	std::uint32_t* outHandle = nullptr,
	float preferMs = 0.f,
	int* outTick = nullptr,
	float* outTickFrac = nullptr,
	int* outHb = nullptr);

// Lagged record for pawn handle. preferMs 0 = default window.
// requireSeparation: skeleton/chams only — skip when lag pose ≈ live (standing still).
const Record* GetLagged(std::uint32_t handle, float preferMs = 0.f, bool requireSeparation = false);

// Resolve handle from live pawn pointer.
std::uint32_t HandleOf(const C_CSPlayerPawn* pawn);

bool StampLagComp(CUserCmd* cmd, float simTime, float preferMs = 0.f);
// Prefer capture tickcount when known (more accurate than simTime alone).
bool StampLagCompTick(CUserCmd* cmd, int renderTick, float renderFrac, float preferMs = 0.f);

void OnFire(CUserCmd* cmd, C_CSPlayerPawn* local, float preferMs = 0.f);

void SetPendingSim(float simTime);
void SetPendingTick(int tick, float frac = 0.f);
float PendingSim();
int PendingTick();
void ClearPending();

void Draw(const ViewMatrix& vm); // skeleton only
void Clear();

float RecordAgeSec(const Record& r, float curtime);

}
