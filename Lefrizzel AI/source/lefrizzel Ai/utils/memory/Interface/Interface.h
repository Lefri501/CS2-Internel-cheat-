#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
namespace I {
    using InstantiateInterfaceFn = void* (*)();
    class CInterfaceReg
    {
    public:
        InstantiateInterfaceFn m_create_fn;
        const char* m_name;
        CInterfaceReg* m_next;
    };

    inline const CInterfaceReg* Find(const char* module_name)
    {
        if (!module_name || !module_name[0])
            return nullptr;

        HMODULE module_base = GetModuleHandleA(module_name);
        // Accept short names ("engine2") as well as "engine2.dll"
        if (!module_base) {
            char buf[160]{};
            const size_t len = strnlen(module_name, 140);
            if (len > 0 && len < 140 && !strchr(module_name, '.')) {
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s.dll", module_name);
                module_base = GetModuleHandleA(buf);
            }
        }
        if (module_base == nullptr)
            return nullptr;

        const auto symbol = reinterpret_cast<std::uintptr_t>(
            GetProcAddress(module_base, "CreateInterface"));
        if (!symbol)
            return nullptr;

        // CreateInterface prolog: jmp/call into list loader — relative @ +3
        __try {
            const std::uintptr_t list =
                symbol + *reinterpret_cast<std::int32_t*>(symbol + 3) + 7;
            return *reinterpret_cast<CInterfaceReg**>(list);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    template <typename T = void*>
    T* Get(const char* module_name, const char* interface_partial_version)
    {
        for (const CInterfaceReg* current = Find(module_name); current; current = current->m_next)
        {
            if (std::string_view(current->m_name).find(interface_partial_version) != std::string_view::npos)
            {
                return reinterpret_cast<T*>(current->m_create_fn());
            }
        }

        return nullptr;
    }
}
