#include "lefrizzel Ai.h"

#include "utils/module/module.h"

#include "utils/console/console.h"
#include "utils/security/memsecurity.h"
#include "utils/security/vacdetect.h"

#include <cstdio>

// Defined in main.cpp
extern HMODULE g_OurModule;
extern bool    g_ManualMapped;

void LefrizzelAi::init(HWND& window, ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11RenderTargetView* mainRenderTargetView) {
    Con::Section("init");
    Con::ScopedTimer initTimer("LefrizzelAI::init", 0);

    Con::Tag("init", Con::Level::Info, "modules...");
    modules.init();

    Con::Tag("init", Con::Level::Info, "menu...");
    renderer.menu.init(window, pDevice, pContext, mainRenderTargetView);

    Con::Tag("init", Con::Level::Info, "schema...");
    if (!schema.init("client.dll", 0))
        Con::Tag("init", Con::Level::Error, "schema FAILED");

    Con::Tag("init", Con::Level::Info, "interfaces...");
    if (!interfaces.init())
        Con::Tag("init", Con::Level::Error, "interfaces FAILED");

    Con::Tag("init", Con::Level::Info, "visuals...");
    renderer.visuals.init();

    Con::Tag("init", Con::Level::Info, "chams materials...");
    materials.init();

    Con::Tag("init", Con::Level::Info, "hooks...");
    hooks.init();

    Con::Tag("init", Con::Level::Info, "VAC monitor...");
    VacDetect::Install(nullptr);

    // Safe harden: RWX?RX on our image only. No PE wipe (keeps SEH/PE walks intact).
    // SafetyHook trampolines are external allocations � not touched.
    if (g_OurModule) {
        Con::Tag("init", Con::Level::Info, "memory harden (RX)...");
        MemSecurity::HardenAfterInit(g_OurModule, false);
    }

    Con::Tag("init", Con::Level::Ok, "complete");
    Con::Stats();
}
