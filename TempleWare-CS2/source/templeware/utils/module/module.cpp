#include "module.h"
#include "../console/console.h"
#include "../crypto/xorstr.h"
#include <Windows.h>

Module::Module(uintptr_t moduleAddress, const std::string &moduleName) : address(moduleAddress), name(moduleName) {}

// Wait up to ~10s for a module to appear (game DLLs load asynchronously)
static HMODULE WaitForModule(const char* name, int maxRetries = 200) {
    HMODULE h = nullptr;
    for (int i = 0; i < maxRetries && !h; ++i) {
        h = GetModuleHandleA(name);
        if (!h) Sleep(50);
    }
    return h;
}

void Modules::init() {
	registerModule(XS("client.dll"), "client");
	registerModule(XS("scenesystem.dll"), "scenesystem");
	registerModule(XS("particles.dll"), "particles");
	registerModule(XS("materialsystem2.dll"), "materialsystem2");
	registerModule(XS("tier0.dll"), "tier0");
	registerModule(XS("engine2.dll"), "engine2");
	registerModule(XS("inputsystem.dll"), "inputsystem");
	registerModule(XS("resourcesystem.dll"), "resourcesystem");
}

// Completely acceptable solution because there simply just aren't that many modules :)
uintptr_t Modules::getModule(const std::string &moduleName) {
	for (const Module &m : modules) {
		if (m.name == moduleName) {
			return m.address;
		}
	}
	// No module found
	return 0;
}

bool Modules::registerModule(const std::string &aModuleName, const std::string &moduleName) {
	// Use WaitForModule for robustness under manual map (injected before all game DLLs load)
	HMODULE moduleHandle = WaitForModule(aModuleName.c_str());

	if (!moduleHandle) {
		Con::Error("Failed to resolve module: %s (as %s)", aModuleName.c_str(), moduleName.c_str());
		return false;
	}

	Module module(reinterpret_cast<uintptr_t>(moduleHandle), moduleName);
	modules.emplace_back(module);

	Con::Ok("Module: %s (as %s) -> 0x%llX", aModuleName.c_str(), moduleName.c_str(), (uintptr_t)moduleHandle);
	return true;
}

Modules modules;