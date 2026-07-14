#pragma once
#include <cstdint>
#include "..\C_EntityInstance\C_EntityInstance.h"
#include "../../../templeware/utils/memory/memorycommon.h"
#include "../../../templeware/utils/math/vector/vector.h"
#include "..\..\..\..\source\templeware\utils\schema\schema.h"
#include "..\..\..\..\source\templeware\utils\memory\vfunc\vfunc.h"
#include "..\handle.h"

class C_AggregateSceneObjectData
{
public:
	char pad_0000[4]; //0x0000
	int count; //0x0004
	char pad_0008[40]; //0x0008
	int index; //0x0030
};

class C_AggregateSceneObject
{
public:
	void* object; //0x0000
	C_AggregateSceneObjectData* data; //0x0008
};
