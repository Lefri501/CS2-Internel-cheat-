#pragma once

#include <limits>
#include <cstddef>

#include "../../utils/memory/memorycommon.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/math/vector/vector.h"
#include "../../utils/memory/vfunc/vfunc.h"

enum EMoveType : int
{
	MOVETYPE_NONE = 0,
	MOVETYPE_ISOMETRIC,
	MOVETYPE_WALK,
	MOVETYPE_STEP,
	MOVETYPE_FLY, // no gravity, but still collides with stuff
	MOVETYPE_FLYGRAVITY, // flies through the air and is affected by gravity
	MOVETYPE_VPHYSICS,
	MOVETYPE_PUSH, // no clip to world, push and crush
	MOVETYPE_NOCLIP, // no gravity, no collisions, still do velocity/absvelocity
	MOVETYPE_LADDER,
	MOVETYPE_OBSERVER, // observer movement, depends on player's observer mode
	MOVETYPE_CUSTOM,
	MOVETYPE_LAST = MOVETYPE_CUSTOM,
	MOVETYPE_MAX_BITS = 4
};

enum EClientFrameStage : int
{
	FRAME_NET_UPDATE_POSTDATAUPDATE_START = 2,
	FRAME_NET_UPDATE_POSTDATAUPDATE_END,
	FRAME_NET_FULL_FRAME_UPDATE_ON_REMOVE,
	FRAME_RENDER_START,
	FRAME_RENDER_END,
	FRAME_NET_UPDATE_END,
	FRAME_NET_CREATION,
	FRAME_RESTORE_SERVER_STATE,
	FRAME_SIMULATE_END
};

enum EFlags : int
{
	FL_ONGROUND = (1 << 0), // entity is at rest / on the ground
	FL_DUCKING = (1 << 1), // player is fully crouched/uncrouched
	FL_ANIMDUCKING = (1 << 2), // player is in the process of crouching or uncrouching but could be in transition
	FL_WATERJUMP = (1 << 3), // player is jumping out of water
	FL_ONTRAIN = (1 << 4), // player is controlling a train, so movement commands should be ignored on client during prediction
	FL_INRAIN = (1 << 5), // entity is standing in rain
	FL_FROZEN = (1 << 6), // player is frozen for 3rd-person camera
	FL_ATCONTROLS = (1 << 7), // player can't move, but keeps key inputs for controlling another entity
	FL_CLIENT = (1 << 8), // entity is a client (player)
	FL_FAKECLIENT = (1 << 9), // entity is a fake client, simulated server side; don't send network messages to them
	FL_INWATER = (1 << 10), // entity is in water
	FL_FLY = (1 << 11),
	FL_SWIM = (1 << 12),
	FL_CONVEYOR = (1 << 13),
	FL_NPC = (1 << 14),
	FL_GODMODE = (1 << 15),
	FL_NOTARGET = (1 << 16),
	FL_AIMTARGET = (1 << 17),
	FL_PARTIALGROUND = (1 << 18), // entity is standing on a place where not all corners are valid
	FL_STATICPROP = (1 << 19), // entity is a static property
	FL_GRAPHED = (1 << 20),
	FL_GRENADE = (1 << 21),
	FL_STEPMOVEMENT = (1 << 22),
	FL_DONTTOUCH = (1 << 23),
	FL_BASEVELOCITY = (1 << 24), // entity have applied base velocity this frame
	FL_WORLDBRUSH = (1 << 25), // entity is not moveable/removeable brush (part of the world, but represented as an entity for transparency or something)
	FL_OBJECT = (1 << 26),
	FL_KILLME = (1 << 27), // entity is marked for death and will be freed by the game
	FL_ONFIRE = (1 << 28),
	FL_DISSOLVING = (1 << 29),
	FL_TRANSRAGDOLL = (1 << 30), // entity is turning into client-side ragdoll
	FL_UNBLOCKABLE_BY_PLAYER = (1 << 31)
};

// @source: server.dll
enum ECommandButtons : std::uint64_t
{
	IN_ATTACK = 1 << 0,
	IN_JUMP = 1 << 1,
	IN_DUCK = 1 << 2,
	IN_FORWARD = 1 << 3,
	IN_BACK = 1 << 4,
	IN_USE = 1 << 5,
	IN_LEFT = 1 << 7,
	IN_RIGHT = 1 << 8,
	IN_MOVELEFT = 1 << 9,
	IN_MOVERIGHT = 1 << 10,
	IN_SECOND_ATTACK = 1 << 11,
	IN_RELOAD = 1 << 13,
	IN_SPRINT = 1 << 16,
	IN_JOYAUTOSPRINT = 1 << 17,
	IN_SHOWSCORES = 1ULL << 33,
	IN_ZOOM = 1ULL << 34,
	IN_LOOKATWEAPON = 1ULL << 35
};

// compiled protobuf messages and looked at what bits are used in them
enum ESubtickMoveStepBits : std::uint32_t
{
	MOVESTEP_BITS_BUTTON = 0x1U,
	MOVESTEP_BITS_PRESSED = 0x2U,
	MOVESTEP_BITS_WHEN = 0x4U,
	MOVESTEP_BITS_ANALOG_FORWARD_DELTA = 0x8U,
	MOVESTEP_BITS_ANALOG_LEFT_DELTA = 0x10U
};

enum EInputHistoryBits : std::uint32_t
{
	INPUT_HISTORY_BITS_VIEWANGLES = 0x1U,
	INPUT_HISTORY_BITS_SHOOTPOSITION = 0x2U,
	INPUT_HISTORY_BITS_TARGETHEADPOSITIONCHECK = 0x4U,
	INPUT_HISTORY_BITS_TARGETABSPOSITIONCHECK = 0x8U,
	INPUT_HISTORY_BITS_TARGETANGCHECK = 0x10U,
	INPUT_HISTORY_BITS_CL_INTERP = 0x20U,
	INPUT_HISTORY_BITS_SV_INTERP0 = 0x40U,
	INPUT_HISTORY_BITS_SV_INTERP1 = 0x80U,
	INPUT_HISTORY_BITS_PLAYER_INTERP = 0x100U,
	INPUT_HISTORY_BITS_RENDERTICKCOUNT = 0x200U,
	INPUT_HISTORY_BITS_RENDERTICKFRACTION = 0x400U,
	INPUT_HISTORY_BITS_PLAYERTICKCOUNT = 0x800U,
	INPUT_HISTORY_BITS_PLAYERTICKFRACTION = 0x1000U,
	INPUT_HISTORY_BITS_FRAMENUMBER = 0x2000U,
	INPUT_HISTORY_BITS_TARGETENTINDEX = 0x4000U
};

enum EButtonStatePBBits : uint32_t
{
	BUTTON_STATE_PB_BITS_BUTTONSTATE1 = 0x1U,
	BUTTON_STATE_PB_BITS_BUTTONSTATE2 = 0x2U,
	BUTTON_STATE_PB_BITS_BUTTONSTATE3 = 0x4U
};

enum EBaseCmdBits : std::uint32_t
{
	BASE_BITS_MOVE_CRC = 0x1U,
	BASE_BITS_BUTTONPB = 0x2U,
	BASE_BITS_VIEWANGLES = 0x4U,
	BASE_BITS_COMMAND_NUMBER = 0x8U,
	BASE_BITS_CLIENT_TICK = 0x10U,
	BASE_BITS_FORWARDMOVE = 0x20U,
	BASE_BITS_LEFTMOVE = 0x40U,
	BASE_BITS_UPMOVE = 0x80U,
	BASE_BITS_IMPULSE = 0x100U,
	BASE_BITS_WEAPON_SELECT = 0x200U,
	BASE_BITS_RANDOM_SEED = 0x400U,
	BASE_BITS_MOUSEDX = 0x800U,
	BASE_BITS_MOUSEDY = 0x1000U,
	BASE_BITS_CONSUMED_SERVER_ANGLE = 0x2000U,
	BASE_BITS_CMD_FLAGS = 0x4000U,
	BASE_BITS_ENTITY_HANDLE = 0x8000U
};

enum ECSGOUserCmdBits : std::uint32_t
{
	CSGOUSERCMD_BITS_BASECMD = 0x1U,
	CSGOUSERCMD_BITS_LEFTHAND = 0x2U,
	CSGOUSERCMD_BITS_PREDICTING_BODY_SHOT = 0x4U,
	CSGOUSERCMD_BITS_PREDICTING_HEAD_SHOT = 0x8U,
	CSGOUSERCMD_BITS_PREDICTING_KILL_RAGDOLLS = 0x10U,
	CSGOUSERCMD_BITS_ATTACK3START = 0x20U,
	CSGOUSERCMD_BITS_ATTACK1START = 0x40U,
	CSGOUSERCMD_BITS_ATTACK2START = 0x80U
};

template <typename T>
struct RepeatedPtrField_t
{
	struct Rep_t
	{
		int nAllocatedSize;
		T* tElements[(1024 * 1024 - sizeof(int)) / sizeof(T*)];  // ~1MB max
	};

	void* pArena;
	int nCurrentSize;
	int nTotalSize;
	Rep_t* pRep;

	// @ida: #STR: "cl: CreateMove clamped invalid attack h" go down a bit and you will find it
	// @ida: #STR: "cl: CreateMove - Invalid player history [ %d, %d, %.3f ] f
	template <typename T>
	T* add(T* element)
	{
		// Define the function pointer correctly
		static auto add_to_rep_addr = reinterpret_cast<T * (__fastcall*)(RepeatedPtrField_t*, T*)>(M::GetAbsoluteAddress(M::FindPattern("client", "E8 ? ? ? ? 48 8B C8 8B 51"), 0x1));

		// Use the function pointer to call the function
		return add_to_rep_addr(this, element);
	}
};

class CBasePB
{
public:
	MEM_PAD(0x8); // 0x0 VTABLE
	std::uint32_t nHasBits; // 0x8
	// MSVC aligns nCachedBits to 0x10 (same as IDA CSubtickMoveStep)
	std::uint64_t nCachedBits; // 0x10

	void SetBits(std::uint64_t nBits)
	{
		nCachedBits |= nBits;
	}
};

class CMsgQAngle : public CBasePB
{
public:
	QAngle_t angValue; // 0x18
};

class CMsgVector : public CBasePB
{
public:
	Vector4D_t vecValue; // 0x18
};

class CCSGOInterpolationInfoPB : public CBasePB
{
public:
	float flFraction; // 0x18
	int nSrcTick; // 0x1C
	int nDstTick; // 0x20
};

class CCSGOInputHistoryEntryPB : public CBasePB
{
public:
	CMsgQAngle* pViewAngles; // 0x18
	CMsgVector* pShootPosition; // 0x20
	CMsgVector* pTargetHeadPositionCheck; // 0x28
	CMsgVector* pTargetAbsPositionCheck; // 0x30
	CMsgQAngle* pTargetAngPositionCheck; // 0x38
	CCSGOInterpolationInfoPB* cl_interp; // 0x40
	CCSGOInterpolationInfoPB* sv_interp0; // 0x48
	CCSGOInterpolationInfoPB* sv_interp1; // 0x50
	CCSGOInterpolationInfoPB* player_interp; // 0x58
	int nRenderTickCount; // 0x60
	float flRenderTickFraction; // 0x64
	int nPlayerTickCount; // 0x68
	float flPlayerTickFraction; // 0x6C
	int nFrameNumber; // 0x70
	int nTargetEntIndex; // 0x74
};

struct CInButtonStatePB : CBasePB
{
	std::uint64_t nValue;
	std::uint64_t nValueChanged;
	std::uint64_t nValueScroll;
};

struct CSubtickMoveStep : CBasePB
{
public:
	// IDA ctor 0x1804E34D0 + fill 0x180B0994F (size 56):
	//   +0x10 presence (nCachedBits), +0x18 button, +0x20 pressed, +0x24 when
	std::uint64_t nButton;           // 0x18
	bool bPressed;                   // 0x20
	float flWhen;                    // 0x24
	float flAnalogForwardDelta;      // 0x28
	float flAnalogLeftDelta;         // 0x2C
	std::uint32_t padEnd[2];         // 0x30..0x37 → sizeof 56
};

static_assert(offsetof(CSubtickMoveStep, nButton) == 0x18, "CSubtickMoveStep.nButton");
static_assert(offsetof(CSubtickMoveStep, bPressed) == 0x20, "CSubtickMoveStep.bPressed");
static_assert(offsetof(CSubtickMoveStep, flWhen) == 0x24, "CSubtickMoveStep.flWhen");
static_assert(sizeof(CSubtickMoveStep) == 56, "CSubtickMoveStep size");

class CBaseUserCmdExecutionNotes : public CBasePB
{
public:
	std::string* m_pIgnoredReason;
};

class CBaseUserCmdPB : public CBasePB
{
public:
	RepeatedPtrField_t<CSubtickMoveStep> subtickMovesField;
	std::string* strMoveCrc;
	CInButtonStatePB* pInButtonState;
	CMsgQAngle* pViewAngles;
	CBaseUserCmdExecutionNotes* m_pExecutionNotes;
	std::int32_t nLegacyCommandNumber;
	int nClientTick;
	float flForwardMove;
	float flSideMove;
	float flUpMove;
	std::int32_t nImpulse;
	std::int32_t nWeaponSelect;
	std::int32_t nRandomSeed;
	std::int32_t nMousedX;
	std::int32_t nMousedY;
	std::uint32_t m_uPredictionOffsetTicksx256;
	std::uint32_t m_uConsumedServerAngleChanges;
	int m_nCmdFlags;
	std::uint32_t m_uPawnEntityHandle;

	CSubtickMoveStep* add_subtick_move()
	{
		using fn_create = CSubtickMoveStep* (__fastcall*)(void* arena);
		using fn_add = CSubtickMoveStep* (__fastcall*)(void* repeated, CSubtickMoveStep* step);

		// IDA a2bd4708 @ 0x180B09920-0x180B0994C:
		//   reuse: if pRep && nCurrentSize < *pRep → take slot, ++nCurrentSize
		//   else:  CreateNewSubtickMoveStep(pArena) → RepeatedPtrField::Add(field, step)
		// Presence bits written to step+0x10 (nCachedBits), button@+0x18, pressed@+0x20, when@+0x24
		static fn_create create_fn = nullptr;
		static fn_add add_fn = nullptr;
		if (!create_fn || !add_fn) {
			std::uint8_t* create_hit = M::FindPattern("client",
				"E8 ? ? ? ? 48 8B D0 48 8B CE E8 ? ? ? ? 48 8B C8");
			if (create_hit) {
				create_fn = reinterpret_cast<fn_create>(M::GetAbsoluteAddress(create_hit, 0x1));
				// second E8 in the same sequence is RepeatedPtrField::Add
				std::uint8_t* add_hit = create_hit + 0xB; // E8 at +0xB from pattern start? 
				// pattern: E8(0) +4 rel = 5, 48 8B D0 (3) = 8, 48 8B CE (3) = 11 = 0xB, E8 at 0xB
				add_fn = reinterpret_cast<fn_add>(M::GetAbsoluteAddress(add_hit, 0x1));
			}
		}
		if (!create_fn || !add_fn)
			return nullptr;

		auto& field = subtickMovesField;
		if (field.pRep) {
			const int cur = field.nCurrentSize;
			const int cap = field.pRep->nAllocatedSize;
			if (cur >= 0 && cur < cap && cur < 64) {
				CSubtickMoveStep* slot = field.pRep->tElements[cur];
				field.nCurrentSize = cur + 1;
				if (slot)
					return slot;
			}
		}

		// MUST pass pArena — nullptr bypasses protobuf arena and crashes on serialize (uc4)
		CSubtickMoveStep* step = create_fn(field.pArena);
		if (!step)
			return nullptr;
		return add_fn(&field, step);
	}

	int CalculateCmdCRCSize()
	{
		return M::CallVFunc<int, 7U>(this);
	}
};

class CCSGOUserCmdPB
{
public:
	enum : std::uint32_t {
		BITS_BASECMD = 0x1u,
		BITS_LEFTHAND = 0x2u,
		BITS_ATTACK3START = 0x4u,
		BITS_ATTACK1START = 0x8u,
		BITS_ATTACK2START = 0x10u
	};

	std::uint32_t nHasBits;
	std::uint64_t nCachedSize;
	RepeatedPtrField_t<CCSGOInputHistoryEntryPB> inputHistoryField;
	CBaseUserCmdPB* pBaseCmd;
	bool bLeftHandDesired;
	std::int32_t nAttack3StartHistoryIndex;
	std::int32_t nAttack1StartHistoryIndex;
	std::int32_t nAttack2StartHistoryIndex;

	// @note: this function is used to check if the bits are set and set them if they are not
	void CheckAndSetBits(std::uint32_t nBits)
	{
		if (!(nHasBits & nBits))
			nHasBits |= nBits;
	}

	void SetAttack1StartHistoryIndex(std::int32_t idx)
	{
		nAttack1StartHistoryIndex = idx;
		// Marusense-style mirror bit (0x8) + google-protobuf field-6 bit (0x20).
		// Missing hasbit → server keeps default -1 → seed desync / always miss.
		CheckAndSetBits(BITS_ATTACK1START | 0x20u);
	}
};

struct CInButtonState
{
public:
	MEM_PAD(0x8); // vtable
	std::uint64_t nValue;
	std::uint64_t nValueChanged;
	std::uint64_t nValueScroll;
};

class CUserCmd
{
public:
	MEM_PAD(0x8); // vtable
	MEM_PAD(0x10);
	CCSGOUserCmdPB csgoUserCmd;
	CInButtonState nButtons;

	CCSGOInputHistoryEntryPB* GetInputHistoryEntry(int nIndex)
	{
		if (!csgoUserCmd.inputHistoryField.pRep)
			return nullptr;
		if (nIndex < 0 || nIndex >= csgoUserCmd.inputHistoryField.nCurrentSize)
			return nullptr;
		if (nIndex >= csgoUserCmd.inputHistoryField.pRep->nAllocatedSize)
			return nullptr;

		return csgoUserCmd.inputHistoryField.pRep->tElements[nIndex];
	}

	void SetSubTickAngle(const QAngle_t& angView)
	{
		if (!csgoUserCmd.inputHistoryField.pRep)
			return;

		const int count = csgoUserCmd.inputHistoryField.nCurrentSize;
		if (count <= 0)
			return;
		// Only stamp newest slots (attack subtick). Rewriting the full history
		// desyncs client/server angle streams and breaks silent aim.
		const int start = (count > 2) ? (count - 2) : 0;
		for (int i = start; i < count; i++)
		{
			CCSGOInputHistoryEntryPB* pInputEntry = this->GetInputHistoryEntry(i);
			if (!pInputEntry || !pInputEntry->pViewAngles)
				continue;

			pInputEntry->pViewAngles->angValue = angView;
			pInputEntry->SetBits(EInputHistoryBits::INPUT_HISTORY_BITS_VIEWANGLES);
		}
	}

	// UC / server weapon-fire path: stamp the exact eye used for the client trace
	// into input_history.pShootPosition so server rewound fire matches (diff>1 → server pos).
	void SetSubTickShootPosition(const Vector_t& shootPos)
	{
		if (!csgoUserCmd.inputHistoryField.pRep)
			return;
		const int count = csgoUserCmd.inputHistoryField.nCurrentSize;
		if (count <= 0)
			return;
		const int start = (count > 2) ? (count - 2) : 0;
		for (int i = start; i < count; ++i) {
			CCSGOInputHistoryEntryPB* e = this->GetInputHistoryEntry(i);
			if (!e || !e->pShootPosition)
				continue;
			e->pShootPosition->vecValue.x = shootPos.x;
			e->pShootPosition->vecValue.y = shootPos.y;
			e->pShootPosition->vecValue.z = shootPos.z;
			e->pShootPosition->vecValue.w = 0.f;
			e->SetBits(EInputHistoryBits::INPUT_HISTORY_BITS_SHOOTPOSITION);
		}
	}

	void SetAttackHistoryFire(int histIndex, const QAngle_t& angView, const Vector_t& shootPos)
	{
		const int count = csgoUserCmd.inputHistoryField.nCurrentSize;
		if (count <= 0)
			return;
		int idx = histIndex;
		if (idx < 0 || idx >= count)
			idx = count - 1;
		csgoUserCmd.SetAttack1StartHistoryIndex(idx);

		CCSGOInputHistoryEntryPB* e = GetInputHistoryEntry(idx);
		if (!e)
			return;
		if (e->pViewAngles) {
			e->pViewAngles->angValue = angView;
			e->SetBits(EInputHistoryBits::INPUT_HISTORY_BITS_VIEWANGLES);
		}
		if (e->pShootPosition) {
			e->pShootPosition->vecValue.x = shootPos.x;
			e->pShootPosition->vecValue.y = shootPos.y;
			e->pShootPosition->vecValue.z = shootPos.z;
			e->pShootPosition->vecValue.w = 0.f;
			e->SetBits(EInputHistoryBits::INPUT_HISTORY_BITS_SHOOTPOSITION);
		}
	}
};


