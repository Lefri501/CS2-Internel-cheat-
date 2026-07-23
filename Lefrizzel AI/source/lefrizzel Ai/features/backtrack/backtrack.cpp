#define NOMINMAX
#include "backtrack.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../interfaces/IEngineClient/IEngineClient.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/math/vector/vector.h"
#include "../../utils/module/module.h"
#include "../aim/aim_common.h"
#include "../bones/bones.h"
#include "../gamemode/gamemode.h"
#include "../prediction/prediction.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../../external/imgui/imgui.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Backtrack {
namespace {

struct Track {
	std::uint32_t handle = 0;
	Record rec[kMaxRecords]{};
	int write = 0;
	int count = 0;
	float lastSim = -1.f;
	Vector_t lastOrigin{};
	bool used = false;
	int missFrames = 0; // consecutive OnCreateMove passes without this pawn
};

Track g_tracks[kMaxTracks]{};
float g_pendingSim = 0.f;
int g_pendingTick = 0;
float g_pendingFrac = 0.f;

// ---- time (SEH helpers must stay C-style — no C++ objects) ----

void* SehReadPtr(void** pp)
{
	if (!pp)
		return nullptr;
	void* v = nullptr;
	__try { v = *pp; }
	__except (EXCEPTION_EXECUTE_HANDLER) { v = nullptr; }
	return v;
}

float SehReadFloat(const void* addr)
{
	if (!addr)
		return 0.f;
	float v = 0.f;
	__try { v = *reinterpret_cast<const float*>(addr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { v = 0.f; }
	return v;
}

int SehReadInt(const void* addr)
{
	if (!addr)
		return 0;
	int v = 0;
	__try { v = *reinterpret_cast<const int*>(addr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
	return v;
}

void* GlobalVarsPtr()
{
	const auto& e = Pred::Engines();
	if (e.globalVars && Mem::Valid(e.globalVars, 0x60))
		return e.globalVars;

	// Same resolve as Pred / visuals GetCurTime
	static void* s_gv = nullptr;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		std::uint8_t* hit = M::FindPattern("client", "48 8B 05 ? ? ? ? F3 0F 10 40 30 C3");
		if (!hit)
			hit = M::FindPattern("client", "48 8B 05 ? ? ? ? 0F 57 C0 8B 48");
		if (hit) {
			void** pp = reinterpret_cast<void**>(M::GetAbsoluteAddress(hit, 3, 0));
			s_gv = SehReadPtr(pp);
		}
		if (!s_gv) {
			const uintptr_t base = modules.getModule("client");
			if (base)
				s_gv = SehReadPtr(reinterpret_cast<void**>(base + Pred::kRvaGlobalVars));
		}
	}
	return s_gv;
}

float ReadCurtime()
{
	void* gv = GlobalVarsPtr();
	if (!gv)
		return 0.f;
	const float t = SehReadFloat(
		reinterpret_cast<const void*>(
			reinterpret_cast<std::uintptr_t>(gv) + kGvCurtimeOff));
	if (t > 1.f && t < 1.0e7f && std::isfinite(t))
		return t;
	return 0.f;
}

int ReadTickcount()
{
	void* gv = GlobalVarsPtr();
	if (!gv)
		return 0;
	return SehReadInt(
		reinterpret_cast<const void*>(
			reinterpret_cast<std::uintptr_t>(gv) + kGvTickcountOff));
}

// Live "now" for lag-comp clamp. Prefer hist/engine render tick over
// Pred-mutated GlobalVars tickcount (pred advances tickbase mid-CreateMove).
int ReadLiveRenderTick(CUserCmd* cmd)
{
	if (cmd && cmd->csgoUserCmd.inputHistoryField.pRep) {
		const int n = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
		if (n > 0 && n <= 64) {
			// Newest hist entry — engine filled this tick's render tick
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(n - 1);
			if (e && reinterpret_cast<std::uintptr_t>(e) >= 0x10000ull
				&& e->nRenderTickCount > 0)
				return e->nRenderTickCount;
		}
	}
	// Networked client info (pre-pred render clock)
	if (I::EngineClient) {
		c_networked_client_info info{};
		if (I::EngineClient->get_networked_client_info(info)
			&& info.m_nRenderTick > 0)
			return info.m_nRenderTick;
	}
	return ReadTickcount();
}

float ReadSimTime(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return 0.f;
	const float t = SehReadFloat(
		reinterpret_cast<const void*>(
			reinterpret_cast<std::uintptr_t>(pawn) + kSimTimeOff));
	if (std::isfinite(t) && t > 0.f)
		return t;
	return 0.f;
}

std::uint64_t NowWallMs()
{
	return GetTickCount64();
}

// Age for pick/draw. MUST be Pred-safe: Pred::Start mutates GlobalVars
// curtime/tickcount → CreateMove ages ≠ Present draw ages → ManualGhost
// fails or stamps wrong record → aim at ghost, 0 damage.
// Order: wall (stable) → captureRenderTick delta → simTime → captureCur.
float AgeSec(const Record& r, float curtime)
{
	if (r.wallMs) {
		const float a = static_cast<float>(NowWallMs() - r.wallMs) * 0.001f;
		if (a >= 0.f && a < 5.f)
			return a;
	}
	// live render tick vs capture (no cmd here — engine / net info)
	if (r.captureRenderTick > 0) {
		int live = 0;
		if (I::EngineClient) {
			c_networked_client_info info{};
			if (I::EngineClient->get_networked_client_info(info)
				&& info.m_nRenderTick > 0)
				live = info.m_nRenderTick;
		}
		if (live <= 0)
			live = ReadTickcount();
		if (live > 0) {
			const int d = live - r.captureRenderTick;
			if (d >= 0 && d < 320) // < 5s @ 64
				return static_cast<float>(d) * kTickInterval;
		}
	}
	if (curtime > 1.f && r.simTime > 0.f) {
		const float a = curtime - r.simTime;
		if (a >= 0.f && a < 5.f)
			return a;
	}
	if (curtime > 1.f && r.captureCur > 1.f) {
		const float a = curtime - r.captureCur;
		if (a >= 0.f && a < 5.f)
			return a;
	}
	return 999.f;
}

// ---- tracks ----

Track* FindTrack(std::uint32_t handle, bool create)
{
	if (!handle)
		return nullptr;
	for (int i = 0; i < kMaxTracks; ++i) {
		if (g_tracks[i].used && g_tracks[i].handle == handle)
			return &g_tracks[i];
	}
	if (!create)
		return nullptr;
	for (int i = 0; i < kMaxTracks; ++i) {
		if (!g_tracks[i].used) {
			g_tracks[i] = Track{};
			g_tracks[i].used = true;
			g_tracks[i].handle = handle;
			return &g_tracks[i];
		}
	}
	// Evict track with oldest newest-record
	int best = 0;
	float oldest = 1.0e30f;
	for (int i = 0; i < kMaxTracks; ++i) {
		float t = 0.f;
		if (g_tracks[i].count > 0) {
			const int idx = (g_tracks[i].write - 1 + kMaxRecords) % kMaxRecords;
			t = g_tracks[i].rec[idx].simTime;
		}
		if (t < oldest) {
			oldest = t;
			best = i;
		}
	}
	g_tracks[best] = Track{};
	g_tracks[best].used = true;
	g_tracks[best].handle = handle;
	return &g_tracks[best];
}

void PushRecord(Track& tr, const Record& r)
{
	// Only keep one sample per simTime (network update)
	if (tr.count > 0) {
		const int prev = (tr.write - 1 + kMaxRecords) % kMaxRecords;
		const Record& last = tr.rec[prev];
		if (last.valid) {
			const float ds = std::fabs(last.simTime - r.simTime);
			if (ds < 1e-4f)
				return; // same net tick — would fill ring with live pose

			// Also reject if origin barely moved AND sim barely advanced (< half tick)
			const float dx = last.origin.x - r.origin.x;
			const float dy = last.origin.y - r.origin.y;
			const float dz = last.origin.z - r.origin.z;
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (ds < (kTickInterval * 0.49f) && d2 < 0.01f)
				return;
		}
	}
	tr.rec[tr.write] = r;
	tr.write = (tr.write + 1) % kMaxRecords;
	if (tr.count < kMaxRecords)
		++tr.count;
	tr.lastSim = r.simTime;
	tr.lastOrigin = r.origin;
}

// Newest valid record in track (live / current pose)
const Record* NewestRecord(const Track& tr)
{
	if (tr.count <= 0)
		return nullptr;
	const int idx = (tr.write - 1 + kMaxRecords) % kMaxRecords;
	const Record& r = tr.rec[idx];
	return r.valid ? &r : nullptr;
}

// Ghost must sit clearly behind live model — standing still keeps lag on body.
// Horizontal ~8u or 3D ~10u from newest origin/head.
constexpr float kGhostSepXY = 8.f;
constexpr float kGhostSep3D = 10.f;

bool GhostSeparated(const Track& tr, const Record& lag)
{
	const Record* live = NewestRecord(tr);
	if (!live || !live->valid)
		return false;
	// Same slot = not behind
	if (live == &lag)
		return false;
	// Need history depth
	if (tr.count < 2)
		return false;

	const float dx = live->origin.x - lag.origin.x;
	const float dy = live->origin.y - lag.origin.y;
	const float dz = live->origin.z - lag.origin.z;
	const float d2xy = dx * dx + dy * dy;
	const float d2 = d2xy + dz * dz;
	if (d2xy >= (kGhostSepXY * kGhostSepXY) || d2 >= (kGhostSep3D * kGhostSep3D))
		return true;

	// Head can diverge even if feet plant (strafe / peek)
	if (Bones::IsValidPos(live->head) && Bones::IsValidPos(lag.head)) {
		const float hx = live->head.x - lag.head.x;
		const float hy = live->head.y - lag.head.y;
		const float hz = live->head.z - lag.head.z;
		const float h2xy = hx * hx + hy * hy;
		const float h2 = h2xy + hz * hz;
		if (h2xy >= (kGhostSepXY * kGhostSepXY) || h2 >= (kGhostSep3D * kGhostSep3D))
			return true;
	}
	return false;
}

// Pick record whose age is closest to targetSec, within [minSec, maxSec].
// requireSeparation: only records clearly behind live pose (draw path).
const Record* PickLagged(
	const Track& tr,
	float curtime,
	float targetSec,
	float minSec,
	float maxSec,
	bool requireSeparation)
{
	const Record* best = nullptr;
	float bestErr = 1.0e30f;

	for (int i = 0; i < tr.count; ++i) {
		const int idx = (tr.write - 1 - i + kMaxRecords * 2) % kMaxRecords;
		const Record& r = tr.rec[idx];
		if (!r.valid)
			continue;
		const float age = AgeSec(r, curtime);
		if (age < minSec || age > maxSec)
			continue;
		if (requireSeparation && !GhostSeparated(tr, r))
			continue;
		const float err = std::fabs(age - targetSec);
		if (err < bestErr) {
			bestErr = err;
			best = &r;
		}
	}

	// Fallback: oldest valid in window (max lag)
	if (!best) {
		float oldestAge = -1.f;
		for (int i = 0; i < tr.count; ++i) {
			const int idx = (tr.write - 1 - i + kMaxRecords * 2) % kMaxRecords;
			const Record& r = tr.rec[idx];
			if (!r.valid)
				continue;
			const float age = AgeSec(r, curtime);
			if (age < minSec || age > maxSec)
				continue;
			if (requireSeparation && !GhostSeparated(tr, r))
				continue;
			if (age > oldestAge) {
				oldestAge = age;
				best = &r;
			}
		}
	}
	return best;
}

bool CaptureBones(C_CSPlayerPawn* pawn, Record& r)
{
	if (!Bones::GetOrigin(pawn, r.origin) || !Bones::IsValidPos(r.origin))
		return false;

	// Prefer live head hitbox; fallback origin+64
	if (!Bones::GetHitboxPoint(pawn, Config::HB_HEAD, r.head) || !Bones::IsValidPos(r.head)) {
		r.head = r.origin;
		r.head.z += 64.f;
	}

	Vector_t slots[Bones::S_COUNT]{};
	bool ok[Bones::S_COUNT]{};
	const int n = Bones::CollectSkeletonPoints(pawn, slots, ok);
	if (n < 2) {
		for (int i = 0; i < Bones::S_COUNT; ++i) {
			r.slots[i] = Vector_t{ 0.f, 0.f, 0.f };
			r.slotOk[i] = false;
		}
		if (Bones::IsValidPos(r.head)) {
			r.slots[Bones::S_HEAD] = r.head;
			r.slotOk[Bones::S_HEAD] = true;
		}
		r.slots[Bones::S_PELVIS] = r.origin;
		r.slotOk[Bones::S_PELVIS] = true;
	} else {
		for (int i = 0; i < Bones::S_COUNT; ++i) {
			r.slots[i] = slots[i];
			r.slotOk[i] = ok[i] && Bones::IsValidPos(slots[i]);
		}
	}
	return true;
}

// simTime → lag-comp render tick. Round nearest (floor can be 1 tick early).
// captureRenderTick stored separately for age; stamp uses sim tick.
void FillTickFromSim(Record& r, int captureRenderTick)
{
	r.captureRenderTick = (captureRenderTick > 0) ? captureRenderTick : 0;
	r.tickcount = 0;
	r.tickFrac = 0.f;
	if (!(r.simTime > 0.f) || !std::isfinite(r.simTime)) {
		// No sim: stamp capture render (best available)
		r.tickcount = r.captureRenderTick;
		return;
	}
	const float tf = r.simTime / kTickInterval;
	if (!(tf > 1.f) || tf > 1.0e8f) {
		r.tickcount = r.captureRenderTick;
		return;
	}
	int simTick = static_cast<int>(tf + 0.5f);
	if (simTick < 1)
		simTick = static_cast<int>(tf);
	// Always prefer sim-derived tick for lag-comp (IDA: render time = tick*ti+frac*ti).
	// captureRender is "now" — only use if sim totally diverged (map change / desync).
	if (r.captureRenderTick > 0 && std::abs(simTick - r.captureRenderTick) > 128)
		r.tickcount = r.captureRenderTick;
	else
		r.tickcount = simTick;
	r.tickFrac = 0.f;
}

void RecordPawn(C_CSPlayerPawn* pawn, std::uint32_t handle, float curtime, int captureRenderTick)
{
	if (!pawn || !handle)
		return;

	const float sim = ReadSimTime(pawn);
	Track* tr = FindTrack(handle, true);
	if (!tr)
		return;
	tr->missFrames = 0;

	// Gate: only on simTime advance (network update). Without this, high-FPS
	// CreateMove floods ring with identical live poses → ghost stuck on body.
	if (tr->count > 0 && sim > 0.f && tr->lastSim > 0.f) {
		if (sim <= tr->lastSim + 1e-5f)
			return;
	}

	Record r{};
	r.simTime = sim;
	r.captureCur = curtime;
	r.wallMs = NowWallMs();
	FillTickFromSim(r, captureRenderTick);
	if (!CaptureBones(pawn, r))
		return;
	r.valid = true;
	PushRecord(*tr, r);
}

float ActiveWindowMs()
{
	float ms = Config::backtrack_ms;
	if (!(ms >= 50.f))
		ms = 200.f;
	return std::clamp(ms, 50.f, 400.f);
}

float WindowSecFromMs(float preferMs)
{
	float ms = preferMs;
	if (!(ms >= 50.f))
		ms = ActiveWindowMs();
	ms = std::clamp(ms, 50.f, 400.f);
	return ms * 0.001f;
}

} // namespace

bool WantRecords()
{
	// Master only — skeleton/chams are visual sub-toggles, not record gates
	return Config::backtrack;
}

float WindowSec(float preferMs)
{
	return WindowSecFromMs(preferMs);
}

float RecordAgeSec(const Record& r, float curtime)
{
	return AgeSec(r, curtime);
}

void Clear()
{
	for (int i = 0; i < kMaxTracks; ++i)
		g_tracks[i] = Track{};
	g_pendingSim = 0.f;
	g_pendingTick = 0;
	g_pendingFrac = 0.f;
}

void SetPendingSim(float simTime)
{
	g_pendingSim = (simTime > 0.f && std::isfinite(simTime)) ? simTime : 0.f;
	// Derive tick from sim if no explicit tick set yet (round, not floor)
	if (g_pendingTick <= 0 && g_pendingSim > 0.f) {
		const float tf = g_pendingSim / kTickInterval;
		g_pendingTick = static_cast<int>(tf + 0.5f);
		if (g_pendingTick < 1)
			g_pendingTick = static_cast<int>(tf);
		// Enemy pose has no subtick history — frac 0 keeps stamp on record tick
		g_pendingFrac = 0.f;
	}
}

void SetPendingTick(int tick, float frac)
{
	g_pendingTick = (tick > 0) ? tick : 0;
	// Lag records are tick-boundary poses; ignore noisy frac from floor(sim/ti)
	(void)frac;
	g_pendingFrac = 0.f;
}

float PendingSim()
{
	return g_pendingSim;
}

int PendingTick()
{
	return g_pendingTick;
}

void ClearPending()
{
	g_pendingSim = 0.f;
	g_pendingTick = 0;
	g_pendingFrac = 0.f;
}

const Record* GetLagged(std::uint32_t handle, float preferMs, bool requireSeparation)
{
	if (!handle)
		return nullptr;
	Track* tr = FindTrack(handle, false);
	if (!tr || tr->count <= 0)
		return nullptr;
	const float curtime = ReadCurtime();
	const float maxSec = WindowSecFromMs(preferMs);
	const float targetSec = (std::max)(kTickInterval * 4.f, maxSec * 0.75f);
	// Draw: slightly older min so ghost is not on body even mid-step
	const float minSec = requireSeparation
		? (kTickInterval * 3.f)
		: (kTickInterval * 2.f);
	return PickLagged(*tr, curtime, targetSec, minSec, maxSec, requireSeparation);
}

std::uint32_t HandleOf(const C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return 0;
	std::uint32_t out = 0;
	__try {
		auto* inst = reinterpret_cast<CEntityInstance*>(
			const_cast<C_CSPlayerPawn*>(pawn));
		CBaseHandle h = inst->handle();
		if (h.valid())
			out = h.raw();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out = 0;
	}
	return out;
}

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* local)
{
	// Fresh pending each CreateMove — AF/TR re-set if BT pick wins
	ClearPending();

	if (!WantRecords() || !local)
		return;
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return;

	const float curtime = ReadCurtime();
	// Capture client render clock (not Pred tickcount) for age + clamp.
	const int captureRender = ReadLiveRenderTick(cmd);

	// Prefer shared aim list (same CreateMove tick) — skip second entity walk
	const int nAim = AimCommon::AimTargetCount();
	const AimCommon::AimTarget* aimList = AimCommon::AimTargets();
	bool seen[kMaxTracks]{};

	if (nAim > 0 && aimList) {
		for (int i = 0; i < nAim; ++i) {
			C_CSPlayerPawn* pawn = aimList[i].pawn;
			const std::uint32_t raw = aimList[i].handle;
			if (!raw || !Mem::ValidEntity(pawn))
				continue;
			RecordPawn(pawn, raw, curtime, captureRender);
			if (Track* tr = FindTrack(raw, false)) {
				const int idx = static_cast<int>(tr - g_tracks);
				if (idx >= 0 && idx < kMaxTracks)
					seen[idx] = true;
			}
		}
	} else {
		// Fallback when collect skipped (menu / soft-pause edge)
		const int nMaxRaw = I::GameEntity->Instance->GetHighestEntityIndex();
		if (nMaxRaw <= 0)
			return;
		const int nMax = (nMaxRaw > 128) ? 128 : nMaxRaw;
		const uint8_t localTeam = local->getTeam();
		int checked = 0;
		for (int i = 1; i <= nMax; ++i) {
			auto* Entity = I::GameEntity->Instance->Get(i);
			if (!Mem::ValidEntity(Entity) || !Entity->handle().valid())
				continue;

			bool isCtrl = false;
			__try {
				CEntityIdentity* id = nullptr;
				if (!Mem::ReadField(Entity, 0x10, id) || !id || !Mem::Valid(id, 0x28))
					id = Entity->m_pEntityIdentity();
				if (id && Mem::Valid(id, 0x28)) {
					const char* designer = nullptr;
					if (!Mem::ReadField(id, 0x20, designer) || !designer)
						designer = id->m_designerName();
					if (designer && Mem::IsReadable(designer, 2) && designer[0]) {
						if (std::strcmp(designer, "cs_player_controller") == 0
							|| std::strstr(designer, "player_controller") != nullptr)
							isCtrl = true;
						else
							continue;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				continue;
			}
			if (!isCtrl)
				continue;
			if (checked >= Mem::kMaxPlayers)
				break;
			++checked;

			auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
			if (Controller->IsLocalPlayer())
				continue;

			CBaseHandle hPawn = Controller->m_hPlayerPawn();
			if (!hPawn.valid())
				hPawn = Controller->m_hPawn();
			if (!hPawn.valid())
				continue;

			auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
			if (!Mem::ValidEntity(pawn))
				continue;
			const int hp = Mem::ClampHealth(pawn->m_iHealth());
			if (hp < 1 || pawn->m_lifeState() != 0)
				continue;
			const uint8_t team = pawn->m_iTeamNum();
			if (!Mem::ValidTeam(static_cast<int>(team)))
				continue;
			if (GameMode::WantTeamCheck(Config::team_check) && team == localTeam)
				continue;

			const std::uint32_t raw = hPawn.raw();
			RecordPawn(pawn, raw, curtime, captureRender);
			if (Track* tr = FindTrack(raw, false)) {
				const int idx = static_cast<int>(tr - g_tracks);
				if (idx >= 0 && idx < kMaxTracks)
					seen[idx] = true;
			}
		}
	}

	// Drop tracks only after a few misses — 1-frame entity-list flicker
	// was wiping full history mid-strafe (ghost/aim BT flicker).
	constexpr int kTrackMissGrace = 8;
	for (int i = 0; i < kMaxTracks; ++i) {
		if (!g_tracks[i].used)
			continue;
		if (seen[i]) {
			g_tracks[i].missFrames = 0;
			continue;
		}
		++g_tracks[i].missFrames;
		if (g_tracks[i].missFrames >= kTrackMissGrace)
			g_tracks[i] = Track{};
	}
}

bool BestHead(
	C_CSPlayerPawn* /*target*/,
	std::uint32_t handle,
	const Vector_t& eye,
	const QAngle_t& view,
	float maxFov,
	Vector_t& outPoint,
	float& outFov,
	float* outSimTime,
	float preferMs,
	int* outTick,
	float* outTickFrac)
{
	outFov = maxFov + 1.f;
	if (outSimTime)
		*outSimTime = 0.f;
	if (outTick)
		*outTick = 0;
	if (outTickFrac)
		*outTickFrac = 0.f;
	if (!WantRecords() || !handle)
		return false;
	Track* tr = FindTrack(handle, false);
	if (!tr || tr->count <= 0)
		return false;

	const float curtime = ReadCurtime();
	const float maxSec = WindowSecFromMs(preferMs);
	// Skip freshest ~2 ticks so aim uses lagged pose, not live bones
	const float minSec = kTickInterval * 2.f;

	bool found = false;
	float bestSim = 0.f;
	int bestTick = 0;
	float bestFrac = 0.f;
	float bestAge = 1.0e30f;
	// Prefer record near mid-window (not newest, not max stale)
	const float targetAge = (std::max)(minSec + kTickInterval, maxSec * 0.55f);

	for (int i = 0; i < tr->count; ++i) {
		const int idx = (tr->write - 1 - i + kMaxRecords * 2) % kMaxRecords;
		const Record& r = tr->rec[idx];
		if (!r.valid || !Bones::IsValidPos(r.head))
			continue;
		const float age = AgeSec(r, curtime);
		if (age < minSec || age > maxSec)
			continue;

		QAngle_t ang{};
		if (!AimCommon::CalcAngles(eye, r.head, ang))
			continue;
		const float fov = AimCommon::GetFov(view, ang);
		if (!Mem::Finite(fov) || fov > maxFov)
			continue;

		// FOV primary; age-to-target as tie-break (more reliable unlag)
		const float ageErr = std::fabs(age - targetAge);
		const bool betterFov = !found || fov < outFov - 0.05f;
		const bool similarFov = found && std::fabs(fov - outFov) <= 0.05f;
		const bool betterAge = similarFov && ageErr < std::fabs(bestAge - targetAge);
		if (betterFov || betterAge) {
			outFov = fov;
			outPoint = r.head;
			bestSim = r.simTime;
			bestTick = r.tickcount;
			bestFrac = r.tickFrac;
			bestAge = age;
			found = true;
		}
	}
	if (found && outSimTime)
		*outSimTime = bestSim;
	if (found && outTick)
		*outTick = bestTick;
	if (found && outTickFrac)
		*outTickFrac = bestFrac;
	return found;
}

// Map Config hitbox → lag skeleton slots (Record.slots)
void CollectLagPoints(
	const Record& r,
	const bool* hbMask,
	Vector_t* pts,
	int* hbs,
	int& nPts)
{
	nPts = 0;
	auto add = [&](int hb, const Vector_t& p, bool ok) {
		if (!ok || nPts >= 16)
			return;
		if (!Bones::IsValidPos(p))
			return;
		pts[nPts] = p;
		hbs[nPts] = hb;
		++nPts;
	};

	const bool anyHb = hbMask != nullptr;
	auto want = [&](int hb) -> bool {
		if (!anyHb)
			return true;
		return hb >= 0 && hb < Config::HB_COUNT && hbMask[hb];
	};

	// Always try head field + slots when HB enabled
	if (want(Config::HB_HEAD)) {
		add(Config::HB_HEAD, r.head, true);
		if (r.slotOk[Bones::S_HEAD])
			add(Config::HB_HEAD, r.slots[Bones::S_HEAD], true);
	}
	if (want(Config::HB_NECK) && r.slotOk[Bones::S_NECK])
		add(Config::HB_NECK, r.slots[Bones::S_NECK], true);
	if (want(Config::HB_CHEST)) {
		if (r.slotOk[Bones::S_SPINE3])
			add(Config::HB_CHEST, r.slots[Bones::S_SPINE3], true);
		if (r.slotOk[Bones::S_SPINE2])
			add(Config::HB_CHEST, r.slots[Bones::S_SPINE2], true);
	}
	if (want(Config::HB_STOMACH)) {
		if (r.slotOk[Bones::S_SPINE1])
			add(Config::HB_STOMACH, r.slots[Bones::S_SPINE1], true);
		if (r.slotOk[Bones::S_SPINE0])
			add(Config::HB_STOMACH, r.slots[Bones::S_SPINE0], true);
	}
	if (want(Config::HB_PELVIS) && r.slotOk[Bones::S_PELVIS])
		add(Config::HB_PELVIS, r.slots[Bones::S_PELVIS], true);
	if (want(Config::HB_ARMS)) {
		if (r.slotOk[Bones::S_ARM_U_L])
			add(Config::HB_ARMS, r.slots[Bones::S_ARM_U_L], true);
		if (r.slotOk[Bones::S_ARM_U_R])
			add(Config::HB_ARMS, r.slots[Bones::S_ARM_U_R], true);
		if (r.slotOk[Bones::S_ARM_L_L])
			add(Config::HB_ARMS, r.slots[Bones::S_ARM_L_L], true);
		if (r.slotOk[Bones::S_ARM_L_R])
			add(Config::HB_ARMS, r.slots[Bones::S_ARM_L_R], true);
	}
	if (want(Config::HB_LEGS)) {
		if (r.slotOk[Bones::S_LEG_U_L])
			add(Config::HB_LEGS, r.slots[Bones::S_LEG_U_L], true);
		if (r.slotOk[Bones::S_LEG_U_R])
			add(Config::HB_LEGS, r.slots[Bones::S_LEG_U_R], true);
		if (r.slotOk[Bones::S_LEG_L_L])
			add(Config::HB_LEGS, r.slots[Bones::S_LEG_L_L], true);
		if (r.slotOk[Bones::S_LEG_L_R])
			add(Config::HB_LEGS, r.slots[Bones::S_LEG_L_R], true);
	}
	if (want(Config::HB_FEET)) {
		if (r.slotOk[Bones::S_ANKLE_L])
			add(Config::HB_FEET, r.slots[Bones::S_ANKLE_L], true);
		if (r.slotOk[Bones::S_ANKLE_R])
			add(Config::HB_FEET, r.slots[Bones::S_ANKLE_R], true);
	}

	// Fallback: head only if mask empty / nothing collected
	if (nPts == 0 && Bones::IsValidPos(r.head))
		add(Config::HB_HEAD, r.head, true);
}

bool BestUnderCrosshair(
	const Vector_t& eye,
	const QAngle_t& view,
	float maxFov,
	Vector_t& outPoint,
	float& outFov,
	float& outSimTime,
	std::uint32_t* outHandle,
	float preferMs,
	int* outTick,
	float* outTickFrac,
	int* outHb)
{
	outFov = maxFov + 1.f;
	outSimTime = 0.f;
	if (outHandle)
		*outHandle = 0;
	if (outTick)
		*outTick = 0;
	if (outTickFrac)
		*outTickFrac = 0.f;
	if (outHb)
		*outHb = Config::HB_HEAD;
	if (!WantRecords())
		return false;

	// Prefer trigger hitbox list; if none enabled, use core body set
	bool hbMask[Config::HB_COUNT]{};
	bool any = false;
	for (int h = 0; h < Config::HB_COUNT; ++h) {
		hbMask[h] = Config::trigger_hitboxes[h];
		if (hbMask[h])
			any = true;
	}
	if (!any) {
		hbMask[Config::HB_HEAD] = true;
		hbMask[Config::HB_NECK] = true;
		hbMask[Config::HB_CHEST] = true;
		hbMask[Config::HB_STOMACH] = true;
		hbMask[Config::HB_PELVIS] = true;
	}

	const float curtime = ReadCurtime();
	const float maxSec = WindowSecFromMs(preferMs);
	const float minSec = kTickInterval * 2.f;
	const float targetAge = (std::max)(minSec + kTickInterval, maxSec * 0.55f);
	bool found = false;
	float bestAge = 1.0e30f;
	int bestTick = 0;
	float bestFrac = 0.f;
	int bestHb = Config::HB_HEAD;

	for (int t = 0; t < kMaxTracks; ++t) {
		const Track& tr = g_tracks[t];
		if (!tr.used || tr.count <= 0)
			continue;
		for (int i = 0; i < tr.count; ++i) {
			const int idx = (tr.write - 1 - i + kMaxRecords * 2) % kMaxRecords;
			const Record& r = tr.rec[idx];
			if (!r.valid)
				continue;
			const float age = AgeSec(r, curtime);
			if (age < minSec || age > maxSec)
				continue;

			Vector_t pts[16]{};
			int hbs[16]{};
			int nPts = 0;
			CollectLagPoints(r, hbMask, pts, hbs, nPts);

			for (int p = 0; p < nPts; ++p) {
				QAngle_t ang{};
				if (!AimCommon::CalcAngles(eye, pts[p], ang))
					continue;
				const float fov = AimCommon::GetFov(view, ang);
				if (!Mem::Finite(fov) || fov > maxFov)
					continue;
				const float ageErr = std::fabs(age - targetAge);
				const bool betterFov = !found || fov < outFov - 0.05f;
				const bool similarFov = found && std::fabs(fov - outFov) <= 0.05f;
				const bool betterAge = similarFov && ageErr < std::fabs(bestAge - targetAge);
				// Prefer higher-value HB on near-equal FOV (head > neck > chest...)
				const bool betterHb = similarFov && !betterAge && hbs[p] < bestHb;
				if (betterFov || betterAge || betterHb) {
					outFov = fov;
					outPoint = pts[p];
					outSimTime = r.simTime;
					bestTick = r.tickcount;
					bestFrac = r.tickFrac;
					bestAge = age;
					bestHb = hbs[p];
					if (outHandle)
						*outHandle = tr.handle;
					found = true;
				}
			}
		}
	}
	if (found && outTick)
		*outTick = bestTick;
	if (found && outTickFrac)
		*outTickFrac = bestFrac;
	if (found && outHb)
		*outHb = bestHb;
	return found;
}

// SEH-only write — no C++ objects (C2712).
// Stamp EVERY hist entry — server fire may sample attack index or tip.
// Never touch cl_interp/sv_interp* (often null/garbage → crash on fire).
// Never rewrite nPlayerTickCount (spread seed).
bool SehStampRenderTick(CUserCmd* cmd, int renderTick, float renderFrac)
{
	if (!cmd || renderTick <= 0)
		return false;
	bool ok = false;
	__try {
		if (!cmd->csgoUserCmd.inputHistoryField.pRep)
			return false;
		const int histCount = cmd->csgoUserCmd.inputHistoryField.nCurrentSize;
		if (histCount <= 0 || histCount > 64)
			return false;

		int idx = cmd->csgoUserCmd.nAttack1StartHistoryIndex;
		if (idx < 0 || idx >= histCount)
			idx = histCount - 1;

		// Bind attack hist index (hasbit) before write
		cmd->csgoUserCmd.nAttack1StartHistoryIndex = idx;
		cmd->csgoUserCmd.CheckAndSetBits(CCSGOUserCmdPB::BITS_ATTACK1START);

		for (int i = 0; i < histCount; ++i) {
			CCSGOInputHistoryEntryPB* e = cmd->GetInputHistoryEntry(i);
			if (!e || reinterpret_cast<std::uintptr_t>(e) < 0x10000ull)
				continue;
			e->nRenderTickCount = renderTick;
			e->flRenderTickFraction = renderFrac;
			e->SetBits(
				EInputHistoryBits::INPUT_HISTORY_BITS_RENDERTICKCOUNT
				| EInputHistoryBits::INPUT_HISTORY_BITS_RENDERTICKFRACTION);
			ok = true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return ok;
}

// Clamp into unlag window vs engine render tick (not pred GlobalVars).
// Match menu window (50–400ms). Hard 13-tick cap was killing stamps for
// ghosts drawn at >200ms (aim ghost, stamp live-200ms pose → 0 dmg).
void ClampRenderTick(CUserCmd* cmd, int& renderTick, float preferMs)
{
	const int liveTick = ReadLiveRenderTick(cmd);
	if (liveTick <= 0)
		return;
	const int menuTicks = static_cast<int>(WindowSecFromMs(preferMs) / kTickInterval) + 2;
	// 400ms menu → 26 ticks; leave headroom for subtick/net jitter
	const int hardMax = 28;
	const int maxLagTicks = (std::min)((std::max)(menuTicks, 2), hardMax);
	const int minTick = liveTick - maxLagTicks;
	if (renderTick > liveTick)
		renderTick = liveTick;
	if (minTick > 0 && renderTick < minTick)
		renderTick = minTick;
}

// Prefer record tickcount. Fallback: round(simTime/tick).
// Keep nPlayerTickCount alone. No interp PB writes.
bool StampLagCompTick(CUserCmd* cmd, int renderTick, float renderFrac, float preferMs)
{
	if (!cmd || renderTick <= 0)
		return false;
	// Enemy records are tick-boundary — frac 0 for lag-comp
	(void)renderFrac;
	renderFrac = 0.f;
	ClampRenderTick(cmd, renderTick, preferMs);
	if (renderTick <= 0)
		return false;
	return SehStampRenderTick(cmd, renderTick, renderFrac);
}

bool StampLagComp(CUserCmd* cmd, float simTime, float preferMs)
{
	if (!cmd || !(simTime > 0.f) || !std::isfinite(simTime))
		return false;

	const float ticksF = simTime / kTickInterval;
	if (!(ticksF > 1.f) || ticksF > 1.0e8f)
		return false;

	int renderTick = static_cast<int>(ticksF + 0.5f);
	if (renderTick < 1)
		renderTick = static_cast<int>(ticksF);

	return StampLagCompTick(cmd, renderTick, 0.f, preferMs);
}

// Safe view read for manual ghost fire (no SEH+C++ mix)
bool SehReadCmdView(CUserCmd* cmd, float* outP, float* outY, float* outR)
{
	if (!cmd || !outP || !outY || !outR)
		return false;
	bool ok = false;
	__try {
		CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
		if (!base || reinterpret_cast<std::uintptr_t>(base) < 0x10000ull)
			return false;
		CMsgQAngle* va = base->pViewAngles;
		if (!va || reinterpret_cast<std::uintptr_t>(va) < 0x10000ull)
			return false;
		*outP = va->angValue.x;
		*outY = va->angValue.y;
		*outR = va->angValue.z;
		ok = std::isfinite(*outP) && std::isfinite(*outY);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return ok;
}

// Manual M1 at BT skeleton.
// IDA 0x180C93780: lag-comp time = nRenderTickCount + frac (tick*0.015625+…).
// Pick: 1) same PickLagged ghost as Draw  2) any lag multipoint under crosshair.
// Age uses wall clock (Pred-safe) so CreateMove pick matches Present skeleton.
bool ManualGhostStamp(
	CUserCmd* cmd,
	C_CSPlayerPawn* local,
	float preferMs,
	int& outTick,
	float& outSim)
{
	outTick = 0;
	outSim = 0.f;
	if (!cmd || !local || !Mem::ValidEntity(local))
		return false;

	Vector_t eye = Bones::GetShootPos(local);
	if (!Bones::IsValidPos(eye))
		return false;

	float p = 0.f, y = 0.f, r = 0.f;
	if (!SehReadCmdView(cmd, &p, &y, &r))
		return false;
	QAngle_t view{ p, y, r };
	view.Normalize();
	if (!view.IsValid())
		return false;

	const float curtime = ReadCurtime();
	const float maxSec = WindowSecFromMs(preferMs);
	const float targetSec = (std::max)(kTickInterval * 4.f, maxSec * 0.75f);
	// Allow fresher than Draw min (2t) so fire still stamps if ghost age drifts
	const float minSec = kTickInterval * 2.f;
	const float maxFov = 28.f; // bone multipoint + lag offset

	bool hbMask[Config::HB_COUNT]{};
	hbMask[Config::HB_HEAD] = true;
	hbMask[Config::HB_NECK] = true;
	hbMask[Config::HB_CHEST] = true;
	hbMask[Config::HB_STOMACH] = true;
	hbMask[Config::HB_PELVIS] = true;
	hbMask[Config::HB_ARMS] = true;
	hbMask[Config::HB_LEGS] = true;

	bool found = false;
	float bestFov = maxFov + 1.f;
	int bestTick = 0;
	float bestSim = 0.f;

	auto consider = [&](const Record& r) {
		if (!r.valid)
			return;
		const float age = AgeSec(r, curtime);
		if (age < minSec || age > maxSec)
			return;
		Vector_t pts[16]{};
		int hbs[16]{};
		int nPts = 0;
		CollectLagPoints(r, hbMask, pts, hbs, nPts);
		for (int i = 0; i < nPts; ++i) {
			QAngle_t ang{};
			if (!AimCommon::CalcAngles(eye, pts[i], ang))
				continue;
			const float fov = AimCommon::GetFov(view, ang);
			if (!Mem::Finite(fov) || fov > maxFov)
				continue;
			if (!found || fov < bestFov) {
				bestFov = fov;
				// Prefer sim tick; captureRender only if tick missing
				bestTick = (r.tickcount > 0) ? r.tickcount : r.captureRenderTick;
				bestSim = r.simTime;
				found = true;
			}
		}
	};

	// Pass 1: same PickLagged ghost Draw paints (requireSeparation)
	for (int t = 0; t < kMaxTracks; ++t) {
		const Track& tr = g_tracks[t];
		if (!tr.used || tr.count < 2)
			continue;
		const Record* pr = PickLagged(tr, curtime, targetSec, minSec, maxSec, true);
		if (pr)
			consider(*pr);
	}

	// Pass 2: any valid lag record under crosshair (still in window)
	if (!found) {
		for (int t = 0; t < kMaxTracks; ++t) {
			const Track& tr = g_tracks[t];
			if (!tr.used || tr.count <= 0)
				continue;
			for (int i = 0; i < tr.count; ++i) {
				const int idx = (tr.write - 1 - i + kMaxRecords * 2) % kMaxRecords;
				consider(tr.rec[idx]);
			}
		}
	}

	if (!found)
		return false;
	if (bestTick > 0)
		outTick = bestTick;
	if (bestSim > 0.f)
		outSim = bestSim;
	// Derive tick from sim if only sim known
	if (outTick <= 0 && outSim > 0.f) {
		const float tf = outSim / kTickInterval;
		outTick = static_cast<int>(tf + 0.5f);
		if (outTick < 1)
			outTick = static_cast<int>(tf);
	}
	return outTick > 0;
}

void OnFire(CUserCmd* cmd, C_CSPlayerPawn* local, float preferMs)
{
	if (!WantRecords() || !cmd)
		return;

	// Manual fire always re-resolves ghost under crosshair.
	// Ignore stale pending from AF/TR clear races — manual path owns stamp.
	int tick = 0;
	float frac = 0.f;
	float sim = 0.f;

	if (local && Mem::ValidEntity(local)) {
		int gTick = 0;
		float gSim = 0.f;
		if (ManualGhostStamp(cmd, local, preferMs, gTick, gSim)) {
			tick = gTick;
			sim = gSim;
			frac = 0.f;
		}
	}

	// AF/aim pending only if ghost pick missed (aimbot BT path)
	if (tick <= 0 && !(sim > 0.f)) {
		tick = g_pendingTick;
		frac = g_pendingFrac;
		sim = g_pendingSim;
	}

	if (tick > 0)
		StampLagCompTick(cmd, tick, frac, preferMs);
	else if (sim > 0.f)
		StampLagComp(cmd, sim, preferMs);
}

// Skeleton ESP only. Lag pose behind live model (enemy moving). Standing = no draw.
// Age = wall clock (same as ManualGhostStamp) so drawn ghost = stampable record.
void Draw(const ViewMatrix& vm)
{
	if (!Config::backtrack || !Config::backtrack_skeleton)
		return;
	if (!vm.viewMatrix)
		return;
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;

	const ImU32 colSkel = ImGui::ColorConvertFloat4ToU32(Config::backtrack_color);
	const float th = 1.6f;
	const float curtime = ReadCurtime();
	const float maxSec = WindowSecFromMs(0.f);
	const float targetSec = (std::max)(kTickInterval * 4.f, maxSec * 0.75f);
	// Match ManualGhostStamp min so ghost always stampable when visible
	const float minSec = kTickInterval * 2.f;

	for (int t = 0; t < kMaxTracks; ++t) {
		Track& tr = g_tracks[t];
		if (!tr.used || tr.count < 2)
			continue;

		const Record* pr = PickLagged(tr, curtime, targetSec, minSec, maxSec, true);
		if (!pr)
			continue;
		const Record& r = *pr;

		for (int e = 0; e < Bones::kSkeletonCount; ++e) {
			const int a = Bones::kSkeleton[e].from;
			const int b = Bones::kSkeleton[e].to;
			if (a < 0 || b < 0 || a >= Bones::S_COUNT || b >= Bones::S_COUNT)
				continue;
			if (!r.slotOk[a] || !r.slotOk[b])
				continue;
			Vector_t sa{}, sb{};
			if (!vm.WorldToScreen(r.slots[a], sa) || !vm.WorldToScreen(r.slots[b], sb))
				continue;
			dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), colSkel, th);
		}
		if (Bones::IsValidPos(r.head)) {
			Vector_t sh{};
			if (vm.WorldToScreen(r.head, sh))
				dl->AddCircleFilled(ImVec2(sh.x, sh.y), 2.5f, colSkel);
		}
	}
}

} // namespace Backtrack
