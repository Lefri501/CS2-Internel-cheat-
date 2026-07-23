#include "trace.h"

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/console/console.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CGameEntitySystem/CGameEntitySystem.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/handle.h"
#include "../../config/config.h"

#include <cstring>
#include <cmath>
#include <atomic>
#include <Windows.h>

namespace Trace {
namespace {

// Live client TraceShape (IDA sub_1809CCD90):
//   call sites: mov rcx, [pGameTraceManager]  → pass *global (NOT &global)
//   body: if (!*a1) return; iface=*a1; vt=*iface; call vt[0x548]
//   Ray type byte @ +0x28; filter[0x37]=skip rewrite; filter[0x40]=multi-hit

struct Ray_t {
	alignas(16) std::uint8_t raw[0x30]{};

	void set_line() {
		std::memset(raw, 0, sizeof(raw));
		raw[0x28] = 0;
	}
	void set_hull(const Vector_t& mins, const Vector_t& maxs) {
		std::memset(raw, 0, sizeof(raw));
		std::memcpy(raw + 0x00, &mins, sizeof(float) * 3);
		std::memcpy(raw + 0x0C, &maxs, sizeof(float) * 3);
		raw[0x28] = 1;
	}
};

using TraceShapeFn = bool(__fastcall*)(void* world, const Ray_t* ray,
                                       const Vector_t* start, const Vector_t* end,
                                       const void* filter, CGameTrace* out);
using InitFilterFn = void*(__fastcall*)(void* filter, void* skip, std::uint64_t mask,
                                        int layer, int a5);
using InitTraceInfoFn = void*(__fastcall*)(CGameTrace* tr);
using GetSurfaceDataFn = void*(__fastcall*)(void* surfaceOrTrace);

TraceShapeFn     g_traceShape = nullptr;
InitFilterFn     g_initFilter = nullptr;
InitTraceInfoFn  g_initTraceInfo = nullptr;
GetSurfaceDataFn g_getSurfaceData = nullptr;
void**           g_ppTraceMgr = nullptr;
bool             g_ready = false;

std::atomic<int>  g_avStreak{0};
std::atomic<bool> g_traceDisabled{false};
std::atomic<int>  g_okStreak{0};
// Soft-disable only after repeated faults; single AV must not kill vis permanently
constexpr int kAvDisableThreshold = 6;
// After this many clean traces while disabled, re-enable fully
constexpr int kOkReenableThreshold = 3;
// m_hOwnerEntity fallback (C_BaseEntity schema dump)
constexpr std::uint32_t kOwnerEntityOff = 0x520;

// Prologue + stack alloc size unique to TraceShape (0x24E8)
constexpr const char* kPatTraceShape =
	"48 89 54 24 ? 48 89 4C 24 ? 55 53 56 57 41 56 41 57 48 8D AC 24 ? ? ? ? B8 E8 24 00 00";
constexpr const char* kPatTraceShapeLoose =
	"48 89 54 24 ? 48 89 4C 24 ? 55 53 56 57 41 56 41 57 48 8D AC 24";
constexpr const char* kPatInitFilter =
	"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 41 39 33 FF 24 C9 C7";
constexpr const char* kPatInitFilterLoose =
	"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F B6 41 ? 33 FF 24";
constexpr const char* kPatInitTraceInfo =
	"40 55 41 55 41 57 48 83 EC 30 4C 8B 3D";
constexpr const char* kPatGetSurfaceData =
	"48 63 41 10 48 8B 0D ? ? ? ? 48 C1 E0 05 48";
// mov rcx, [pGameTraceManager] at real FireBullet-style sites
constexpr const char* kPatGameTraceMgr =
	"48 8B 0D ? ? ? ? 48 8B D0 C7 44 24 ? 04 00 00 00 48 C7 44 24 ? 01 30 1C 00";
constexpr const char* kPatGameTraceMgrLoose =
	"48 8B 0D ? ? ? ? 48 8B D0 C7 44 24";
constexpr const char* kPatPhysWorld =
	"48 8B 1D ? ? ? ? 48 8B 01 FF 90 ? ? ? ? 4C 8B 0B 4C 8D 44 24 ? 48 8B C8";

constexpr std::size_t kTraceShapeVtableOff = 0x548;

std::uint8_t* FindClient(const char* pat) {
	auto* p = M::FindPattern("client.dll", pat);
	if (!p)
		p = M::FindPattern("client", pat);
	return p;
}

bool VtableLooksGood(void* iface) {
	if (!iface || !Mem::IsReadable(iface, sizeof(void*)))
		return false;
	void* vt = nullptr;
	__try {
		vt = *reinterpret_cast<void**>(iface);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!vt || !Mem::IsUserPtr(vt))
		return false;
	if (!Mem::IsReadable(reinterpret_cast<std::uint8_t*>(vt) + kTraceShapeVtableOff, sizeof(void*)))
		return false;
	void* method = nullptr;
	__try {
		method = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(vt) + kTraceShapeVtableOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return method && Mem::IsUserPtr(method);
}

// Game: mov rcx, [pGameTraceManager] → a1 = *global
// TraceShape: if (!*a1) return; iface=*a1; vt=*iface; call vt[0x548]
// Passing &global (one extra indirection) AVs inside the phys vcall.
void* ResolveWorldArg() {
	if (!g_ppTraceMgr || !Mem::IsReadable(g_ppTraceMgr, sizeof(void*)))
		return nullptr;

	void* world = *g_ppTraceMgr; // same value game loads into rcx
	if (!world || !Mem::IsUserPtr(world) || !Mem::IsReadable(world, sizeof(void*)))
		return nullptr;

	void* iface = *reinterpret_cast<void**>(world);
	if (!VtableLooksGood(iface))
		return nullptr;

	return world;
}

void NoteAv(unsigned long code) {
	g_okStreak.store(0, std::memory_order_relaxed);
	const int n = g_avStreak.fetch_add(1) + 1;
	if (n >= kAvDisableThreshold) {
		g_traceDisabled.store(true);
		Con::Seh("Trace::TraceShape (disabled)", code);
	} else {
		Con::Seh("Trace::TraceShape", code);
	}
}

void NoteTraceOk() {
	g_avStreak.store(0, std::memory_order_relaxed);
	// Require a few clean calls before clearing soft-disable (avoid flap)
	if (g_traceDisabled.load(std::memory_order_relaxed)) {
		const int ok = g_okStreak.fetch_add(1) + 1;
		if (ok >= kOkReenableThreshold) {
			g_traceDisabled.store(false, std::memory_order_relaxed);
			g_okStreak.store(0, std::memory_order_relaxed);
			Con::Ok("Trace re-enabled after clean calls");
		}
	} else {
		g_okStreak.store(0, std::memory_order_relaxed);
	}
}

bool SehTraceShape(void* world, const Ray_t* ray, const Vector_t* start, const Vector_t* end,
                   void* filter, CGameTrace* out) {
	__try {
		if (!Mem::IsReadable(world, sizeof(void*))) {
			Con::Warn("TraceShape: world ptr unreadable %p", world);
			return false;
		}
		void* iface = *reinterpret_cast<void**>(world);
		if (!iface || !Mem::IsUserPtr(iface)) {
			Con::Warn("TraceShape: iface invalid %p (world=%p)", iface, world);
			return false;
		}
		// DidHit return is not success — call surviving without AV is success
		(void)g_traceShape(world, ray, start, end, filter, out);
		NoteTraceOk();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		NoteAv(GetExceptionCode());
		return false;
	}
}

void* SehInitFilter(void* filter, void* skip, std::uint64_t mask, int layer, int a5) {
	__try {
		return g_initFilter(filter, skip, mask, layer, a5);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void SehInitTraceInfo(CGameTrace* tr) {
	__try {
		g_initTraceInfo(tr);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Match FireBullet wrapper filter (IDA InitFilter 0x1803432D0 + FireBullet sites):
//   +0   vtable
//   +8   mask qword
//   +0x34 (52) InitFilter writes 0x0F00FFFF-ish; FireBullet uses -65536@+0x32, 0x0F00@+0x36
//   +0x38 layer byte
//   +0x40 multi-hit = 0
void NormalizeFilter(std::uint8_t* filter, std::uint64_t mask) {
	if (!filter)
		return;
	*reinterpret_cast<std::uint64_t*>(filter + 8) = mask;
	// FireBullet: v25=-65536 @+0x32, v26=3840 @+0x36
	*reinterpret_cast<std::int32_t*>(filter + 0x32) = -65536;
	*reinterpret_cast<std::int16_t*>(filter + 0x36) = 3840;
	// Keep layer from InitFilter (a4) — only force multi-hit off
	filter[0x40] = 0;
	// Ensure skip-rewrite path: filter[0x37] left as InitFilter a5 (0 for vis)
}

bool PrepareAndCall(const Ray_t* ray, const Vector_t* start, const Vector_t* end,
                    void* filter, CGameTrace* out) {
	if (!g_ready)
		return false;
	// Soft-disable: probe every 16th call (was 64 — too slow to recover)
	if (g_traceDisabled.load(std::memory_order_relaxed)) {
		static std::atomic<int> s_probe{0};
		if ((s_probe.fetch_add(1) % 16) != 0)
			return false;
	}
	if (!g_traceShape || !Mem::IsUserPtr(reinterpret_cast<void*>(g_traceShape)))
		return false;
	if (!g_initFilter || !Mem::IsUserPtr(reinterpret_cast<void*>(g_initFilter)))
		return false;

	void* world = ResolveWorldArg();
	if (!world) {
		// Manager may lag one frame after map load — not fatal
		return false;
	}

	return SehTraceShape(world, ray, start, end, filter, out);
}

} // namespace

// Hit entity may be pawn, weapon, or attachment — walk owner chain + IsBasePlayer.
bool HitsTarget(void* hit, void* target) {
	constexpr std::uint32_t kOwnerOff = 0x520; // C_BaseEntity::m_hOwnerEntity fallback
	if (!hit || !target)
		return false;
	if (hit == target)
		return true;

	void* cur = hit;
	for (int depth = 0; depth < 6 && cur && Mem::ValidEntity(cur); ++depth) {
		if (cur == target)
			return true;

		auto* asBase = reinterpret_cast<C_BaseEntity*>(cur);
		__try {
			if (asBase->IsBasePlayer() && static_cast<void*>(asBase) == target)
				return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}

		CBaseHandle owner{};
		__try {
			owner = *reinterpret_cast<CBaseHandle*>(
				reinterpret_cast<std::uint8_t*>(cur) + kOwnerOff);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			break;
		}
		if (!owner.valid() || !I::GameEntity || !I::GameEntity->Instance)
			break;
		void* next = nullptr;
		__try {
			next = I::GameEntity->Instance->Get(owner);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			break;
		}
		if (!next || next == cur)
			break;
		cur = next;
	}
	return false;
}

bool Init() {
	g_ready = false;
	g_traceShape = nullptr;
	g_initFilter = nullptr;
	g_initTraceInfo = nullptr;
	g_getSurfaceData = nullptr;
	g_ppTraceMgr = nullptr;
	g_avStreak.store(0);
	g_traceDisabled.store(false);

	auto* pShape = FindClient(kPatTraceShape);
	if (!pShape)
		pShape = FindClient(kPatTraceShapeLoose);
	g_traceShape = reinterpret_cast<TraceShapeFn>(pShape);

	auto* pInitF = FindClient(kPatInitFilter);
	if (!pInitF)
		pInitF = FindClient(kPatInitFilterLoose);
	g_initFilter = reinterpret_cast<InitFilterFn>(pInitF);

	auto* pInitTr = FindClient(kPatInitTraceInfo);
	if (!pInitTr)
		pInitTr = FindClient("40 55 41 55 41 57 48 83 EC 30");
	g_initTraceInfo = reinterpret_cast<InitTraceInfoFn>(pInitTr);

	g_getSurfaceData = reinterpret_cast<GetSurfaceDataFn>(FindClient(kPatGetSurfaceData));

	const uintptr_t mgrSite = M::patternScan("client", kPatGameTraceMgr);
	if (mgrSite)
		g_ppTraceMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(mgrSite, 3));
	if (!g_ppTraceMgr) {
		const uintptr_t mgrLoose = M::patternScan("client", kPatGameTraceMgrLoose);
		if (mgrLoose)
			g_ppTraceMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(mgrLoose, 3));
	}
	if (!g_ppTraceMgr) {
		const uintptr_t physSite = M::patternScan("client", kPatPhysWorld);
		if (physSite)
			g_ppTraceMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(physSite, 3));
	}

	void* slot = (g_ppTraceMgr && Mem::IsReadable(g_ppTraceMgr, sizeof(void*))) ? *g_ppTraceMgr : nullptr;
	void* world = ResolveWorldArg();
	void* iface = (world && Mem::IsReadable(world, sizeof(void*))) ? *reinterpret_cast<void**>(world) : nullptr;

	Con::Info("Trace: shape=%p filter=%p global=%p slot=%p world=%p iface=%p",
		(void*)g_traceShape, (void*)g_initFilter, (void*)g_ppTraceMgr, slot, world, iface);

	if (!g_traceShape) Con::OffsetMiss("Trace::TraceShape");
	if (!g_initFilter) Con::OffsetMiss("Trace::InitFilter");
	if (!g_ppTraceMgr) Con::OffsetMiss("Trace::GameTraceManager");

	g_ready = g_traceShape && g_initFilter && g_ppTraceMgr;
	if (g_ready)
		Con::Ok("Trace system ready (pass *global, mask rewrite on)");
	else
		Con::Error("Trace system incomplete");
	return g_ready;
}

bool Ready() {
	return g_ready && !g_traceDisabled.load(std::memory_order_relaxed);
}

bool DidHit(const CGameTrace& tr) {
	// IDA TraceShape: *(float*)(tr+172) < 1.0 || *(BYTE*)(tr+187)
	const float f = tr.fraction();
	if (!std::isfinite(f))
		return false;
	return f < 1.0f || tr.startsolid();
}

bool TraceLine(const Vector_t& start, const Vector_t& end, void* skip, CGameTrace& out,
               std::uint64_t mask, int layer, std::uint16_t a5) {
	std::memset(out.raw, 0, sizeof(out.raw));
	if (!Mem::Finite(start.x) || !Mem::Finite(start.y) || !Mem::Finite(start.z))
		return false;
	if (!Mem::Finite(end.x) || !Mem::Finite(end.y) || !Mem::Finite(end.z))
		return false;
	// Degenerate / absurd rays — engine can fault inside phys
	const float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
	const float len2 = dx * dx + dy * dy + dz * dz;
	if (len2 < 1e-6f || len2 > 1e14f)
		return false;
	if (skip && !Mem::ValidEntity(skip))
		skip = nullptr;

	if (g_initTraceInfo && Mem::IsUserPtr(reinterpret_cast<void*>(g_initTraceInfo)))
		SehInitTraceInfo(&out);

	alignas(16) std::uint8_t filter[0x80]{};
	// IDA FireBullet: mask=0x1C3001/0x1C300B, layer=4. Multi-hit for world 0x8000.
	if (!SehInitFilter(filter, skip, mask, layer, static_cast<int>(a5)))
		return false;
	NormalizeFilter(filter, mask);

	Ray_t ray{};
	ray.set_line();
	// Must return false on AV — InitTraceInfo leaves fraction=1.0 which looks like clear LOS.
	return PrepareAndCall(&ray, &start, &end, filter, &out);
}

bool TraceHull(const Vector_t& start, const Vector_t& end,
               const Vector_t& mins, const Vector_t& maxs,
               void* skip, CGameTrace& out,
               std::uint64_t mask, int layer, std::uint16_t a5) {
	std::memset(out.raw, 0, sizeof(out.raw));
	if (!Mem::Finite(start.x) || !Mem::Finite(end.x))
		return false;
	if (skip && !Mem::ValidEntity(skip))
		skip = nullptr;

	if (g_initTraceInfo)
		SehInitTraceInfo(&out);

	alignas(16) std::uint8_t filter[0x80]{};
	if (!SehInitFilter(filter, skip, mask, layer, static_cast<int>(a5)))
		return false;
	NormalizeFilter(filter, mask);

	Ray_t ray{};
	ray.set_hull(mins, maxs);
	return PrepareAndCall(&ray, &start, &end, filter, &out);
}

bool IsVisible(const Vector_t& start, const Vector_t& end, void* skip, void* target,
               std::uint64_t mask, bool penetrate) {
	if (target && !Mem::ValidEntity(target))
		return false;
	// Allow probe while soft-disabled (Ready() false) — PrepareAndCall still samples
	if (!g_ready)
		return false;

	// Line first (FireBullet path). If only high-frac player hit / clear,
	// re-probe with a thin hull — static world often collides better on hull.
	CGameTrace tr{};
	bool ok = TraceLine(start, end, skip, tr, mask);
	if (!ok)
		return false;

	auto eval = [&](const CGameTrace& t, const char* tag) -> int {
		// return: 1=visible, 0=occluded, -1=need more
		const float frac = t.fraction();
		void* hit = t.hit_entity();
		const bool hitTarget = target && HitsTarget(hit, target);
		const bool solid = DidHit(t) && frac < 0.99f;
		(void)tag;
		if (hitTarget)
			return 1;
		if (!solid)
			return 1; // clear
		// solid non-target (player or world) → occluded unless penetrate
		if (penetrate && hit && hit != skip && hit != target) {
			Vector_t hitPos = t.endpos();
			const float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
			const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len > 1.f) {
				const float inv = 1.f / len;
				hitPos.x += dx * inv * 2.f;
				hitPos.y += dy * inv * 2.f;
				hitPos.z += dz * inv * 2.f;
				CGameTrace tr2{};
				if (TraceLine(hitPos, end, hit, tr2, mask)) {
					if (target && HitsTarget(tr2.hit_entity(), target))
						return 1;
					if (!DidHit(tr2) || tr2.fraction() >= 0.99f)
						return 1;
				}
			}
		}
		return 0;
	};

	int r = eval(tr, "vis");
	if (r == 1) {
		// Visible by line — but if we only grazed target at frac>~0.97,
		// confirm no earlier wall via thin hull (world brush catch).
		const float frac = tr.fraction();
		const bool hitTarget = target && HitsTarget(tr.hit_entity(), target);
		if (hitTarget || frac >= 0.97f) {
			CGameTrace th{};
			const Vector_t mins{ -0.5f, -0.5f, -0.5f };
			const Vector_t maxs{  0.5f,  0.5f,  0.5f };
			if (TraceHull(start, end, mins, maxs, skip, th, mask)) {
				const float hf = th.fraction();
				void* hh = th.hit_entity();
				const bool hullTgt = target && HitsTarget(hh, target);
				// Hull hit something before target that isn't the target → wall/prop
				if (DidHit(th) && hf < 0.97f && !hullTgt)
					return false;
			}
		}
		return true;
	}
	return false;
}

bool IsAnyVisible(const Vector_t& start, const Vector_t* ends, int count, void* skip,
                  void* target, std::uint64_t mask, bool penetrate) {
	if (!ends || count <= 0 || count > 64)
		return false;
	if (!Mem::IsReadable(ends, static_cast<std::size_t>(count) * sizeof(Vector_t)))
		return false;
	for (int i = 0; i < count; ++i) {
		const Vector_t& e = ends[i];
		if (!Mem::Finite(e.x) || !Mem::Finite(e.y) || !Mem::Finite(e.z))
			continue;
		if (IsVisible(start, e, skip, target, mask, penetrate))
			return true;
	}
	return false;
}

bool IsBodyVisible(const Vector_t& eye, const Vector_t* bones, int boneCount,
                   void* skip, void* target, std::uint64_t mask) {
	if (!bones || boneCount <= 0 || boneCount > 64 || !Mem::ValidEntity(target))
		return false;
	return IsAnyVisible(eye, bones, boneCount, skip, target, mask, false);
}

bool IsWorldVisible(const Vector_t& eye, const Vector_t* bones, int boneCount,
                    void* skip, std::uint64_t mask) {
	if (!bones || boneCount <= 0)
		return false;
	for (int i = 0; i < boneCount && i < 6; ++i) {
		CGameTrace tr{};
		if (!TraceLine(eye, bones[i], skip, tr, mask))
			continue;
		if (!DidHit(tr) || tr.fraction() > 0.97f)
			return true;
	}
	return false;
}

bool GetHitSurfaceData(const CGameTrace& tr, float& outPenetration, float& outHardness) {
	outPenetration = 0.f;
	outHardness = 0.f;
	if (!g_getSurfaceData)
		return false;
	void* surf = tr.surface();
	if (!surf)
		return false;
	void* data = nullptr;
	__try {
		data = g_getSurfaceData(surf);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!data || !Mem::IsReadable(data, 0x20))
		return false;
	__try {
		const float* f = reinterpret_cast<const float*>(data);
		if (std::isfinite(f[2]))
			outPenetration = f[2];
		if (std::isfinite(f[3]))
			outHardness = f[3];
		return std::isfinite(outPenetration) || std::isfinite(outHardness);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void* GetTraceWorld() {
	if (!g_ppTraceMgr)
		return nullptr;
	return *g_ppTraceMgr;
}

void* GetTraceShapeFn() {
	return reinterpret_cast<void*>(g_traceShape);
}

void* GetInitFilterFn() {
	return reinterpret_cast<void*>(g_initFilter);
}

} // namespace Trace
