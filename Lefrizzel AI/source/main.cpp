#include "debug/debug.h"

#include "includes.h"
#include "lefrizzel Ai/lefrizzel Ai.h"
#include "lefrizzel Ai/config/config.h"
#include "lefrizzel Ai/features/movement/movement.h"
#include "lefrizzel Ai/features/notify/notify.h"
#include "lefrizzel Ai/features/sound_esp/sound_esp.h"
#include "lefrizzel Ai/features/vote/vote.h"
#include "lefrizzel Ai/features/world/weather.h"
#include "lefrizzel Ai/renderer/icons.h"
#include "lefrizzel Ai/utils/console/console.h"
#include "lefrizzel Ai/utils/security/threadspoof.h"
#include "lefrizzel Ai/utils/security/vacdetect.h"
#include "lefrizzel Ai/utils/security/sehsupport.h"

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

LefrizzelAi lefrizzelAi;

Present oPresent;
HWND window = NULL;
WNDPROC oWndProc;
ID3D11Device* pDevice = NULL;
ID3D11DeviceContext* pContext = NULL;
ID3D11RenderTargetView* mainRenderTargetView;

bool g_bMenuOpen = false; // synced with menu for input blocking in cmd/hooks

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Keep CreateMove strip in sync even between Present frames
    const bool menuOpen = lefrizzelAi.renderer.menu.isOpen();
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
        case WM_SETCURSOR: {
            static HCURSOR s_arrow = LoadCursor(nullptr, IDC_ARROW);
            SetCursor(s_arrow);
            return 1;
        }
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
    // Latch before init body so re-entrant Present never double-inits hooks
    if (!init)
    {
        ID3D11Device* dev = nullptr;
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
            return oPresent(pSwapChain, SyncInterval, Flags);

        pDevice = dev;
        pDevice->GetImmediateContext(&pContext);
        if (!pContext) {
            pDevice->Release();
            pDevice = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        DXGI_SWAP_CHAIN_DESC sd{};
        if (FAILED(pSwapChain->GetDesc(&sd)) || !sd.OutputWindow) {
            pContext->Release();
            pContext = nullptr;
            pDevice->Release();
            pDevice = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
        window = sd.OutputWindow;

        ID3D11Texture2D* pBackBuffer = nullptr;
        if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))
            || !pBackBuffer) {
            pContext->Release();
            pContext = nullptr;
            pDevice->Release();
            pDevice = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
        pBackBuffer->Release();
        if (!mainRenderTargetView) {
            pContext->Release();
            pContext = nullptr;
            pDevice->Release();
            pDevice = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        // Prevent second Present from re-entering init while we install hooks
        init = true;

        oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
#ifdef LEFRIZZELDEBUG
        initCall();
#endif
        lefrizzelAi.init(window, pDevice, pContext, mainRenderTargetView);
    }

    // CS2 can re-enter Present; never nest ImGui frames
    if (g_imguiInPresent)
        return oPresent(pSwapChain, SyncInterval, Flags);
    g_imguiInPresent = true;

    // Map resize / device recycle: recreate RTV when backbuffer changes
    if (init && pDevice && pSwapChain) {
        static UINT s_bbW = 0, s_bbH = 0;
        DXGI_SWAP_CHAIN_DESC scd{};
        if (SUCCEEDED(pSwapChain->GetDesc(&scd))) {
            if (s_bbW == 0) {
                s_bbW = scd.BufferDesc.Width;
                s_bbH = scd.BufferDesc.Height;
            } else if (scd.BufferDesc.Width != s_bbW || scd.BufferDesc.Height != s_bbH
                || !mainRenderTargetView) {
                s_bbW = scd.BufferDesc.Width;
                s_bbH = scd.BufferDesc.Height;
                if (mainRenderTargetView) {
                    if (pContext)
                        pContext->OMSetRenderTargets(0, nullptr, nullptr);
                    mainRenderTargetView->Release();
                    mainRenderTargetView = nullptr;
                }
                ID3D11Texture2D* bb = nullptr;
                if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
                    pDevice->CreateRenderTargetView(bb, nullptr, &mainRenderTargetView);
                    bb->Release();
                }
            }
        }
    }

    bool menuOpen = lefrizzelAi.renderer.menu.isOpen();
    static bool s_wasMenuOpen = false;
    if (GetAsyncKeyState(VK_INSERT) & 1) {
        lefrizzelAi.renderer.menu.toggleMenu();
        menuOpen = lefrizzelAi.renderer.menu.isOpen();
    }
    g_bMenuOpen = menuOpen;

    // Bhop move-services poll every frame (not only when overlay draws).
    // CreateMove alone is often after CheckJumpButton already sampled buttons.
    if (g_movement && !menuOpen) {
        __try {
            g_movement->OnFrame();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Auto-vote delay — independent of ImGui overlay
    __try { Vote::OnFrame(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Skip full ImGui stack when menu closed and nothing draws on-screen.
    // Glow / knifebot are engine-side — do NOT force Present ImGui for them.
    const bool needOverlay =
        menuOpen
        || Config::watermark
        || (Config::fov_circle && Config::aimbot)
        || (Config::fov_circle_autofire && Config::autofire)
        || (Config::fov_circle_magnet && Config::triggerbot && Config::trigger_magnet)
        || Config::widget_keybinds || Config::widget_bomb
        || Config::widget_spectators || Config::widget_radar
        || Config::sound_esp
        || Config::esp || Config::espFill || Config::showHealth || Config::showArmor
        || Config::showNameTags || Config::esp_skeleton || Config::showWeapon
        || Config::showWeaponIcon || Config::showDistance
        || Config::flag_flashed || Config::flag_scoped
        || Config::flag_defusing || Config::flag_bomb || Config::flag_reloading
        || Config::nade_pred || Config::nade_warn || Config::nade_lineup
        || Config::world_esp_weapons || Config::world_esp_bomb
        || Config::world_esp_smoke || Config::world_esp_molotov
        || Config::world_esp_he || Config::world_esp_flash || Config::world_esp_decoy
        || Config::hitmarker
        || Config::hitlog
        || Config::tracers
        || (Config::backtrack && Config::backtrack_skeleton)
        || (Config::weather && Config::weather_mode >= 1 && Config::weather_mode <= 4)
        || Notify::HasPending();

    if (!needOverlay) {
        if (s_wasMenuOpen) {
            while (ShowCursor(FALSE) >= 0) {}
            if (H::g_pInputSystem) {
                if (auto orig = H::IsRelativeMouseMode.GetOriginal())
                    orig(H::g_pInputSystem, H::g_wantRelativeMouse);
            }
            s_wasMenuOpen = false;
        }
        g_imguiInPresent = false;
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    bool imguiFrameOpen = false;
    __try {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        imguiFrameOpen = true;

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
            // Throttle ClipCursor — per-frame calls add input jitter
            static ULONGLONG s_lastClipMs = 0;
            const ULONGLONG nowClip = GetTickCount64();
            if (nowClip - s_lastClipMs >= 50ull) {
                s_lastClipMs = nowClip;
                ClipCursor(nullptr);
            }
        } else {
            // Hide OS cursor once after inject if something left it visible
            static bool s_hidCursor = false;
            if (!s_hidCursor) {
                while (ShowCursor(FALSE) >= 0) {}
                s_hidCursor = true;
            }
        }

        // Isolate each draw path — SEH mid-menu can leave BeginChild open; EndFrame recovers.
        __try { lefrizzelAi.renderer.hud.render(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.hud", GetExceptionCode()); }
        __try { lefrizzelAi.renderer.menu.render(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.menu", GetExceptionCode()); }
        __try { lefrizzelAi.renderer.visuals.esp(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.esp", GetExceptionCode()); }
        __try { SoundEsp::Draw(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.sound_esp", GetExceptionCode()); }
        __try { World::Weather::Draw(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Con::Seh("hkPresent.weather", GetExceptionCode()); }

        __try {
            ImGui::Render();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Con::Seh("hkPresent.ImGuiRender", GetExceptionCode());
            __try { ImGui::EndFrame(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
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
    static bool s_inited = false;
    if (s_inited)
        return;
    s_inited = true;

    // AllocConsole fails if a console already exists — still attach + log
    const bool allocated = ::AllocConsole() != FALSE;
    if (!allocated && ::GetConsoleWindow() == nullptr) {
        // No console available; continue with file + OutputDebugString only
    }

    FILE* conOut = nullptr;
    freopen_s(&conOut, "CONOUT$", "w", stdout);
    freopen_s(&conOut, "CONOUT$", "w", stderr);

    // Larger scrollback for long debug sessions
    if (HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); hOut && hOut != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFOEX info{};
        info.cbSize = sizeof(info);
        if (GetConsoleScreenBufferInfoEx(hOut, &info)) {
            if (info.dwSize.Y < 4000)
                info.dwSize.Y = 4000;
            // Windows quirk: must re-apply ColorTable or Set*Ex can corrupt palette
            SetConsoleScreenBufferInfoEx(hOut, &info);
        }
    }

    char logDir[MAX_PATH]{};
    char logPath[MAX_PATH]{};
    char latestPath[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableA("USERPROFILE", logDir, sizeof(logDir));
    if (envLen > 0 && envLen < sizeof(logDir)) {
        strcat_s(logDir, "\\Documents\\Lefrizzel AI");
        CreateDirectoryA(logDir, nullptr);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        _snprintf_s(logPath, sizeof(logPath), _TRUNCATE,
            "%s\\LefrizzelAI_%04u%02u%02u_%02u%02u%02u.log",
            logDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fopen_s(&g_logFile, logPath, "w");

        // Stable "latest" alias (overwrite each session)
        _snprintf_s(latestPath, sizeof(latestPath), _TRUNCATE, "%s\\LefrizzelAI.log", logDir);
        FILE* latest = nullptr;
        if (fopen_s(&latest, latestPath, "w") == 0 && latest) {
            fprintf(latest, "Lefrizzel AI debug log alias\nSee: %s\n", logPath);
            fclose(latest);
        }
    }

    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hConsole == INVALID_HANDLE_VALUE)
        g_hConsole = nullptr;

    Con::Init(g_hConsole, g_logFile);
    initDebug();

    wchar_t title[128];
    swprintf_s(title, L"Lefrizzel AI DEBUG  pid=%lu", GetCurrentProcessId());
    SetConsoleTitleW(title);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    Con::Section("console");
    Con::Ok("ready  pid=%lu  alloc=%d", GetCurrentProcessId(), allocated ? 1 : 0);
    if (logPath[0])
        Con::Info("log     %s", logPath);
    else
        Con::Warn("log     USERPROFILE missing — file logging off");
    if (latestPath[0])
        Con::Info("alias   %s", latestPath);
    Con::Info("ods     OutputDebugString -> VS Output");
    Con::Info("env     LEFRIZZEL_LOG_TRACE=1  verbose Trace (TRY / timers)");
    Con::Info("env     LEFRIZZEL_LOG_FLUSH=1  flush every line");
    Con::Info("env     LEFRIZZEL_LOG_TID=1    show thread id");
    Con::Info("format  HH:MM:SS.mmm  LVL  [tag]  message");
    Con::Info("        ...............|  key   value   (detail lines)");
    Con::Info("levels  TRC  OK  INF  WRN  ERR  SEH");
}
#else
void init_console() {}
#endif
DWORD WINAPI MainThread(LPVOID lpReserved)
{
    __try {
        // Wait for game modules — manual map often lands before client/d3d ready
        while (!GetModuleHandleA("d3d11.dll") || !GetModuleHandleA("client.dll")) {
            if (GetAsyncKeyState(VK_F4) & 0x8000)
                break;
            Sleep(100);
        }

        if (!GetModuleHandleA("d3d11.dll") || !GetModuleHandleA("client.dll")) {
            // Aborted wait (F4) before modules — exit cleanly
            if (g_ManualMapped)
                ExitThread(0);
            FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpReserved), 0);
            return 0;
        }

        // Swap chain may lag modules; brief settle
        Sleep(500);

        init_console();

        bool init_hook = false;
        int kieroFails = 0;
        do
        {
            // hook hkPresent — full cheat init happens on first Present (DX ready)
            if (!init_hook) {
                __try {
                    const auto st = kiero::init(kiero::RenderType::D3D11);
                    if (st == kiero::Status::Success
                        || st == kiero::Status::AlreadyInitializedError)
                    {
                        if (kiero::bind(8, (void**)&oPresent, hkPresent)
                            == kiero::Status::Success) {
                            init_hook = true;
                            Con::Ok("kiero Present hooked (idx 8)");
                        } else {
                            ++kieroFails;
                            Con::Warn("kiero bind Present failed (try %d)", kieroFails);
                            Sleep(300);
                        }
                    } else {
                        ++kieroFails;
                        Sleep(300);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // D3D11 not ready — retry
                    ++kieroFails;
                    Sleep(500);
                }
            }
        } while (!init_hook && !(GetAsyncKeyState(VK_F4) & 0x8000));

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
    // Drop Con handles before closing FILE*/console (hooks may still race briefly)
    Con::Stats();
    Con::Shutdown();
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_hConsole = nullptr;
    ::FreeConsole();
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
