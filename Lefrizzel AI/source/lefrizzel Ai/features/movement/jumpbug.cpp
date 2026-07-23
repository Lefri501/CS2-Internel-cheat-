#define NOMINMAX
#include "jumpbug.h"

#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../prediction/prediction.h"
#include "../input_inject/input_inject.h"
#include "../trace/trace.h"
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace JumpBug {
namespace {

constexpr std::uintptr_t kFlagsOff = 0x3F4;
constexpr std::uintptr_t kAbsVelOff = 0x3F8;
constexpr std::uint64_t kJump = IN_JUMP;
constexpr std::uint64_t kDuck = IN_DUCK;
// MASK_PLAYERSOLID-ish (same as Pred land hull)
constexpr std::uint64_t kMaskPlayerSolid = 0x201400Bull;
constexpr float kFloorNormalMin = 0.50f;

// One edge per land / edge leave — re-edge every fall tick = sticky spam.
bool g_jbFired = false;
bool g_ejFired = false;
bool g_wasAir = false;
bool g_claimedJump = false;

bool ReadLive(C_CSPlayerPawn* pawn, Vector_t& vel, std::uint32_t& flags)
{
	vel = Vector_t{ 0.f, 0.f, 0.f };
	flags = 0;
	if (!pawn)
		return false;
	__try {
		const auto base = reinterpret_cast<std::uintptr_t>(pawn);
		// Prefer schema, hard dump 0x3F4 as truth for FL_ONGROUND
		flags = *reinterpret_cast<std::uint32_t*>(base + kFlagsOff);
		const std::uint32_t schemaF = pawn->m_fFlags();
		if (schemaF & FL_ONGROUND)
			flags |= FL_ONGROUND;
		vel = pawn->m_vecAbsVelocity();
		if (vel.x == 0.f && vel.y == 0.f && vel.z == 0.f)
			vel = *reinterpret_cast<Vector_t*>(base + kAbsVelOff);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void SetBtn(CUserCmd* cmd, std::uint64_t button, bool down, bool edge)
{
	if (!cmd)
		return;
	if (down) {
		cmd->nButtons.nValue |= button;
		if (edge)
			cmd->nButtons.nValueChanged |= button;
		else
			cmd->nButtons.nValueChanged &= ~button;
		cmd->nButtons.nValueScroll &= ~button;
	} else {
		cmd->nButtons.nValue &= ~button;
		cmd->nButtons.nValueChanged &= ~button;
		cmd->nButtons.nValueScroll &= ~button;
	}
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pInButtonState) {
			if (down) {
				base->pInButtonState->nValue |= button;
				if (edge)
					base->pInButtonState->nValueChanged |= button;
				else
					base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			} else {
				base->pInButtonState->nValue &= ~button;
				base->pInButtonState->nValueChanged &= ~button;
				base->pInButtonState->nValueScroll &= ~button;
			}
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
		}
		base->SetBits(BASE_BITS_BUTTONPB);
	}
}

// Rising edge + one press subtick @ when (IDA IsButtonActive needs value|changed).
void InjectJumpEdge(CUserCmd* cmd, float when)
{
	SetBtn(cmd, kJump, true, true);
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		InputInject::ClearJumpSubticks(base);
		const float w = std::clamp(when, 0.f, 0.999f);
		InputInject::SubtickButton(base, kJump, true, w);
		InputInject::SanitizeSubticks(base);
	}
	g_claimedJump = true;
}

// Hold unbound = Always so checkbox alone works.
bool FeatureOn(bool& feature)
{
	if (!feature)
		return false;
	if (keybind.isActive(feature))
		return true;
	const int k = keybind.getKey(feature);
	const int m = keybind.getMode(feature);
	if (k <= 0 && m == static_cast<int>(KeyMode::Hold))
		return true;
	return false;
}

// Land window: hull predict this tick. Prefer late contact for JB/EB.
bool LandImminent(C_CSPlayerPawn* pawn, const Vector_t& vel, float& outFrac)
{
	outFrac = 0.f;
	// Rising hard = not landing. Soft step-down (vz near 0) still allowed by Pred.
	if (vel.z > 30.f)
		return false;
	if (!Pred::PredictLandThisTick(pawn, vel, outFrac))
		return false;
	// Jumpbug wants contact in this tick — allow full [0,1). Early frac still OK
	// if hull says we hit (stairs/lips). Cap release-safe.
	return outFrac >= 0.f && outFrac < 1.f;
}

// Edgejump: on ground, next step in vel/view direction loses floor support.
bool AboutToLeaveEdge(C_CSPlayerPawn* pawn, const Vector_t& vel, float viewYawDeg)
{
	if (!pawn)
		return false;
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

	// Wish direction: velocity if moving, else view forward.
	float dx = vel.x;
	float dy = vel.y;
	const float spd = std::sqrt(dx * dx + dy * dy);
	if (spd < 20.f) {
		const float yaw = viewYawDeg * (3.14159265358979323846f / 180.f);
		dx = std::cos(yaw);
		dy = std::sin(yaw);
	} else {
		dx /= spd;
		dy /= spd;
	}

	// Probe slightly ahead + down (ledge lip).
	const float ahead = 18.f;
	const float down = 28.f;
	Vector_t start = origin;
	start.x += dx * ahead;
	start.y += dy * ahead;
	start.z += 2.f; // lift off floor noise
	Vector_t end = start;
	end.z -= down;

	// Thinner hull so lip doesn't always report floor under center
	Vector_t pmin = mins;
	Vector_t pmax = maxs;
	pmin.x *= 0.55f; pmin.y *= 0.55f;
	pmax.x *= 0.55f; pmax.y *= 0.55f;

	Trace::CGameTrace tr{};
	if (!Trace::TraceHull(start, end, pmin, pmax, pawn, tr, kMaskPlayerSolid))
		return true; // no trace API hit → treat as open (edge)
	if (tr.startsolid())
		return false;
	if (!Trace::DidHit(tr))
		return true; // open air below probe = leaving edge

	const Vector_t n = tr.normal();
	const float frac = tr.fraction();
	// Floor far below or steep wall = edge
	if (!std::isfinite(n.z) || n.z < kFloorNormalMin)
		return true;
	// Hit very late in long down probe = deep drop under feet ahead
	if (std::isfinite(frac) && frac > 0.55f)
		return true;
	return false;
}

bool ReadYaw(CUserCmd* cmd, float& yaw)
{
	yaw = 0.f;
	if (!cmd)
		return false;
	if (auto* base = cmd->csgoUserCmd.pBaseCmd) {
		if (base->pViewAngles) {
			yaw = base->pViewAngles->angValue.y;
			return true;
		}
	}
	return false;
}

} // namespace

bool ClaimedJumpThisTick()
{
	return g_claimedJump;
}

void OnCreateMove(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	g_claimedJump = false;
	if (!cmd || !pawn)
		return;
	if (!Config::jumpbug && !Config::edgebug && !Config::edgejump)
		return;

	const bool wantJb = FeatureOn(Config::jumpbug);
	const bool wantEb = FeatureOn(Config::edgebug);
	const bool wantEj = FeatureOn(Config::edgejump);

	int hp = 0;
	__try { hp = pawn->m_iHealth(); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (hp <= 0 || hp > 200)
		return;

	Vector_t vel{};
	std::uint32_t flags = 0;
	if (!ReadLive(pawn, vel, flags))
		return;

	const bool onGround = (flags & FL_ONGROUND) != 0U;

	// Reset one-shots when solid ground after air.
	if (onGround) {
		if (g_wasAir) {
			g_jbFired = false;
			// edgejump re-arms on each ground contact after air
			g_ejFired = false;
		}
		g_wasAir = false;
	} else {
		g_wasAir = true;
	}

	// ---- Edgejump: ground + key + about to leave ledge ----
	// Must run while still ONGROUND so server sees ground jump (ModernJump).
	if (wantEj && onGround && !g_ejFired) {
		float yaw = 0.f;
		ReadYaw(cmd, yaw);
		if (AboutToLeaveEdge(pawn, vel, yaw)) {
			g_ejFired = true;
			// Fresh rising edge @ frac 0 — leave ground with jump impulse
			InjectJumpEdge(cmd, 0.f);
			// Don't unduck/duck — pure edge leave hop
			return;
		}
	}

	// ---- Jumpbug / Edgebug need air + land window ----
	if (onGround)
		return;
	if (!wantJb && !wantEb)
		return;

	float landFrac = 0.f;
	if (!LandImminent(pawn, vel, landFrac))
		return;

	const float speed2d = std::sqrt(vel.x * vel.x + vel.y * vel.y);

	// Edgebug first if both: duck hold on land window only (no jump).
	if (wantEb && speed2d > 40.f) {
		SetBtn(cmd, kDuck, true, false);
		// If jumpbug also wants this land, still allow unduck+jump below
		// only when not pure edgebug-priority. Priority: EB alone → duck only.
		if (!wantJb)
			return;
	}

	// Jumpbug: one unduck + jump edge per land @ predicted contact frac.
	// Never re-edge same fall (ModernJump spam window).
	if (wantJb && !g_jbFired) {
		g_jbFired = true;
		// Unduck so crouch-land cancel fires (classic JB)
		SetBtn(cmd, kDuck, false, true);
		// Press slightly before contact so subtick lands on ground sample
		const float pressWhen = std::clamp(landFrac * 0.92f, 0.f, 0.999f);
		InjectJumpEdge(cmd, pressWhen);
	}
}

} // namespace JumpBug
