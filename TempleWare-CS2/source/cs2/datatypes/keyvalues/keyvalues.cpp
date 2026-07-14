#pragma once
#include <cstdint>
#include "../cutlbuffer/cutlbuffer.h"
#include "keyvalues.h"
#include "../../../templeware/utils/memory/vfunc/vfunc.h"
#include "../../../templeware/utils/memory/patternscan/patternscan.h"
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../cutl/utlhash/utlhash.h"
#include "..\..\..\templeware\utils\math\utlvector\utlvector.h"
#include "..\..\..\templeware\interfaces\interfaces.h"



void CKeyValues3::LoadFromBuffer(const char* szString)
{
	// Create buffer with correct size
	int bufferSize = static_cast<int>(std::strlen(szString)) + 10;
	CUtlBuffer buffer(0, bufferSize, 1); // (growSize, initSize, flags)

	buffer.PutString(szString);

	LoadKV3(&buffer);
}

bool CKeyValues3::LoadKV3(CUtlBuffer* buffer)
{
	// Verify the pattern is correct
	static const auto oLoadKeyValues = reinterpret_cast<bool(__fastcall*)(
		CKeyValues3*, void*, CUtlBuffer*, KV3ID_t*, void*, void*, void*, void*, const char*
		)>(M::abs(M::FindPattern("tier0.dll", "E8 ? ? ? ? EB ? F7 43"), 0x1, 0x0)); // bool __fastcall LoadKV3FromKV3OrKV1

	if (!oLoadKeyValues) {
		return false;
	}

	KV3ID_t kv3ID = { "generic", 0x41B818518343427E, 0xB5F447C23C0CDF8C };

	bool result = oLoadKeyValues(this, nullptr, buffer, &kv3ID, nullptr, nullptr, nullptr, nullptr, "");

	return result;
}

CKeyValues3* CKeyValues3::create_material_from_resource()
{
	using fnSetTypeKV3 = CKeyValues3 * (__fastcall*)(CKeyValues3*, unsigned int, unsigned int);
	static const fnSetTypeKV3 oSetTypeKV3 = reinterpret_cast<fnSetTypeKV3>(
		M::FindPattern("client.dll", "40 53 48 83 EC ? 80 FA"));

	if (!oSetTypeKV3) {
		return nullptr;
	}

	// Allocate a single object, not an array
	CKeyValues3* pKeyValue = new CKeyValues3();

	CKeyValues3* result = oSetTypeKV3(pKeyValue, 1U, 6U);

	return result;
}