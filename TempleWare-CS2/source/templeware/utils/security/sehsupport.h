#pragma once
#include <Windows.h>

// ============================================================================
// x64 SEH support for manually-mapped DLLs
//
// On x64, __try/__except uses RUNTIME_FUNCTION tables from the .pdata section.
// The Windows loader auto-registers these when a DLL is loaded via LoadLibrary.
// Manual mappers DO NOT do this — result: any __try in our code crashes when
// exception unwinding tries to look up the function.
//
// This module manually calls RtlAddFunctionTable with our .pdata contents.
// Must be called BEFORE any code path that uses __try/__except is executed.
// ============================================================================

namespace SehSupport {

    // Register our .pdata section with the OS runtime function table.
    // Call once, immediately in DllMain, when manual-mapped.
    // baseAddr = our image base
    // Returns true on success.
    bool RegisterExceptionTable(void* baseAddr);

    // Manually initialize the security cookie if the CRT didn't run.
    // Prevents /GS-related crashes on manual map when CRT init is skipped.
    void InitializeSecurityCookie();

} // namespace SehSupport
