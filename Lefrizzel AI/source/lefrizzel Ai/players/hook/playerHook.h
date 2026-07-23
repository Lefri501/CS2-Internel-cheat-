#pragma once
#include "../../hooks/includeHooks.h"

typedef void(__fastcall* onAddEntity)(__int64 CGameEntitySystem, void* entityPointer, int entityHandle);
extern onAddEntity oOnAddEntity;

// @Author: basmannetjeeee
// @IDA:
// Signature: 48 89 74 24 ? 57 48 83 EC ? 48 8B F9 41 8B C0 B9
// __int64 __fastcall sub_651290(__int64 a1, __int64 a2, int a3)
void onAddEntityHook(__int64 CGameEntitySystem, void* entityPointer, int entityHandle);


typedef void(__fastcall* onRemoveEntity)(__int64 CGameEntitySystem, void* entityPointer, int entityHandle);
extern onRemoveEntity oOnRemoveEntity;

// @Author: basmannetjeeee
// @IDA:
// Signature: 48 89 74 24 ? 57 48 83 EC ? 48 8B F9 41 8B C0 25
// __int64 __fastcall sub_651890(__int64 a1, _QWORD *a2, int a3)
void onRemoveEntityHook(__int64 CGameEntitySystem, void* entityPointer, int entityHandle);