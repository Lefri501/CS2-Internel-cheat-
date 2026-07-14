#include "templeware.h"

#include "utils/module/module.h"
#include "utils/console/console.h"
#include "utils/security/memsecurity.h"
#include "utils/security/vacdetect.h"

#include <cstdio>

// Defined in main.cpp
extern HMODULE g_OurModule;
extern bool    g_ManualMapped;

void TempleWare::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView) {
    Con::Section("INIT");
    Con::ScopedTimer initTimer("TempleWare::init", 0);

    Con::Info("modules...");
    modules.init();

    Con::Info("menu...");
    renderer.menu.init(window, pDevice, pContext, mainRenderTargetView);

    Con::Info("schema...");
    if (!schema.init("client.dll", 0))
        Con::Error("Schema initialization FAILED");

    Con::Info("interfaces...");
    if (!interfaces.init())
        Con::Error("Interface initialization FAILED");

    Con::Info("visuals...");
    renderer.visuals.init();

    Con::Info("materials (chams)...");
    materials.init();

    Con::Info("hooks...");
    hooks.init();

    Con::Info("VAC monitor...");
    VacDetect::Install(nullptr);

    // Safe harden: RWX→RX on our image only. No PE wipe (keeps SEH/PE walks intact).
    // SafetyHook trampolines are external allocations — not touched.
    if (g_OurModule) {
        Con::Info("memory harden (RX)...");
        MemSecurity::HardenAfterInit(g_OurModule, false);
    }

    Con::Ok("Init complete");
    Con::Stats();
}
