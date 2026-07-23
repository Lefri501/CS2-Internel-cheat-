#include "hitmarker.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/math/vector/vector.h"
#include "../bones/bones.h"
#include "../hitsound/hitsound.h"
#include "../hitlog/hitlog.h"
#include "../tracers/tracers.h"
#include "../aim/aim_common.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/handle.h"
#include "../w2s/w2s.h"
#include "../../../../external/imgui/imgui.h"

#include <Windows.h>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cstdint>

namespace {
	constexpr int kMaxMarks = 24;
	constexpr int kMaxFloats = 32;
	constexpr float kScreenLife = 0.42f;
	constexpr float kWorldLife = 1.15f;

	struct Mark {
		bool active = false;
		bool world = false;
		bool kill = false;
		bool head = false;
		float born = 0.f;
		float life = kScreenLife;
		float size = 12.f;
		Vector_t worldPos{};
		int damage = 0;
	};

	struct FloatDmg {
		bool active = false;
		bool kill = false;
		bool head = false;
		float born = 0.f;
		float life = 1.f;
		Vector_t worldPos{};
		int damage = 0;
	};

	Mark g_marks[kMaxMarks]{};
	int g_write = 0;
	FloatDmg g_floats[kMaxFloats]{};
	int g_floatWrite = 0;

	// Screen-center COD pulse (no world pos)
	float g_screenBorn = -1.f;
	float g_screenLife = kScreenLife;
	bool g_screenKill = false;
	bool g_screenHead = false;
	int g_screenDmg = 0;

	using FnEvtGetName = const char* (__fastcall*)(void*);
	using FnEvtGetController = void* (__fastcall*)(void*, void*);
	// Pattern differs from GetString (no early [rdx] hash load) — takes const char* name
	using FnEvtGetInt64 = std::int64_t (__fastcall*)(void*, const char*, unsigned int);

	FnEvtGetName g_getName = nullptr;
	FnEvtGetController g_getController = nullptr;
	FnEvtGetInt64 g_getInt64 = nullptr;
	uintptr_t g_reportHit = 0;
	bool g_ready = false;

	struct EvtToken {
		std::uint32_t hash = 0;
		std::uint32_t pad = 0xFFFFFFFFu;
		const char* name = nullptr;
	};

	// Pending world markers — resolve bones on Draw, never on FireEvent thread.
	// Store pawn handle (not controller*) so online entity recycle cannot point mid-map.
	struct PendingWorld {
		bool active = false;
		std::uint32_t pawnHandle = 0; // CBaseHandle raw
		int hitgroup = 0;
		int damage = 0;
		bool kill = false;
		bool head = false;
		int tries = 0; // wait frames for bullet_impact / bones
		bool havePos = false; // freeze only after impact or ray pin (not last-hit cache)
		Vector_t worldPos{};
		float queuedAt = 0.f; // ImGui time when hurt/death queued
	};
	constexpr int kMaxPending = 8;
	PendingWorld g_pending[kMaxPending]{};
	int g_pendingWrite = 0;

	// Last good hit pos per pawn — kill shot bones/ragdoll online = garbage mid-map.
	struct LastHitPos {
		std::uint32_t pawnHandle = 0;
		Vector_t pos{};
		float time = 0.f;
		bool valid = false;
	};
	constexpr int kMaxLastHit = 16;
	LastHitPos g_lastHit[kMaxLastHit]{};
	int g_lastHitWrite = 0;

	// Recent local bullet_impact points — true server impact (best world mark).
	struct ImpactPt {
		Vector_t pos{};
		float time = 0.f;
		bool valid = false;
	};
	constexpr int kMaxImpacts = 12;
	ImpactPt g_impacts[kMaxImpacts]{};
	int g_impactWrite = 0;

	// Last local fire ray (AF/TR/aim) — pin mark before bullet_impact, match impact to ray.
	struct LastFireRay {
		Vector_t eye{};
		Vector_t dir{};
		float time = 0.f;
		bool valid = false;
	};
	LastFireRay g_lastFire{};

	float Now() {
		return static_cast<float>(ImGui::GetTime());
	}

	void RememberHitPos(std::uint32_t ph, const Vector_t& pos) {
		if (!ph || !Bones::IsValidPos(pos))
			return;
		for (int i = 0; i < kMaxLastHit; ++i) {
			if (g_lastHit[i].valid && g_lastHit[i].pawnHandle == ph) {
				g_lastHit[i].pos = pos;
				g_lastHit[i].time = Now();
				return;
			}
		}
		LastHitPos& s = g_lastHit[g_lastHitWrite];
		g_lastHitWrite = (g_lastHitWrite + 1) % kMaxLastHit;
		s.pawnHandle = ph;
		s.pos = pos;
		s.time = Now();
		s.valid = true;
	}

	bool LookupHitPos(std::uint32_t ph, Vector_t& out, float maxAge = 2.5f) {
		if (!ph)
			return false;
		const float now = Now();
		for (int i = 0; i < kMaxLastHit; ++i) {
			if (!g_lastHit[i].valid || g_lastHit[i].pawnHandle != ph)
				continue;
			if ((now - g_lastHit[i].time) > maxAge)
				continue;
			if (!Bones::IsValidPos(g_lastHit[i].pos))
				continue;
			out = g_lastHit[i].pos;
			return true;
		}
		return false;
	}

	bool PawnIsDeadOrDying(C_CSPlayerPawn* pawn) {
		if (!pawn)
			return true;
		int hp = 0;
		std::uint8_t life = 1;
		__try {
			hp = pawn->m_iHealth();
			life = pawn->m_lifeState();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;
		}
		return hp <= 0 || life != 0;
	}

	std::uint32_t Murmur2Token(const char* name) {
		if (!name || !*name) return 0;
		char buf[128]{};
		int len = 0;
		for (; name[len] && len < 127; ++len) {
			const unsigned char c = static_cast<unsigned char>(name[len]);
			buf[len] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
		}
		const std::uint32_t m = 0x5bd1e995u;
		const int r = 24;
		std::uint32_t h = 0x31415926u ^ static_cast<std::uint32_t>(len);
		const auto* d = reinterpret_cast<const unsigned char*>(buf);
		int n = len;
		while (n >= 4) {
			std::uint32_t k = 0;
			std::memcpy(&k, d, 4);
			k *= m; k ^= k >> r; k *= m;
			h *= m; h ^= k; d += 4; n -= 4;
		}
		switch (n) {
		case 3: h ^= d[2] << 16; [[fallthrough]];
		case 2: h ^= d[1] << 8;  [[fallthrough]];
		case 1: h ^= d[0]; h *= m;
		}
		h ^= h >> 13; h *= m; h ^= h >> 15;
		return h;
	}

	EvtToken MakeTok(const char* s) {
		EvtToken t{};
		t.name = s;
		t.hash = Murmur2Token(s);
		return t;
	}

	void PushWorld(const Vector_t& pos, bool kill, bool head, int dmg) {
		Mark& m = g_marks[g_write];
		g_write = (g_write + 1) % kMaxMarks;
		m.active = true;
		m.world = true;
		m.kill = kill;
		m.head = head;
		m.born = Now();
		m.life = kWorldLife * std::clamp(Config::hitmarker_duration, 0.25f, 2.5f);
		m.size = Config::hitmarker_world_size;
		m.worldPos = pos;
		m.damage = dmg;
	}

	void PushFloatDamage(const Vector_t& pos, bool kill, bool head, int dmg) {
		if (!Config::float_damage || dmg <= 0)
			return;
		FloatDmg& f = g_floats[g_floatWrite];
		g_floatWrite = (g_floatWrite + 1) % kMaxFloats;
		f.active = true;
		f.kill = kill;
		f.head = head;
		f.born = Now();
		f.life = std::clamp(Config::float_damage_duration, 0.3f, 3.f);
		f.worldPos = pos;
		f.worldPos.z += 4.f;
		f.damage = dmg;
	}

	void PulseScreen(bool kill, bool head, int dmg) {
		g_screenBorn = Now();
		g_screenLife = kScreenLife * std::clamp(Config::hitmarker_duration, 0.25f, 2.5f);
		g_screenKill = kill;
		g_screenHead = head;
		g_screenDmg = dmg;
	}

	// HITGROUP → studio set. Arms/legs include BOTH sides — side is chosen by aim ray
	// (engine hitgroup L/R is inverted vs player model on some builds).
	void StudioListForHitgroup(int hg, const int*& list, int& count) {
		static const int head[] = { 0 };
		static const int neck[] = { 1 };
		static const int chest[] = { 6, 4, 5 };      // upper, chest, lower
		static const int stomach[] = { 3, 2 };       // stomach, pelvis
		// Both arms / both legs — pick nearest to shot, never trust L/R hitgroup alone
		static const int arms[] = {
			17, 18, 14,  // L upper / forearm / hand
			15, 16, 13   // R upper / forearm / hand
		};
		static const int legs[] = {
			8, 10, 12,   // L thigh / calf / foot
			7, 9, 11     // R thigh / calf / foot
		};
		static const int generic[] = {
			0, 6, 4, 5, 3, 2, 17, 18, 14, 15, 16, 13, 8, 10, 12, 7, 9, 11
		};

		switch (hg) {
		case 1: list = head; count = 1; break;
		case 8: list = neck; count = 1; break;
		case 2: list = chest; count = 3; break;
		case 3: list = stomach; count = 2; break;
		case 4:
		case 5: list = arms; count = 6; break;
		case 6:
		case 7: list = legs; count = 6; break;
		default: list = generic; count = 18; break;
		}
	}

	void SkeletonSlotsForHitgroup(int hg, const int*& slots, int& count) {
		static const int head[] = { Bones::S_HEAD, Bones::S_NECK };
		static const int neck[] = { Bones::S_NECK, Bones::S_HEAD, Bones::S_SPINE3 };
		static const int chest[] = { Bones::S_SPINE3, Bones::S_SPINE2, Bones::S_SPINE1 };
		static const int stomach[] = { Bones::S_SPINE1, Bones::S_SPINE0, Bones::S_PELVIS };
		static const int arms[] = {
			Bones::S_ARM_U_L, Bones::S_ARM_L_L, Bones::S_HAND_L, Bones::S_CLAV_L,
			Bones::S_ARM_U_R, Bones::S_ARM_L_R, Bones::S_HAND_R, Bones::S_CLAV_R
		};
		static const int legs[] = {
			Bones::S_LEG_U_L, Bones::S_LEG_L_L, Bones::S_ANKLE_L,
			Bones::S_LEG_U_R, Bones::S_LEG_L_R, Bones::S_ANKLE_R
		};
		static const int body[] = { Bones::S_SPINE2, Bones::S_PELVIS, Bones::S_HEAD };

		switch (hg) {
		case 1: slots = head; count = 2; break;
		case 8: slots = neck; count = 3; break;
		case 2: slots = chest; count = 3; break;
		case 3: slots = stomach; count = 3; break;
		case 4:
		case 5: slots = arms; count = 8; break;
		case 6:
		case 7: slots = legs; count = 6; break;
		default: slots = body; count = 3; break;
		}
	}

	void PushImpact(const Vector_t& pos) {
		if (!Bones::IsValidPos(pos))
			return;
		ImpactPt& e = g_impacts[g_impactWrite];
		g_impactWrite = (g_impactWrite + 1) % kMaxImpacts;
		e.pos = pos;
		e.time = Now();
		e.valid = true;
	}

	// Score impact: near hitgroup + on last-fire ray (exact pin when multi-impacts).
	float ImpactScore(const Vector_t& p, const Vector_t& anchor, bool haveRay,
		const Vector_t& eye, const Vector_t& dir)
	{
		const float dx = p.x - anchor.x, dy = p.y - anchor.y, dz = p.z - anchor.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (!haveRay)
			return d2;
		// Lateral distance from fire ray (smaller = better)
		const Vector_t w{ p.x - eye.x, p.y - eye.y, p.z - eye.z };
		const float t = w.x * dir.x + w.y * dir.y + w.z * dir.z;
		if (t < 0.f)
			return d2 + 1.0e6f;
		const Vector_t onRay{
			eye.x + dir.x * t, eye.y + dir.y * t, eye.z + dir.z * t
		};
		const float lx = p.x - onRay.x, ly = p.y - onRay.y, lz = p.z - onRay.z;
		const float lat2 = lx * lx + ly * ly + lz * lz;
		// Prefer ray-aligned impact over mere body-center nearest (wrong wall impact).
		return lat2 * 4.f + d2 * 0.15f;
	}

	// Anchor for impact match without pawn (kill / entity gone).
	bool AnchorFromCache(std::uint32_t ph, int hitgroup, Vector_t& anchor) {
		// Last known hit on this pawn is a good body region estimate
		if (LookupHitPos(ph, anchor, 3.f))
			return true;
		(void)hitgroup;
		return false;
	}

	// Forward decl — defined below FindImpactNear
	bool LocalFireRay(Vector_t& eye, Vector_t& dir);

	// Nearest recent local impact near victim (prefer hitgroup + fire ray).
	// minTime: ignore impacts before this shot (stops kill pin on prior limb hit).
	// ph: optional handle for dead/missing pawn (uses last-hit as body region only).
	// preferCache: kill shot on dead pawn — use last-hit cache as primary anchor
	//   (ragdoll origin drops/shifts, making capsule/origin anchors unreliable).
	bool FindImpactNear(std::uint32_t ph, C_CSPlayerPawn* pawn, int hitgroup, Vector_t& out,
		float maxAge = 0.45f, float maxDist = 96.f, float minTime = -1.f, bool preferCache = false)
	{
		const float now = Now();
		// Default minTime = this fire (or slightly before) so multi-hit doesn't reuse old impact
		if (minTime < 0.f) {
			if (g_lastFire.valid && (now - g_lastFire.time) <= 0.55f)
				minTime = g_lastFire.time - 0.04f;
			else
				minTime = now - maxAge;
		}

		Vector_t anchor{};
		bool haveAnchor = false;

		// Kill on dead/dying pawn: ragdoll origin drops, capsules go garbage.
		// Last-hit cache (from prior hits while alive) is the true standing position.
		if (preferCache && ph && LookupHitPos(ph, anchor, 3.f))
			haveAnchor = true;

		// When preferCache is set (dead pawn kill) and no cache exists (one-shot kill),
		// skip pawn-based anchor entirely — ragdoll capsules/origin are unreliable.
		// Fire-ray-only path (below) handles this case more accurately.
		if (!haveAnchor && !preferCache && pawn && Mem::ValidEntity(pawn)) {
			const int* studioList = nullptr;
			int studioCount = 0;
			StudioListForHitgroup(hitgroup, studioList, studioCount);
			if (studioList && studioCount > 0) {
				for (int i = 0; i < studioCount && !haveAnchor; ++i) {
					Bones::Capsule cap{};
					if (Bones::GetStudioCapsule(pawn, studioList[i], cap) && cap.ok && Bones::IsValidPos(cap.center)) {
						anchor = cap.center;
						haveAnchor = true;
					}
				}
			}
			if (!haveAnchor) {
				const int hb = Bones::HitgroupToHitbox(hitgroup);
				if (hb >= 0 && Bones::GetHitboxPoint(pawn, hb, anchor) && Bones::IsValidPos(anchor))
					haveAnchor = true;
			}
			if (!haveAnchor && Bones::GetOrigin(pawn, anchor) && Bones::IsValidPos(anchor)) {
				// Hitgroup-appropriate height so impact matching anchors near correct body part.
				float zOff = 48.f;
				switch (hitgroup) {
				case 1: zOff = 72.f; break;
				case 8: zOff = 64.f; break;
				case 2: zOff = 52.f; break;
				case 3: zOff = 40.f; break;
				case 4: case 5: zOff = 50.f; break;
				case 6: case 7: zOff = 18.f; break;
				default: zOff = 48.f; break;
				}
				anchor.z += zOff;
				haveAnchor = true;
			}
		}
		// Last-hit is region hint only — never use its coords as the mark itself here
		if (!haveAnchor && ph)
			haveAnchor = AnchorFromCache(ph, hitgroup, anchor);

		Vector_t fEye{}, fDir{};
		const bool haveRay = LocalFireRay(fEye, fDir);
		if (!haveAnchor && !haveRay)
			return false;

		const float maxD2 = maxDist * maxDist;

		float bestScore = 1.0e20f;
		Vector_t best{};
		bool found = false;
		for (int i = 0; i < kMaxImpacts; ++i) {
			if (!g_impacts[i].valid)
				continue;
			if (g_impacts[i].time < minTime)
				continue;
			if ((now - g_impacts[i].time) > maxAge)
				continue;
			const Vector_t& p = g_impacts[i].pos;
			if (haveAnchor) {
				const float dx = p.x - anchor.x, dy = p.y - anchor.y, dz = p.z - anchor.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 > maxD2)
					continue;
				// Prefer newer + ray-aligned impacts (kill shot after body shot)
				float sc = ImpactScore(p, anchor, haveRay, fEye, fDir);
				sc -= (g_impacts[i].time - minTime) * 8.f; // slight bias to freshest
				if (sc >= bestScore)
					continue;
				bestScore = sc;
				best = p;
				found = true;
			} else {
				const Vector_t w{ p.x - fEye.x, p.y - fEye.y, p.z - fEye.z };
				const float t = w.x * fDir.x + w.y * fDir.y + w.z * fDir.z;
				if (t < 0.f || t > 8192.f)
					continue;
				const Vector_t onRay{
					fEye.x + fDir.x * t, fEye.y + fDir.y * t, fEye.z + fDir.z * t
				};
				const float lx = p.x - onRay.x, ly = p.y - onRay.y, lz = p.z - onRay.z;
				const float lat2 = lx * lx + ly * ly + lz * lz;
				if (lat2 > (40.f * 40.f))
					continue;
				float sc = lat2 - (g_impacts[i].time - minTime) * 8.f;
				if (sc >= bestScore)
					continue;
				bestScore = sc;
				best = p;
				found = true;
			}
		}
		if (!found)
			return false;
		out = best;
		return true;
	}

	// Local fire ray: prefer NoteLastFire (seed silent ang), else view+punch.
	bool LocalFireRay(Vector_t& eye, Vector_t& dir) {
		const float now = Now();
		if (g_lastFire.valid && (now - g_lastFire.time) <= 0.30f
			&& Bones::IsValidPos(g_lastFire.eye)) {
			const float dl = g_lastFire.dir.Length();
			if (dl > 1e-4f) {
				eye = g_lastFire.eye;
				dir = g_lastFire.dir;
				dir.x /= dl; dir.y /= dl; dir.z /= dl;
				return true;
			}
		}
		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local || !Mem::ValidEntity(local))
			return false;
		eye = Bones::GetEyePos(local);
		if (!Bones::IsValidPos(eye))
			return false;
		QAngle_t ang{};
		if (!AimCommon::GetViewAngles(ang) || !ang.IsValid())
			return false;
		// Note: punch may already include this shot — ray is fallback only
		QAngle_t punch{};
		if (AimCommon::GetFirePunch(local, punch) && punch.IsValid()) {
			ang.x += punch.x;
			ang.y += punch.y;
		}
		if (ang.x > 89.f) ang.x = 89.f;
		if (ang.x < -89.f) ang.x = -89.f;
		ang.z = 0.f;
		ang.ToDirections(&dir, nullptr, nullptr);
		const float len = dir.Length();
		if (len < 1e-4f)
			return false;
		dir.x /= len; dir.y /= len; dir.z /= len;
		return true;
	}

	// Closest surface point on capsule to ray (when spread misses exact capsule).
	bool ClosestOnCapsule(const Vector_t& eye, const Vector_t& dir, const Bones::Capsule& cap, Vector_t& out) {
		if (!cap.ok)
			return false;
		const Vector_t axis = cap.maxs - cap.mins;
		const float axisLen = axis.Length();
		Vector_t aN{ 0.f, 0.f, 1.f };
		if (axisLen > 1e-3f) {
			aN.x = axis.x / axisLen;
			aN.y = axis.y / axisLen;
			aN.z = axis.z / axisLen;
		}
		// Closest point on infinite ray to capsule segment mid, then clamp to capsule
		const Vector_t mid = cap.center;
		const Vector_t w = mid - eye;
		const float t = w.x * dir.x + w.y * dir.y + w.z * dir.z;
		if (t < 0.f)
			return false;
		const Vector_t onRay{
			eye.x + dir.x * t,
			eye.y + dir.y * t,
			eye.z + dir.z * t
		};
		// Project onto capsule axis segment
		Vector_t rel{ onRay.x - cap.mins.x, onRay.y - cap.mins.y, onRay.z - cap.mins.z };
		float u = (axisLen > 1e-3f) ? ((rel.x * aN.x + rel.y * aN.y + rel.z * aN.z) / axisLen) : 0.5f;
		u = std::clamp(u, 0.f, 1.f);
		const Vector_t onAxis{
			cap.mins.x + axis.x * u,
			cap.mins.y + axis.y * u,
			cap.mins.z + axis.z * u
		};
		Vector_t lat{ onRay.x - onAxis.x, onRay.y - onAxis.y, onRay.z - onAxis.z };
		const float latLen = lat.Length();
		const float r = (std::max)(cap.radius, 0.5f);
		if (latLen < 1e-4f) {
			out = onAxis;
			return Bones::IsValidPos(out);
		}
		// Pull to capsule surface (or inside if already in)
		const float scale = (latLen > r) ? (r / latLen) : 1.f;
		out = {
			onAxis.x + lat.x * scale,
			onAxis.y + lat.y * scale,
			onAxis.z + lat.z * scale
		};
		return Bones::IsValidPos(out);
	}

	// Pin marker via ray∩capsule / bones. Impact matching is owned by DrainPendingWorld
	// (shot-time filter) — do not re-pick arbitrary recent impacts here (kills wrong limb).
	// originOnly: kill/ragdoll — no studio bones (online = garbage coords).
	bool ResolveHitWorld(C_CSPlayerPawn* pawn, int hitgroup, Vector_t& out, bool originOnly = false) {
		if (!pawn || !Mem::ValidEntity(pawn))
			return false;

		if (originOnly || PawnIsDeadOrDying(pawn)) {
			// Caller already waited for bullet_impact. Last resort only.
			// Use hitgroup-appropriate height so headshot kills mark at head, not chest.
			if (Bones::GetOrigin(pawn, out) && Bones::IsValidPos(out)) {
				float zOff = 48.f; // default chest
				switch (hitgroup) {
				case 1: zOff = 72.f; break;  // head
				case 8: zOff = 64.f; break;  // neck
				case 2: zOff = 52.f; break;  // chest
				case 3: zOff = 40.f; break;  // stomach
				case 4: case 5: zOff = 50.f; break; // arms
				case 6: case 7: zOff = 18.f; break; // legs
				default: zOff = 48.f; break;
				}
				out.z += zOff;
				return true;
			}
			return false;
		}

		// Impact matching is Drain-only (shot-time filter). Here: ray ∩ hitgroup.

		Vector_t eye{}, dir{};
		const bool haveRay = LocalFireRay(eye, dir);

		const int* studioList = nullptr;
		int studioCount = 0;
		StudioListForHitgroup(hitgroup, studioList, studioCount);
		const int hb = Bones::HitgroupToHitbox(hitgroup);

		// 1) Ray ∩ Config hitbox group (true impact along aim)
		if (haveRay && hb >= 0) {
			float t = 0.f;
			Vector_t pt{};
			if (Bones::RayHitsConfiguredHitbox(pawn, hb, eye, dir, 1.08f, t, pt, 1.f)
				&& Bones::IsValidPos(pt)) {
				out = pt;
				return true;
			}
		}

		// 2) Ray ∩ each studio capsule for hitgroup (L/R limbs, neck, etc.)
		if (haveRay && studioList && studioCount > 0) {
			float bestT = 1.0e12f;
			Vector_t bestPt{};
			bool found = false;
			for (int i = 0; i < studioCount; ++i) {
				Bones::Capsule cap{};
				if (!Bones::GetStudioCapsule(pawn, studioList[i], cap) || !cap.ok)
					continue;
				float t = 0.f;
				Vector_t pt{};
				if (!Bones::RayHitsCapsule(eye, dir, cap, 1.08f, t, pt, 1.f))
					continue;
				if (t >= bestT || !Bones::IsValidPos(pt))
					continue;
				bestT = t;
				bestPt = pt;
				found = true;
			}
			if (found) {
				out = bestPt;
				return true;
			}

			// 3) Spread miss: closest surface on hitgroup capsules to aim ray
			float bestDist2 = 1.0e20f;
			Vector_t bestSurf{};
			bool haveSurf = false;
			for (int i = 0; i < studioCount; ++i) {
				Bones::Capsule cap{};
				if (!Bones::GetStudioCapsule(pawn, studioList[i], cap) || !cap.ok)
					continue;
				Vector_t surf{};
				if (!ClosestOnCapsule(eye, dir, cap, surf))
					continue;
				const Vector_t d{ surf.x - eye.x, surf.y - eye.y, surf.z - eye.z };
				// Prefer point near ray (lateral error)
				const float along = d.x * dir.x + d.y * dir.y + d.z * dir.z;
				if (along < 0.f)
					continue;
				const Vector_t onRay{ eye.x + dir.x * along, eye.y + dir.y * along, eye.z + dir.z * along };
				const float lx = surf.x - onRay.x, ly = surf.y - onRay.y, lz = surf.z - onRay.z;
				const float dist2 = lx * lx + ly * ly + lz * lz;
				if (dist2 >= bestDist2)
					continue;
				bestDist2 = dist2;
				bestSurf = surf;
				haveSurf = true;
			}
			// Only accept if reasonably near ray (avoid opposite limb)
			if (haveSurf && bestDist2 < (48.f * 48.f)) {
				out = bestSurf;
				return true;
			}
		}

		// 4) Studio capsule center for hitgroup (first valid)
		if (studioList && studioCount > 0) {
			for (int i = 0; i < studioCount; ++i) {
				Bones::Capsule cap{};
				if (!Bones::GetStudioCapsule(pawn, studioList[i], cap) || !cap.ok)
					continue;
				if (Bones::IsValidPos(cap.center)) {
					out = cap.center;
					return true;
				}
			}
		}

		// 5) Skeleton slots for that limb
		Vector_t slots[Bones::S_COUNT]{};
		bool valid[Bones::S_COUNT]{};
		if (Bones::CollectSkeletonPoints(pawn, slots, valid, /*forceUpdate=*/false) > 0) {
			const int* sk = nullptr;
			int skN = 0;
			SkeletonSlotsForHitgroup(hitgroup, sk, skN);
			if (sk && skN > 0) {
				for (int i = 0; i < skN; ++i) {
					const int s = sk[i];
					if (s >= 0 && s < Bones::S_COUNT && valid[s] && Bones::IsValidPos(slots[s])) {
						out = slots[s];
						return true;
					}
				}
			}
		}

		// 6) Hitbox group center
		if (hb >= 0 && Bones::GetHitboxPoint(pawn, hb, out) && Bones::IsValidPos(out))
			return true;

		// 7) Origin + chest height
		if (Bones::GetOrigin(pawn, out) && Bones::IsValidPos(out)) {
			out.z += 48.f;
			return true;
		}
		return false;
	}

	std::uint32_t PawnHandleFromController(void* victimCtrl) {
		if (!victimCtrl || !Mem::ValidEntity(victimCtrl) || !I::GameEntity || !I::GameEntity->Instance)
			return 0;
		std::uint32_t raw = 0;
		__try {
			auto* ctrl = reinterpret_cast<CCSPlayerController*>(victimCtrl);
			CBaseHandle h = ctrl->m_hPlayerPawn();
			if (!h.valid())
				h = ctrl->m_hPawn();
			if (h.valid())
				raw = h.raw();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
		return raw;
	}

	C_CSPlayerPawn* PawnFromHandleRaw(std::uint32_t raw) {
		if (!raw || raw == INVALID_EHANDLE_INDEX || !I::GameEntity || !I::GameEntity->Instance)
			return nullptr;
		C_CSPlayerPawn* pawn = nullptr;
		__try {
			CBaseHandle h(
				static_cast<int>(raw & ENT_ENTRY_MASK),
				static_cast<int>(raw >> NUM_SERIAL_NUM_SHIFT_BITS));
			if (!h.valid())
				return nullptr;
			pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(h);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
		if (!pawn || !Mem::ValidEntity(pawn))
			return nullptr;
		return pawn;
	}

	void EmitWorldMark(const Vector_t& pos, bool kill, bool head, int dmg, std::uint32_t ph) {
		if (!Bones::IsValidPos(pos))
			return;
		// Always cache real impact pin (kill shot included) for future anchors only
		if (ph)
			RememberHitPos(ph, pos);
		if (Config::hitmarker && Config::hitmarker_world)
			PushWorld(pos, kill, head, dmg);
		PushFloatDamage(pos, kill, head, dmg);
	}

	void ClearPending(PendingWorld& p) {
		p.active = false;
		p.pawnHandle = 0;
		p.havePos = false;
		p.tries = 0;
		p.queuedAt = 0.f;
	}

	void QueueWorld(void* victimCtrl, int hitgroup, int dmg, bool kill, bool head) {
		const std::uint32_t ph = PawnHandleFromController(victimCtrl);
		if (!ph)
			return;

		// KILL SHOT: bullet_impact fires BEFORE player_hurt. The impact is already
		// in the buffer. The NEWEST impact along the fire ray IS the kill shot's
		// exact landing point — use it directly (no anchor guessing).
		if (kill) {
			const float now = Now();
			const float minT = now - 0.6f; // impact up to 600ms ago

			// 1) Best path: newest bullet_impact along fire ray = exact hit pos
			Vector_t fEye{}, fDir{};
			if (LocalFireRay(fEye, fDir)) {
				float newestTime = -1.f;
				Vector_t newestPos{};
				bool found = false;
				for (int i = 0; i < kMaxImpacts; ++i) {
					if (!g_impacts[i].valid)
						continue;
					if (g_impacts[i].time < minT)
						continue;
					if ((now - g_impacts[i].time) > 0.6f)
						continue;
					const Vector_t& p = g_impacts[i].pos;
					const Vector_t w{ p.x - fEye.x, p.y - fEye.y, p.z - fEye.z };
					const float t = w.x * fDir.x + w.y * fDir.y + w.z * fDir.z;
					if (t < 0.f || t > 8192.f)
						continue;
					const Vector_t onRay{
						fEye.x + fDir.x * t, fEye.y + fDir.y * t, fEye.z + fDir.z * t
					};
					const float lx = p.x - onRay.x, ly = p.y - onRay.y, lz = p.z - onRay.z;
					const float lat2 = lx * lx + ly * ly + lz * lz;
					if (lat2 > (60.f * 60.f))
						continue;
					// Take the NEWEST impact on ray — that's the kill shot
					if (g_impacts[i].time > newestTime) {
						newestTime = g_impacts[i].time;
						newestPos = p;
						found = true;
					}
				}
				if (found) {
					EmitWorldMark(newestPos, true, head, dmg, ph);
					for (int i = 0; i < kMaxPending; ++i) {
						if (g_pending[i].active && g_pending[i].pawnHandle == ph)
							ClearPending(g_pending[i]);
					}
					return;
				}
			}

			// 2) Fallback: anchor-based matching (fire ray unavailable)
			Vector_t killPos{};
			C_CSPlayerPawn* pawn = PawnFromHandleRaw(ph);
			const bool dead = PawnIsDeadOrDying(pawn);
			if (FindImpactNear(ph, pawn, hitgroup, killPos, 0.6f, 130.f, minT, /*preferCache=*/dead)) {
				EmitWorldMark(killPos, true, head, dmg, ph);
				for (int i = 0; i < kMaxPending; ++i) {
					if (g_pending[i].active && g_pending[i].pawnHandle == ph)
						ClearPending(g_pending[i]);
				}
				return;
			}
			// Impact not found yet — fall through to pending (DrainPendingWorld will wait)
		}

		// Merge into existing pending for same pawn (hurt → death same frame)
		for (int i = 0; i < kMaxPending; ++i) {
			PendingWorld& e = g_pending[i];
			if (!e.active || e.pawnHandle != ph)
				continue;
			e.hitgroup = hitgroup;
			e.damage = dmg;
			e.kill = e.kill || kill;
			e.head = e.head || head;
			// Never keep a stale freeze from an older non-lethal — re-resolve this shot
			e.havePos = false;
			e.tries = 0;
			e.queuedAt = Now();
			return;
		}
		PendingWorld& p = g_pending[g_pendingWrite];
		g_pendingWrite = (g_pendingWrite + 1) % kMaxPending;
		p.active = true;
		p.pawnHandle = ph;
		p.hitgroup = hitgroup;
		p.damage = dmg;
		p.kill = kill;
		p.head = head;
		p.tries = 0;
		p.havePos = false;
		p.worldPos = Vector_t{};
		p.queuedAt = Now();
	}

	void DrainPendingWorld() {
		const float now = Now();
		for (int i = 0; i < kMaxPending; ++i) {
			PendingWorld& p = g_pending[i];
			if (!p.active)
				continue;

			// Frozen only after this shot's impact/ray pin (never last non-lethal cache)
			if (p.havePos && Bones::IsValidPos(p.worldPos)) {
				EmitWorldMark(p.worldPos, p.kill, p.head, p.damage, p.pawnHandle);
				ClearPending(p);
				continue;
			}

			const std::uint32_t ph = p.pawnHandle;
			Vector_t pos{};
			C_CSPlayerPawn* pawn = PawnFromHandleRaw(ph);

			// Impacts for THIS shot: prefer fire time (impact often arrives before hurt).
			// Do NOT use max(queuedAt, fire) — that drops impacts between fire and hurt.
			float shotMinT = -1.f;
			if (g_lastFire.valid && (now - g_lastFire.time) <= 0.55f)
				shotMinT = g_lastFire.time - 0.05f;
			else if (p.queuedAt > 0.f)
				shotMinT = p.queuedAt - 0.12f;

			// 1) Server bullet_impact — true landing point (kill + non-kill)
			//    Kill: newest impact on fire ray = exact kill pos (no anchor guessing)
			if (p.kill) {
				Vector_t fEye{}, fDir{};
				if (LocalFireRay(fEye, fDir)) {
					float newestTime = -1.f;
					Vector_t newestPos{};
					bool found = false;
					const float minT = (shotMinT >= 0.f) ? shotMinT : (now - 0.55f);
					for (int j = 0; j < kMaxImpacts; ++j) {
						if (!g_impacts[j].valid || g_impacts[j].time < minT)
							continue;
						if ((now - g_impacts[j].time) > 0.55f)
							continue;
						const Vector_t& ip = g_impacts[j].pos;
						const Vector_t w{ ip.x - fEye.x, ip.y - fEye.y, ip.z - fEye.z };
						const float t = w.x * fDir.x + w.y * fDir.y + w.z * fDir.z;
						if (t < 0.f || t > 8192.f)
							continue;
						const Vector_t onRay{ fEye.x + fDir.x * t, fEye.y + fDir.y * t, fEye.z + fDir.z * t };
						const float lx = ip.x - onRay.x, ly = ip.y - onRay.y, lz = ip.z - onRay.z;
						if ((lx * lx + ly * ly + lz * lz) > (60.f * 60.f))
							continue;
						if (g_impacts[j].time > newestTime) {
							newestTime = g_impacts[j].time;
							newestPos = ip;
							found = true;
						}
					}
					if (found) {
						EmitWorldMark(newestPos, true, p.head, p.damage, ph);
						ClearPending(p);
						continue;
					}
				}
			}

			// 1b) Anchor-based impact matching (non-kill, or kill with no fire ray)
			//    Wider window on kill: impact often lands after player_hurt health=0
			const float impactAge = p.kill ? 0.55f : 0.40f;
			const float impactDist = p.kill ? 110.f : 96.f;
			const bool pawnDead = p.kill && (!pawn || PawnIsDeadOrDying(pawn));
			if (FindImpactNear(ph, pawn, p.hitgroup, pos, impactAge, impactDist, shotMinT, /*preferCache=*/pawnDead)) {
				EmitWorldMark(pos, p.kill, p.head, p.damage, ph);
				ClearPending(p);
				continue;
			}

			// 2) Wait for impact a few frames (event order: hurt often before impact)
			//    Kill MUST wait — last-hit cache is wrong body part for the kill shot.
			const int waitImpactFrames = p.kill ? 6 : 3;
			if (p.tries < waitImpactFrames) {
				++p.tries;
				continue;
			}

			// 3) No impact yet / entity gone
			if (!pawn) {
				// Keep waiting a bit longer for late bullet_impact (no body to raycast)
				if (p.tries < 10) {
					++p.tries;
					continue;
				}
				// Non-kill only: very fresh same-shot pin. Kill: drop if no impact (avoid wrong limb).
				if (!p.kill && LookupHitPos(ph, pos, 0.25f))
					EmitWorldMark(pos, false, p.head, p.damage, ph);
				ClearPending(p);
				continue;
			}

			const bool dead = PawnIsDeadOrDying(pawn);

			// 4) Kill + dead: keep waiting for this shot's bullet_impact.
			//    Never emit last non-lethal limb. Fire-ray pin after long wait.
			if (p.kill && dead) {
				if (p.tries < 12) {
					++p.tries;
					continue;
				}
				// Fire-ray fallback: project ray to pawn distance + hitgroup Z.
				// Prefer last-hit cache (living position) over ragdoll origin (dropped).
				bool rayPinned = false;
				Vector_t fEye{}, fDir{};
				if (LocalFireRay(fEye, fDir)) {
					Vector_t cachedPos{};
					const bool haveCached = LookupHitPos(ph, cachedPos, 3.f);
					Vector_t org{};
					if (haveCached || (Bones::GetOrigin(pawn, org) && Bones::IsValidPos(org))) {
						const Vector_t& base = haveCached ? cachedPos : org;
						// Project ray to pawn distance
						const Vector_t w{ base.x - fEye.x, base.y - fEye.y, base.z - fEye.z };
						const float t = w.x * fDir.x + w.y * fDir.y + w.z * fDir.z;
						if (t > 0.f) {
							Vector_t onRay{
								fEye.x + fDir.x * t,
								fEye.y + fDir.y * t,
								fEye.z + fDir.z * t
							};
							// When no cache, override Z from (ragdoll) origin + hitgroup height.
							// With cache, ray Z is already at correct living height.
							if (!haveCached) {
								float zOff = 48.f;
								switch (p.hitgroup) {
								case 1: zOff = 72.f; break;
								case 8: zOff = 64.f; break;
								case 2: zOff = 52.f; break;
								case 3: zOff = 40.f; break;
								case 4: case 5: zOff = 50.f; break;
								case 6: case 7: zOff = 18.f; break;
								default: zOff = 48.f; break;
								}
								onRay.z = base.z + zOff;
							}
							if (Bones::IsValidPos(onRay)) {
								EmitWorldMark(onRay, true, p.head, p.damage, ph);
								rayPinned = true;
							}
						}
					}
				}
				if (!rayPinned) {
					// Last resort: cache (living pos) > ragdoll origin+Z
					Vector_t cachePt{};
					if (LookupHitPos(ph, cachePt, 3.f))
						EmitWorldMark(cachePt, true, p.head, p.damage, ph);
					else if (ResolveHitWorld(pawn, p.hitgroup, pos, /*originOnly=*/true)
						&& Bones::IsValidPos(pos))
						EmitWorldMark(pos, true, p.head, p.damage, ph);
				}
				ClearPending(p);
				continue;
			}

			// 5) Alive (or kill while still lifeState 0 for a frame): ray ∩ hitgroup
			if (ResolveHitWorld(pawn, p.hitgroup, pos, /*originOnly=*/false)) {
				EmitWorldMark(pos, p.kill, p.head, p.damage, ph);
				ClearPending(p);
				continue;
			}

			// 6) Bones lag online
			const int maxTries = p.kill ? 12 : 10;
			if (++p.tries >= maxTries) {
				// Final: impact for this shot only — never fall back to old limb cache on kill
				if (FindImpactNear(ph, pawn, p.hitgroup, pos, 0.70f, 120.f, shotMinT, /*preferCache=*/p.kill))
					EmitWorldMark(pos, p.kill, p.head, p.damage, ph);
				else if (!p.kill && LookupHitPos(ph, pos, 0.40f))
					EmitWorldMark(pos, false, p.head, p.damage, ph);
				else if (p.kill && LookupHitPos(ph, pos, 3.f))
					EmitWorldMark(pos, true, p.head, p.damage, ph);
				else if (p.kill && ResolveHitWorld(pawn, p.hitgroup, pos, /*originOnly=*/true))
					EmitWorldMark(pos, true, p.head, p.damage, ph);
				ClearPending(p);
			}
		}
	}

	ImU32 ColorFor(bool kill, bool head, float alpha) {
		ImVec4 c = Config::hitmarker_color;
		if (kill)
			c = Config::hitmarker_kill_color;
		else if (head)
			c = Config::hitmarker_head_color;
		c.w *= alpha;
		return ImGui::ColorConvertFloat4ToU32(c);
	}

	// COD-style X: four diagonal segments with center gap
	void DrawCodX(ImDrawList* dl, ImVec2 c, float arm, float gap, float thick, ImU32 col, ImU32 outline) {
		if (!dl || arm < 1.f)
			return;
		// gap >= arm collapses/inverts segments — keep hole open
		gap = std::clamp(gap, 0.5f, arm * 0.45f);
		const float g = gap * 0.70710678f; // along diagonal
		const float a = arm * 0.70710678f;
		const ImVec2 d1(1.f, 1.f);
		const ImVec2 d2(1.f, -1.f);

		auto seg = [&](ImVec2 dir) {
			const ImVec2 p0(c.x + dir.x * g, c.y + dir.y * g);
			const ImVec2 p1(c.x + dir.x * a, c.y + dir.y * a);
			if (outline)
				dl->AddLine(p0, p1, outline, thick + 1.6f);
			dl->AddLine(p0, p1, col, thick);
		};

		seg(d1);
		seg(ImVec2(-d1.x, -d1.y));
		seg(d2);
		seg(ImVec2(-d2.x, -d2.y));
	}

	void DrawDamageLabel(ImDrawList* dl, ImVec2 c, float arm, int dmg, ImU32 col, float alpha, bool below) {
		if (!dl || dmg <= 0)
			return;
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", dmg);
		const ImVec2 ts = ImGui::CalcTextSize(buf);
		const float y = below ? (c.y + arm + 4.f) : (c.y - arm - ts.y - 2.f);
		const ImVec2 tp(c.x - ts.x * 0.5f, y);
		dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, static_cast<int>(200.f * alpha)), buf);
		dl->AddText(tp, col, buf);
	}

	void DrawScreenMarker(ImDrawList* dl) {
		if (!Config::hitmarker || !Config::hitmarker_screen)
			return;
		if (g_screenBorn < 0.f)
			return;

		const float t = Now() - g_screenBorn;
		if (t >= g_screenLife) {
			g_screenBorn = -1.f;
			return;
		}

		const float u = t / g_screenLife;
		// pop then settle
		const float pop = (u < 0.12f) ? (u / 0.12f) : 1.f;
		const float fade = (u > 0.55f) ? (1.f - (u - 0.55f) / 0.45f) : 1.f;
		const float alpha = std::clamp(fade, 0.f, 1.f) * Config::hitmarker_color.w;
		const float scale = 0.75f + 0.45f * pop * (1.f - u * 0.35f);

		const ImGuiIO& io = ImGui::GetIO();
		const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		const float arm = Config::hitmarker_size * scale;
		const float gap = Config::hitmarker_gap * scale;
		const float thick = Config::hitmarker_thickness;

		const ImU32 col = ColorFor(g_screenKill, g_screenHead, alpha);
		const ImU32 out = IM_COL32(0, 0, 0, static_cast<int>(190.f * alpha));
		DrawCodX(dl, c, arm, gap, thick, col, out);

		if (Config::hitmarker_show_damage && g_screenDmg > 0)
			DrawDamageLabel(dl, c, arm, g_screenDmg, col, alpha, true);
	}

	void DrawWorldMarkers(ImDrawList* dl, const ViewMatrix& vm) {
		if (!Config::hitmarker || !Config::hitmarker_world)
			return;
		// W2S live path works even when caller ViewMatrix pointer null
		if (!dl || (!vm.viewMatrix && !W2S::HasLiveMatrix()))
			return;

		const float now = Now();
		for (int i = 0; i < kMaxMarks; ++i) {
			Mark& m = g_marks[i];
			if (!m.active || !m.world)
				continue;
			const float t = now - m.born;
			if (t >= m.life) {
				m.active = false;
				continue;
			}

			Vector_t scr{};
			if (!vm.WorldToScreen(m.worldPos, scr))
				continue;

			const float u = t / m.life;
			// Keep world marker pinned to impact — only fade/pop size
			const float fade = (u > 0.5f) ? (1.f - (u - 0.5f) / 0.5f) : 1.f;
			const float alpha = std::clamp(fade, 0.f, 1.f);
			const float pop = 1.f + 0.25f * (1.f - u);
			const float arm = m.size * pop;
			const float gap = arm * 0.28f;
			const float thick = Config::hitmarker_thickness;

			const ImVec2 c(scr.x, scr.y);
			const ImU32 col = ColorFor(m.kill, m.head, alpha);
			const ImU32 out = IM_COL32(0, 0, 0, static_cast<int>(200.f * alpha));
			DrawCodX(dl, c, arm, gap, thick, col, out);

			if (Config::hitmarker_show_damage && m.damage > 0)
				DrawDamageLabel(dl, c, arm, m.damage, col, alpha, false);
		}
	}

	void DrawFloatingDamage(ImDrawList* dl, const ViewMatrix& vm) {
		if (!Config::float_damage || !dl || (!vm.viewMatrix && !W2S::HasLiveMatrix()))
			return;
		const float now = Now();
		const float speed = std::clamp(Config::float_damage_speed, 10.f, 200.f);
		for (int i = 0; i < kMaxFloats; ++i) {
			FloatDmg& f = g_floats[i];
			if (!f.active)
				continue;
			const float t = now - f.born;
			if (t >= f.life) {
				f.active = false;
				continue;
			}
			Vector_t scr{};
			if (!vm.WorldToScreen(f.worldPos, scr))
				continue;
			const float u = t / f.life;
			const float fade = (u > 0.55f) ? (1.f - (u - 0.55f) / 0.45f) : 1.f;
			const float alpha = std::clamp(fade, 0.f, 1.f);
			const float rise = t * speed;
			const float scale = 1.f + 0.35f * (1.f - u);

			ImVec4 cv = Config::float_damage_color;
			if (f.kill)
				cv = Config::float_damage_kill_color;
			else if (f.head)
				cv = Config::float_damage_head_color;
			cv.w *= alpha;
			const ImU32 col = ImGui::ColorConvertFloat4ToU32(cv);

			char buf[16];
			snprintf(buf, sizeof(buf), "%d", f.damage);
			ImFont* font = ImGui::GetFont();
			const float fs = ImGui::GetFontSize() * scale;
			const ImVec2 ts = font ? font->CalcTextSizeA(fs, FLT_MAX, 0.f, buf) : ImGui::CalcTextSize(buf);
			const ImVec2 tp(scr.x - ts.x * 0.5f, scr.y - rise - ts.y * 0.5f);
			const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(210.f * alpha));
			if (font) {
				dl->AddText(font, fs, ImVec2(tp.x + 1.f, tp.y + 1.f), shadow, buf);
				dl->AddText(font, fs, tp, col, buf);
			} else {
				dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), shadow, buf);
				dl->AddText(tp, col, buf);
			}
		}
	}

	void HandleHurt(void* gameEvent) {
		if (!g_ready || !g_getController || !g_getInt64)
			return;
		if (!I::GameEntity || !I::GameEntity->Instance)
			return;

		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local)
			return;

		void* localCtrl = nullptr;
		__try {
			CBaseHandle hCtrl = local->m_hController();
			if (hCtrl.valid())
				localCtrl = I::GameEntity->Instance->Get(hCtrl);
		} __except (EXCEPTION_EXECUTE_HANDLER) { return; }
		if (!localCtrl)
			return;

		EvtToken atkTok = MakeTok("attacker");
		void* attacker = nullptr;
		__try { attacker = g_getController(gameEvent, &atkTok); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return; }
		if (!attacker || attacker != localCtrl)
			return;

		// Ignore self-damage
		EvtToken usrTok = MakeTok("userid");
		void* victim = nullptr;
		__try { victim = g_getController(gameEvent, &usrTok); }
		__except (EXCEPTION_EXECUTE_HANDLER) { victim = nullptr; }
		if (victim && victim == localCtrl)
			return;

		int dmg = 0;
		int hitgroup = 0;
		int health = 100;
		__try {
			dmg = static_cast<int>(g_getInt64(gameEvent, "dmg_health", 0));
			hitgroup = static_cast<int>(g_getInt64(gameEvent, "hitgroup", 0));
			health = static_cast<int>(g_getInt64(gameEvent, "health", 100));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			dmg = 0;
		}
		if (dmg <= 0)
			return;

		const bool head = (hitgroup == 1);
		const bool kill = (health <= 0);

		if (Config::hitsound)
			Hitsound::Play(head, kill);

		if (Config::hitlog) {
			char nm[64]{};
			if (victim) {
				__try {
					reinterpret_cast<CCSPlayerController*>(victim)->ReadSanitizedName(nm, sizeof(nm));
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					nm[0] = 0;
				}
			}
			// health = remaining after hit (player_hurt); pass for log panel
			HitLog::Push(nm[0] ? nm : "player", dmg, hitgroup, head, kill, health);
		}

		if (Config::hitmarker) {
			if (Config::hitmarker_screen)
				PulseScreen(kill, head, dmg);
		}

		// Defer capsule/skeleton resolve to Present/Draw — event thread hitch → crash
		// World hitmarker X and/or floating damage share the same resolve path
		if (victim && ((Config::hitmarker && Config::hitmarker_world) || Config::float_damage))
			QueueWorld(victim, hitgroup, dmg, kill, head);
	}

	// IDA client (session e30148e9):
	//   GetInt64 name wrapper 0x180A96140 → vt[+56] = vt[7]
	//   GetFloat name wrapper 0x180A98AB0 → vt[+80] = vt[10]
	// Old code used vt[8] — wrong slot → always default 0 → impact rejected.
	using FnEvtGetFloatTok = float (__fastcall*)(void*, EvtToken*, float);
	using FnEvtGetFloatName = float (__fastcall*)(void*, const char*, float);
	// patterns.hpp-style name wrapper (loads vt+0x50)
	FnEvtGetFloatName g_getFloatName = nullptr;

	float EvtGetFloat(void* gameEvent, const char* key, float def) {
		if (!gameEvent || !key)
			return def;
		// 1) Patterned name wrapper (IDA 0x180A98AB0 → vt[+80])
		if (g_getFloatName) {
			__try {
				return g_getFloatName(gameEvent, key, def);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
		// 2) Token form via vtable slot 10 (+80)
		__try {
			void** vt = *reinterpret_cast<void***>(gameEvent);
			if (vt && vt[10]) {
				EvtToken tok = MakeTok(key);
				return reinterpret_cast<FnEvtGetFloatTok>(vt[10])(gameEvent, &tok, def);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return def;
	}

	// Point-to-ray distance (eye + dir unit → point)
	float DistToFireRay(const Vector_t& pos) {
		if (!g_lastFire.valid)
			return 1e9f;
		const Vector_t& o = g_lastFire.eye;
		const Vector_t& d = g_lastFire.dir;
		const Vector_t v{ pos.x - o.x, pos.y - o.y, pos.z - o.z };
		const float along = v.x * d.x + v.y * d.y + v.z * d.z;
		if (along < -8.f)
			return 1e9f; // behind shooter
		const Vector_t closest{
			o.x + d.x * along,
			o.y + d.y * along,
			o.z + d.z * along
		};
		const float dx = pos.x - closest.x;
		const float dy = pos.y - closest.y;
		const float dz = pos.z - closest.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	void HandleBulletImpact(void* gameEvent) {
		// Local bullet_impact → world pin + tracer end snap
		if (!gameEvent)
			return;
		if (!I::GameEntity || !I::GameEntity->Instance)
			return;

		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local)
			return;

		void* localCtrl = nullptr;
		__try {
			CBaseHandle hCtrl = local->m_hController();
			if (hCtrl.valid())
				localCtrl = I::GameEntity->Instance->Get(hCtrl);
		} __except (EXCEPTION_EXECUTE_HANDLER) { localCtrl = nullptr; }

		bool ctrlLocal = false;
		bool ctrlResolved = false;
		if (g_getController && localCtrl) {
			EvtToken usrTok = MakeTok("userid");
			void* shooter = nullptr;
			__try { shooter = g_getController(gameEvent, &usrTok); }
			__except (EXCEPTION_EXECUTE_HANDLER) { shooter = nullptr; }
			if (shooter) {
				ctrlResolved = true;
				ctrlLocal = (shooter == localCtrl);
			}
		}
		if (ctrlResolved && !ctrlLocal)
			return;

		// No controller: accept if fresh gun fire ray
		const bool needRayGate = !ctrlLocal;
		if (needRayGate) {
			if (!g_lastFire.valid)
				return;
			const float age = Now() - g_lastFire.time;
			if (age < 0.f || age > 0.60f)
				return;
		}

		const float x = EvtGetFloat(gameEvent, "x", 0.f);
		const float y = EvtGetFloat(gameEvent, "y", 0.f);
		const float z = EvtGetFloat(gameEvent, "z", 0.f);
		const Vector_t pos{ x, y, z };
		if (std::fabs(x) < 1.f && std::fabs(y) < 1.f && std::fabs(z) < 1.f)
			return;
		if (!Bones::IsValidPos(pos))
			return;

		// Ray gate always when we have fire pin — picks correct impact of multi-hits
		if (g_lastFire.valid && DistToFireRay(pos) > 256.f)
			return;

		PushImpact(pos);
		// Tracers::OnImpact only refines existing gun NoteFire beam (no knife)
		if (Config::tracers)
			Tracers::OnImpact(pos);
	}

	void HandleDeath(void* gameEvent) {
		// Promote kill flag + screen pulse. World pin = bullet_impact / ray, not last limb.
		if (!g_ready || !g_getController)
			return;
		if (!I::GameEntity || !I::GameEntity->Instance)
			return;

		C_CSPlayerPawn* local = H::SafeLocalPlayer();
		if (!local)
			return;

		void* localCtrl = nullptr;
		__try {
			CBaseHandle hCtrl = local->m_hController();
			if (hCtrl.valid())
				localCtrl = I::GameEntity->Instance->Get(hCtrl);
		} __except (EXCEPTION_EXECUTE_HANDLER) { return; }
		if (!localCtrl)
			return;

		EvtToken atkTok = MakeTok("attacker");
		void* attacker = nullptr;
		__try { attacker = g_getController(gameEvent, &atkTok); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return; }
		if (!attacker || attacker != localCtrl)
			return;

		EvtToken usrTok = MakeTok("userid");
		void* victim = nullptr;
		__try { victim = g_getController(gameEvent, &usrTok); }
		__except (EXCEPTION_EXECUTE_HANDLER) { victim = nullptr; }
		if (victim && victim == localCtrl)
			return;

		int hitgroup = 0;
		if (g_getInt64) {
			__try { hitgroup = static_cast<int>(g_getInt64(gameEvent, "hitgroup", 0)); }
			__except (EXCEPTION_EXECUTE_HANDLER) { hitgroup = 0; }
		}
		const bool head = (hitgroup == 1);

		if (Config::hitmarker && Config::hitmarker_screen)
			PulseScreen(true, head, g_screenDmg > 0 ? g_screenDmg : 0);

		if (!((Config::hitmarker && Config::hitmarker_world) || Config::float_damage))
			return;

		const std::uint32_t ph = PawnHandleFromController(victim);
		if (!ph)
			return;

		// Promote existing hurt pending — do NOT freeze last non-lethal pos
		bool promoted = false;
		for (int i = 0; i < kMaxPending; ++i) {
			if (!g_pending[i].active || g_pending[i].pawnHandle != ph)
				continue;
			g_pending[i].kill = true;
			g_pending[i].head = head || g_pending[i].head;
			if (hitgroup > 0)
				g_pending[i].hitgroup = hitgroup;
			// Drop any accidental freeze so Drain waits for this kill's impact
			g_pending[i].havePos = false;
			// Don't reset tries to 0 forever — keep partial wait credit
			if (g_pending[i].tries > 2)
				g_pending[i].tries = 2;
			promoted = true;
		}

		// One-shot / death without hurt queue: wait for impact like hurt path
		if (!promoted) {
			PendingWorld& p = g_pending[g_pendingWrite];
			g_pendingWrite = (g_pendingWrite + 1) % kMaxPending;
			p.active = true;
			p.pawnHandle = ph;
			p.hitgroup = hitgroup;
			p.damage = g_screenDmg > 0 ? g_screenDmg : 0;
			p.kill = true;
			p.head = head;
			p.tries = 0;
			p.havePos = false;
			p.worldPos = Vector_t{};
			p.queuedAt = Now();
		}
	}
}

void Hitmarker::NoteLastFire(const Vector_t& eye, const QAngle_t& fireAngles)
{
	if (!Bones::IsValidPos(eye) || !fireAngles.IsValid())
		return;
	// Guns only — knife swing / nade throw must not arm fire ray or tracers
	{
		C_CSPlayerPawn* lp = H::SafeLocalPlayer();
		C_CSWeaponBase* w = nullptr;
		if (lp) {
			__try { w = lp->GetActiveWeapon(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
		}
		if (!w || !Mem::ValidEntity(w))
			return;
		__try {
			if (w->IsNonGunWeapon())
				return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
	}
	Vector_t dir{};
	QAngle_t ang = fireAngles;
	ang.z = 0.f;
	ang.x = std::clamp(ang.x, -89.f, 89.f);
	ang.Normalize();
	ang.ToDirections(&dir, nullptr, nullptr);
	const float len = dir.Length();
	if (len < 1e-4f || !std::isfinite(len))
		return;
	g_lastFire.eye = eye;
	g_lastFire.dir = Vector_t{ dir.x / len, dir.y / len, dir.z / len };
	g_lastFire.time = Now();
	g_lastFire.valid = true;
	if (Config::tracers)
		Tracers::NoteFire(g_lastFire.eye, g_lastFire.dir);
}

void Hitmarker::Install() {
	g_ready = false;
	g_getName = nullptr;
	g_getController = nullptr;
	g_getInt64 = nullptr;
	g_screenBorn = -1.f;
	g_write = 0;
	g_floatWrite = 0;
	g_pendingWrite = 0;
	for (int i = 0; i < kMaxMarks; ++i)
		g_marks[i] = Mark{};
	for (int i = 0; i < kMaxFloats; ++i)
		g_floats[i] = FloatDmg{};
	for (int i = 0; i < kMaxPending; ++i)
		g_pending[i] = PendingWorld{};
	for (int i = 0; i < kMaxLastHit; ++i)
		g_lastHit[i] = LastHitPos{};
	g_lastHitWrite = 0;
	g_impactWrite = 0;
	for (int i = 0; i < kMaxImpacts; ++i)
		g_impacts[i] = ImpactPt{};
	g_lastFire = LastFireRay{};

	// Same patterns as skinchanger killfeed (client.dll)
	const uintptr_t pName = M::patternScan("client",
		"8B 41 14 0F BA E0 1E 73 05 48 8D 41 18 C3");
	if (pName)
		g_getName = reinterpret_cast<FnEvtGetName>(pName);

	const uintptr_t pCtrl = M::patternScan("client",
		"48 83 EC 38 8B 02 4C 8D 44 24 20");
	if (pCtrl)
		g_getController = reinterpret_cast<FnEvtGetController>(pCtrl);

	// GetInt64(event, name, default) — dumps/patterns.hpp → vt[+56]
	const uintptr_t pInt = M::patternScan("client",
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 01 41 8B F0");
	if (pInt)
		g_getInt64 = reinterpret_cast<FnEvtGetInt64>(pInt);

	// GetFloat(event, name, default) — IDA 0x180A98AB0 (vt[+80]), NOT GetInt64 twin.
	// Unique: mov rbp,[rax+50h] after 48 8B 01 49 8B F0 48 8B DA
	g_getFloatName = nullptr;
	const uintptr_t pFlt = M::patternScan("client",
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 01 49 8B F0 48 8B DA 48 8B F9 48 8B 68 50");
	if (pFlt)
		g_getFloatName = reinterpret_cast<FnEvtGetFloatName>(pFlt);
	if (!g_getFloatName) {
		// shorter: still disambiguates from GetInt64 (41 8B F0) and GetString (+232)
		const uintptr_t pFlt2 = M::patternScan("client",
			"48 8B 01 49 8B F0 48 8B DA 48 8B F9 48 8B 68 50");
		if (pFlt2)
			g_getFloatName = reinterpret_cast<FnEvtGetFloatName>(pFlt2 - 0x14);
	}

	g_reportHit = M::patternScan("client",
		"40 53 48 83 EC 20 48 8D 05 ? ? ? ? 48 8D 59 08 48 89 01");
	if (!g_reportHit)
		g_reportHit = M::patternScan("client.dll",
			"40 53 48 83 EC 20 48 8D 05 ? ? ? ? 48 8D 59 08 48 89 01");

	// Tracers only need GetName + float path; hurt needs full set
	g_ready = g_getName && g_getController && g_getInt64;
}

uintptr_t Hitmarker::ReportHitAddr() {
	return g_reportHit;
}

void Hitmarker::Shutdown() {
	g_ready = false;
	g_screenBorn = -1.f;
	for (int i = 0; i < kMaxMarks; ++i)
		g_marks[i].active = false;
	for (int i = 0; i < kMaxFloats; ++i)
		g_floats[i].active = false;
	for (int i = 0; i < kMaxPending; ++i) {
		g_pending[i].active = false;
		g_pending[i].pawnHandle = 0;
	}
	for (int i = 0; i < kMaxImpacts; ++i)
		g_impacts[i].valid = false;
}

void Hitmarker::OnGameEvent(void* gameEvent) {
	// Hitsound / float dmg / hitlog / tracers can run without hitmarker visuals
	if ((!Config::hitmarker && !Config::hitsound && !Config::float_damage
			&& !Config::hitlog && !Config::tracers)
		|| !gameEvent || !g_ready || !g_getName)
		return;

	const char* name = nullptr;
	__try { name = g_getName(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (!name || !name[0])
		return;

	// Local impacts: world mark / float dmg / tracers share bullet_impact
	if (std::strcmp(name, "bullet_impact") == 0) {
		if ((Config::hitmarker && Config::hitmarker_world) || Config::float_damage
			|| Config::tracers)
			HandleBulletImpact(gameEvent);
		return;
	}
	if (std::strcmp(name, "player_hurt") == 0) {
		HandleHurt(gameEvent);
		return;
	}
	if (std::strcmp(name, "player_death") == 0) {
		if (Config::hitmarker)
			HandleDeath(gameEvent);
		return;
	}
}

void Hitmarker::Draw(const ViewMatrix& vm) {
	if (!Config::hitmarker && !Config::float_damage)
		return;
	DrainPendingWorld();
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl)
		return;
	if (Config::hitmarker) {
		DrawWorldMarkers(dl, vm);
		DrawScreenMarker(dl);
	}
	DrawFloatingDamage(dl, vm);
}

void Hitmarker::DrawPreview(ImDrawList* dl, ImVec2 boxMin, ImVec2 boxMax, int mode)
{
	if (!dl)
		return;
	if (boxMax.x <= boxMin.x || boxMax.y <= boxMin.y)
		return;

	const bool kill = (mode == 2);
	const bool head = (mode == 1);
	const int sampleDmg = kill ? 100 : (head ? 110 : 27);

	// Same pop/fade curve as screen marker; loops so Duration is visible
	const float life = kScreenLife * std::clamp(Config::hitmarker_duration, 0.25f, 2.5f);
	const float t = std::fmod(Now(), life);
	const float u = (life > 1e-4f) ? (t / life) : 0.f;
	const float pop = (u < 0.12f) ? (u / 0.12f) : 1.f;
	const float fade = (u > 0.55f) ? (1.f - (u - 0.55f) / 0.45f) : 1.f;
	const float alpha = std::clamp(fade, 0.f, 1.f) * Config::hitmarker_color.w;
	const float scale = 0.75f + 0.45f * pop * (1.f - u * 0.35f);
	const float thick = Config::hitmarker_thickness;

	dl->PushClipRect(boxMin, boxMax, true);

	const float midY = (boxMin.y + boxMax.y) * 0.5f;
	const bool showWorld = Config::hitmarker_world;
	const bool showScreen = Config::hitmarker_screen || !showWorld;

	auto softGuide = [&](ImVec2 c) {
		dl->AddLine(ImVec2(c.x - 16.f, c.y), ImVec2(c.x + 16.f, c.y), IM_COL32(255, 255, 255, 18), 1.f);
		dl->AddLine(ImVec2(c.x, c.y - 16.f), ImVec2(c.x, c.y + 16.f), IM_COL32(255, 255, 255, 18), 1.f);
	};

	if (showScreen && showWorld) {
		// Split card: screen (left) + world size sample (right)
		const float midX = (boxMin.x + boxMax.x) * 0.5f;
		dl->AddLine(ImVec2(midX, boxMin.y + 8.f), ImVec2(midX, boxMax.y - 8.f),
			IM_COL32(255, 255, 255, 28), 1.f);

		const ImVec2 cScr((boxMin.x + midX) * 0.5f, midY);
		softGuide(cScr);
		const float armS = Config::hitmarker_size * scale;
		const float gapS = Config::hitmarker_gap * scale;
		const ImU32 colS = ColorFor(kill, head, alpha);
		const ImU32 outS = IM_COL32(0, 0, 0, static_cast<int>(190.f * alpha));
		DrawCodX(dl, cScr, armS, gapS, thick, colS, outS);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, cScr, armS, sampleDmg, colS, alpha, true);

		// World: size pop only (matches DrawWorldMarkers)
		const ImVec2 cW((midX + boxMax.x) * 0.5f, midY);
		softGuide(cW);
		const float fadeW = (u > 0.5f) ? (1.f - (u - 0.5f) / 0.5f) : 1.f;
		const float alphaW = std::clamp(fadeW, 0.f, 1.f);
		const float popW = 1.f + 0.25f * (1.f - u);
		const float armW = Config::hitmarker_world_size * popW;
		const float gapW = armW * 0.28f;
		const ImU32 colW = ColorFor(kill, head, alphaW);
		const ImU32 outW = IM_COL32(0, 0, 0, static_cast<int>(200.f * alphaW));
		DrawCodX(dl, cW, armW, gapW, thick, colW, outW);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, cW, armW, sampleDmg, colW, alphaW, false);

		dl->AddText(ImVec2(boxMin.x + 6.f, boxMin.y + 4.f), IM_COL32(180, 180, 190, 140), "Screen");
		dl->AddText(ImVec2(midX + 6.f, boxMin.y + 4.f), IM_COL32(180, 180, 190, 140), "World");
	} else if (showWorld) {
		const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, midY);
		softGuide(c);
		const float fadeW = (u > 0.5f) ? (1.f - (u - 0.5f) / 0.5f) : 1.f;
		const float alphaW = std::clamp(fadeW, 0.f, 1.f);
		const float popW = 1.f + 0.25f * (1.f - u);
		const float armW = Config::hitmarker_world_size * popW;
		const float gapW = armW * 0.28f;
		const ImU32 colW = ColorFor(kill, head, alphaW);
		const ImU32 outW = IM_COL32(0, 0, 0, static_cast<int>(200.f * alphaW));
		DrawCodX(dl, c, armW, gapW, thick, colW, outW);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, c, armW, sampleDmg, colW, alphaW, false);
	} else {
		const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, midY);
		softGuide(c);
		const float arm = Config::hitmarker_size * scale;
		const float gap = Config::hitmarker_gap * scale;
		const ImU32 col = ColorFor(kill, head, alpha);
		const ImU32 out = IM_COL32(0, 0, 0, static_cast<int>(190.f * alpha));
		DrawCodX(dl, c, arm, gap, thick, col, out);
		if (Config::hitmarker_show_damage)
			DrawDamageLabel(dl, c, arm, sampleDmg, col, alpha, true);
	}

	dl->PopClipRect();
}
