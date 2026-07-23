#include "module.h"
#include "../console/console.h"
#include "../crypto/xorstr.h"
#include <Windows.h>
#include <cstring>

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
	// Critical first — hooks/schema depend on these
	const bool clientOk = registerModule(XS("client.dll"), "client");
	const bool engOk = registerModule(XS("engine2.dll"), "engine2");
	registerModule(XS("scenesystem.dll"), "scenesystem");
	registerModule(XS("particles.dll"), "particles");
	registerModule(XS("materialsystem2.dll"), "materialsystem2");
	registerModule(XS("tier0.dll"), "tier0");
	registerModule(XS("inputsystem.dll"), "inputsystem");
	registerModule(XS("resourcesystem.dll"), "resourcesystem");
	registerModule(XS("panorama.dll"), "panorama");
	// schemasystem used by Schema::init via GetModuleHandle, but register for scanners
	registerModule(XS("schemasystem.dll"), "schemasystem");

	if (!clientOk || !engOk)
		Con::Error("Critical module missing (client=%d engine2=%d) — patterns will fail",
			clientOk ? 1 : 0, engOk ? 1 : 0);
}

// Completely acceptable solution because there simply just aren't that many modules :)
// Accept short registry name ("client") or PE name ("client.dll").
uintptr_t Modules::getModule(const std::string &moduleName) {
	if (moduleName.empty())
		return 0;

	for (const Module &m : modules) {
		if (m.name == moduleName)
			return m.address;
	}

	// "client.dll" → "client"
	static const char kDll[] = ".dll";
	const size_t n = moduleName.size();
	if (n > 4) {
		const bool endsDll =
			_stricmp(moduleName.c_str() + (n - 4), kDll) == 0;
		if (endsDll) {
			const std::string shortName = moduleName.substr(0, n - 4);
			for (const Module &m : modules) {
				if (m.name == shortName)
					return m.address;
			}
		}
	}

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