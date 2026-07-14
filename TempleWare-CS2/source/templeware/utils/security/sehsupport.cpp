#include "sehsupport.h"

// __security_cookie / __security_cookie_complement — MSVC CRT globals
// Normally initialized by __security_init_cookie() in _DllMainCRTStartup.
// Under manual map with WipeHeader + no CRT init, they remain at sentinel value
// (0x00002B992DDFA232 on x64) which /GS-checked functions will detect as
// "cookie changed" and abort via __report_gsfailure.
extern "C" {
    extern uintptr_t __security_cookie;
    extern uintptr_t __security_cookie_complement;
}

namespace SehSupport {

bool RegisterExceptionTable(void* baseAddr) {
    if (!baseAddr) return false;

    auto imageBase = reinterpret_cast<uint8_t*>(baseAddr);

    // Parse PE headers
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        // Header wiped by mapper — we can't find .pdata this way.
        // Fall back: this DLL won't have working SEH under such mappers.
        return false;
    }

    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    // Exception directory (IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3)
    IMAGE_DATA_DIRECTORY exDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exDir.VirtualAddress == 0 || exDir.Size == 0)
        return false;

    auto pFuncTable = reinterpret_cast<PRUNTIME_FUNCTION>(imageBase + exDir.VirtualAddress);
    DWORD count = exDir.Size / sizeof(RUNTIME_FUNCTION);

    // Register with the OS. From this point, __try/__except works.
    return RtlAddFunctionTable(pFuncTable, count, reinterpret_cast<DWORD64>(imageBase)) != FALSE;
}

void InitializeSecurityCookie() {
    // Default MSVC sentinel value on x64
    constexpr uintptr_t kDefaultCookie = 0x00002B992DDFA232ULL;

    // If cookie is at default (sentinel), CRT didn't run — initialize manually
    if (__security_cookie == kDefaultCookie) {
        // Generate a cookie from various entropy sources
        LARGE_INTEGER perf;
        QueryPerformanceCounter(&perf);

        uintptr_t cookie = static_cast<uintptr_t>(perf.QuadPart);
        cookie ^= reinterpret_cast<uintptr_t>(&cookie);         // Stack address entropy
        cookie ^= GetCurrentProcessId();
        cookie ^= GetCurrentThreadId();
        cookie ^= GetTickCount64();

        // Ensure top 16 bits are zero (MSVC requirement)
        cookie &= 0x0000FFFFFFFFFFFFULL;

        // Avoid default sentinel
        if (cookie == kDefaultCookie) cookie ^= 0xDEADBEEF;

        __security_cookie = cookie;
        __security_cookie_complement = ~cookie;
    }
}

} // namespace SehSupport
