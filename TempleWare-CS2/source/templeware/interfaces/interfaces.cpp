#include "interfaces.h"
#include "CGameEntitySystem/CGameEntitySystem.h"
#include "CCSGOInput/CCSGOInput.h"
#include "../hooks/hooks.h"
#include "../features/trace/trace.h"
#include "../features/hitchance/hitchance.h"
#include "../features/autowall/autowall.h"
#include "../features/bones/bones.h"
#include "../features/skinchanger/skinchanger.h"
#include "../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"

// @used: I::Get<template>
#include "..\..\templeware\utils\memory\Interface\Interface.h"
#include "..\..\templeware\utils\console\console.h"

void Input::init()
{
    SetupCmd = reinterpret_cast<decltype(SetupCmd)>(M::FindPattern("client", "48 83 EC 28 E8 ? ? ? ? 8B 80"));
    GetControllerCmd = reinterpret_cast<decltype(GetControllerCmd)>(M::FindPattern("client", "40 53 48 83 EC 20 8B DA E8 ? ? ? ? 4C"));
    SetViewAngle = reinterpret_cast<decltype(SetViewAngle)>(M::FindPattern("client", "85 D2 75 3D 48 63 81 ? ? ? ?"));
    GetViewAngles = reinterpret_cast<decltype(GetViewAngles)>(M::FindPattern("client", "4C 8B C1 85 D2 74 08 48 8D 05 ? ? ? ? C3"));

    Con::Info("Input SetupCmd=%p GetControllerCmd=%p SetViewAngle=%p GetViewAngles=%p",
        (void*)SetupCmd, (void*)GetControllerCmd, (void*)SetViewAngle, (void*)GetViewAngles);
    if (!SetupCmd) Con::OffsetMiss("Input::SetupCmd");
    if (!GetControllerCmd) Con::OffsetMiss("Input::GetControllerCmd");
    if (!SetViewAngle) Con::OffsetMiss("Input::SetViewAngle");
    if (!GetViewAngles) Con::OffsetMiss("Input::GetViewAngles");

    uintptr_t addr = M::patternScan("client", "48 8B 0D ? ? ? ? 8B D3 E8 ? ? ? ? F2 0F 10 00");
    if (addr) {
        uintptr_t globalAddr = M::getAbsoluteAddress(addr, 0x3);
        if (globalAddr)
            viewAngleContext = *reinterpret_cast<uintptr_t*>(globalAddr);
    }
}

CUserCmd* Input::get_user_cmd(uintptr_t controller)
{
    if (!GetControllerCmd || !SetupCmd)
        return nullptr;

    __try {
        uintptr_t ctrl = controller;

        // Resolve controller from local pawn->m_hController (NOT the pawn itself)
        if (!ctrl) {
            if (!H::oGetLocalPlayer)
                return nullptr;

            C_CSPlayerPawn* pawn = H::oGetLocalPlayer(0);
            if (!pawn)
                return nullptr;

            CBaseHandle hCtrl = pawn->m_hController();
            if (!hCtrl.valid())
                return nullptr;

            if (!I::GameEntity || !I::GameEntity->Instance)
                return nullptr;

            CCSPlayerController* pCtrl =
                I::GameEntity->Instance->Get<CCSPlayerController>(hCtrl.index());
            if (!pCtrl)
                return nullptr;

            ctrl = reinterpret_cast<uintptr_t>(pCtrl);
        }

        const int seq = SetupCmd(ctrl);
        if (seq < 0)
            return nullptr;

        return reinterpret_cast<CUserCmd*>(GetControllerCmd(ctrl, seq));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("Input::get_user_cmd", GetExceptionCode());
        return nullptr;
    }
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
    CreateMaterial = reinterpret_cast<decltype(CreateMaterial)>(M::FindPattern("materialsystem2.dll", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 8B F2"));
    if (!CreateMaterial) Con::OffsetMiss("CreateMaterial pattern");
    LoadKeyValues = reinterpret_cast<decltype(LoadKeyValues)>(GetProcAddress(tier0_base, "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z"));
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

    if (!AutoWall::Init())
        Con::Error("AutoWall init failed — mindamage uses TraceLine fallback / disabled");

    if (!SkinChanger::Init())
        Con::Error("SkinChanger init failed (knife changer disabled)");

    // return status
    return success;
}
