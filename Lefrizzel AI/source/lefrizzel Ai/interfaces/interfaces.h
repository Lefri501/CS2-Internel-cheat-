#pragma once
#include "IEngineClient/IEngineClient.h"
#include "CGameEntitySystem/CGameEntitySystem.h"
#include "..\..\cs2\entity\C_CSPlayerPawn\C_CSPlayerPawn.h"
#include "..\..\cs2\datatypes\cutlbuffer\cutlbuffer.h"
#include "..\..\cs2\datatypes\keyvalues\keyvalues.h"
#include "..\..\cs2\entity\C_Material\C_Material.h"
#include "CCSGOInput/CCSGOInput.h"
#include "../utils/math/vector/vector.h"

namespace I
{
	inline void(__fastcall* EnsureCapacityBuffer)(CUtlBuffer*, int) = nullptr;
	inline CUtlBuffer* (__fastcall* ConstructUtlBuffer)(CUtlBuffer*, int, int, int) = nullptr;
	inline void(__fastcall* PutUtlString)(CUtlBuffer*, const char*);
	// IDA materialsystem2 CreateMaterial @ 0x18003ADE0 (dump pattern, not 0x3AFD0 sibling):
	//   void* __fastcall(void* this, CStrongHandle* out, const char* name, CKeyValues3* kv, void* resource, char flags)
	inline void*(__fastcall* CreateMaterial)(void*, void*, const char*, void*, void*, char);
	// tier0 export LoadKV3 string overload — cdecl, 6 args (see keyvalues.cpp)
	inline bool(__cdecl* LoadKeyValues)(CKeyValues3*, void*, const char*, const KV3ID_t*, const char*, unsigned int);

	// Logging — tier0 cdecl exports (IDA: ?ConMsg@@YAXPEBDZZ / ConColorMsg Color&)
	// Note: ConMsg silent if LOG_CONSOLE verbosity < 2; use Notify::ConsolePrintf for hits.
	inline void(__cdecl* ConMsg)(const char*, ...);
	inline void(__cdecl* ConColorMsg)(const Color&, const char*, ...);

	inline IEngineClient* EngineClient = nullptr;
	inline IGameResourceService* GameEntity = nullptr;

	class Interfaces {
	public:
		bool init();
	};
}
