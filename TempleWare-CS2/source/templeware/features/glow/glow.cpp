#include "glow.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../gamemode/gamemode.h"

#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/security/vacdetect.h"

#include <Windows.h>
#include <cmath>

namespace Glow {
namespace {

ColorRgba g_white{ 255, 255, 255, 255 };

// schema: C_BaseModelEntity->m_Glow offset (cached)
std::uint32_t g_mGlowOff = 0;
bool g_mGlowTried = false;

std::uint32_t ResolveMGlow() {
	if (g_mGlowTried)
		return g_mGlowOff;
	g_mGlowTried = true;
	g_mGlowOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseModelEntity->m_Glow"));
	if (!g_mGlowOff)
		// fallback from recent dumps (0xDE0 / 0xDD8)
		g_mGlowOff = 0xDE0;
	return g_mGlowOff;
}

bool IsPlayerPawn(C_BaseEntity* ent) {
	if (!ent)
		return false;
	return ent->IsBasePlayer();
}

bool PlayerVisible(C_CSPlayerPawn* local, C_CSPlayerPawn* target) {
	if (!local || !target || !Trace::Ready())
		return true; // fail-open color (visible palette)

	const Vector_t eye = Bones::GetEyePos(local);
	if (!Bones::IsValidPos(eye))
		return true;

	Vector_t samples[6]{};
	int n = 0;
	uintptr_t ba = 0;
	Vector_t origin{};
	float height = 0.f;
	Bones::Map map{};
	if (Bones::GetBoneArray(target, ba, origin, height)
		&& Bones::ResolveMapCached(target, ba, origin, height, map)) {
		const int slots[] = {
			Bones::S_HEAD, Bones::S_PELVIS,
			Bones::S_ARM_L_R, Bones::S_ARM_L_L,
			Bones::S_ANKLE_R, Bones::S_ANKLE_L
		};
		for (int s : slots) {
			Vector_t p{};
			if (n < 6 && Bones::GetSlotPos(ba, map, s, p) && Bones::IsValidPos(p))
				samples[n++] = p;
		}
	}
	if (n < 1) {
		samples[0] = Bones::GetEyePos(target);
		n = 1;
	}
	return Trace::IsBodyVisible(eye, samples, n, local, target);
}

void SehOnDrawGlow(CGlowProperty* glow) {
	__try {
		if (!glow || !glow->ok())
			return;

		if (!I::EngineClient || !I::EngineClient->in_game())
			return;

		// Soft-pause / glow off: force not glowing (no custom color writes)
		if (VacDetect::IsSoftPaused() || !Config::glow) {
			glow->glowing() = false;
			return;
		}

		void* owner = glow->owner();
		if (!Mem::ValidEntity(owner))
			return;

		auto* inst = reinterpret_cast<CEntityInstance*>(owner);
		CEntityIdentity* id = inst->m_pEntityIdentity();
		if (!id || !Mem::Valid(id, sizeof(void*)) || !id->valid())
			return;

		auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(owner);
		if (!IsPlayerPawn(reinterpret_cast<C_BaseEntity*>(pawn)))
			return;

		const int hp = Mem::ClampHealth(pawn->m_iHealth());
		if (hp < 1)
			return;

		if (!H::oGetLocalPlayer)
			return;
		C_CSPlayerPawn* local = H::oGetLocalPlayer(0);
		if (!Mem::ValidEntity(local) || local == pawn)
			return;

		const int team = static_cast<int>(pawn->m_iTeamNum());
		const int localTeam = static_cast<int>(local->m_iTeamNum());
		if (!Mem::ValidTeam(team) || !Mem::ValidTeam(localTeam))
			return;
		const bool isEnemy = (team != localTeam);

		// teamCheck true → only enemies (TempleWare convention); auto-off in DM/FFA
		if (GameMode::WantTeamCheck(Config::teamCheck) && !isEnemy)
			return;
		if (!Config::glow_team && !isEnemy)
			return;
		if (!Config::glow_enemy && isEnemy)
			return;

		const bool visible = PlayerVisible(local, pawn);
		if (Config::glow_only_visible && !visible)
			return;

		const ImVec4 col4 = visible ? Config::glow_color : Config::glow_color_invis;
		glow->color_override() = FromImVec4(col4);
		glow->glowing() = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("Glow::OnDrawGlow", GetExceptionCode());
	}
}

void SehApplyWorld(void* ent, ColorRgba col, bool enable) {
	__try {
		if (!Mem::ValidEntity(ent))
			return;
		CGlowProperty* g = ModelGlow(ent);
		if (!g || !g->ok())
			return;
		if (enable) {
			g->color_override() = col;
			g->glow_type() = 3;
			g->glow_range() = 0;
			g->glow_range_min() = 0;
			g->glowing() = true;
		} else {
			g->glowing() = false;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		// silent — world entities recycle often
	}
}

} // namespace

ColorRgba FromImVec4(const ImVec4& c) {
	auto ch = [](float v) -> std::uint8_t {
		if (v < 0.f) v = 0.f;
		if (v > 1.f) v = 1.f;
		return static_cast<std::uint8_t>(v * 255.f + 0.5f);
	};
	return ColorRgba{ ch(c.x), ch(c.y), ch(c.z), ch(c.w) };
}

CGlowProperty* ModelGlow(void* baseModelEntity) {
	if (!Mem::ValidEntity(baseModelEntity))
		return nullptr;
	const std::uint32_t off = ResolveMGlow();
	if (!off)
		return nullptr;
	auto* g = reinterpret_cast<CGlowProperty*>(
		reinterpret_cast<std::uint8_t*>(baseModelEntity) + off);
	if (!g->ok())
		return nullptr;
	return g;
}

void* __fastcall OnDrawGlow(CGlowProperty* glow) {
	// Andromeda: never call original — CS2 glow pass reads fields after return
	SehOnDrawGlow(glow);
	return nullptr;
}

void ApplyWorld(void* baseModelEntity, const ImVec4& color, bool enable) {
	SehApplyWorld(baseModelEntity, FromImVec4(color), enable);
}

} // namespace Glow
