#define NOMINMAX
#include "prediction.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/module/module.h"
#include "../../utils/console/console.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../interfaces/interfaces.h"
#include "../../hooks/hooks.h"
#include "../engine2/engine2.h"
#include "../trace/trace.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <Windows.h>

namespace Pred {
namespace {

EngineAddrs g_eng{};
bool g_triedInit = false;

PredictedState g_last{};
bool g_active = false;
int g_lastSequence = 0;

// CPrediction flag backup (1.1.5)
struct PredIfaceBackup {
	bool inPrediction = false;
	bool firstPrediction = false;
	bool cmdHasBeenPredicted = false;
	bool ok = false;
};
PredIfaceBackup g_predIfaceBak{};

// ---- SEH helpers (no C++ objects with dtors) ----

void* SehCallGetter(void* fn)
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

void* SehReadPtr(void** pp)
{
	if (!pp)
		return nullptr;
	__try {
		return *pp;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool SehCallProcessMovement(void* fn, void* moveSvc, CUserCmd* cmd)
{
	if (!fn || !moveSvc || !cmd)
		return false;
	using Fn = void(__fastcall*)(void*, CUserCmd*);
	__try {
		reinterpret_cast<Fn>(fn)(moveSvc, cmd);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("Pred::ProcessMovement", GetExceptionCode());
		return false;
	}
}

// IDA RunCommand_Context 0x180A1CB90: free (moveSvc*, cmd*) — advances tickbase + ProcessMovement
// IDA-verified: sub_180A1CB90 = RunCommand_Context(moveSvc, cmd).
// Internally: lock → vfunc[46] SetPredictionCommand → owner tickbase++ →
// backup gv → set gv (tick, curtime, frametime, threadId) → ProcessMovement
// (sub_180A1B1C0) → vfunc[47] Reset → restore gv.
// Prefer this over 3 separate vfunc calls — vfunc[32] is NOT the sim runner.
bool SehCallRunCommand(void* fn, void* moveSvc, CUserCmd* cmd)
{
	if (!fn || !moveSvc || !cmd)
		return false;
	using Fn = void(__fastcall*)(void*, CUserCmd*);
	__try {
		reinterpret_cast<Fn>(fn)(moveSvc, cmd);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("Pred::RunCommand", GetExceptionCode());
		return false;
	}
}

bool SehCallVfuncCmd(void* thisptr, std::size_t index, CUserCmd* cmd)
{
	if (!thisptr)
		return false;
	__try {
		auto** vt = *reinterpret_cast<void***>(thisptr);
		if (!vt || !vt[index])
			return false;
		using Fn = void(__fastcall*)(void*, CUserCmd*);
		reinterpret_cast<Fn>(vt[index])(thisptr, cmd);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehCallVfunc0(void* thisptr, std::size_t index)
{
	if (!thisptr)
		return false;
	__try {
		auto** vt = *reinterpret_cast<void***>(thisptr);
		if (!vt || !vt[index])
			return false;
		using Fn = void(__fastcall*)(void*);
		reinterpret_cast<Fn>(vt[index])(thisptr);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Read ModernJump land frac from movement services (dump 0x6C8+0x24)
// Accept any finite [0,1) — 0 is valid "start of tick" press; caller decides.
bool SehReadLandFrac(void* moveSvc, float& outFrac)
{
	outFrac = 0.f;
	if (!moveSvc)
		return false;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(moveSvc);
		const float lf = *reinterpret_cast<float*>(base + kModernJumpOff + kLastLandedFracOff);
		if (!std::isfinite(lf) || lf < 0.f || lf >= 1.f)
			return false;
		outFrac = lf;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadPredBool(void* pred, std::uintptr_t off, bool& out)
{
	out = false;
	if (!pred)
		return false;
	__try {
		out = *reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(pred) + off);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehWritePredBool(void* pred, std::uintptr_t off, bool val)
{
	if (!pred)
		return false;
	__try {
		*reinterpret_cast<bool*>(reinterpret_cast<std::uint8_t*>(pred) + off) = val;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

C_CSPlayerPawn* SehLocalPawn()
{
	// Pred only valid on alive pawn — dead/respawn services free'd
	return H::SafeLocalAlive();
}

int SehPawnHealth(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return 0;
	__try {
		return pawn->m_iHealth();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

void* SehMoveServices(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return nullptr;
	__try {
		return pawn->m_pMovementServices();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// ---- GlobalVars layout (IDA ProcessMovement / RunCommand) ----
// curtime    +0x30  (float index 12)
// frametime  +0x34  (float index 13)  — 0.015625
// tickcount  +0x44  (int   index 17)
// field20    +0x50  (float index 20)
// threadId   +0x58  (int   index 22)

struct GlobalVarsBackup {
	float curtime = 0.f;
	float frametime = 0.f;
	int tickcount = 0;
	float field20 = 0.f;
	int threadId = 0;
	bool ok = false;
};

// moveSvc+0x50 button state (IDA IsButtonActive). RunCommand latches IN_JUMP
// here; if not restored after pred, next real jump sees held → no rising edge → sticky.
struct MoveSvcBtnBak {
	std::uint64_t nValue = 0;
	std::uint64_t nValueChanged = 0;
	std::uint64_t nValueScroll = 0;
	void* moveSvc = nullptr;
	bool ok = false;
};

// IDA ModernJump @ moveSvc+0x6C8 (client_dll dump build 14169):
//   +0x10 lastActualPressTick, +0x14 frac, +0x18 lastUsableTick, +0x1C frac
//   +0x20 lastLandedTick, +0x24 lastLandedFrac
//   +0x28..+0x30 lastLandedVelocity XYZ  (UC incomplete-restore → land desync online)
// ProcessMovement mutates this; without restore, spam window sticky-locks hop.
struct ModernJumpBak {
	std::int32_t lastActualTick = 0;
	float lastActualFrac = 0.f;
	std::int32_t lastUsableTick = 0;
	float lastUsableFrac = 0.f;
	std::int32_t lastLandedTick = 0;
	float lastLandedFrac = 0.f;
	float lastLandedVelX = 0.f;
	float lastLandedVelY = 0.f;
	float lastLandedVelZ = 0.f;
	std::uint8_t legacyOldJump = 0;
	bool ok = false;
};

// UC "prediction unstable official servers" — incomplete restore leaks sim state.
// Free RunCommand (IDA 0x180A1CB90) mutates: origin/vel/flags/ground/stamina/
// ModernJump/buttons/tickbase. Must restore ALL before returning to engine.
struct PawnBackup {
	Vector_t absOrigin{};
	Vector_t oldOrigin{};
	Vector_t velocity{};
	Vector_t absVelocity{};
	Vector_t baseVelocity{};
	Vector_t viewOffset{};
	std::uint32_t flags = 0;
	std::uint32_t groundEntity = 0xFFFFFFFFu;
	float stamina = 0.f;
	float staminaAtJumpStart = 0.f;
	int tickBase = 0;
	void* sceneNode = nullptr;
	void* controller = nullptr;
	MoveSvcBtnBak moveBtn{};
	ModernJumpBak modernJump{};
	bool ok = false;
};

// Dump offsets (client_dll.hpp)
constexpr std::uintptr_t kPawnFlagsOff = 0x3F4;
constexpr std::uintptr_t kPawnAbsVelOff = 0x3F8;
constexpr std::uintptr_t kPawnVelOff = 0x430;
constexpr std::uintptr_t kPawnBaseVelOff = 0x510;
constexpr std::uintptr_t kPawnGroundOff = 0x530;
constexpr std::uintptr_t kPawnOldOriginOff = 0x13B8;
constexpr std::uintptr_t kStaminaOff = 0x694;
constexpr std::uintptr_t kStaminaJumpOff = 0x6A4;

constexpr std::uintptr_t kMoveSvcButtonsOff = 0x50;

GlobalVarsBackup g_gvBak{};
PawnBackup g_pawnBak{};
C_CSPlayerPawn* g_pawn = nullptr;

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
	// Match IDA AngleVectors 0x18160ED70 (pitch=roll=0): right = (-sy, cy) = visual LEFT.
	// Movement: wishvel = fwd*fmove + right*smove → +smove = LEFT.
	const float yaw = yawDeg * kDeg2Rad;
	const float sy = std::sin(yaw);
	const float cy = std::cos(yaw);
	forward = { cy, sy, 0.f };
	right = { -sy, cy, 0.f };
}

void* ResolveRipCall(std::uint8_t* hit)
{
	if (!hit || hit[0] != 0xE8)
		return nullptr;
	return M::GetAbsoluteAddress(hit, 1, 0);
}

void* ResolveGlobalVars()
{
	// All IDA hits for this sig load off_18208FD60 (curtime @ +0x30).
	std::uint8_t* hit = M::FindPattern("client", "48 8B 05 ? ? ? ? F3 0F 10 40 30 C3");
	if (!hit)
		hit = M::FindPattern("client", "48 8B 05 ? ? ? ? 0F 57 C0 8B 48");
	if (hit) {
		void** pp = reinterpret_cast<void**>(M::GetAbsoluteAddress(hit, 3, 0));
		if (void* gv = SehReadPtr(pp))
			return gv;
	}
	// Dump RVA fallback: client + 0x208FD60 → pointer to CGlobalVarsBase
	const uintptr_t base = modules.getModule("client");
	if (!base)
		return nullptr;
	return SehReadPtr(reinterpret_cast<void**>(base + kRvaGlobalVars));
}

bool BackupGlobalVars(GlobalVarsBackup& out)
{
	out = {};
	if (!g_eng.globalVars)
		return false;
	std::uint8_t* base = reinterpret_cast<std::uint8_t*>(g_eng.globalVars);
	__try {
		out.curtime = *reinterpret_cast<float*>(base + 0x30);
		out.frametime = *reinterpret_cast<float*>(base + 0x34);
		out.tickcount = *reinterpret_cast<int*>(base + 0x44);
		out.field20 = *reinterpret_cast<float*>(base + 0x50);
		out.threadId = *reinterpret_cast<int*>(base + 0x58);
		out.ok = true;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void RestoreGlobalVars(const GlobalVarsBackup& in)
{
	if (!in.ok || !g_eng.globalVars)
		return;
	std::uint8_t* base = reinterpret_cast<std::uint8_t*>(g_eng.globalVars);
	__try {
		*reinterpret_cast<float*>(base + 0x30) = in.curtime;
		*reinterpret_cast<float*>(base + 0x34) = in.frametime;
		*reinterpret_cast<int*>(base + 0x44) = in.tickcount;
		*reinterpret_cast<float*>(base + 0x50) = in.field20;
		*reinterpret_cast<int*>(base + 0x58) = in.threadId;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Set prediction-time globals: curtime = tick * interval, frametime = interval
bool SetupGlobalVarsForTick(int tickBase)
{
	if (!g_eng.globalVars || tickBase <= 0)
		return false;
	std::uint8_t* base = reinterpret_cast<std::uint8_t*>(g_eng.globalVars);
	__try {
		*reinterpret_cast<int*>(base + 0x44) = tickBase;
		*reinterpret_cast<float*>(base + 0x30) = static_cast<float>(tickBase) * kTickInterval;
		*reinterpret_cast<float*>(base + 0x34) = kTickInterval;
		*reinterpret_cast<float*>(base + 0x50) = 0.f;
		*reinterpret_cast<int*>(base + 0x58) = static_cast<int>(GetCurrentThreadId());
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool BackupMoveSvcButtons(void* moveSvc, MoveSvcBtnBak& out)
{
	out = {};
	if (!moveSvc)
		return false;
	__try {
		// layout: vtable@0, nValue@8, nValueChanged@10, nValueScroll@18
		auto* p = reinterpret_cast<std::uint8_t*>(moveSvc) + kMoveSvcButtonsOff;
		out.nValue = *reinterpret_cast<std::uint64_t*>(p + 0x08);
		out.nValueChanged = *reinterpret_cast<std::uint64_t*>(p + 0x10);
		out.nValueScroll = *reinterpret_cast<std::uint64_t*>(p + 0x18);
		out.moveSvc = moveSvc;
		out.ok = true;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void RestoreMoveSvcButtons(const MoveSvcBtnBak& in)
{
	if (!in.ok || !in.moveSvc)
		return;
	__try {
		auto* p = reinterpret_cast<std::uint8_t*>(in.moveSvc) + kMoveSvcButtonsOff;
		*reinterpret_cast<std::uint64_t*>(p + 0x08) = in.nValue;
		*reinterpret_cast<std::uint64_t*>(p + 0x10) = in.nValueChanged;
		*reinterpret_cast<std::uint64_t*>(p + 0x18) = in.nValueScroll;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Dump: m_LegacyJump 0x6B0 (+0x10 m_bOldJumpPressed), m_ModernJump 0x6C8
constexpr std::uintptr_t kLegacyJumpOff = 0x6B0;

bool BackupModernJump(void* moveSvc, ModernJumpBak& out)
{
	out = {};
	if (!moveSvc)
		return false;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(moveSvc);
		const auto mj = base + kModernJumpOff;
		out.lastActualTick = *reinterpret_cast<std::int32_t*>(mj + 0x10);
		out.lastActualFrac = *reinterpret_cast<float*>(mj + 0x14);
		out.lastUsableTick = *reinterpret_cast<std::int32_t*>(mj + 0x18);
		out.lastUsableFrac = *reinterpret_cast<float*>(mj + 0x1C);
		out.lastLandedTick = *reinterpret_cast<std::int32_t*>(mj + 0x20);
		out.lastLandedFrac = *reinterpret_cast<float*>(mj + 0x24);
		out.lastLandedVelX = *reinterpret_cast<float*>(mj + 0x28);
		out.lastLandedVelY = *reinterpret_cast<float*>(mj + 0x2C);
		out.lastLandedVelZ = *reinterpret_cast<float*>(mj + 0x30);
		out.legacyOldJump = *reinterpret_cast<std::uint8_t*>(base + kLegacyJumpOff + 0x10);
		out.ok = true;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void RestoreModernJump(void* moveSvc, const ModernJumpBak& in)
{
	if (!in.ok || !moveSvc)
		return;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(moveSvc);
		const auto mj = base + kModernJumpOff;
		*reinterpret_cast<std::int32_t*>(mj + 0x10) = in.lastActualTick;
		*reinterpret_cast<float*>(mj + 0x14) = in.lastActualFrac;
		*reinterpret_cast<std::int32_t*>(mj + 0x18) = in.lastUsableTick;
		*reinterpret_cast<float*>(mj + 0x1C) = in.lastUsableFrac;
		*reinterpret_cast<std::int32_t*>(mj + 0x20) = in.lastLandedTick;
		*reinterpret_cast<float*>(mj + 0x24) = in.lastLandedFrac;
		*reinterpret_cast<float*>(mj + 0x28) = in.lastLandedVelX;
		*reinterpret_cast<float*>(mj + 0x2C) = in.lastLandedVelY;
		*reinterpret_cast<float*>(mj + 0x30) = in.lastLandedVelZ;
		*reinterpret_cast<std::uint8_t*>(base + kLegacyJumpOff + 0x10) = in.legacyOldJump;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

bool BackupPawn(C_CSPlayerPawn* pawn, PawnBackup& out)
{
	out = {};
	if (!pawn)
		return false;
	__try {
		const auto pbase = reinterpret_cast<std::uintptr_t>(pawn);
		// Dump offsets are source of truth online (schema lag = land desync).
		out.flags = *reinterpret_cast<std::uint32_t*>(pbase + kPawnFlagsOff);
		out.flags |= pawn->m_fFlags();
		out.oldOrigin = pawn->m_vOldOrigin();
		out.velocity = pawn->m_vecVelocity();
		out.absVelocity = pawn->m_vecAbsVelocity();
		out.baseVelocity = *reinterpret_cast<Vector_t*>(pbase + kPawnBaseVelOff);
		out.groundEntity = *reinterpret_cast<std::uint32_t*>(pbase + kPawnGroundOff);
		out.viewOffset = pawn->m_vecViewOffset();
		out.sceneNode = pawn->m_pGameSceneNode();
		if (out.sceneNode)
			out.absOrigin = reinterpret_cast<CGameSceneNode*>(out.sceneNode)->m_vecAbsOrigin();
		else
			out.absOrigin = out.oldOrigin;

		CBaseHandle hCtrl = pawn->m_hController();
		if (hCtrl.valid() && I::GameEntity && I::GameEntity->Instance) {
			CCSPlayerController* ctrl =
				I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl.index());
			if (ctrl) {
				out.controller = ctrl;
				out.tickBase = static_cast<int>(ctrl->m_nTickBase());
			}
		}
		void* moveSvc = pawn->m_pMovementServices();
		BackupMoveSvcButtons(moveSvc, out.moveBtn);
		BackupModernJump(moveSvc, out.modernJump);
		if (moveSvc) {
			const auto mbase = reinterpret_cast<std::uintptr_t>(moveSvc);
			out.stamina = *reinterpret_cast<float*>(mbase + kStaminaOff);
			out.staminaAtJumpStart = *reinterpret_cast<float*>(mbase + kStaminaJumpOff);
		}
		out.ok = true;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void RestorePawn(C_CSPlayerPawn* pawn, const PawnBackup& in)
{
	if (!pawn || !in.ok)
		return;
	__try {
		const auto pbase = reinterpret_cast<std::uintptr_t>(pawn);
		// Write dump + schema so neither path keeps sim residue.
		*reinterpret_cast<std::uint32_t*>(pbase + kPawnFlagsOff) = in.flags;
		pawn->m_fFlags() = in.flags;
		pawn->m_vOldOrigin() = in.oldOrigin;
		pawn->m_vecVelocity() = in.velocity;
		pawn->m_vecAbsVelocity() = in.absVelocity;
		*reinterpret_cast<Vector_t*>(pbase + kPawnBaseVelOff) = in.baseVelocity;
		*reinterpret_cast<std::uint32_t*>(pbase + kPawnGroundOff) = in.groundEntity;
		pawn->m_vecViewOffset() = in.viewOffset;
		if (in.sceneNode) {
			reinterpret_cast<CGameSceneNode*>(in.sceneNode)->m_vecAbsOrigin() = in.absOrigin;
		}
		// Free RunCommand (IDA 0x180A1CB90) advances tickbase — must restore.
		if (in.controller && in.tickBase > 0) {
			reinterpret_cast<CCSPlayerController*>(in.controller)->m_nTickBase() =
				static_cast<std::uint32_t>(in.tickBase);
		}
		RestoreMoveSvcButtons(in.moveBtn);
		if (in.moveBtn.moveSvc) {
			RestoreModernJump(in.moveBtn.moveSvc, in.modernJump);
			const auto mbase = reinterpret_cast<std::uintptr_t>(in.moveBtn.moveSvc);
			*reinterpret_cast<float*>(mbase + kStaminaOff) = in.stamina;
			*reinterpret_cast<float*>(mbase + kStaminaJumpOff) = in.staminaAtJumpStart;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

bool CapturePredicted(C_CSPlayerPawn* pawn, void* moveSvc, const PawnBackup& pre, PredictedState& out, bool usedRunCommand)
{
	out = {};
	if (!pawn)
		return false;
	__try {
		out.preFlags = pre.flags;
		out.flags = pawn->m_fFlags();
		// Merge hard dump offset in case schema lags
		const auto pbase = reinterpret_cast<std::uintptr_t>(pawn);
		const std::uint32_t hf = *reinterpret_cast<std::uint32_t*>(pbase + 0x3F4);
		out.flags |= hf;
		out.postFlags = out.flags;
		// FL_ONGROUND only — PARTIALGROUND true mid-air (edges/stairs) → false bhop ground
		out.onGround = (out.flags & FL_ONGROUND) != 0u;

		out.velocity = pawn->m_vecVelocity();
		out.absVelocity = pawn->m_vecAbsVelocity();
		const Vector_t vo = pawn->m_vecViewOffset();
		CGameSceneNode* node = pawn->m_pGameSceneNode();
		if (node)
			out.absOrigin = node->m_vecAbsOrigin();
		else
			out.absOrigin = pawn->m_vOldOrigin();
		out.origin = out.absOrigin;
		out.eye = {
			out.absOrigin.x + vo.x,
			out.absOrigin.y + vo.y,
			out.absOrigin.z + vo.z
		};
		if (pre.controller)
			out.tickBase = static_cast<int>(
				reinterpret_cast<CCSPlayerController*>(pre.controller)->m_nTickBase());
		else
			out.tickBase = pre.tickBase;
		if (g_eng.globalVars) {
			std::uint8_t* base = reinterpret_cast<std::uint8_t*>(g_eng.globalVars);
			out.curtime = *reinterpret_cast<float*>(base + 0x30);
			out.frametime = *reinterpret_cast<float*>(base + 0x34);
		} else {
			out.curtime = static_cast<float>(out.tickBase) * kTickInterval;
			out.frametime = kTickInterval;
		}

		// Capture ModernJump land frac for subtick timing (UC post_landind_frac).
		// Stale non-zero after land is common — bhop only trusts this on landThisTick.
		// Prefer non-zero; 0 still valid press-at-start when caller is on land tick.
		float lf = 0.f;
		if (SehReadLandFrac(moveSvc, lf) && std::isfinite(lf)) {
			out.landFrac = lf;
			out.hasLandFrac = (lf > 0.f && lf < 1.f);
		} else {
			out.landFrac = 0.f;
			out.hasLandFrac = false;
		}

		out.usedRunCommand = usedRunCommand;
		out.valid = true;
		out.fromEngine = true;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Local-math one-tick fallback. Prefer pre-sim backup so we never double-sim
// after a partial ProcessMovement that left the pawn mutated.
void LocalFallbackPredict(
	C_CSPlayerPawn* pawn,
	CUserCmd* cmd,
	PredictedState& out,
	const PawnBackup* pre = nullptr)
{
	out = {};
	if (!pawn)
		return;
	__try {
		Vector_t absOrigin{};
		Vector_t vel{};
		Vector_t viewOff{};
		std::uint32_t flags = 0;
		bool onGround = false;

		if (pre && pre->ok) {
			absOrigin = pre->absOrigin;
			vel = pre->absVelocity;
			viewOff = pre->viewOffset;
			flags = pre->flags;
			onGround = (pre->flags & FL_ONGROUND) != 0;
		} else {
			CGameSceneNode* node = pawn->m_pGameSceneNode();
			if (node)
				absOrigin = node->m_vecAbsOrigin();
			else
				absOrigin = pawn->m_vOldOrigin();
			vel = pawn->m_vecAbsVelocity();
			viewOff = pawn->m_vecViewOffset();
			flags = pawn->m_fFlags();
			onGround = (flags & FL_ONGROUND) != 0;
		}

		float yaw = 0.f;
		float fmove = 0.f;
		float smove = 0.f;
		if (cmd && cmd->csgoUserCmd.pBaseCmd) {
			CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
			fmove = base->flForwardMove;
			smove = base->flSideMove;
			if (base->pViewAngles)
				yaw = base->pViewAngles->angValue.y;
		}

		MoveState in{};
		in.origin = absOrigin;
		in.velocity = vel;
		in.onGround = onGround;
		in.maxSpeed = 260.f;
		const MoveState sim = SimulateTick(in, yaw, fmove, smove, kTickInterval);

		out.origin = sim.origin;
		out.absOrigin = sim.origin;
		out.velocity = sim.velocity;
		out.absVelocity = sim.velocity;
		// Local sim does not resolve ground contacts — keep pre flags but mark air if vz clear up
		out.preFlags = flags;
		out.flags = flags;
		out.postFlags = flags;
		out.onGround = onGround;
		if (!onGround && sim.velocity.z > 50.f)
			out.onGround = false;
		out.eye = {
			sim.origin.x + viewOff.x,
			sim.origin.y + viewOff.y,
			sim.origin.z + viewOff.z
		};
		out.tickBase = (pre && pre->ok) ? pre->tickBase : 0;
		out.frametime = kTickInterval;
		out.curtime = static_cast<float>(out.tickBase > 0 ? out.tickBase + 1 : 0) * kTickInterval;
		out.valid = true;
		out.fromEngine = false;
		out.usedRunCommand = false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out = {};
	}
}

} // namespace

bool Init()
{
	if (g_triedInit)
		return g_eng.resolved;
	g_triedInit = true;

	// IDA pPrediction (client.dll.i64, imagebase 0x180000000):
	//   hit 0x180B916B0: lea rax,[unk_1823A4140]; ret  → returns &CPrediction
	//   pattern unique (1 match). Do NOT use dump json rva 0x23A5140 (off-by-0x1000).
	//   Fallback RVA 0x23A4140 only if pattern miss.
	//   Interface factory registers "Source2ClientPrediction001" → this getter.
	//   CPrediction ctor 0x180B6A0B0 zeros in_prediction@+0x34, first_prediction@+0xF0.
	if (auto* p = M::FindPattern("client",
		"48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 40 53 56 41 54")) {
		// Call getter (returns CPrediction*); do not treat pattern hit as the object.
		g_eng.pPrediction = SehCallGetter(p);
	}
	if (!g_eng.pPrediction) {
		const uintptr_t base = modules.getModule("client");
		if (base)
			g_eng.pPrediction = reinterpret_cast<void*>(base + kRvaPrediction);
	}

	// ProcessMovement 0x180A1B1C0 — unique prologue
	g_eng.processMovement = M::FindPattern("client",
		"48 8B C4 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 78 44");
	if (!g_eng.processMovement) {
		if (auto* hit = M::FindPattern("client",
			"E8 ? ? ? ? 48 8B 06 48 8B CE FF 90 ? ? ? ? 48 85 DB")) {
			g_eng.processMovement = ResolveRipCall(hit);
		}
	}

	// RunCommand_Context 0x180A1CB90 (resolved for consumers; Start must NOT call it)
	g_eng.runCommand = M::FindPattern("client",
		"48 8B C4 48 81 EC C8 00 00 00 48 89 58 10 48 89 68 18");
	if (!g_eng.runCommand)
		g_eng.runCommand = M::FindPattern("client",
			"48 8B C4 48 81 EC C8 00 00 00 48 89 58 10 48 89");

	// ProcessSubTickInput 0x180B03E40
	g_eng.processSubtickInput = M::FindPattern("client",
		"89 54 24 10 48 89 4C 24 08 53 56 57 48 83 EC 70");

	// ModernSubtickJumpCheck 0x180882870 — prefer long unique sig
	g_eng.modernSubtickJumpCheck = M::FindPattern("client",
		"48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 40 48 8B EA 48 8B D9 48 8B 49 08 BA 02 00 00 00");
	if (!g_eng.modernSubtickJumpCheck)
		g_eng.modernSubtickJumpCheck = M::FindPattern("client",
			"48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 40 48 8B EA 48 8B");

	// CreateNewSubtickMoveStep 0x1804E23F0 — E8 from CreateMove fill first (unique).
	// Short prologue hits siblings (0x4E2300 etc.); do not use it first.
	if (auto* createHit = M::FindPattern("client",
		"E8 ? ? ? ? 48 8B D0 48 8B CE E8 ? ? ? ? 48 8B C8"))
		g_eng.createNewSubtickMoveStep = ResolveRipCall(createHit);
	if (!g_eng.createNewSubtickMoveStep)
		g_eng.createNewSubtickMoveStep = M::FindPattern("client",
			"48 89 5C 24 10 57 48 83 EC 20 33 DB 48 8B F9 48 85 C9 75 2D B9 38 00 00 00");

	// MovementServices_CheckJumpButton 0x180B0D130
	g_eng.checkJumpButton = M::FindPattern("client",
		"4C 89 44 24 18 55 56 41 56 48 8D AC 24 80 EC FF");

	// WriteSubtickFromEntry 0x180C8E7E0
	g_eng.writeSubtickFromEntry = M::FindPattern("client",
		"48 89 5C 24 18 55 57 41 56 48 8D 6C 24 C9 48 81");

	// ForceButtonsDown 0x180A11230 (called from ProcessMovement)
	g_eng.forceButtonsDown = M::FindPattern("client",
		"40 53 57 41 56 48 81 EC 30 02 00 00 48 83 79 38 00");

	// QueueForceSubtickMove / ProcessForceSubtickMoves — DEAD on this build.
	// IDA 855b7a01: dump patterns land on schema registrars (sub_180A0A890 /
	// sub_180A15A90 — CPlayer_MovementServices field table including
	// m_arrForceSubtickMoveWhen). No separate runtime queue/drain exists.
	// Silent aim uses AimCommon::StampCmdAngles (base pViewAngles + tip hist).
	g_eng.queueForceSubtickMove = nullptr;
	g_eng.processForceSubtickMoves = nullptr;

	// SetupMove 0x180D54460 — patterns.hpp
	g_eng.setupMove = M::FindPattern("client",
		"48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 20 48 8B EA 4C 8B F1");

	g_eng.globalVars = ResolveGlobalVars();

	// CreateMove pred path: free RunCommand_Context (sub_180A1CB90) with attack
	// stripped. IDA-verified: it does vfunc[46] SetPredictionCommand → tickbase++
	// → gv setup → ProcessMovement → vfunc[47] Reset → gv restore atomically.
	// The old vfunc[32] path is retained only as a fallback and does NOT run
	// ProcessMovement (verified against sub_180A1CB90 flow).
	g_eng.resolved = g_eng.pPrediction != nullptr || g_eng.globalVars != nullptr;

	Con::Info(
		"Pred: pPred=%p gv=%p runCmdCtx=%p procMove=%p setupMove=%p subtickIn=%p createStep=%p forceBtn=%p",
		g_eng.pPrediction, g_eng.globalVars, g_eng.runCommand, g_eng.processMovement,
		g_eng.setupMove, g_eng.processSubtickInput,
		g_eng.createNewSubtickMoveStep, g_eng.forceButtonsDown);

	if (!g_eng.pPrediction) Con::OffsetMiss("Pred::pPrediction (Source2ClientPrediction001)");
	else Con::Ok("Pred: CPrediction @ %p (in_pred@+0x34 first@+0xF0)", g_eng.pPrediction);
	if (!g_eng.globalVars) Con::OffsetMiss("Pred::GlobalVars");
	if (!g_eng.processSubtickInput) Con::OffsetMiss("Pred::ProcessSubTickInput");
	if (!g_eng.createNewSubtickMoveStep) Con::OffsetMiss("Pred::CreateNewSubtickMoveStep");
	if (!g_eng.writeSubtickFromEntry) Con::OffsetMiss("Pred::WriteSubtickFromEntry");
	if (!g_eng.setupMove) Con::OffsetMiss("Pred::SetupMove");
	// Force-subtick funcs intentionally NOT resolved — old patterns hit schema
	// registration fns (sub_180A0A890 / sub_180A15A90), not runtime queue/drain.
	if (g_eng.runCommand)
		Con::Ok("Pred: RunCommand_Context @ %p (canonical engine sim path)",
			g_eng.runCommand);
	else
		Con::OffsetMiss("Pred::RunCommand_Context (falling back to vfunc[32] — inaccurate)");

	return g_eng.resolved;
}

// IDA QueueForceSubtickMove 0x180A0A890: void(QAngle* ang)
//   TLS-gated ring; ProcessForceSubtickMoves drains into subtick steps.
// Polish: finite angles, normalize pitch/yaw, zero roll, then queue.
bool QueueForceSubtick(float pitch, float yaw)
{
	if (!g_triedInit)
		Init();
	if (!g_eng.queueForceSubtickMove)
		return false;
	if (!std::isfinite(pitch) || !std::isfinite(yaw))
		return false;

	// Clamp pitch like engine view; wrap yaw to [-180, 180]
	pitch = std::clamp(pitch, -89.f, 89.f);
	while (yaw > 180.f) yaw -= 360.f;
	while (yaw < -180.f) yaw += 360.f;

	__try {
		// 12-byte QAngle — matches engine Vector/QAngle read (3 floats)
		alignas(16) float ang[3] = { pitch, yaw, 0.f };
		using Fn = void(__fastcall*)(float*);
		reinterpret_cast<Fn>(g_eng.queueForceSubtickMove)(ang);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// IDA ProcessForceSubtickMoves 0x180A15A90: void(CUserCmd* cmd) — drain queue.
// Call after QueueForceSubtick on silent fire so same-tick subtick gets angles.
bool ProcessForceSubtick(CUserCmd* cmd)
{
	if (!g_triedInit)
		Init();
	if (!cmd || !g_eng.processForceSubtickMoves)
		return false;
	__try {
		using Fn = void(__fastcall*)(CUserCmd*);
		reinterpret_cast<Fn>(g_eng.processForceSubtickMoves)(cmd);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// IDA WriteSubtickFromEntry 0x180C8E7E0:
//   src = 96-byte frame-history entry (ticks + angles + shoot)
//   dst = CCSGOInputHistoryEntryPB*
// Matches CreateMove's protobuf fill so silent stamps set all has_bits.
bool StampInputHistory(
	void* historyEntryPb,
	int renderTick,
	float renderFrac,
	int playerTick,
	float playerFrac,
	const QAngle_t& angles,
	const Vector_t& shootPos)
{
	if (!g_triedInit)
		Init();
	if (!historyEntryPb || !g_eng.writeSubtickFromEntry)
		return false;
	if (!angles.IsValid())
		return false;

	alignas(16) std::uint8_t src[96]{};
	auto* d = reinterpret_cast<std::int32_t*>(src);
	d[0] = renderTick;
	*reinterpret_cast<float*>(d + 1) = renderFrac;
	d[2] = playerTick;
	*reinterpret_cast<float*>(d + 3) = playerFrac;
	*reinterpret_cast<float*>(d + 4) = angles.x;
	*reinterpret_cast<float*>(d + 5) = angles.y;
	*reinterpret_cast<float*>(d + 6) = angles.z;
	*reinterpret_cast<float*>(d + 7) = shootPos.x;
	*reinterpret_cast<float*>(d + 8) = shootPos.y;
	*reinterpret_cast<float*>(d + 9) = shootPos.z;

	// a3=1 include shoot/interp; a4/a5 = NaN (same as CreateMove when unset); a6=0
	const double nanBits = std::bit_cast<double>(0x7FF8000000000000ull);
	__try {
		using Fn = void(__fastcall*)(void* src, void* dst, char incl, double t0, int t1, void* ctx);
		reinterpret_cast<Fn>(g_eng.writeSubtickFromEntry)(
			src, historyEntryPb, 1, nanBits, static_cast<int>(0x7FC00000), nullptr);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

const EngineAddrs& Engines()
{
	if (!g_triedInit)
		Init();
	return g_eng;
}

// Attack bits — ProcessMovement / RunCommand with IN_ATTACK held re-fires the
// weapon in our local re-sim. Original CreateMove already ran real fire for this
// cmd; second fire → double sound / desync on official servers.
// Also strip jump so bhop edges do not re-fire ModernJump in pred re-sim.
constexpr std::uint64_t kPredStripAttack =
	IN_ATTACK | IN_SECOND_ATTACK | IN_RELOAD | IN_JUMP;

struct CmdAttackBak {
	std::uint64_t nValue = 0;
	std::uint64_t nValueChanged = 0;
	std::uint64_t nValueScroll = 0;
	std::uint64_t pbValue = 0;
	std::uint64_t pbChanged = 0;
	std::uint64_t pbScroll = 0;
	bool hadPb = false;
	bool ok = false;
};

void BackupStripAttack(CUserCmd* cmd, CmdAttackBak& out)
{
	out = {};
	if (!cmd)
		return;
	out.nValue = cmd->nButtons.nValue;
	out.nValueChanged = cmd->nButtons.nValueChanged;
	out.nValueScroll = cmd->nButtons.nValueScroll;
	cmd->nButtons.nValue &= ~kPredStripAttack;
	cmd->nButtons.nValueChanged &= ~kPredStripAttack;
	cmd->nButtons.nValueScroll &= ~kPredStripAttack;
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pInButtonState) {
			out.hadPb = true;
			out.pbValue = base->pInButtonState->nValue;
			out.pbChanged = base->pInButtonState->nValueChanged;
			out.pbScroll = base->pInButtonState->nValueScroll;
			base->pInButtonState->nValue &= ~kPredStripAttack;
			base->pInButtonState->nValueChanged &= ~kPredStripAttack;
			base->pInButtonState->nValueScroll &= ~kPredStripAttack;
		}
	}
	out.ok = true;
}

void RestoreAttack(CUserCmd* cmd, const CmdAttackBak& in)
{
	if (!cmd || !in.ok)
		return;
	// Put back only attack bits we cleared (leave other mutations from features)
	const std::uint64_t kept = ~kPredStripAttack;
	cmd->nButtons.nValue =
		(cmd->nButtons.nValue & kept) | (in.nValue & kPredStripAttack);
	cmd->nButtons.nValueChanged =
		(cmd->nButtons.nValueChanged & kept) | (in.nValueChanged & kPredStripAttack);
	cmd->nButtons.nValueScroll =
		(cmd->nButtons.nValueScroll & kept) | (in.nValueScroll & kPredStripAttack);
	if (in.hadPb && cmd->csgoUserCmd.pBaseCmd && cmd->csgoUserCmd.pBaseCmd->pInButtonState) {
		auto* pb = cmd->csgoUserCmd.pBaseCmd->pInButtonState;
		pb->nValue = (pb->nValue & kept) | (in.pbValue & kPredStripAttack);
		pb->nValueChanged = (pb->nValueChanged & kept) | (in.pbChanged & kPredStripAttack);
		pb->nValueScroll = (pb->nValueScroll & kept) | (in.pbScroll & kPredStripAttack);
	}
}

bool Start(CUserCmd* cmd)
{
	// UC https://www.unknowncheats.me/forum/counter-strike-2-a/748659
	//   "prediction unstable on official servers" — incomplete restore / wrong
	//   first_prediction / CPrediction::Update after RunCommand.
	// IDA-verified 2026-07-22 (session 390125b5, client.dll.i64):
	//   free RunCommand 0x180A1CB90 already tickbase++ + gv setup + ProcessMovement
	//   + gv restore. We must FULLY restore pawn (ground/stamina/ModernJump/vel)
	//   or official latency shows rubberband / short traces.

	if (g_active)
		End();

	if (!cmd)
		return false;

	// Sequence gate BEFORE mutating g_last (bhop uses previous postFlags)
	if (cmd->nCommandNumber != 0 && cmd->nCommandNumber == g_lastSequence && g_last.valid) {
		g_active = true;
		return true;
	}

	if (!g_triedInit)
		Init();

	C_CSPlayerPawn* pawn = SehLocalPawn();
	if (!pawn)
		return false;

	const int hp = SehPawnHealth(pawn);
	if (hp <= 0 || hp > 200)
		return false;

	void* moveSvc = SehMoveServices(pawn);
	if (!moveSvc) {
		if (!g_last.valid)
			LocalFallbackPredict(pawn, cmd, g_last, nullptr);
		g_active = g_last.valid;
		return g_last.valid;
	}

	if (!BackupPawn(pawn, g_pawnBak)) {
		if (!g_last.valid)
			LocalFallbackPredict(pawn, cmd, g_last, nullptr);
		g_active = g_last.valid;
		return g_last.valid;
	}
	BackupGlobalVars(g_gvBak);

	const PredictedState prev = g_last;
	g_pawn = nullptr;
	g_predIfaceBak = {};

	float preLand = 0.f;
	(void)SehReadLandFrac(moveSvc, preLand);

	// CPrediction flags (IDA ctor 0x180B6A0B0)
	g_predIfaceBak.ok = false;
	if (g_eng.pPrediction) {
		bool ip = false, fp = false;
		if (SehReadPredBool(g_eng.pPrediction, kPredOffInPrediction, ip)
			&& SehReadPredBool(g_eng.pPrediction, kPredOffFirstPrediction, fp)) {
			g_predIfaceBak.inPrediction = ip;
			g_predIfaceBak.firstPrediction = fp;
			g_predIfaceBak.cmdHasBeenPredicted = cmd->bHasBeenPredicted;
			g_predIfaceBak.ok = true;
		}
	}

	// INetworkClient m_bShouldPredict @ +0xF8 (1.1.5). Not engine2 RunPrediction.
	bool shouldPredBak = false;
	bool shouldPredOk = false;
	void* ngc = Engine2::NetworkGameClient();
	if (ngc) {
		__try {
			shouldPredBak = *reinterpret_cast<bool*>(
				reinterpret_cast<std::uint8_t*>(ngc) + 0xF8);
			shouldPredOk = true;
			*reinterpret_cast<bool*>(
				reinterpret_cast<std::uint8_t*>(ngc) + 0xF8) = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			shouldPredOk = false;
		}
	}

	cmd->bHasBeenPredicted = false;
	// UC fix + IDA CPrediction ctor: first_prediction@+0xF0, in_prediction@+0x34.
	// Engine path sets first=true for a single cmd re-sim; old false left accumulated
	// state that desyncs under official latency ("not reaching objects").
	if (g_predIfaceBak.ok && g_eng.pPrediction) {
		SehWritePredBool(g_eng.pPrediction, kPredOffFirstPrediction, true);
		SehWritePredBool(g_eng.pPrediction, kPredOffInPrediction, true);
	}

	// Strip IN_ATTACK / IN_JUMP / IN_RELOAD / IN_SECOND_ATTACK so ProcessMovement
	// under RunCommand_Context does NOT re-fire weapon / re-trigger jump.
	// Real CreateMove already fired this cmd; a second call = double-shot / desync.
	CmdAttackBak atkBak{};
	BackupStripAttack(cmd, atkBak);

	// IDA free RunCommand_Context 0x180A1CB90 (verified session 390125b5):
	//   vfunc[46] SetPredictionCommand(cmd)
	//   if cmd+148 != 3: controller tickbase = GetTickBase()+1
	//   setup GlobalVars (curtime=tick*0.015625, frametime, tickcount, thread)
	//   ProcessMovement 0x180A1B1C0
	//   vfunc[47] Reset
	//   restore GlobalVars (engine already does this — we still restore full pawn)
	// Do NOT call CPrediction::Update after — UC post: wrong Update args desync.
	// Do NOT double-setup GlobalVars before free RunCommand (it owns gv).
	bool ran = false;
	bool usedFree = false;
	if (g_eng.runCommand) {
		ran = SehCallRunCommand(g_eng.runCommand, moveSvc, cmd);
		usedFree = ran;
	}
	if (!ran) {
		// Fallback: manual path when free fn missing — must tickbase+gv ourselves.
		// UC Begin pattern: SetPredCmd → tickbase+1 → gv tick time → Run → Reset.
		if (g_pawnBak.ok && g_pawnBak.controller && g_pawnBak.tickBase > 0) {
			__try {
				reinterpret_cast<CCSPlayerController*>(g_pawnBak.controller)->m_nTickBase() =
					static_cast<std::uint32_t>(g_pawnBak.tickBase + 1);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			SetupGlobalVarsForTick(g_pawnBak.tickBase + 1);
		}
		const bool setOk = SehCallVfuncCmd(moveSvc, kVfuncSetPredictionCommand, cmd);
		if (g_eng.processMovement)
			ran = SehCallProcessMovement(g_eng.processMovement, moveSvc, cmd);
		if (!ran)
			ran = SehCallVfuncCmd(moveSvc, kVfuncRunCommand, cmd);
		if (setOk)
			SehCallVfunc0(moveSvc, kVfuncResetPredictionCommand);
	}

	// Restore attack bits BEFORE we return so the outbound cmd fires normally
	RestoreAttack(cmd, atkBak);

	PredictedState captured{};
	if (ran) {
		if (!CapturePredicted(pawn, moveSvc, g_pawnBak, captured, true))
			LocalFallbackPredict(pawn, cmd, captured, &g_pawnBak);
		else
			captured.fromEngine = true;
	} else {
		LocalFallbackPredict(pawn, cmd, captured, &g_pawnBak);
	}

	// Sample post-sim flags + landFrac from LIVE pawn BEFORE restore — commit
	// after restore would read pre-sim state (bhop / ModernJump would break).
	__try {
		const auto pbase = reinterpret_cast<std::uintptr_t>(pawn);
		std::uint32_t f = *reinterpret_cast<std::uint32_t*>(pbase + 0x3F4);
		f |= pawn->m_fFlags();
		captured.preFlags = g_pawnBak.flags;
		captured.postFlags = f;
		captured.flags = f;
		captured.onGround = (f & FL_ONGROUND) != 0u;
		float lf = 0.f;
		if (SehReadLandFrac(moveSvc, lf)) {
			captured.landFrac = lf;
			captured.hasLandFrac = (lf > 0.f && lf < 1.f);
		} else if (preLand > 0.f && preLand < 1.f) {
			captured.landFrac = preLand;
			captured.hasLandFrac = true;
		}
		captured.valid = true;
		g_last = captured;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		if (prev.valid)
			g_last = prev;
		else
			g_last = captured;
	}

	// FULL restore — free RunCommand_Context ran real ProcessMovement which
	// mutates origin/oldOrigin/viewOff/scene absOrigin/moveSvc buttons/ModernJump.
	// Prior code only restored velocity → position drift + sticky IN_JUMP after hop.
	RestorePawn(pawn, g_pawnBak);

	RestoreGlobalVars(g_gvBak);
	if (g_predIfaceBak.ok && g_eng.pPrediction) {
		SehWritePredBool(g_eng.pPrediction, kPredOffInPrediction, g_predIfaceBak.inPrediction);
		SehWritePredBool(g_eng.pPrediction, kPredOffFirstPrediction, g_predIfaceBak.firstPrediction);
		cmd->bHasBeenPredicted = g_predIfaceBak.cmdHasBeenPredicted;
	}
	if (shouldPredOk && ngc) {
		__try {
			*reinterpret_cast<bool*>(
				reinterpret_cast<std::uint8_t*>(ngc) + 0xF8) = shouldPredBak;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	(void)usedFree;

	if (cmd->nCommandNumber != 0)
		g_lastSequence = cmd->nCommandNumber;

	g_pawn = pawn;
	g_active = true;
	return g_last.valid;
}

void End()
{
	if (!g_active) {
		g_pawn = nullptr;
		return;
	}

	// UC: do NOT re-sample live pawn here.
	// Start already captured postFlags/landFrac BEFORE RestorePawn.
	// Re-read after restore = pre-sim flags (wrong) + overwrites good g_last.
	// Free RunCommand already restored GlobalVars; Start restored pawn fully.
	// Never call CPrediction::Update here (UC failure mode #3).

	g_active = false;
	g_pawn = nullptr;
	g_pawnBak = {};
	g_gvBak = {};
	g_predIfaceBak = {};
}

bool Active()
{
	return g_active;
}

void Invalidate()
{
	if (g_active)
		End();
	g_last = {};
	g_lastSequence = 0;
	g_pawn = nullptr;
	g_active = false;
	g_pawnBak = {};
	g_gvBak = {};
	g_predIfaceBak = {};
}

const PredictedState& Last()
{
	return g_last;
}

bool LastFromEngine()
{
	return g_last.valid && g_last.fromEngine;
}

bool FlushForceSubticks()
{
	if (!g_triedInit)
		Init();
	if (!g_eng.processForceSubtickMoves)
		return false;
	__try {
		using Fn = void(__fastcall*)();
		// Some builds take no args (TLS queue); if SEH fires, mark dead.
		reinterpret_cast<Fn>(g_eng.processForceSubtickMoves)();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

Vector_t Velocity(C_CSPlayerPawn* pawn)
{
	if (g_last.valid)
		return g_last.absVelocity;
	if (!pawn)
		return {};
	__try {
		return pawn->m_vecAbsVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return {};
	}
}

std::uint32_t Flags(C_CSPlayerPawn* pawn)
{
	if (g_last.valid)
		return g_last.flags;
	if (!pawn)
		return 0;
	__try {
		return pawn->m_fFlags();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

Vector_t Origin(C_CSPlayerPawn* pawn)
{
	if (g_last.valid)
		return g_last.absOrigin;
	if (!pawn)
		return {};
	__try {
		if (CGameSceneNode* n = pawn->m_pGameSceneNode())
			return n->m_vecAbsOrigin();
		return pawn->m_vOldOrigin();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return {};
	}
}

Vector_t Eye(C_CSPlayerPawn* pawn)
{
	if (g_last.valid) {
		const Vector_t& e = g_last.eye;
		if (std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z)
			&& (e.x != 0.f || e.y != 0.f || e.z != 0.f))
			return e;
	}
	if (!pawn)
		return {};
	__try {
		const Vector_t o = Origin(pawn);
		const Vector_t vo = pawn->m_vecViewOffset();
		return { o.x + vo.x, o.y + vo.y, o.z + vo.z };
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return {};
	}
}

float LandFrac(C_CSPlayerPawn* pawn)
{
	if (g_last.valid && g_last.hasLandFrac)
		return g_last.landFrac;
	if (!pawn)
		return 0.f;
	void* moveSvc = SehMoveServices(pawn);
	float lf = 0.f;
	if (SehReadLandFrac(moveSvc, lf))
		return lf;
	return 0.f;
}

bool OnGround(C_CSPlayerPawn* pawn)
{
	// Strict FL_ONGROUND only (IDA CheckJumpButton / ModernJump). PARTIAL = false ground.
	if (g_last.valid)
		return g_last.onGround || (g_last.flags & FL_ONGROUND) != 0u;
	if (!pawn)
		return false;
	__try {
		const std::uint32_t f = pawn->m_fFlags();
		return (f & FL_ONGROUND) != 0u;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// ---- Local math ----

// Ground accelerate: accelspeed = accel * frametime * maxspeed * friction (Source).
void Accelerate(
	Vector_t& vel,
	const Vector_t& wishdir,
	float wishspeed,
	float accel,
	float frametime,
	float maxspeed,
	float surfaceFriction)
{
	if (wishspeed <= 0.f)
		return;

	const float currentspeed = vel.x * wishdir.x + vel.y * wishdir.y;
	const float addspeed = wishspeed - currentspeed;
	if (addspeed <= 0.f)
		return;

	float accelspeed = accel * frametime * maxspeed * surfaceFriction;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	vel.x += accelspeed * wishdir.x;
	vel.y += accelspeed * wishdir.y;
}

// Air accelerate: accelspeed = accel * wishspeed * frametime * friction; wish capped at 30.
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
		Accelerate(out.velocity, wishdir, wishspeed, kSvAccelerate, frametime, out.maxSpeed, out.surfaceFriction);
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

// Air strafe max-gain angle from velocity vector.
// Source air accel: addspeed = wishspeed - v·wishdir. Gain when addspeed > 0.
// Optimal: v·wishdir just below (wishspeed - accelspeed) so full accel applied.
// θ_opt = arccos((wishspeed - accelspeed) / |v|) where accelspeed = accel*wishspeed*dt.
// At speed 250: acos((30 - 5.625)/250) ≈ 84.4°. atan(30/250)=6.84° was the COMPLEMENT
// — wishdir nearly parallel to velocity → addspeed negative → zero gain.
float IdealStrafeDeltaDeg(float speed2d, float wishspeed)
{
	// Below wishspeed: full addspeed at any angle → perpendicular gives cleanest gain
	if (speed2d <= wishspeed + 0.001f)
		return 90.f;
	// AccelSpeed cap: accel * wishspeed * dt (Source AirAccelerate)
	const float accelSpeed = kSvAirAccelerate * wishspeed * kTickInterval;
	const float threshold = wishspeed - accelSpeed;
	if (threshold <= 0.f)
		return 90.f;
	const float cosT = threshold / speed2d;
	if (cosT >= 1.f)
		return 0.f;
	if (cosT <= -1.f)
		return 180.f;
	return std::acos(cosT) * kRad2Deg;
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

// MASK_PLAYERSOLID-ish: world + props. Vis mask misses some floors.
constexpr std::uint64_t kMaskPlayerSolid = 0x201400Bull;
// Stairs/ramps/Mirage lips often report ~0.5–0.7.
constexpr float kFloorNormalMin = 0.50f;

bool PredictLandThisTick(
	C_CSPlayerPawn* pawn,
	const Vector_t& vel,
	float& outFrac,
	float frametime)
{
	outFrac = 0.f;
	// Allow near-zero vz (step-down / lip) — only reject strong rising
	if (!pawn || !std::isfinite(vel.z) || vel.z > 20.f)
		return false;
	if (frametime <= 0.f)
		frametime = kTickInterval;

	if (!Trace::Ready())
		Trace::Init();
	if (!Trace::Ready())
		return false;

	Vector_t origin{};
	Vector_t mins{ -16.f, -16.f, 0.f };
	Vector_t maxs{ 16.f, 16.f, 72.f };
	__try {
		origin = pawn->getPosition();
		if (CCollisionProperty* col = pawn->m_pCollision()) {
			const Vector_t cm = col->m_vecMins();
			const Vector_t cM = col->m_vecMaxs();
			if (std::isfinite(cm.x) && std::isfinite(cM.z) && cM.z > cm.z) {
				mins = cm;
				maxs = cM;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}

	// Uneven ground (Mirage stairs/ramps): multi-step + longer horizon + fat down probe.
	const float horizon = frametime * 1.75f;
	constexpr int kSteps = 12;
	const float dt = horizon / static_cast<float>(kSteps);

	Vector_t pos = origin;
	Vector_t v = vel;
	{
		Trace::CGameTrace stuck{};
		if (Trace::TraceHull(pos, pos, mins, maxs, pawn, stuck, kMaskPlayerSolid)
			&& stuck.startsolid()) {
			pos.z += 4.f;
		}
	}

	for (int i = 0; i < kSteps; ++i) {
		const Vector_t next{
			pos.x + v.x * dt,
			pos.y + v.y * dt,
			pos.z + v.z * dt - 0.5f * kGravity * dt * dt
		};
		v.z -= kGravity * dt;

		Trace::CGameTrace tr{};
		if (!Trace::TraceHull(pos, next, mins, maxs, pawn, tr, kMaskPlayerSolid)) {
			pos = next;
			continue;
		}
		if (tr.startsolid()) {
			Vector_t up = pos;
			up.z += 6.f;
			Trace::CGameTrace tr2{};
			if (Trace::TraceHull(up, next, mins, maxs, pawn, tr2, kMaskPlayerSolid)
				&& Trace::DidHit(tr2) && !tr2.startsolid()) {
				const Vector_t n = tr2.normal();
				if (std::isfinite(n.z) && n.z >= kFloorNormalMin) {
					const float sub = tr2.fraction();
					if (std::isfinite(sub) && sub < 1.f) {
						const float t = (static_cast<float>(i) + sub) * dt;
						outFrac = std::clamp(t / frametime, kMinSubtickFrac, kMaxSubtickFrac);
						return true;
					}
				}
			}
			pos = next;
			continue;
		}
		if (!Trace::DidHit(tr)) {
			pos = next;
			continue;
		}

		const Vector_t n = tr.normal();
		if (!std::isfinite(n.z) || n.z < kFloorNormalMin) {
			pos = next;
			continue;
		}

		const float sub = tr.fraction();
		if (!std::isfinite(sub) || sub >= 1.f) {
			pos = next;
			continue;
		}

		const float t = (static_cast<float>(i) + sub) * dt;
		if (t > frametime * 1.15f) {
			outFrac = kMaxSubtickFrac;
			return true;
		}
		outFrac = std::clamp(t / frametime, kMinSubtickFrac, kMaxSubtickFrac);
		return true;
	}

	// Fat long hull (ledges / thin stairs)
	{
		Vector_t end{
			origin.x + vel.x * frametime,
			origin.y + vel.y * frametime,
			origin.z + vel.z * frametime - 0.5f * kGravity * frametime * frametime
		};
		const float extra = 4.f + std::min(18.f, std::max(0.f, -vel.z) * frametime);
		end.z -= extra;

		Trace::CGameTrace tr{};
		if (Trace::TraceHull(origin, end, mins, maxs, pawn, tr, kMaskPlayerSolid)
			&& Trace::DidHit(tr) && !tr.startsolid()) {
			const Vector_t n = tr.normal();
			const float frac = tr.fraction();
			if (std::isfinite(n.z) && n.z >= kFloorNormalMin
				&& std::isfinite(frac) && frac < 1.f) {
				outFrac = std::clamp(frac, kMinSubtickFrac, kMaxSubtickFrac);
				return true;
			}
		}
	}

	// Straight-down probe (uneven / low lateral speed Mirage spots)
	{
		const float down = 18.f + std::min(24.f, std::max(0.f, -vel.z) * frametime * 2.f);
		Vector_t end = origin;
		end.z -= down;
		// Slightly thinner mins for step lips
		Vector_t pmin = mins;
		Vector_t pmax = maxs;
		pmin.x *= 0.75f; pmin.y *= 0.75f;
		pmax.x *= 0.75f; pmax.y *= 0.75f;

		Trace::CGameTrace tr{};
		if (Trace::TraceHull(origin, end, pmin, pmax, pawn, tr, kMaskPlayerSolid)
			&& Trace::DidHit(tr) && !tr.startsolid()) {
			const Vector_t n = tr.normal();
			const float frac = tr.fraction();
			if (std::isfinite(n.z) && n.z >= kFloorNormalMin
				&& std::isfinite(frac) && frac < 1.f) {
				// Map vertical fraction into tick frac (approx)
				const float dist = down * frac;
				const float dropPerTick = std::max(1.f, -vel.z * frametime + 0.5f * kGravity * frametime * frametime);
				outFrac = std::clamp(dist / dropPerTick, kMinSubtickFrac, kMaxSubtickFrac);
				return true;
			}
		}
	}
	return false;
}

bool ProbeFloorDistance(C_CSPlayerPawn* pawn, float& outDist, float maxDist)
{
	outDist = 0.f;
	if (!pawn || !(maxDist > 0.f))
		return false;
	if (!Trace::Ready())
		Trace::Init();
	if (!Trace::Ready())
		return false;

	Vector_t origin{};
	Vector_t mins{ -12.f, -12.f, 0.f };
	Vector_t maxs{ 12.f, 12.f, 72.f };
	__try {
		origin = pawn->getPosition();
		if (CCollisionProperty* col = pawn->m_pCollision()) {
			const Vector_t cm = col->m_vecMins();
			const Vector_t cM = col->m_vecMaxs();
			if (std::isfinite(cm.x) && std::isfinite(cM.z) && cM.z > cm.z) {
				// Slightly thinner XY for stair lips
				mins = { cm.x * 0.75f, cm.y * 0.75f, cm.z };
				maxs = { cM.x * 0.75f, cM.y * 0.75f, cM.z };
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}

	// Unstick if slightly in floor
	{
		Trace::CGameTrace stuck{};
		if (Trace::TraceHull(origin, origin, mins, maxs, pawn, stuck, kMaskPlayerSolid)
			&& stuck.startsolid()) {
			origin.z += 2.f;
		}
	}

	Vector_t end = origin;
	end.z -= maxDist;
	Trace::CGameTrace tr{};
	if (!Trace::TraceHull(origin, end, mins, maxs, pawn, tr, kMaskPlayerSolid))
		return false;
	if (!Trace::DidHit(tr) || tr.startsolid())
		return false;
	const Vector_t n = tr.normal();
	const float frac = tr.fraction();
	if (!std::isfinite(n.z) || n.z < kFloorNormalMin)
		return false;
	if (!std::isfinite(frac) || frac >= 1.f)
		return false;

	outDist = maxDist * frac;
	return outDist >= 0.f && std::isfinite(outDist);
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

float PredictSpeedAfterWishMoves(
	const Vector_t& vel,
	float viewYawDeg,
	float wishYawDeg,
	float frametime,
	float surfaceFriction)
{
	float delta = wishYawDeg - viewYawDeg;
	while (delta > 180.f) delta -= 360.f;
	while (delta < -180.f) delta += 360.f;
	delta *= kDeg2Rad;

	// wish = fmove*forward(view) + smove*right(view) → (cos wish, sin wish)
	const float fmove = std::cos(delta) * kMaxMove;
	const float smove = std::sin(delta) * kMaxMove;

	MoveState st{};
	st.velocity = vel;
	st.onGround = false;
	st.surfaceFriction = surfaceFriction;
	st.maxSpeed = 260.f;

	const MoveState out = SimulateTick(st, viewYawDeg, fmove, smove, frametime);
	return Length2D(out.velocity);
}

} // namespace Pred
