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

// has_bit index → mask (cs2-sdk / IDA WriteSubtickFromEntry)
enum EInputHistoryBits : std::uint32_t
{
	INPUT_HISTORY_BITS_VIEWANGLES = 0x1U,             // bit 0
	INPUT_HISTORY_BITS_CL_INTERP = 0x2U,               // bit 1
	INPUT_HISTORY_BITS_SV_INTERP0 = 0x4U,              // bit 2
	INPUT_HISTORY_BITS_SV_INTERP1 = 0x8U,              // bit 3
	INPUT_HISTORY_BITS_PLAYER_INTERP = 0x10U,          // bit 4
	INPUT_HISTORY_BITS_SHOOTPOSITION = 0x20U,          // bit 5
	INPUT_HISTORY_BITS_TARGETHEADPOSITIONCHECK = 0x40U,// bit 6
	INPUT_HISTORY_BITS_TARGETABSPOSITIONCHECK = 0x80U, // bit 7
	INPUT_HISTORY_BITS_TARGETANGCHECK = 0x100U,        // bit 8
	INPUT_HISTORY_BITS_RENDERTICKCOUNT = 0x200U,       // bit 9
	INPUT_HISTORY_BITS_RENDERTICKFRACTION = 0x400U,    // bit 10
	INPUT_HISTORY_BITS_PLAYERTICKCOUNT = 0x800U,       // bit 11
	INPUT_HISTORY_BITS_PLAYERTICKFRACTION = 0x1000U,   // bit 12
	INPUT_HISTORY_BITS_FRAMENUMBER = 0x2000U,          // bit 13
	INPUT_HISTORY_BITS_TARGETENTINDEX = 0x4000U        // bit 14
};

enum EButtonStatePBBits : uint32_t
{
	BUTTON_STATE_PB_BITS_BUTTONSTATE1 = 0x1U,
	BUTTON_STATE_PB_BITS_BUTTONSTATE2 = 0x2U,
	BUTTON_STATE_PB_BITS_BUTTONSTATE3 = 0x4U
};

// CBaseUserCmdPB has_bits — IDA copy-ctor 0x1804E3230 (NOT sdk dump field-number order)
enum EBaseCmdBits : std::uint32_t
{
	BASE_BITS_MOVE_CRC = 0x1U,
	BASE_BITS_BUTTONPB = 0x2U,
	BASE_BITS_VIEWANGLES = 0x4U,
	BASE_BITS_EXECUTION_NOTES = 0x8U,
	BASE_BITS_COMMAND_NUMBER = 0x10U,
	BASE_BITS_CLIENT_TICK = 0x20U,
	BASE_BITS_FORWARDMOVE = 0x40U,
	BASE_BITS_LEFTMOVE = 0x80U,
	BASE_BITS_UPMOVE = 0x100U,
	BASE_BITS_IMPULSE = 0x200U,
	BASE_BITS_WEAPON_SELECT = 0x400U,
	BASE_BITS_RANDOM_SEED = 0x800U,
	BASE_BITS_MOUSEDX = 0x1000U,
	BASE_BITS_MOUSEDY = 0x2000U,
	BASE_BITS_ENTITY_HANDLE = 0x4000U,
	BASE_BITS_CONSUMED_SERVER_ANGLE = 0x8000U,
	BASE_BITS_CMD_FLAGS = 0x10000U
};

enum ECSGOUserCmdBits : std::uint32_t
{
	CSGOUSERCMD_BITS_BASECMD = 0x1U,
	CSGOUSERCMD_BITS_LEFTHAND = 0x2U,
	CSGOUSERCMD_BITS_PREDICTING_BODY_SHOT = 0x4U,
	CSGOUSERCMD_BITS_PREDICTING_HEAD_SHOT = 0x8U,
	CSGOUSERCMD_BITS_PREDICTING_KILL_RAGDOLLS = 0x10U,
	CSGOUSERCMD_BITS_ATTACK1START = 0x20U,
	CSGOUSERCMD_BITS_ATTACK2START = 0x40U
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

// IDA WriteSubtickFromEntry / cs2-sdk CSGOInputHistoryEntryPB (sizeof 0x78)
class CCSGOInputHistoryEntryPB : public CBasePB
{
public:
	CMsgQAngle* pViewAngles;                         // 0x18
	CCSGOInterpolationInfoPB* cl_interp;             // 0x20
	CCSGOInterpolationInfoPB* sv_interp0;            // 0x28
	CCSGOInterpolationInfoPB* sv_interp1;            // 0x30
	CCSGOInterpolationInfoPB* player_interp;         // 0x38
	CMsgVector* pShootPosition;                      // 0x40
	CMsgVector* pTargetHeadPositionCheck;            // 0x48
	CMsgVector* pTargetAbsPositionCheck;             // 0x50
	CMsgQAngle* pTargetAngPositionCheck;             // 0x58
	int nRenderTickCount;                            // 0x60
	float flRenderTickFraction;                      // 0x64
	int nPlayerTickCount;                            // 0x68
	float flPlayerTickFraction;                      // 0x6C
	int nFrameNumber;                                // 0x70
	int nTargetEntIndex;                             // 0x74
};
static_assert(offsetof(CCSGOInputHistoryEntryPB, pViewAngles) == 0x18, "hist.view");
static_assert(offsetof(CCSGOInputHistoryEntryPB, pShootPosition) == 0x40, "hist.shoot");
static_assert(sizeof(CCSGOInputHistoryEntryPB) == 0x78, "hist.size");

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

// IDA CBaseUserCmdPB ctor 0x1804E31B0 / copy 0x1804E3230
// Memory order ≠ protobuf field numbers (sdk dump was wrong here).
class CBaseUserCmdPB : public CBasePB
{
public:
	RepeatedPtrField_t<CSubtickMoveStep> subtickMovesField; // 0x18
	std::string* strMoveCrc;                                // 0x30
	CInButtonStatePB* pInButtonState;                       // 0x38  hasbit 0x2
	CMsgQAngle* pViewAngles;                                // 0x40  hasbit 0x4
	CBaseUserCmdExecutionNotes* m_pExecutionNotes;          // 0x48  hasbit 0x8
	std::int32_t nLegacyCommandNumber;                      // 0x50
	int nClientTick;                                        // 0x54
	float flForwardMove;                                    // 0x58
	float flSideMove;                                       // 0x5C
	float flUpMove;                                         // 0x60
	std::int32_t nImpulse;                                  // 0x64
	std::int32_t nWeaponSelect;                             // 0x68
	std::int32_t nRandomSeed;                               // 0x6C
	std::int32_t nMousedX;                                  // 0x70
	std::int32_t nMousedY;                                  // 0x74
	std::uint32_t m_uConsumedServerAngleChanges;            // 0x78
	int m_nCmdFlags;                                        // 0x7C
	std::uint32_t m_uPredictionOffsetTicksx256;             // 0x80
	std::uint32_t m_uPawnEntityHandle;                      // 0x84 default 0xFFFFFF

	CSubtickMoveStep* add_subtick_move()
	{
		using fn_create = CSubtickMoveStep* (__fastcall*)(void* arena);
		using fn_add = CSubtickMoveStep* (__fastcall*)(void* repeated, CSubtickMoveStep* step);

		// IDA CreateMove fill @ 0x180B09D5A (unique pattern hit 0x180B09D6C):
		//   if cur < alloc: reuse tElements[cur++]
		//   else: step = CreateNewSubtickMoveStep(arena) @ 0x1804E23F0
		//         step = ProtobufAddToRepeated(field, step) @ 0x181195510
		// REJECTED UC post client+0x9CA720 — that is interp debug, not this factory.
		static fn_create create_fn = nullptr;
		static fn_add add_fn = nullptr;
		static bool resolved = false;
		if (!resolved) {
			resolved = true;
			std::uint8_t* create_hit = M::FindPattern("client",
				"E8 ? ? ? ? 48 8B D0 48 8B CE E8 ? ? ? ? 48 8B C8");
			if (create_hit) {
				create_fn = reinterpret_cast<fn_create>(M::GetAbsoluteAddress(create_hit, 0x1));
				// second E8 is +0xB (5 + 3 + 3)
				add_fn = reinterpret_cast<fn_add>(M::GetAbsoluteAddress(create_hit + 0xB, 0x1));
			}
		}
		if (!create_fn || !add_fn)
			return nullptr;

		auto& field = subtickMovesField;
		// Hard cap — engine ProcessSubTick path uses up to 32-ish; stay under
		if (field.nCurrentSize >= 30)
			return nullptr;

		if (field.pRep) {
			const int cur = field.nCurrentSize;
			const int cap = field.pRep->nAllocatedSize;
			if (cur >= 0 && cur < cap) {
				CSubtickMoveStep* slot = field.pRep->tElements[cur];
				if (slot) {
					field.nCurrentSize = cur + 1;
					if (field.nTotalSize < field.nCurrentSize)
						field.nTotalSize = field.nCurrentSize;
					return slot;
				}
			}
		}

		// nullptr arena → heap alloc 56 + CSubtickMoveStep ctor (IDA 0x1804E23F0)
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
static_assert(offsetof(CBaseUserCmdPB, pInButtonState) == 0x38, "base.buttons");
static_assert(offsetof(CBaseUserCmdPB, pViewAngles) == 0x40, "base.viewangles");
static_assert(offsetof(CBaseUserCmdPB, flForwardMove) == 0x58, "base.forward");
static_assert(offsetof(CBaseUserCmdPB, flSideMove) == 0x5C, "base.leftmove");
static_assert(offsetof(CBaseUserCmdPB, m_uPawnEntityHandle) == 0x84, "base.pawn");
static_assert(sizeof(CBaseUserCmdPB) == 0x88, "base.size");

// IDA CreateMove / cs2-sdk CSGOUserCmdPB (sizeof 0x48, has_bits @ 0x10)
class CCSGOUserCmdPB
{
public:
	enum : std::uint32_t {
		BITS_BASECMD = 0x1u,
		BITS_LEFTHAND = 0x2u,
		BITS_PREDICTING_BODY = 0x4u,
		BITS_PREDICTING_HEAD = 0x8u,
		BITS_PREDICTING_KILL = 0x10u,
		BITS_ATTACK1START = 0x20u,
		BITS_ATTACK2START = 0x40u
	};

	MEM_PAD(0x10);
	std::uint32_t nHasBits; // 0x10
	MEM_PAD(0x4);
	RepeatedPtrField_t<CCSGOInputHistoryEntryPB> inputHistoryField; // 0x18
	CBaseUserCmdPB* pBaseCmd;                                       // 0x30
	bool bLeftHandDesired;                                          // 0x38
	bool bPredictingBodyShotFx;                                     // 0x39
	bool bPredictingHeadShotFx;                                     // 0x3a
	bool bPredictingKillRagdolls;                                   // 0x3b
	std::int32_t nAttack1StartHistoryIndex;                         // 0x3c
	std::int32_t nAttack2StartHistoryIndex;                         // 0x40
	MEM_PAD(0x4);

	void CheckAndSetBits(std::uint32_t nBits)
	{
		if (!(nHasBits & nBits))
			nHasBits |= nBits;
	}

	void SetAttack1StartHistoryIndex(std::int32_t idx)
	{
		nAttack1StartHistoryIndex = idx;
		// IDA CreateMove: hasbit 0x20 for attack1_start_history_index
		CheckAndSetBits(BITS_ATTACK1START);
	}
};
static_assert(offsetof(CCSGOUserCmdPB, nHasBits) == 0x10, "cmd.has");
static_assert(offsetof(CCSGOUserCmdPB, inputHistoryField) == 0x18, "cmd.hist");
static_assert(offsetof(CCSGOUserCmdPB, pBaseCmd) == 0x30, "cmd.base");
static_assert(offsetof(CCSGOUserCmdPB, nAttack1StartHistoryIndex) == 0x3c, "cmd.atk1");
static_assert(offsetof(CCSGOUserCmdPB, nAttack2StartHistoryIndex) == 0x40, "cmd.atk2");
static_assert(sizeof(CCSGOUserCmdPB) == 0x48, "cmd.size");

struct CInButtonState
{
public:
	MEM_PAD(0x8); // vtable
	std::uint64_t nValue;
	std::uint64_t nValueChanged;
	std::uint64_t nValueScroll;
};

// IDA: CUserCmd stride 152 (0x98); seq @ +8; CSGOUserCmdPB host @ +0x10; buttons @ +0x58
// RunCommand reads cmd type/flags around +0x94 (a2+148).
class CUserCmd
{
public:
	MEM_PAD(0x8); // vtable
	std::int32_t nCommandNumber; // +0x8 (sequence check in GetCUserCmdBySequenceNumber)
	MEM_PAD(0x4);
	CCSGOUserCmdPB csgoUserCmd; // +0x10
	CInButtonState nButtons;    // +0x58
	MEM_PAD(0x10);              // +0x78
	bool bHasBeenPredicted;     // +0x88 — 1.1.5 pred clears this around Start
	MEM_PAD(0x3);
	std::int32_t nPredictionCmdType; // +0x8C
	std::int32_t nCmdType;           // +0x90
	std::int32_t nCmdFlag;           // +0x94

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

	// IDA FillGunFireData 0x1807CFDD0: can INTERPOLATE between consecutive
	// input_history slots (mode 1) for fire ang/eye. Stamping ONE slot while
	// neighbor keeps cam angles → lerp mid-shot → wrong SPREADSEEDGEN + model flick.
	// Stamp primary + neighbors with same ang/eye; bind attack1 index to primary.
	void SetAttackHistoryFire(int histIndex, const QAngle_t& angView, const Vector_t& shootPos)
	{
		if (!csgoUserCmd.inputHistoryField.pRep)
			return;
		const int count = csgoUserCmd.inputHistoryField.nCurrentSize;
		if (count <= 0)
			return;
		int idx = histIndex;
		if (idx < 0 || idx >= count)
			idx = count - 1;
		csgoUserCmd.SetAttack1StartHistoryIndex(idx);

		const int lo = (idx > 0) ? (idx - 1) : idx;
		const int hi = (idx + 1 < count) ? (idx + 1) : idx;
		// Also cover newest tip if primary is older (interp with last)
		const int tip = count - 1;
		const int tipLo = (tip > 0) ? tip - 1 : tip;
		const int lo2 = (lo < tipLo) ? lo : tipLo;
		const int hi2 = (hi > tip) ? hi : tip;

		CCSGOInputHistoryEntryPB* primary = GetInputHistoryEntry(idx);
		const int keepTick = (primary && primary->nPlayerTickCount > 0)
			? primary->nPlayerTickCount : 0;
		const float keepFrac = (primary && std::isfinite(primary->flPlayerTickFraction))
			? primary->flPlayerTickFraction : 0.f;

		for (int i = lo2; i <= hi2; ++i) {
			CCSGOInputHistoryEntryPB* e = GetInputHistoryEntry(i);
			if (!e)
				continue;
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
			// Same player tick on neighbors → interp degenerates to exact (mode 0 path).
			if (keepTick > 0) {
				e->nPlayerTickCount = keepTick;
				e->flPlayerTickFraction = keepFrac;
				if (e->nRenderTickCount <= 0
					|| (e->nRenderTickCount > keepTick
						? e->nRenderTickCount - keepTick
						: keepTick - e->nRenderTickCount) > 1) {
					e->nRenderTickCount = keepTick;
					e->flRenderTickFraction = keepFrac;
				}
				e->SetBits(
					EInputHistoryBits::INPUT_HISTORY_BITS_PLAYERTICKCOUNT
					| EInputHistoryBits::INPUT_HISTORY_BITS_PLAYERTICKFRACTION
					| EInputHistoryBits::INPUT_HISTORY_BITS_RENDERTICKCOUNT
					| EInputHistoryBits::INPUT_HISTORY_BITS_RENDERTICKFRACTION);
			}
		}
	}
};
static_assert(offsetof(CUserCmd, csgoUserCmd) == 0x10, "CUserCmd.pb");
static_assert(offsetof(CUserCmd, nButtons) == 0x58, "CUserCmd.buttons");


