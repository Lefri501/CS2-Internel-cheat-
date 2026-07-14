#include "debug/debug.h"

#include "includes.h"
#include "templeware/templeware.h"
#include "templeware/features/world/weather.h"
#include "templeware/renderer/icons.h"
#include "templeware/utils/console/console.h"
#include "templeware/utils/security/threadspoof.h"
#include "templeware/utils/security/vacdetect.h"
#include "templeware/utils/security/sehsupport.h"

#include <intrin.h>   // __readgsqword / __readfsdword

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HANDLE g_hConsole = nullptr;
static FILE* g_logFile = nullptr;

// --- Manual map support ---
// When injected via manual mapper, hModule passed to DllMain is NOT in PEB
// loader data structures. GetModuleHandle on our own DLL will fail, and
// FreeLibraryAndExitThread will crash. We detect this and adapt.
HMODULE g_OurModule = nullptr;
bool    g_ManualMapped = false;

static bool IsModuleInPEB(HMODULE hMod) {
    // Walk PEB → Ldr → InLoadOrderModuleList using raw offsets.
    // This avoids any SDK struct definition issues and works on all x64 Windows.
    // PEB layout (x64): offset 0x18 = Ldr (PEB_LDR_DATA*)
    // PEB_LDR_DATA:     offset 0x10 = InLoadOrderModuleList (LIST_ENTRY)
    // LDR_DATA_TABLE_ENTRY: offset 0x30 = DllBase (void*)

    const auto peb = reinterpret_cast<uintptr_t>( 
        reinterpret_cast<void*>(__readgsqword(0x60)) );
    if (!peb) return false;

    const auto ldr = *reinterpret_cast<uintptr_t*>(peb + 0x18);
    if (!ldr) return false;

    // InLoadOrderModuleList head
    const auto headFlink = reinterpret_cast<LIST_ENTRY*>(ldr + 0x10);
    auto entry = headFlink->Flink;

    while (entry && entry != headFlink) {
        // entry IS the start of LDR_DATA_TABLE_ENTRY (InLoadOrderLinks is at offset 0)
        // DllBase is at offset 0x30
        const auto dllBase = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(entry) + 0x30);
        if (dllBase == static_cast<void*>(hMod))
            return true;
        entry = entry->Flink;
    }
    return false;
}

TempleWare templeWare;

Present oPresent;
HWND window = NULL;
WNDPROC oWndProc;
ID3D11Device* pDevice = NULL;
ID3D11DeviceContext* pContext = NULL;
ID3D11RenderTargetView* mainRenderTargetView;

bool g_bMenuOpen = false; // synced with menu for input blocking in cmd/hooks

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Keep CreateMove strip in sync even between Present frames
    const bool menuOpen = templeWare.renderer.menu.isOpen();
    g_bMenuOpen = menuOpen;

    if (menuOpen) {
        // Always feed ImGui first
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);

        ClipCursor(nullptr);
        // Don't call ShowCursor every message — Present handles refcount on toggle

        // Eat game-bound input while menu is open (raw + legacy mouse wheel)
        switch (uMsg) {
        case WM_INPUT:
            return 1;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // INSERT toggled via GetAsyncKeyState in Present — still block game
            return 1;
        case WM_CHAR:
        case WM_DEADCHAR:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return 1;
        default:
            break;
        }
    }

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

bool init = false;
static bool g_imguiInPresent = false;

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    if (!init)
    {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice)))
        {
            pDevice->GetImmediateContext(&pContext);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwapChain->GetDesc(&sd);
            window = sd.OutputWindow;
            ID3D11Texture2D* pBackBuffer;
            pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
            pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
            pBackBuffer->Release();
            oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
#ifdef TEMPLEDEBUG
            initCall();
#endif


            templeWare.init(window, pDevice, pContext, mainRenderTargetView);
            init = true;
        }
        else
            return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // CS2 can re-enter Present; never nest ImGui frames
    if (g_imguiInPresent)
        return oPresent(pSwapChain, SyncInterval, Flags);
    g_imguiInPresent = true;

    bool imguiFrameOpen = false;
    __try {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        imguiFrameOpen = true;

        bool menuOpen = templeWare.renderer.menu.isOpen();
        static bool s_wasMenuOpen = false;
        g_bMenuOpen = menuOpen;

        if (GetAsyncKeyState(VK_INSERT) & 1) {
            templeWare.renderer.menu.toggleMenu();
            menuOpen = templeWare.renderer.menu.isOpen();
            g_bMenuOpen = menuOpen;
        }

        ImGui::GetIO().MouseDrawCursor = menuOpen;

        // Cursor show/hide only on edge — ShowCursor has a refcount; per-frame spam breaks it
        if (menuOpen != s_wasMenuOpen) {
            if (menuOpen) {
                ClipCursor(nullptr);
                while (ShowCursor(TRUE) < 0) {}
                // Force absolute via original (bypass hook latch) for free ImGui cursor
                if (H::g_pInputSystem) {
                    if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                        orig(H::g_pInputSystem, false);
                }
            } else {
                while (ShowCursor(FALSE) >= 0) {}
                // Restore game preference (do NOT force true — breaks CS2 main menu cursor)
                if (H::g_pInputSystem) {
                    if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                        orig(H::g_pInputSystem, H::g_wantRelativeMouse);
                }
            }
            s_wasMenuOpen = menuOpen;
        } else if (menuOpen) {
            // Keep cursor free every frame (game may re-clip)
            ClipCursor(nullptr);
        } else {
            // Hide OS cursor once after inject if something left it visible
            static bool s_hidCursor = false;
            if (!s_hidCursor) {
                while (ShowCursor(FALSE) >= 0) {}
                s_hidCursor = true;
            }
        }

        // Isolate draw crashes so we still EndFrame/Render
        __try {
            templeWare.renderer.hud.render();
            templeWare.renderer.menu.render();
            templeWare.renderer.visuals.esp();
            // Screen-space weather (always around camera) — engine rain_fx is impact/volume only
            World::Weather::Draw();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Con::Seh("hkPresent.draw", GetExceptionCode());
        }

        ImGui::Render();
        imguiFrameOpen = false;

        if (pContext && mainRenderTargetView) {
            pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Con::Seh("hkPresent", GetExceptionCode());
        // NewFrame without EndFrame/Render → next frame asserts imgui.cpp:10942
        if (imguiFrameOpen) {
            __try {
                ImGui::EndFrame();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            imguiFrameOpen = false;
        }
    }

    g_imguiInPresent = false;
    return oPresent(pSwapChain, SyncInterval, Flags);
}

#ifdef _DEBUG
void init_console() {
    if (::AllocConsole()) {
        FILE* conOut = nullptr;
        freopen_s(&conOut, "CONOUT$", "w", stdout);
        freopen_s(&conOut, "CONOUT$", "w", stderr);

        // Larger scrollback for long debug sessions
        if (HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE)) {
            CONSOLE_SCREEN_BUFFER_INFOEX info{};
            info.cbSize = sizeof(info);
            if (GetConsoleScreenBufferInfoEx(hOut, &info)) {
                if (info.dwSize.Y < 4000)
                    info.dwSize.Y = 4000;
                SetConsoleScreenBufferInfoEx(hOut, &info);
            }
        }

        char logDir[MAX_PATH], logPath[MAX_PATH];
        GetEnvironmentVariableA("USERPROFILE", logDir, sizeof(logDir));
        strcat_s(logDir, "\\Documents\\TempleWare");
        CreateDirectoryA(logDir, NULL);
        // Append-friendly name with date so sessions don't wipe each other
        SYSTEMTIME st{};
        GetLocalTime(&st);
        snprintf(logPath, sizeof(logPath),
            "%s\\TempleWare_%04u%02u%02u_%02u%02u%02u.log",
            logDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fopen_s(&g_logFile, logPath, "w");

        // Also keep a stable "latest" pointer path
        char latestPath[MAX_PATH];
        snprintf(latestPath, sizeof(latestPath), "%s\\TempleWare.log", logDir);
        // Best-effort copy path note only — primary is dated file

        g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        Con::Init(g_hConsole, g_logFile);

        wchar_t title[128];
        swprintf_s(title, L"TempleWare DEBUG  pid=%lu", GetCurrentProcessId());
        SetConsoleTitleW(title);

        Con::Info("___________                   .__         __      __                       ");
        Con::Info("\\__    ___/___   _____ ______ |  |   ____/  \\    /  \\_____ _______   ____  ");
        Con::Info("  |    |_/ __ \\ /     \\\\____ \\|  | _/ __ \\   \\/\\/   /\\__  \\\\_  __ \\_/ __ \\ ");
        Con::Info("  |    |\\  ___/|  Y Y  \\  |_> >  |_\\  ___/\\        /  / __ \\|  | \\/\\  ___/ ");
        Con::Info("  |____| \\___  >__|_|  /   __/|____/\\___  >\\__/\\  /  (____  /__|    \\___  >");
        Con::Info("             \\/      \\/|__|             \\/      \\/        \\/            \\/ ");
        // UTF-8 so Section/arrows don't mojibake (optional; we use ASCII anyway)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        Con::Section("DEBUG CONSOLE");
        Con::Ok("Console ready  pid=%lu", GetCurrentProcessId());
        Con::Info("Log file: %s", logPath);
        Con::Info("Also: OutputDebugString -> VS Output window");
        Con::Info("Env: TEMPLEWARE_LOG_TRACE=1  (verbose Trace)");
        Con::Info("Env: TEMPLEWARE_LOG_FLUSH=1  (flush every line)");
        Con::Info("Levels: TRC OK INF WRN ERR SEH  |  format: time [tid] LVL | msg");
        Con::Info("API: Con::Once / Rate / Hex / Section / ScopedTimer / Stats");
        Con::Info("Latest alias path (manual): %s", latestPath);
    }
}
#else
void init_console() {}
#endif
DWORD WINAPI MainThread(LPVOID lpReserved)
{
    __try {
        // Wait for game to be fully loaded (D3D11 must be ready)
        // Manual mappers may inject before the game finishes loading
        while (!GetModuleHandleA("d3d11.dll") || !GetModuleHandleA("client.dll"))
            Sleep(100);

        // Small extra delay for stability (let the swap chain be created)
        Sleep(500);

        bool init_hook = false;
        do
        {
            init_console();

            // hook hkPresent and init cheat
            if (!init_hook) {
                __try {
                    if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success)
                    {
                        kiero::bind(8, (void**)&oPresent, hkPresent);
                        init_hook = true;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // Kiero init crashed (D3D11 not ready yet?) — retry
                    Sleep(500);
                }
            }
        } while (!init_hook && !GetAsyncKeyState(VK_F4));

    // If we never hooked (user pressed F4 to abort), just exit
    if (!init_hook) {
        if (g_ManualMapped)
            ExitThread(0);
        else
            FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), 0);
        return 0;
    }

    // Main loop — wait for unload key
    while (!GetAsyncKeyState(VK_F4))
        Sleep(50);

    if (oWndProc != nullptr)
    {
        // restore wnd proc
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));

        // invalidate old wnd proc
        oWndProc = nullptr;
    }

    kiero::shutdown();

    // Uninstall VAC monitor
    VacDetect::Uninstall();

#ifdef _DEBUG
    // Console/log only exist in Debug builds
    ::FreeConsole();
    if (const HWND hConsoleWindow = ::GetConsoleWindow(); hConsoleWindow != nullptr)
        ::PostMessageW(hConsoleWindow, WM_CLOSE, 0U, 0L);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    fclose(stdout);
    fclose(stderr);
#endif

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Top-level crash guard: if anything in MainThread crashes,
        // the game survives. Log exception code if console was initialized.
        Con::Seh("MainThread top-level", GetExceptionCode());
    }

    // close thread — manual mapped DLLs can't use FreeLibraryAndExitThread
    if (g_ManualMapped) {
        // Just exit the thread; memory stays allocated (manual mapper owns cleanup)
        ExitThread(EXIT_SUCCESS);
    } else {
        FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), EXIT_SUCCESS);
    }

    return TRUE;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_OurModule = hMod;
        g_ManualMapped = !IsModuleInPEB(hMod);

        // *** CRITICAL: Manual map support setup ***
        // Under manual map, the loader doesn't:
        //  1. Register our .pdata (exception directory) — any __try crashes
        //  2. Initialize the /GS security cookie — any /GS function crashes
        //  3. Handle DisableThreadLibraryCalls (crashes walking PEB)
        // We handle each of these before running any code that depends on them.
        if (g_ManualMapped) {
            SehSupport::InitializeSecurityCookie();
            SehSupport::RegisterExceptionTable(hMod);
        } else {
            DisableThreadLibraryCalls(hMod);
        }

        // Spoofed thread: VAC sees start address as kernel32!SleepEx
        // Fallback plain CreateThread if spoof fails (cheat must still load)
        if (!ThreadSpoof::CreateSpoofedThread(MainThread, hMod))
            CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        if (!g_ManualMapped)
            kiero::shutdown();
        break;
    }
    return TRUE;
}

// --- Manual map alternative entry point ---
// Many manual mappers call a exported function or shellcode stub that
// jumps directly here instead of going through DllMain. This handles that case.
// The mapper passes the base address of the mapped image as the parameter.
extern "C" __declspec(dllexport) void ManualMapEntry(HMODULE hBase)
{
    if (g_OurModule != nullptr)
        return; // Already initialized via DllMain

    g_OurModule = hBase;
    g_ManualMapped = true;

    // Set up manual map support BEFORE anything using SEH runs
    SehSupport::InitializeSecurityCookie();
    SehSupport::RegisterExceptionTable(hBase);

    // Spin up the main thread (spoofed). Fallback if APC path fails.
    if (!ThreadSpoof::CreateSpoofedThread(MainThread, hBase))
        CreateThread(nullptr, 0, MainThread, hBase, 0, nullptr);
}
