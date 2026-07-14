#include "../../../cs2/datatypes/schema/ISchemaClass/ISchemaClass.h"
#include "../players.h"
#include "playerHook.h"

#include <iostream>
/*
  // @READ ME: This should be storing Handles and not address pointers
  // it s hould store C_BaseEntity Handle 
  // which can later globally be used to grab its right instance 


void onAddEntityHook(__int64 CGameEntitySystem, void* entityPointer, int entityHandle) {
	if (!entityPointer || !CGameEntitySystem || !entityHandle) {
		oOnAddEntity(CGameEntitySystem, entityPointer, entityHandle);
		return;
	}

	uintptr_t uEntityPointer = reinterpret_cast<uintptr_t>(entityPointer);

	SchemaClassInfoData_t* entityInfo = nullptr;
	GetSchemaClassInfo(uEntityPointer, &entityInfo);

	if (entityInfo == nullptr) {
		oOnAddEntity(CGameEntitySystem, entityPointer, entityHandle);
		return;
	}

	if (strcmp(entityInfo->szName, "C_CSPlayerPawn") == 0) {

		Players::pawns.emplace_back(uEntityPointer);
		std::cout << "Added pawn " << Players::pawns.size() << "\n";

		oOnAddEntity(CGameEntitySystem, entityPointer, entityHandle);
		return;
	}
	if (strcmp(entityInfo->szName, "CCSPlayerController") == 0) {

		Players::controllers.emplace_back(uEntityPointer);

		oOnAddEntity(CGameEntitySystem, entityPointer, entityHandle);
		return;
	}

	oOnAddEntity(CGameEntitySystem, entityPointer, entityHandle);
	return;
}

void onRemoveEntityHook(__int64 CGameEntitySystem, void* entityPointer, int entityHandle) {
	if (!entityPointer || !CGameEntitySystem || !entityHandle) {
		oOnRemoveEntity(CGameEntitySystem, entityPointer, entityHandle);
		return;
	}

	uintptr_t uEntityPointer = reinterpret_cast<uintptr_t>(entityPointer);

	SchemaClassInfoData_t* entityInfo = nullptr;
	GetSchemaClassInfo(uEntityPointer, &entityInfo);

	if (strcmp(entityInfo->szName, "C_CSPlayerPawn") == 0) {
		for (auto it = Players::pawns.begin(); it != Players::pawns.end(); ++it) {
			if (it->getAddress() == uEntityPointer) {
				Players::pawns.erase(it);
				std::cout << "Removed pawn " << Players::pawns.size();
				break;
			}
		}
	}

	if (strcmp(entityInfo->szName, "CCSPlayerController") == 0) {
		for (auto it = Players::controllers.begin(); it != Players::controllers.end(); ++it) {
			if (it->getAddress() == uEntityPointer) {
				Players::controllers.erase(it);
				break;
			}
		}
	}

	oOnRemoveEntity(CGameEntitySystem, entityPointer, entityHandle);
	return;
}

onAddEntity oOnAddEntity = nullptr;
onRemoveEntity oOnRemoveEntity = nullptr;*/