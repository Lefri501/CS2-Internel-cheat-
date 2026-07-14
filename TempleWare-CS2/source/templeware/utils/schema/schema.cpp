#include "schema.h"
#include <vector>
#include <algorithm>
#include <Shlobj.h>
#include <shlobj_core.h>


#include "../fnv1a/fnv1a.h"
#include "../console/console.h"

#include "../math/utlstring/utlstring.h"
#include "../memory/Interface/Interface.h"

struct SchemaDumpedData_t
{
    uint32_t hashedName = 0x0ULL;
    std::uint32_t uOffset = 0x0U;
};

static std::vector<SchemaDumpedData_t> dumped_data;

static bool sehReadHeaders(uint8_t* pScope, uint16_t& nClasses, void*& pArray) {
    __try {
        nClasses = *reinterpret_cast<uint16_t*>(pScope + 0x470);
        pArray = *reinterpret_cast<void**>(pScope + 0x478);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("Schema class table headers", GetExceptionCode());
        return false;
    }
}

static bool sehReadClassEntry(uint8_t* pEntries, int i, const char*& szClassName, SchemaClassInfoData_t*& pClassInfo) {
    __try {
        uint8_t* entry = pEntries + (i * 0x18);
        void* pDecl = *reinterpret_cast<void**>(entry + 0x10);
        if (!pDecl) return false;
        szClassName = *reinterpret_cast<const char**>(reinterpret_cast<uint8_t*>(pDecl) + 0x08);
        pClassInfo = *reinterpret_cast<SchemaClassInfoData_t**>(reinterpret_cast<uint8_t*>(pDecl) + 0x20);
        return (pClassInfo != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Schema::init(const char* ModuleName, int module_type)
{
    schema_system = I::Get<ISchemaSystem>("schemasystem.dll", "SchemaSystem_00");
    if (!schema_system) {
        Con::Error("Schema::init: failed to get SchemaSystem_00");
        return false;
    }

    CSchemaSystemTypeScope* pTypeScope = schema_system->FindTypeScopeForModule(ModuleName);
    if (!pTypeScope) {
        Con::Error("Schema::init: failed to find type scope for %s", ModuleName);
        return false;
    }

    uint8_t* pScope = reinterpret_cast<uint8_t*>(pTypeScope);

    uint16_t nClasses = 0;
    void* pArray = nullptr;
    if (!sehReadHeaders(pScope, nClasses, pArray))
        return false;

    Con::Ok("Schema: found %u classes in %s", nClasses, ModuleName);

    if (nClasses == 0 || !pArray)
        return false;

    uint8_t* pEntries = reinterpret_cast<uint8_t*>(pArray);

    for (int i = 0; i < nClasses; i++)
    {
        const char* szClassName = nullptr;
        SchemaClassInfoData_t* pClassInfo = nullptr;

        if (!sehReadClassEntry(pEntries, i, szClassName, pClassInfo))
            continue;

        if (pClassInfo->nFieldSize == 0)
            continue;

        for (int j = 0; j < pClassInfo->nFieldSize; j++)
        {
            SchemaClassFieldData_t* pFields = pClassInfo->pFields;
            if (!pFields[j].szName) continue;

            std::string szFieldClassBuffer = std::string(szClassName) + "->" + std::string(pFields[j].szName);
            dumped_data.emplace_back(hash_32_fnv1a_const(szFieldClassBuffer.c_str()), pFields[j].nSingleInheritanceOffset);
        }
    }

    return true;
}

std::uint32_t SchemaFinder::Get(const uint32_t hashedName)
{
    for (size_t i = 0; i < dumped_data.size(); i++) {
        if (dumped_data[i].hashedName == hashedName) {
            return dumped_data[i].uOffset;
        }
    }

    // Rate-limited via Con::OffsetMiss
    char name[48];
    snprintf(name, sizeof(name), "schema hash 0x%08X", hashedName);
    Con::OffsetMiss(name);

    return 0U;
}

