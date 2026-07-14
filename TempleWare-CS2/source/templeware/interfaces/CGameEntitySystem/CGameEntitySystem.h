#pragma once

#include <cstdint>
#include "../../../cs2/entity/handle.h"
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/memory/memsafe/memsafe.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "..\..\..\..\source\templeware\utils\schema\schema.h"
#include "..\..\..\..\source\templeware\utils\memory\vfunc\vfunc.h"

#include "..\..\..\cs2\entity\C_BaseEntity\C_BaseEntity.h"
#include "..\..\..\cs2\entity\C_CSPlayerPawn\C_CSPlayerPawn.h"
#include "..\..\..\cs2\entity\CCSPlayerController\CCSPlayerController.h"

class CGameEntitySystem
{
public:
	template <typename T = C_BaseEntity>
	T* Get(int nIndex)
	{
		if (!Mem::ValidEntityIndex(nIndex))
			return nullptr;
		void* e = this->GetEntityByIndex(nIndex);
		if (!Mem::ValidEntity(e))
			return nullptr;
		return reinterpret_cast<T*>(e);
	}

	template <typename T = C_BaseEntity>
	T* Get(const CBaseHandle hHandle)
	{
		if (!hHandle.valid())
			return nullptr;
		return Get<T>(hHandle.index());
	}

	int GetHighestEntityIndex()
	{
		if (!Mem::Valid(this, 0x2090 + sizeof(int)))
			return 0;
		int n = 0;
		if (!Mem::ReadField(this, 0x2090, n))
			return 0;
		// Cap iteration for ESP/scan loops (not handle lookup — use Get() for high indices)
		if (n < 0) return 0;
		if (n > 8192) return 8192;
		return n;
	}

	C_CSPlayerPawn* get_entity(int index)
	{
		if (!Mem::ValidEntityIndex(index) || !Mem::Valid(this, 16 + sizeof(void*)))
			return nullptr;

		if ((unsigned int)index > 0x7FFE)
			return nullptr;
		if ((unsigned int)(index >> 9) > 0x3F)
			return nullptr;

		// CEntityIdentity chunk stride is 0x70 (112) — not 120 (IDA GetBaseEntity)
		constexpr std::uintptr_t kIdentityStride = 0x70;
		void* chunk = nullptr;
		if (!Mem::ReadField(this, 8ull * (index >> 9) + 16, chunk)
			|| !Mem::Valid(chunk, kIdentityStride))
			return nullptr;

		const std::uintptr_t v3 = kIdentityStride * (index & 0x1FF);
		std::uintptr_t entry = 0;
		if (!Mem::ReadField(chunk, v3, entry))
			return nullptr;

		std::uint64_t handleBits = 0;
		if (!Mem::ReadField(chunk, v3 + 16, handleBits))
			return nullptr;
		if ((handleBits & 0x7FFF) != static_cast<std::uint64_t>(index))
			return nullptr;

		if (!Mem::ValidEntity(reinterpret_cast<void*>(entry)))
			return nullptr;
		return reinterpret_cast<C_CSPlayerPawn*>(entry);
	}

private:
	void* GetEntityByIndex(int nIndex);
};

class IGameResourceService
{
public:
	MEM_PAD(0x58);
	CGameEntitySystem* Instance;
};
