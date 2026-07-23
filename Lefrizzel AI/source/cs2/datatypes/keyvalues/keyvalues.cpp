#include "keyvalues.h"

#include <cstring>
#include <Windows.h>

#include "../../../lefrizzel Ai/utils/memory/patternscan/patternscan.h"
#include "../../../lefrizzel Ai/utils/console/console.h"
#include "../../../lefrizzel Ai/interfaces/interfaces.h"

// IDA tier0 (imagebase 0x180000000):
//   LoadKV3 (buffer)  @ 0x1801252d0
//     char __fastcall LoadKV3(KeyValues3*, CUtlString*, CUtlBuffer*, KV3ID_t const*, char*, unsigned)
//   LoadKV3 (string)  @ 0x180125200 — same args with const char* instead of CUtlBuffer*
//   Export mangling: ?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z  (cdecl)
//
// IDA client SetTypeKV3 @ 0x181867120:
//   unsigned __int64* __fastcall SetTypeKV3(unsigned __int64* kv, unsigned __int8 type, unsigned __int8 subtype)
//   pattern: 40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16 00

namespace {

constexpr KV3ID_t kGenericKv3Id{
	"generic",
	0x41B818518343427Eull,
	0xB5F447C23C0CDF8Cull
};

// tier0 buffer LoadKV3 — 6 args (NOT the old 9-arg guess)
using FnLoadKV3Buffer = bool(__fastcall*)(
	CKeyValues3* kv,
	void* errorString,          // CUtlString* (optional)
	CUtlBuffer* buffer,
	const KV3ID_t* id,
	const char* unkPath,        // optional
	unsigned int flags);

// tier0 string LoadKV3 — cdecl export
using FnLoadKV3String = bool(__cdecl*)(
	CKeyValues3* kv,
	void* errorString,
	const char* text,
	const KV3ID_t* id,
	const char* unkPath,
	unsigned int flags);

using FnSetTypeKV3 = CKeyValues3*(__fastcall*)(CKeyValues3* kv, unsigned int type, unsigned int subtype);

FnLoadKV3Buffer ResolveLoadKV3Buffer()
{
	// Unique function prologue (dump patterns.hpp LoadKV3)
	if (uint8_t* p = M::FindPattern("tier0.dll",
		"48 89 5C 24 08 57 48 83 EC 70 4C 8B D1 48 C7 C0"))
		return reinterpret_cast<FnLoadKV3Buffer>(p);

	// Fallback: E8 call inside LoadKV3FromKV3OrKV1 → resolve target
	if (uint8_t* callSite = M::FindPattern("tier0.dll", "E8 ? ? ? ? EB ? F7 43"))
		return reinterpret_cast<FnLoadKV3Buffer>(M::abs(callSite, 0x1, 0x0));

	return nullptr;
}

FnSetTypeKV3 ResolveSetTypeKV3()
{
	// Unique dump pattern (client)
	if (uint8_t* p = M::FindPattern("client.dll",
		"40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16 00"))
		return reinterpret_cast<FnSetTypeKV3>(p);

	// Short fallback
	if (uint8_t* p = M::FindPattern("client.dll", "40 53 48 83 EC ? 80 FA"))
		return reinterpret_cast<FnSetTypeKV3>(p);

	return nullptr;
}

} // namespace

void CKeyValues3::LoadFromBuffer(const char* szString)
{
	if (!szString || !szString[0])
		return;

	// 1) Preferred: tier0 string export (no CUtlBuffer layout risk)
	if (I::LoadKeyValues) {
		auto fn = reinterpret_cast<FnLoadKV3String>(I::LoadKeyValues);
		if (fn(this, nullptr, szString, &kGenericKv3Id, "", 0u))
			return;
	}

	// 2) Buffer path — correct 6-arg LoadKV3
	static FnLoadKV3Buffer s_loadBuf = nullptr;
	static bool s_triedBuf = false;
	if (!s_triedBuf) {
		s_triedBuf = true;
		s_loadBuf = ResolveLoadKV3Buffer();
		if (!s_loadBuf)
			Con::Error("LoadKV3 buffer pattern miss");
		else
			Con::Ok("LoadKV3 buffer @ 0x%p", reinterpret_cast<void*>(s_loadBuf));
	}
	if (!s_loadBuf)
		return;

	const int bufferSize = static_cast<int>(std::strlen(szString)) + 16;
	CUtlBuffer buffer(0, bufferSize, 1); // TEXT_BUFFER
	buffer.PutString(szString);
	s_loadBuf(this, nullptr, &buffer, &kGenericKv3Id, nullptr, 0u);
}

bool CKeyValues3::LoadKV3(CUtlBuffer* buffer)
{
	if (!buffer)
		return false;

	static FnLoadKV3Buffer s_loadBuf = nullptr;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		s_loadBuf = ResolveLoadKV3Buffer();
	}
	if (!s_loadBuf)
		return false;

	return s_loadBuf(this, nullptr, buffer, &kGenericKv3Id, nullptr, 0u);
}

CKeyValues3* CKeyValues3::create_material_from_resource()
{
	// Lazy resolve — do NOT cache a permanent null on a one-shot miss
	static FnSetTypeKV3 s_setType = nullptr;
	if (!s_setType) {
		s_setType = ResolveSetTypeKV3();
		if (!s_setType) {
			Con::Error("SetTypeKV3 pattern miss");
			return nullptr;
		}
		Con::Ok("SetTypeKV3 @ 0x%p", reinterpret_cast<void*>(s_setType));
	}

	// HeapAlloc: safe under manual map (operator new / CRT heap often broken)
	constexpr size_t kKvBytes = 0x100;
	void* mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kKvBytes);
	if (!mem) {
		Con::Error("SetTypeKV3: HeapAlloc failed");
		return nullptr;
	}

	auto* kv = reinterpret_cast<CKeyValues3*>(mem);
	// IDA: type=1 (object), subtype=6 — material KV root
	CKeyValues3* result = s_setType(kv, 1u, 6u);
	if (!result) {
		HeapFree(GetProcessHeap(), 0, mem);
		Con::Error("SetTypeKV3 returned null");
		return nullptr;
	}
	return result;
}
