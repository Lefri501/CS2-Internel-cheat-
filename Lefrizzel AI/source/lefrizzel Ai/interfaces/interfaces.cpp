#include "interfaces.h"
#include "CGameEntitySystem/CGameEntitySystem.h"
#include "CCSGOInput/CCSGOInput.h"
#include "../hooks/hooks.h"
#include "../features/trace/trace.h"
#include "../features/hitchance/hitchance.h"
#include "../features/nospread/nospread.h"
#include "../features/autowall/autowall.h"
#include "../features/bones/bones.h"
#include "../features/skinchanger/skinchanger.h"
#include "../features/prediction/prediction.h"
#include "../features/input_inject/input_inject.h"
#include "../features/subtick_move/subtick_move.h"
#include "../features/glow/glow.h"
#include "../features/nade_lineup/nade_lineup.h"
#include "../features/engine2/engine2.h"
#include "../features/w2s/w2s.h"
#include "../features/bomb/bomb.h"
#include "../features/knifebot/knifebot.h"
#include "../features/auto_pistol/auto_pistol.h"
#include "../features/enemy_spec/enemy_spec.h"
#include "../features/sdk_prio_a/sdk_prio_a.h"
#include "../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

// @used: I::Get<template>
#include "..\..\lefrizzel Ai\utils\memory\Interface\Interface.h"
#include "..\..\lefrizzel Ai\utils\console\console.h"

namespace {
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

CUserCmd* SehGetCmdBySeq(uintptr_t controller, int seq)
{
	if (!Input::GetCUserCmdBySequenceNumber || seq <= 0)
		return nullptr;
	__try {
		return reinterpret_cast<CUserCmd*>(Input::GetCUserCmdBySequenceNumber(controller, seq));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void* SehMoveServices(void* pawn)
{
	if (!pawn)
		return nullptr;
	__try {
		return reinterpret_cast<C_CSPlayerPawn*>(pawn)->m_pMovementServices();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool SehForceButtonsDown(void* moveSvc, std::uint64_t buttons)
{
	if (!Input::ForceButtonsDown || !moveSvc)
		return false;
	__try {
		Input::ForceButtonsDown(moveSvc, buttons);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

uintptr_t SehResolveLocalController()
{
	__try {
		C_CSPlayerPawn* pawn = H::SafeLocalPlayer();
		if (!pawn)
			return 0;
		CBaseHandle hCtrl = pawn->m_hController();
		if (!hCtrl.valid())
			return 0;
		if (!I::GameEntity || !I::GameEntity->Instance)
			return 0;
		CCSPlayerController* pCtrl =
			I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl.index());
		if (!pCtrl)
			return 0;
		return reinterpret_cast<uintptr_t>(pCtrl);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// Andromeda path: tick → array → sequence @ +0x5910 → GetBySeq
CUserCmd* SehGetUserCmdAndromeda(uintptr_t ctrl)
{
	if (!ctrl || !Input::GetCUserCmdBySequenceNumber)
		return nullptr;
	if (!Input::GetEntityCmdSlot || !Input::GetCUserCmdArray || !Input::ppUserCmdArrayTable)
		return nullptr;
	__try {
		void* table = SehReadPtr(Input::ppUserCmdArrayTable);
		if (!table)
			return nullptr;

		int outputTick = 0;
		Input::GetEntityCmdSlot(reinterpret_cast<void*>(ctrl), &outputTick);
		int tick = (outputTick == -1) ? -1 : (outputTick - 1);

		void* arr = Input::GetCUserCmdArray(table, tick);
		if (!arr)
			return nullptr;

		const int seq = *reinterpret_cast<int*>(
			reinterpret_cast<std::uintptr_t>(arr) + Input::kCmdArraySequenceOff);
		if (seq <= 0)
			return nullptr;

		CUserCmd* cmd = reinterpret_cast<CUserCmd*>(
			Input::GetCUserCmdBySequenceNumber(ctrl, seq));
		if (cmd)
			return cmd;
		// Retry seq-1 if ring just advanced
		if (seq > 1)
			return reinterpret_cast<CUserCmd*>(
				Input::GetCUserCmdBySequenceNumber(ctrl, seq - 1));
		return nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

CUserCmd* SehGetUserCmd(uintptr_t controller)
{
	if (!Input::GetCUserCmdBySequenceNumber)
		return nullptr;
	__try {
		uintptr_t ctrl = controller ? controller : SehResolveLocalController();
		if (!ctrl)
			return nullptr;

		// Path 1: full SetupCmd pattern (returns array sequence)
		if (Input::SetupCmd) {
			const int seq = Input::SetupCmd(ctrl);
			if (seq > 0) {
				if (CUserCmd* cmd = SehGetCmdBySeq(ctrl, seq))
					return cmd;
				if (seq > 1) {
					if (CUserCmd* cmd = SehGetCmdBySeq(ctrl, seq - 1))
						return cmd;
				}
			}
		}

		// Path 2: Andromeda GetCUserCmdTick + GetCUserCmdArray + sequence
		if (CUserCmd* cmd = SehGetUserCmdAndromeda(ctrl))
			return cmd;

		return nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Con::Seh("Input::get_user_cmd", GetExceptionCode());
		return nullptr;
	}
}
} // namespace

void Input::init()
{
	// dump: SetupCmd — full pattern with +0x5910 read (short form can hit siblings)
	SetupCmd = reinterpret_cast<decltype(SetupCmd)>(
		M::FindPattern("client", "48 83 EC 28 E8 ? ? ? ? 8B 80 10 59 00 00 48"));
	if (!SetupCmd)
		SetupCmd = reinterpret_cast<decltype(SetupCmd)>(
			M::FindPattern("client", "48 83 EC 28 E8 ? ? ? ? 8B 80"));
	// dump: GetCUserCmdBySequenceNumber — IDA 0x180901120 (elem size 152, backup 150)
	GetCUserCmdBySequenceNumber = reinterpret_cast<decltype(GetCUserCmdBySequenceNumber)>(
		M::FindPattern("client", "40 53 48 83 EC 20 8B DA E8 ? ? ? ? 4C 8B C0"));
	if (!GetCUserCmdBySequenceNumber)
		GetCUserCmdBySequenceNumber = reinterpret_cast<decltype(GetCUserCmdBySequenceNumber)>(
			M::FindPattern("client", "40 53 48 83 EC 20 8B DA E8 ? ? ? ? 4C"));
	GetControllerCmd = GetCUserCmdBySequenceNumber;

	// dump: GetCUserCmdArray — IDA 0x180901320 (allocates 150×CUserCmd ring)
	GetCUserCmdArray = reinterpret_cast<decltype(GetCUserCmdArray)>(
		M::FindPattern("client", "48 89 4C 24 08 41 56 41 57 48 83 EC 48 4C 63 FA"));

	// dump misname GetCUserCmdTick — entity → cmd slot index (IDA 0x18150F580)
	GetEntityCmdSlot = reinterpret_cast<decltype(GetEntityCmdSlot)>(
		M::FindPattern("client", "48 83 EC 08 4C 8B 0D ? ? ? ? 4C 8B DA 48 8B"));

	// dump: ForceButtonsDown — IDA 0x180A11230 (moveServices, buttonMask)
	ForceButtonsDown = reinterpret_cast<decltype(ForceButtonsDown)>(
		M::FindPattern("client", "40 53 57 41 56 48 81 EC 30 02 00 00 48 83 79 38"));

	// dump: GetViewAngles / SetViewAngles
	SetViewAngle = reinterpret_cast<decltype(SetViewAngle)>(
		M::FindPattern("client", "85 D2 75 3D 48 63 81 ? ? ? ?"));
	GetViewAngles = reinterpret_cast<decltype(GetViewAngles)>(
		M::FindPattern("client", "4C 8B C1 85 D2 74 08 48 8D 05 ? ? ? ? C3"));

	// dump: pCSGOInput — mov rcx/r8,[pCSGOInput]
	std::uint8_t* hit = M::FindPattern("client", "48 8B 0D ? ? ? ? 4C 8B C6 8B 10 E8");
	if (!hit)
		hit = M::FindPattern("client", "4C 8B 05 ? ? ? ? 41 8B 80 50 0B 00 00 85 C0");
	if (hit) {
		ppCSGOInput = reinterpret_cast<void**>(M::GetAbsoluteAddress(hit, 3, 0));
		pCSGOInput = SehReadPtr(ppCSGOInput);
	}

	// First CUserCmdArray table — IDA off_18207DF70 @ CreateMove path
	// was: ... 48 8B CF 48 8B F0  (mov rsi,rax) — now mov r15,rax
	std::uint8_t* arrHit = M::FindPattern("client",
		"48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CF 4C 8B F8 44 8B B0 10 59 00 00");
	if (!arrHit)
		arrHit = M::FindPattern("client",
			"48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CF 4C 8B F8");
	if (arrHit) {
		ppUserCmdArrayTable = reinterpret_cast<void**>(M::GetAbsoluteAddress(arrHit, 3, 0));
	}

	// View-angle context (existing project pattern)
	uintptr_t addr = M::patternScan("client", "48 8B 0D ? ? ? ? 8B D3 E8 ? ? ? ? F2 0F 10 00");
	if (addr) {
		uintptr_t globalAddr = M::getAbsoluteAddress(addr, 0x3);
		if (globalAddr)
			viewAngleContext = *reinterpret_cast<uintptr_t*>(globalAddr);
	}
	if (!viewAngleContext && pCSGOInput)
		viewAngleContext = reinterpret_cast<uintptr_t>(pCSGOInput);

	Con::Info(
		"Input SetupCmd=%p GetCmdBySeq=%p GetCmdArray=%p CmdTable=%p ForceBtn=%p pCSGOInput=%p",
		(void*)SetupCmd, (void*)GetCUserCmdBySequenceNumber, (void*)GetCUserCmdArray,
		ppUserCmdArrayTable ? *ppUserCmdArrayTable : nullptr,
		(void*)ForceButtonsDown, pCSGOInput);
	if (!SetupCmd) Con::OffsetMiss("Input::SetupCmd");
	if (!GetCUserCmdBySequenceNumber) Con::OffsetMiss("Input::GetCUserCmdBySequenceNumber");
	if (!GetCUserCmdArray) Con::OffsetMiss("Input::GetCUserCmdArray");
	if (!GetEntityCmdSlot) Con::OffsetMiss("Input::GetCUserCmdTick");
	if (!ppUserCmdArrayTable) Con::OffsetMiss("Input::UserCmdArrayTable");
	if (!ForceButtonsDown) Con::OffsetMiss("Input::ForceButtonsDown");
	if (!pCSGOInput) Con::OffsetMiss("Input::pCSGOInput");
	if (!SetViewAngle) Con::OffsetMiss("Input::SetViewAngle");
	if (!GetViewAngles) Con::OffsetMiss("Input::GetViewAngles");
}

void* Input::GetCSGOInput()
{
	pCSGOInput = SehReadPtr(ppCSGOInput);
	return pCSGOInput;
}

CUserCmd* Input::get_user_cmd_by_sequence(uintptr_t controller, int seq)
{
	return SehGetCmdBySeq(controller, seq);
}

CUserCmd* Input::get_user_cmd(uintptr_t controller)
{
	return SehGetUserCmd(controller);
}

bool Input::ForceButtons(void* pawn, std::uint64_t buttons)
{
	if (!pawn || !buttons)
		return false;
	void* moveSvc = SehMoveServices(pawn);
	if (!moveSvc)
		return false;
	return SehForceButtonsDown(moveSvc, buttons);
}

bool I::Interfaces::init()
{
    const HMODULE tier0_base = GetModuleHandleA("tier0.dll");
    if (!tier0_base)
        return false;

    bool success = true;

    // interfaces
    EngineClient = I::Get<IEngineClient>(("engine2.dll"), "Source2EngineToClient00");
    if (!EngineClient) Con::Error("Failed to get Source2EngineToClient00");
    success &= (EngineClient != nullptr);

    GameEntity = I::Get<IGameResourceService>(("engine2.dll"), "GameResourceServiceClientV00");
    if (!GameEntity) Con::Error("Failed to get GameResourceServiceClientV00");
    success &= (GameEntity != nullptr);

    // exports
    ConstructUtlBuffer = reinterpret_cast<decltype(ConstructUtlBuffer)>(GetProcAddress(tier0_base, "??0CUtlBuffer@@QEAA@HHW4BufferFlags_t@0@@Z"));
    if (!ConstructUtlBuffer) Con::Error("Failed to get ConstructUtlBuffer export");
    EnsureCapacityBuffer = reinterpret_cast<decltype(EnsureCapacityBuffer)>(GetProcAddress(tier0_base, "?EnsureCapacity@CUtlBuffer@@QEAAXH@Z"));
    if (!EnsureCapacityBuffer) Con::Error("Failed to get EnsureCapacityBuffer export");
    PutUtlString = reinterpret_cast<decltype(PutUtlString)>(GetProcAddress(tier0_base, "?PutString@CUtlBuffer@@QEAAXPEBD@Z"));
    if (!PutUtlString) Con::Error("Failed to get PutUtlString export");
    // IDA: real CreateMaterial ends `4C 8B F2 BA` (mov r14,rdx; mov edx,size).
    // Old pattern ended `48 8B F2` and hit sibling @ +0x1F0 that still works for KV
    // but dump / resource-binding path is the ADE0 one — prefer it, fall back to sibling.
    {
        uintptr_t cm = reinterpret_cast<uintptr_t>(M::FindPattern("materialsystem2.dll",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC 10 01 00 00 48 8B 05 ? ? ? ? 4C 8B F2 BA"));
        if (!cm)
            cm = reinterpret_cast<uintptr_t>(M::FindPattern("materialsystem2.dll",
                "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 8B F2"));
        CreateMaterial = reinterpret_cast<decltype(CreateMaterial)>(cm);
        if (!CreateMaterial) Con::OffsetMiss("CreateMaterial pattern");
    }
    // cdecl 6-arg string LoadKV3 — used by CKeyValues3::LoadFromBuffer
    LoadKeyValues = reinterpret_cast<decltype(LoadKeyValues)>(GetProcAddress(tier0_base,
        "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z"));
    if (!LoadKeyValues) Con::Error("Failed to get LoadKeyValues export");
    ConMsg = reinterpret_cast<decltype(ConMsg)>(GetProcAddress(tier0_base, "?ConMsg@@YAXPEBDZZ"));
    ConColorMsg = reinterpret_cast<decltype(ConColorMsg)>(GetProcAddress(tier0_base, "?ConColorMsg@@YAXAEBVColor@@PEBDZZ"));

    Con::Ok("Source2EngineToClient00: 0x%p", reinterpret_cast<void*>(EngineClient));
    Con::Ok("GameResourceServiceClientV00: 0x%p", reinterpret_cast<void*>(GameEntity));
    Con::Ok("CreateMaterial: 0x%p", reinterpret_cast<void*>(CreateMaterial));

    Input::init();

    if (!Trace::Init())
        Con::Error("Trace init failed (visibility disabled)");

    if (!Bones::Init())
        Con::Error("Bones init failed — LookupBone missing; geometry fallback only");

    if (!HitChance::Init())
        Con::Error("HitChance init failed — hitchance will retry / fail-closed when enabled");

    if (!NoSpread::Init())
        Con::Error("NoSpread init failed — seed nospread mode will wait / fail-closed");

    if (!AutoWall::Init())
        Con::Error("AutoWall init failed — mindamage uses TraceLine fallback / disabled");

    if (!SkinChanger::Init())
        Con::Error("SkinChanger init failed (knife changer disabled)");

    if (!Pred::Init())
        Con::Error("Pred init failed — local move sim / subtick addrs partial");

    if (!InputInject::Init())
        Con::Warn("InputInject init partial");

    if (!SubtickMove::Init())
        Con::Warn("SubtickMove init partial");

    if (!SdkPrioA::Init())
        Con::Warn("SdkPrioA init partial — Priority A patterns incomplete");

    Glow::Init();

    if (!Engine2::Init())
        Con::Warn("Engine2 init partial — map/in-game helpers may be limited");

    W2S::Init();
    Bomb::Init();
    KnifeBot::Init();
    AutoPistol::Init();
    EnemySpec::Init();

    NadeLineup::Init();

    // return status
    return success;
}
