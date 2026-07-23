#include "threadspoof.h"
#include "../crypto/xorstr.h"

// ============================================================================
// Thread spoof via suspended thread + QueueUserAPC
//
// Win32StartAddress (NtQueryInformationThread) = kernel32!SleepEx.
// MSDN: if a user APC is queued before the thread begins running, the thread
// begins by calling the APC function — so real entry runs first.
// SleepEx is only reached if APC delivery fails; param = INFINITE (harmless hang
// vs silent exit). Caller must fall back to CreateThread if this returns NULL.
// ============================================================================

struct SpoofContext {
	LPTHREAD_START_ROUTINE realEntry;
	LPVOID                 param;
};

static void NTAPI SpoofApcRoutine(ULONG_PTR ctx) {
	auto* context = reinterpret_cast<SpoofContext*>(ctx);
	if (!context)
		return;

	LPTHREAD_START_ROUTINE entry = context->realEntry;
	LPVOID param = context->param;
	HeapFree(GetProcessHeap(), 0, context);

	const DWORD exitCode = entry(param);
	ExitThread(exitCode);
}

namespace ThreadSpoof {

HANDLE CreateSpoofedThread(
	LPTHREAD_START_ROUTINE realEntry,
	LPVOID param,
	DWORD* outTid)
{
	if (!realEntry)
		return nullptr;

	HMODULE hK32 = GetModuleHandleA(XS("kernel32.dll"));
	if (!hK32)
		return nullptr;

	auto pSleepEx = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		GetProcAddress(hK32, XS("SleepEx")));
	if (!pSleepEx)
		return nullptr;

	auto* ctx = reinterpret_cast<SpoofContext*>(
		HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SpoofContext)));
	if (!ctx)
		return nullptr;

	ctx->realEntry = realEntry;
	ctx->param = param;

	DWORD tid = 0;
	HANDLE hThread = CreateThread(
		nullptr,
		0,
		pSleepEx,
		reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(INFINITE)),
		CREATE_SUSPENDED,
		&tid);

	if (!hThread) {
		HeapFree(GetProcessHeap(), 0, ctx);
		return nullptr;
	}

	// MUST queue before resume — APC-before-start is the whole technique
	if (!QueueUserAPC(SpoofApcRoutine, hThread, reinterpret_cast<ULONG_PTR>(ctx))) {
		TerminateThread(hThread, 0);
		CloseHandle(hThread);
		HeapFree(GetProcessHeap(), 0, ctx);
		return nullptr;
	}

	if (ResumeThread(hThread) == static_cast<DWORD>(-1)) {
		TerminateThread(hThread, 0);
		CloseHandle(hThread);
		// ctx may already be in flight if resume partially ran — only free on hard fail before resume
		// Resume failed: thread still suspended, safe to free
		HeapFree(GetProcessHeap(), 0, ctx);
		return nullptr;
	}

	if (outTid)
		*outTid = tid;
	return hThread;
}

} // namespace ThreadSpoof
