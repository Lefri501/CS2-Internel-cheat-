#define NOMINMAX
#include "cl_bypass.h"

#include "../../utils/console/console.h"

#include <Windows.h>

// CL_Bypass intentionally DISABLED for move_crc / SerializePartialToArray.
//
// Root cause of FATAL "Failed to serialize one usercommand?":
//   LefrizzelAi has no linked libprotobuf. The live CBaseUserCmdPB::move_crc
//   field is an ArenaStringPtr-backed string, not a free-standing std::string
//   we can clear/assign safely. Re-serializing and writing CRC corrupted the
//   message so the parent usercmd failed to encode.
//
// Andromeda works because it owns a real CBaseUserCmdPB protobuf object
// (m_Backup) and uses set_move_crc() / SerializePartialToArray on THAT copy.
//
// Safe surface kept: button helpers + optional subtick queue (unused while
// CRC is off). Features still mutate cmd buttons / subticks directly.

namespace CL_Bypass {
namespace {

	constexpr std::uint64_t kJump = IN_JUMP;
	constexpr std::uint64_t kAttack = IN_ATTACK;

	bool g_inOriginalCreateMove = false;

	struct InternalSubTick {
		std::uint64_t button = 0;
		bool pressed = false;
		float when = 0.f;
	};
	std::vector<InternalSubTick> g_subticks; // rarely used; keep capacity via clear

	// SEH helpers — no C++ objects with destructors in these functions
	void SehSyncButtonsToPb(CUserCmd* cmd)
	{
		if (!cmd)
			return;
		__try {
			auto* base = cmd->csgoUserCmd.pBaseCmd;
			if (!base || !base->pInButtonState)
				return;
			base->pInButtonState->nValue = cmd->nButtons.nValue;
			base->pInButtonState->nValueChanged = cmd->nButtons.nValueChanged;
			base->pInButtonState->nValueScroll = cmd->nButtons.nValueScroll;
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			base->SetBits(BASE_BITS_BUTTONPB);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	void SehAddOneSubtick(CUserCmd* cmd, std::uint64_t button, bool pressed, float when)
	{
		if (!cmd)
			return;
		__try {
			CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
			if (!base)
				return;
			CSubtickMoveStep* step = base->add_subtick_move();
			if (!step)
				return;
			step->nHasBits = 0;
			step->nCachedBits = 0;
			step->nButton = button;
			step->bPressed = pressed;
			step->flWhen = when;
			step->flAnalogForwardDelta = 0.f;
			step->flAnalogLeftDelta = 0.f;
			step->SetBits(MOVESTEP_BITS_BUTTON | MOVESTEP_BITS_PRESSED | MOVESTEP_BITS_WHEN);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	void SyncButtonsToPb(CUserCmd* cmd)
	{
		SehSyncButtonsToPb(cmd);
	}

} // namespace

void SetInOriginalCreateMove(bool v)
{
	g_inOriginalCreateMove = v;
}

bool InOriginalCreateMove()
{
	return g_inOriginalCreateMove;
}

void OnCBaseUserCmdPB(void* /*pMsg*/)
{
	// no-op — no protobuf backup without real Message class
}

void PreClientCreateMove(CUserCmd* /*cmd*/)
{
	g_subticks.clear();
}

void PostClientCreateMove(void* /*pCSGOInput*/, CUserCmd* cmd)
{
	// Flush queued subticks only. NEVER recompute / write move_crc here.
	if (!cmd)
		return;

	const size_t n = g_subticks.size();
	for (size_t i = 0; i < n; ++i)
		SehAddOneSubtick(cmd, g_subticks[i].button, g_subticks[i].pressed, g_subticks[i].when);
	g_subticks.clear();

	// Keep PB buttons in sync with CUserCmd (safe field writes only)
	SyncButtonsToPb(cmd);
}

void SetAttack(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue |= kAttack;
	cmd->nButtons.nValueChanged |= kAttack;
	SyncButtonsToPb(cmd);
	cmd->csgoUserCmd.SetAttack1StartHistoryIndex(0);
	if (addSubtick)
		AddProcessSubTick(kAttack, true);
}

void SetDontAttack(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue &= ~kAttack;
	cmd->nButtons.nValueScroll &= ~kAttack;
	SyncButtonsToPb(cmd);
	if (addSubtick)
		AddProcessSubTick(kAttack, false);
}

void SetJump(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue |= kJump;
	cmd->nButtons.nValueChanged |= kJump;
	cmd->nButtons.nValueScroll |= kJump;
	SyncButtonsToPb(cmd);
	if (addSubtick)
		AddProcessSubTick(kJump, true, 0.01f);
}

void SetDontJump(CUserCmd* cmd, bool addSubtick)
{
	if (!cmd)
		return;
	cmd->nButtons.nValue &= ~kJump;
	cmd->nButtons.nValueScroll &= ~kJump;
	SyncButtonsToPb(cmd);
	if (addSubtick)
		AddProcessSubTick(kJump, false, 0.01f);
}

void AddProcessSubTick(std::uint64_t button, bool pressed)
{
	if (g_subticks.capacity() < 16)
		g_subticks.reserve(16);
	g_subticks.push_back({ button, pressed, 0.99f });
}

void AddProcessSubTick(std::uint64_t button, bool pressed, float when)
{
	if (g_subticks.capacity() < 16)
		g_subticks.reserve(16);
	g_subticks.push_back({ button, pressed, when });
}

bool __fastcall hkSerializePartialToArray(void* /*msg*/, void* /*out*/, int /*size*/)
{
	// Not installed — Init() no longer hooks MessageLite.
	return false;
}

bool Init()
{
	// Do NOT hook SerializePartialToArray / rewrite move_crc until we have a
	// real protobuf CBaseUserCmdPB backup (Andromeda-style) or verified
	// ArenaStringPtr write helpers. Both previously caused usercmd serialize fatals.
	Con::Ok("CL_Bypass: CRC hook DISABLED (safe mode — buttons/subticks only)");
	return true;
}

} // namespace CL_Bypass
