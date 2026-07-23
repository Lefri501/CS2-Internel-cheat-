#pragma once
#include <memory>
#include <type_traits>

#include "..\fnv1a\fnv1a.h"
#include "..\memory\vfunc\vfunc.h"
#include "..\memory\memsafe\memsafe.h"
#include "..\math\utlmemory\utlmemory.h"
#include "..\math\utlvector\utlvector.h"
#include "..\..\..\cs2\datatypes\schema\ISchemaClass\ISchemaClass.h"

// Schema access: cheap user-range check only (no VirtualQuery — FPS critical)
// Lazy offset: retry until Schema::init fills the map (static const 0 was sticky forever).
#define SCHEMA_ADD_OFFSET(TYPE, NAME, OFFSET)                                                                   \
	[[nodiscard]] inline std::add_lvalue_reference_t<TYPE> NAME()                                            \
	{                                                                                                           \
		static TYPE s_fallback{};                                                                              \
		static std::uint32_t uOffset = 0;                                                                      \
		if (!uOffset) {                                                                                        \
			uOffset = (OFFSET);                                                                                \
			if (!uOffset)                                                                                      \
				return s_fallback;                                                                             \
		}                                                                                                       \
		auto* base = reinterpret_cast<std::uint8_t*>(this);                                                    \
		if (!Mem::IsUserPtr(base))                                                                             \
			return s_fallback;                                                                                 \
		return *reinterpret_cast<std::add_pointer_t<TYPE>>(base + uOffset);                                    \
	}

#define SCHEMA_ADD_POFFSET(TYPE, NAME, OFFSET)                                                                 \
	[[nodiscard]] inline std::add_pointer_t<TYPE> NAME()                                                    \
	{                                                                                                          \
		static std::uint32_t uOffset = 0;                                                                      \
		if (!uOffset) {                                                                                        \
			uOffset = (OFFSET);                                                                                \
			if (!uOffset)                                                                                      \
				return nullptr;                                                                                \
		}                                                                                                       \
		auto* base = reinterpret_cast<std::uint8_t*>(this);                                                    \
		if (!Mem::IsUserPtr(base))                                                                             \
			return nullptr;                                                                                    \
		return reinterpret_cast<std::add_pointer_t<TYPE>>(base + uOffset);                                     \
	}

#define SCHEMA_ARRAY(TYPE, NAME, FIELD) \
    [[nodiscard]] inline TYPE* NAME() { \
        static uint32_t uOffset = 0; \
        if (!uOffset) \
            uOffset = SchemaFinder::Get(hash_32_fnv1a_const(FIELD)); \
        auto* base = reinterpret_cast<std::uint8_t*>(this); \
        if (!uOffset || !Mem::IsUserPtr(base)) \
            return nullptr; \
        return reinterpret_cast<TYPE*>(base + uOffset); \
    }

#define schema(TYPE, NAME, FIELD)  SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::Get(hash_32_fnv1a_const(FIELD)) + 0u)

#define schema_pfield(TYPE, NAME, FIELD, ADDITIONAL) SCHEMA_ADD_OFFSET(TYPE, NAME, SchemaFinder::Get(hash_32_fnv1a_const(FIELD)) + ADDITIONAL)

#define SCHEMA_ADD_RAW_OFFSET(TYPE, NAME, OFFSET) \
    [[nodiscard]] inline TYPE NAME() noexcept \
    { \
        TYPE fallback{}; \
        if (!Mem::IsUserPtr(this)) \
            return fallback; \
        return *reinterpret_cast<std::add_pointer_t<TYPE>>( \
            reinterpret_cast<std::uint8_t*>(this) + OFFSET); \
    }

#define add_offset_near(_class, _name, _type, _field_name, _offset)              \
[[nodiscard]] inline std::add_lvalue_reference_t<_type> _name()                  \
{                                                                                \
    static _type s_fallback{};                                                   \
    static const uint32_t baseOffset = SchemaFinder::Get(                        \
        hash_32_fnv1a_const(_field_name)                                         \
    );                                                                           \
    static const uint32_t totalOffset = baseOffset + (_offset);                  \
    auto* base = reinterpret_cast<uint8_t*>(this);                               \
    if (!Mem::IsUserPtr(base))                                                   \
        return s_fallback;                                                       \
    return *reinterpret_cast<std::add_pointer_t<_type>>(base + totalOffset);     \
}

class Schema {
public:
    bool init(const char* module_name, int module_type);

    ISchemaSystem* schema_system = nullptr;

};

namespace SchemaFinder {

    [[nodiscard]] std::uint32_t Get(const uint32_t hashed);
    [[nodiscard]] std::uint32_t GetExternal(const char* moduleName, const uint32_t HashedClass, const uint32_t HashedFieldName);
    // True after Schema::init filled the map (so a 0 from Get is a real miss).
    [[nodiscard]] bool Ready();
}
