#include "ISchemaClass.h"

bool CSchemaType::GetSizes(int* pOutSize, uint8_t* unkPtr) {
	return M::vfunc<bool, 3U>(this, pOutSize, unkPtr);
}

bool CSchemaType::GetSize(int* out_size) {
	uint8_t smh = 0;
	return GetSizes(out_size, &smh);
}

bool SchemaClassInfoData_t::InheritsFrom(SchemaClassInfoData_t* pClassInfo) {
	if (pClassInfo == this && pClassInfo != nullptr)
		return true;
	else if (pBaseClasses == nullptr || pClassInfo == nullptr)
		return false;

	for (int i = 0; i < nBaseClassesCount; i++) {
		auto& baseClass = pBaseClasses[i];
		if (baseClass.pClass->InheritsFrom(pClassInfo))
			return true;
	}

	return false;
}

void CSchemaSystemTypeScope::FindDeclaredClass(SchemaClassInfoData_t** pReturnClass, const char* szClassName) {
	return M::vfunc<void, 2U>(this, pReturnClass, szClassName);
}

CSchemaType* CSchemaSystemTypeScope::FindSchemaTypeByName(const char* szName, std::uintptr_t* pSchema) {
	return M::vfunc<CSchemaType*, 4U>(this, szName, pSchema);
}

CSchemaType* CSchemaSystemTypeScope::FindTypeDeclaredClass(const char* szName) {
	return M::vfunc<CSchemaType*, 5U>(this, szName);
}

CSchemaType* CSchemaSystemTypeScope::FindTypeDeclaredEnum(const char* szName) {
	return M::vfunc<CSchemaType*, 6U>(this, szName);
}

CSchemaClassBinding* CSchemaSystemTypeScope::FindRawClassBinding(const char* szName) {
	return M::vfunc<CSchemaClassBinding*, 7U>(this, szName);
}

void GetSchemaClassInfo(uintptr_t address, SchemaClassInfoData_t** pReturn) {
	// IDA (client.dll): CEntityInstance::GetSchemaClassInfo is vfunc 46
	// (writes *out from per-class static SchemaClassInfo). Index 44 is unrelated.
	return M::vfunc<void, 46U>(reinterpret_cast<void*>(address), pReturn);
}

CSchemaSystemTypeScope* ISchemaSystem::FindTypeScopeForModule(const char* m_module_name)
{

	return M::vfunc<CSchemaSystemTypeScope*, 13U>(this, m_module_name, nullptr);
}