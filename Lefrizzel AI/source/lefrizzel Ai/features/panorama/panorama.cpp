#include "panorama.h"

#include "../../config/config.h"
#include "../../hooks/includeHooks.h"
#include "../../keybinds/keybinds.h"
#include "../../utils/console/console.h"
#include "../../utils/crypto/xorstr.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/security/vacdetect.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

	// ── Resolved ─────────────────────────────────────────────────────────────

	using RunFrameFn = __int64(__fastcall*)(void* uiEngine);
	// IDA: RunScript(engine, panel, scriptUtf8, originPath, flags)
	using RunScriptFn = __int64(__fastcall*)(void* uiEngine, void* panel, const char* script,
		const char* originPath, __int64 flags);
	using MatchFoundFn = void(__fastcall*)(void* thisptr, void* kv);
	using GetLobbyFn = void*(__fastcall*)();
	// IDA sub_180F5BD20 — PanoramaComponent_Lobby_ReadyUpForMatch
	using ReadyUpFn = void(__fastcall*)(void* lobby, char ready, int a3, int a4, char confirm);

	CInlineHookObj<RunFrameFn> g_hkRunFrame{};
	CInlineHookObj<MatchFoundFn> g_hkMatchFound{};

	RunScriptFn g_fnRunScript = nullptr;
	GetLobbyFn g_fnGetLobby = nullptr;
	ReadyUpFn g_fnReadyUp = nullptr;
	void** g_ppPanelMgr = nullptr; // client qword (FindPanel @ +0x4D0) — NOT RunFrame CUIEngine*

	std::atomic<void*> g_uiEngine{ nullptr };
	std::atomic<void*> g_cachedScriptPanel{ nullptr }; // last good IUIPanel
	// Set true only after panel lookup is verified safe (wrong vfuncs crash hard).
	constexpr bool kEnableKeybindStrip = false;
	// Temporary: skip installing RunFrame/MatchFound hooks so inject stays stable.
	constexpr bool kInstallPanoramaHooks = false;

	// Auto-accept retry window
	std::atomic<int> g_acceptAttempts{ 0 };
	std::atomic<ULONGLONG> g_acceptUntil{ 0 };
	std::atomic<ULONGLONG> g_lastAcceptTry{ 0 };

	constexpr int kMaxHudTicks = 16;
	struct HudTick {
		char id[32]{};
		Panorama::HudTickFn fn = nullptr;
	};
	HudTick g_ticks[kMaxHudTicks]{};
	CRITICAL_SECTION g_tickCs{};
	bool g_tickCsReady = false;

	void EnsureTickCs() {
		if (!g_tickCsReady) {
			InitializeCriticalSection(&g_tickCs);
			g_tickCsReady = true;
		}
	}

	// Client-stored UI ptr FindPanel @ +0x4D0 (cs_clientui CSGOHud path).
	// Do NOT call this on RunFrame's CUIEngine* — different vtable layout; wrong slot crashes.
	__declspec(noinline) static void* SehClientFindPanel(void* clientUi, const char* id) {
		if (!clientUi || !id || !id[0])
			return nullptr;
		void* out = nullptr;
		__try {
			void** vt = *reinterpret_cast<void***>(clientUi);
			if (!vt)
				return nullptr;
			using Fn = void*(__fastcall*)(void*, const char*);
			auto fn = reinterpret_cast<Fn>(vt[0x4D0 / 8]);
			if (!fn)
				return nullptr;
			out = fn(clientUi, id);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out = nullptr;
		}
		return out;
	}

	// Panel2D wrapper → IUIPanel* (+0x40). Do not call IsValidPanel on CUIEngine here:
	// that vfunc offset is easy to get wrong and will crash the UI thread.
	__declspec(noinline) static void* SehToUIPanel(void* panelOrWrapper) {
		if (!panelOrWrapper)
			return nullptr;
		void* out = nullptr;
		__try {
			void** vt = *reinterpret_cast<void***>(panelOrWrapper);
			if (!vt)
				return nullptr;
			using Fn = void*(__fastcall*)(void*);
			auto getUi = reinterpret_cast<Fn>(vt[0x40 / 8]);
			if (getUi)
				out = getUi(panelOrWrapper);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out = nullptr;
		}
		return out;
	}

	void* ResolveUIEngine() {
		return g_uiEngine.load(std::memory_order_relaxed);
	}

	void* ClientUiPtr() {
		if (!g_ppPanelMgr)
			return nullptr;
		void* mgr = nullptr;
		__try { mgr = *g_ppPanelMgr; }
		__except (EXCEPTION_EXECUTE_HANDLER) { mgr = nullptr; }
		if (!mgr || reinterpret_cast<uintptr_t>(mgr) < 0x10000ull)
			return nullptr;
		return mgr;
	}

	void* ToScriptPanel(void* found) {
		if (!found)
			return nullptr;
		if (void* ui = SehToUIPanel(found))
			return ui;
		return found;
	}

	// Only client FindPanel (+0x4D0). Never call that slot on RunFrame's CUIEngine*.
	void* ResolveScriptPanel(void* /*engine*/) {
		void* cached = g_cachedScriptPanel.load(std::memory_order_relaxed);
		if (cached)
			return cached;

		static const char* kIds[] = {
			"CSGOHud", "CSGOMainMenu", "CSGOPopupManager", "PopupManager"
		};

		void* clientUi = ClientUiPtr();
		if (!clientUi)
			return nullptr;

		for (const char* id : kIds) {
			void* ui = ToScriptPanel(SehClientFindPanel(clientUi, id));
			if (ui) {
				g_cachedScriptPanel.store(ui, std::memory_order_relaxed);
				return ui;
			}
		}
		return nullptr;
	}

	// IDA RunScript(a1, panel, script, originPath, flags):
	//   a4 = script-origin file path (const char*) — NOT an out ptr
	//   a5 != 0 forces compile-from-string (skip code cache)
	__declspec(noinline) static bool SehRunScript(void* engine, void* panel, const char* js) {
		if (!g_fnRunScript || !engine || !panel || !js)
			return false;
		bool ok = false;
		__try {
			g_fnRunScript(engine, panel, js, "panorama/lefrizzelAi.js", 1);
			ok = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		return ok;
	}


	__declspec(noinline) static void SehReadyUp(void* lobby, char confirm) {
		if (!g_fnReadyUp || !lobby)
			return;
		__try {
			// MatchFound uses (true, counts, 0); confirm=1 sets accept flag (IDA +0x178 path).
			g_fnReadyUp(lobby, 1, 0, 0, confirm);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	__declspec(noinline) static void* SehGetLobby() {
		if (!g_fnGetLobby)
			return nullptr;
		void* out = nullptr;
		__try { out = g_fnGetLobby(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
		return out;
	}

	void TryAutoAcceptOnce() {
		if (!Config::auto_accept)
			return;
		if (Config::loading.load(std::memory_order_acquire))
			return;
		if (VacDetect::IsSoftPaused())
			return;

		const ULONGLONG now = GetTickCount64();
		if (now > g_acceptUntil.load(std::memory_order_relaxed))
			return;
		if (g_acceptAttempts.load(std::memory_order_relaxed) <= 0)
			return;

		const ULONGLONG last = g_lastAcceptTry.load(std::memory_order_relaxed);
		if (now - last < 120ull)
			return;
		g_lastAcceptTry.store(now, std::memory_order_relaxed);

		void* lobby = SehGetLobby();
		if (!lobby)
			return;

		SehReadyUp(lobby, 1);
		g_acceptAttempts.fetch_sub(1, std::memory_order_relaxed);

		// JS backup only when we already have a validated script panel + engine.
		void* eng = g_uiEngine.load(std::memory_order_relaxed);
		void* panel = ResolveScriptPanel(eng);
		if (eng && panel) {
			static const char kJs[] =
				"(function(){"
				"try{"
				"$.DispatchEvent('PanoramaComponent_Lobby_ReadyUpForMatch',true,0,0);"
				"}catch(e){}"
				"})();";
			SehRunScript(eng, panel, kJs);
		}
	}

	void RunHudTicks() {
		EnsureTickCs();
		HudTick local[kMaxHudTicks]{};
		int n = 0;
		EnterCriticalSection(&g_tickCs);
		for (int i = 0; i < kMaxHudTicks; ++i) {
			if (g_ticks[i].fn)
				local[n++] = g_ticks[i];
		}
		LeaveCriticalSection(&g_tickCs);

		for (int i = 0; i < n; ++i) {
			__try { local[i].fn(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Con::Seh("Panorama HudTick", GetExceptionCode());
			}
		}
	}

	__int64 __fastcall hkRunFrame(void* uiEngine) {
		if (uiEngine)
			g_uiEngine.store(uiEngine, std::memory_order_relaxed);

		// Call original first — never risk skipping the UI frame on our failures.
		__int64 ret = 0;
		if (g_hkRunFrame.IsHooked()) {
			auto orig = g_hkRunFrame.GetOriginal();
			if (orig)
				ret = orig(uiEngine);
		}

		__try { TryAutoAcceptOnce(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}

		if constexpr (kEnableKeybindStrip) {
			__try { RunHudTicks(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		return ret;
	}

	void __fastcall hkMatchFound(void* thisptr, void* kv) {
		if (g_hkMatchFound.IsHooked()) {
			auto orig = g_hkMatchFound.GetOriginal();
			if (orig)
				orig(thisptr, kv);
		}
		if (Config::auto_accept && !Config::loading.load(std::memory_order_acquire)
			&& !VacDetect::IsSoftPaused()) {
			Panorama::RequestAutoAccept();
		}
	}

	// ── Keybind strip (Panorama HUD) ─────────────────────────────────────────

	void AppendJsEscaped(char* dst, size_t dstSz, size_t& used, const char* src) {
		if (!dst || !src || used >= dstSz)
			return;
		for (const char* p = src; *p; ++p) {
			if (used + 2 >= dstSz)
				break;
			const char c = *p;
			if (c == '\\' || c == '\'' || c == '"') {
				dst[used++] = '\\';
				if (used >= dstSz)
					break;
			}
			if (c == '\n' || c == '\r')
				continue;
			dst[used++] = c;
		}
		if (used < dstSz)
			dst[used] = 0;
	}

	void HideKeybindStrip() {
		static const char kHide[] =
			"(function(){try{"
			"var h=$.GetContextPanel();if(!h)return;"
			"var s=h.FindChildTraverse('TwKbStrip');"
			"if(s){s.visible=false;s.DeleteAsync(0.0);}"
			"}catch(e){}})();";
		Panorama::RunScript(kHide);
	}

	void KeybindStripTick() {
		static char s_lastSig[384]{};
		static bool s_shown = false;

		if (Config::loading.load(std::memory_order_acquire)
			|| VacDetect::IsSoftPaused())
			return;

		if (!Config::hud_keybind_strip) {
			if (s_shown) {
				HideKeybindStrip();
				s_shown = false;
				s_lastSig[0] = 0;
			}
			return;
		}

		KeybindSnapshot snaps[8]{};
		const int nAll = keybind.listSnapshots(snaps, 8);
		KeybindSnapshot rows[8]{};
		int n = 0;
		bool anyActive = false;

		for (int i = 0; i < nAll; ++i) {
			if (!snaps[i].enabled)
				continue;
			if (snaps[i].active)
				anyActive = true;
		}

		if (Config::widget_keybinds_only_when_active && !anyActive) {
			if (s_shown) {
				HideKeybindStrip();
				s_shown = false;
				s_lastSig[0] = 0;
			}
			return;
		}

		const bool showAll = Config::widget_keybinds_show_all;
		for (int i = 0; i < nAll && n < 8; ++i) {
			if (!snaps[i].enabled)
				continue;
			if (!showAll && !snaps[i].active)
				continue;
			rows[n++] = snaps[i];
		}

		// Dirty check — skip RunScript when nothing changed
		char sig[384]{};
		size_t su = 0;
		if (n <= 0) {
			std::snprintf(sig, sizeof(sig), "empty");
		} else {
			for (int i = 0; i < n; ++i) {
				char keyNm[32]{};
				Keybinds::formatKeyName(rows[i].key, keyNm, sizeof(keyNm));
				const int wrote = std::snprintf(sig + static_cast<int>(su),
					sizeof(sig) - su, "%s|%s|%d;",
					rows[i].name ? rows[i].name : "?",
					keyNm, rows[i].active ? 1 : 0);
				if (wrote <= 0)
					break;
				su += static_cast<size_t>(wrote);
				if (su >= sizeof(sig))
					break;
			}
		}
		if (s_shown && std::strcmp(sig, s_lastSig) == 0)
			return;
		std::snprintf(s_lastSig, sizeof(s_lastSig), "%s", sig);

		// Build JS: create bottom strip + chips
		char js[4096]{};
		size_t ju = 0;
		auto append = [&](const char* s) {
			if (!s || ju >= sizeof(js))
				return;
			const size_t len = std::strlen(s);
			if (ju + len >= sizeof(js))
				return;
			std::memcpy(js + ju, s, len);
			ju += len;
			js[ju] = 0;
		};

		append(
			"(function(){try{"
			"var h=$.GetContextPanel();if(!h)return;"
			"var strip=h.FindChildTraverse('TwKbStrip');"
			"if(strip){strip.DeleteAsync(0.0);strip=null;}"
			"strip=$.CreatePanel('Panel',h,'TwKbStrip',{"
			"style:'width:fit-children;height:fit-children;horizontal-align:center;"
			"vertical-align:bottom;margin-bottom:78px;flow-children:right;"
			"padding:6px 10px;background-color:rgba(12,14,16,0.92);"
			"border:1px solid rgba(90,96,102,0.95);border-radius:6px;'"
			"});"
			"if(!strip)return;"
			"strip.hittest=false;strip.hittestchildren=false;"
			"strip.visible=true;"
			"var items=[");

		if (n <= 0) {
			append("{n:'Keybinds',k:'—',on:0}");
		} else {
			for (int i = 0; i < n; ++i) {
				char keyNm[32]{};
				Keybinds::formatKeyName(rows[i].key, keyNm, sizeof(keyNm));
				if (i)
					append(",");
				append("{n:'");
				AppendJsEscaped(js, sizeof(js), ju, rows[i].name ? rows[i].name : "Bind");
				append("',k:'");
				AppendJsEscaped(js, sizeof(js), ju, keyNm);
				append("',on:");
				append(rows[i].active ? "1" : "0");
				append("}");
			}
		}

		append(
			"];"
			"for(var i=0;i<items.length;i++){"
			"var it=items[i];"
			"var chip=$.CreatePanel('Panel',strip,'TwKbChip'+i,{"
			"style:'flow-children:right;padding:4px 10px;margin:0px 3px;border-radius:4px;'"
			"});"
			"chip.hittest=false;"
			"chip.style.backgroundColor=it.on?'rgba(140,194,220,0.28)':'rgba(255,255,255,0.05)';"
			"var nl=$.CreatePanel('Label',chip,'TwKbN'+i);"
			"nl.hittest=false;nl.text=it.n;"
			"nl.style.fontSize='14px';"
			"nl.style.marginRight='7px';"
			"nl.style.color=it.on?'#e8f6fb':'#9aa0a8';"
			"var kl=$.CreatePanel('Label',chip,'TwKbK'+i);"
			"kl.hittest=false;kl.text=it.k;"
			"kl.style.fontSize='13px';"
			"kl.style.fontWeight='bold';"
			"kl.style.color=it.on?'#ffffff':'#6a7078';"
			"}"
			"}catch(e){$.Msg('[TW] kb strip: '+e);}})();");

		void* eng = ResolveUIEngine();
		void* panel = ResolveScriptPanel(eng);
		if (!eng) {
			Con::Rate("pano_kb_noeng", 2000, "Keybind strip: no UIEngine");
			return;
		}
		if (!panel) {
			void* clientUi = ClientUiPtr();
			Con::Rate("pano_kb_nopanel", 2000,
				"Keybind strip: no IUIPanel eng=%p clientUi=%p findHud=%p",
				eng, clientUi, SehClientFindPanel(clientUi, "CSGOHud"));
			return;
		}
		if (SehRunScript(eng, panel, js)) {
			s_shown = true;
		} else {
			Con::Rate("pano_kb_seh", 2000,
				"Keybind strip: RunScript SEH eng=%p panel=%p", eng, panel);
		}
	}

} // namespace

void Panorama::Install() {
	if constexpr (!kInstallPanoramaHooks) {
		Con::Info("Panorama hooks disabled (crash triage)");
		return;
	}

	// CUIEngine::RunFrame — panorama.dll
	{
		uintptr_t addr = M::patternScan(XS("panorama"),
			XS("48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00 45 33 F6"));
		if (addr) {
			if (!g_hkRunFrame.Add(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&hkRunFrame)))
				Con::Error("Panorama RunFrame hook.Add failed");
			else
				Con::Ok("Panorama RunFrame @ 0x%p", (void*)addr);
		} else {
			Con::OffsetMiss("Panorama RunFrame");
		}
	}

	// CUIEngine::RunScript
	g_fnRunScript = reinterpret_cast<RunScriptFn>(M::patternScan(XS("panorama"),
		XS("48 89 5C 24 10 4C 89 4C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48")));
	if (g_fnRunScript)
		Con::Ok("Panorama RunScript @ 0x%p", (void*)g_fnRunScript);
	else
		Con::OffsetMiss("Panorama RunScript");

	// MatchFoundHandler — client.dll (popup_accept_match_found)
	{
		uintptr_t addr = M::patternScan(XS("client"),
			XS("48 85 D2 0F 84 ? ? ? ? 48 8B C4 55 53 56 57"));
		if (addr) {
			if (!g_hkMatchFound.Add(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&hkMatchFound)))
				Con::Error("MatchFoundHandler hook.Add failed");
			else
				Con::Ok("MatchFoundHandler @ 0x%p", (void*)addr);
		} else {
			Con::OffsetMiss("MatchFoundHandler");
		}
	}

	// Lobby component getter → this+0 used as ReadyUp target
	g_fnGetLobby = reinterpret_cast<GetLobbyFn>(M::patternScan(XS("client"),
		XS("40 53 48 83 EC 20 48 8B 0D ? ? ? ? 33 DB 48 85 C9")));
	if (g_fnGetLobby)
		Con::Ok("GetLobbyComponent @ 0x%p", (void*)g_fnGetLobby);
	else
		Con::OffsetMiss("GetLobbyComponent");

	g_fnReadyUp = reinterpret_cast<ReadyUpFn>(M::patternScan(XS("client"),
		XS("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 0F B6")));
	if (g_fnReadyUp)
		Con::Ok("Lobby ReadyUp @ 0x%p", (void*)g_fnReadyUp);
	else
		Con::OffsetMiss("Lobby ReadyUp");

	// Client UI FindPanel global: mov rcx,[rip] ; mov rdx,[rcx] ; mov r8,[rdx+4D0h] ; lea rdx,"CSGOHud"
	{
		uintptr_t addr = M::patternScan(XS("client"),
			XS("48 8B 0D ? ? ? ? 48 8B 11 4C 8B 82 D0 04 00 00 48 8D 15"));
		if (addr) {
			g_ppPanelMgr = reinterpret_cast<void**>(M::getAbsoluteAddress(addr, 3));
			Con::Ok("PanelMgr @ 0x%p", (void*)g_ppPanelMgr);
		} else {
			Con::OffsetMiss("PanelMgr FindPanel");
		}
	}

	if constexpr (kEnableKeybindStrip)
		RegisterHudTick(XS("keybind_strip"), &KeybindStripTick);
}

void* Panorama::UIEngine() {
	return g_uiEngine.load(std::memory_order_relaxed);
}

void* Panorama::FindPanel(const char* id) {
	if (!id || !id[0])
		return nullptr;
	void* clientUi = ClientUiPtr();
	if (!clientUi)
		return nullptr;
	return SehClientFindPanel(clientUi, id);
}

bool Panorama::RunScript(const char* js) {
	void* eng = ResolveUIEngine();
	void* panel = ResolveScriptPanel(eng);
	if (!eng || !panel || !js)
		return false;
	return SehRunScript(eng, panel, js);
}

bool Panorama::RunScript(void* panel, const char* js) {
	if (!panel || !js)
		return false;
	void* engine = ResolveUIEngine();
	if (!engine)
		return false;
	void* ui = ToScriptPanel(panel);
	return SehRunScript(engine, ui ? ui : panel, js);
}

bool Panorama::DispatchEvent(const char* eventName) {
	if (!eventName || !eventName[0])
		return false;
	// Keep script tiny / stack-friendly
	char buf[384]{};
	const int n = std::snprintf(buf, sizeof(buf),
		"(function(){try{$.DispatchEvent('%s');}catch(e){}})();", eventName);
	if (n <= 0 || n >= static_cast<int>(sizeof(buf)))
		return false;
	// Reject quotes in event name (injection / broken JS)
	for (const char* p = eventName; *p; ++p) {
		if (*p == '\'' || *p == '"' || *p == '\\')
			return false;
	}
	return RunScript(buf);
}

bool Panorama::RegisterHudTick(const char* id, HudTickFn fn) {
	if (!id || !id[0] || !fn)
		return false;
	EnsureTickCs();
	EnterCriticalSection(&g_tickCs);
	int freeSlot = -1;
	for (int i = 0; i < kMaxHudTicks; ++i) {
		if (g_ticks[i].fn && std::strcmp(g_ticks[i].id, id) == 0) {
			g_ticks[i].fn = fn;
			LeaveCriticalSection(&g_tickCs);
			return true;
		}
		if (!g_ticks[i].fn && freeSlot < 0)
			freeSlot = i;
	}
	bool ok = false;
	if (freeSlot >= 0) {
		std::snprintf(g_ticks[freeSlot].id, sizeof(g_ticks[freeSlot].id), "%s", id);
		g_ticks[freeSlot].fn = fn;
		ok = true;
	}
	LeaveCriticalSection(&g_tickCs);
	return ok;
}

void Panorama::UnregisterHudTick(const char* id) {
	if (!id)
		return;
	EnsureTickCs();
	EnterCriticalSection(&g_tickCs);
	for (int i = 0; i < kMaxHudTicks; ++i) {
		if (g_ticks[i].fn && std::strcmp(g_ticks[i].id, id) == 0) {
			g_ticks[i].fn = nullptr;
			g_ticks[i].id[0] = 0;
			break;
		}
	}
	LeaveCriticalSection(&g_tickCs);
}

void Panorama::RequestAutoAccept() {
	g_acceptAttempts.store(12, std::memory_order_relaxed);
	g_acceptUntil.store(GetTickCount64() + 8000ull, std::memory_order_relaxed);
	g_lastAcceptTry.store(0, std::memory_order_relaxed);
	TryAutoAcceptOnce();
}
