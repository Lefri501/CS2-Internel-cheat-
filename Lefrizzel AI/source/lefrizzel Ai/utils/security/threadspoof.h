#pragma once
#include <Windows.h>

// ============================================================================
// Thread Start Address Spoofing
//
// VAC enumerates threads via NtQueryInformationThread(ThreadQuerySetWin32StartAddress)
// and flags threads whose start address is in unbacked (MEM_PRIVATE) memory.
//
// This module creates threads whose reported start address is inside a
// legitimate system DLL (kernel32), while actual execution begins at our code.
//
// Technique: Create thread suspended at a kernel32 export, queue a user APC
// to redirect to our real entry, resume. The Win32StartAddress remains the
// kernel32 function forever (it's set at creation time and never updated).
// ============================================================================

namespace ThreadSpoof {

    // Create a thread with spoofed start address.
    // Actual execution begins at `realEntry` with parameter `param`.
    // Returns thread handle (caller owns), or NULL on failure.
    HANDLE CreateSpoofedThread(
        LPTHREAD_START_ROUTINE realEntry,
        LPVOID param,
        DWORD* outTid = nullptr
    );

} // namespace ThreadSpoof
