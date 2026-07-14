// Skin image preview via Panorama (Andromeda approach) — not raw vtex decode.
// Path: panorama/.../foo_light_png.vtex_c → s2r://panorama/.../foo_light_png.vtex
#include "skin_preview.h"

#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"

#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

namespace SkinPreview {
namespace {

// ── Panorama image types (Andromeda IPanoramaUIEngine.hpp) ──────────────────

enum EImageFormat : std::uint32_t {
	EIMGFMT_RGBA8888 = 4,
};

struct PanoramaImageData_t {
	PanoramaImageData_t() {
		ZeroMemory(this, sizeof(*this));
		m_szImagePath = nullptr;
		m_iWidth = -1;
		m_iHeight = -1;
		m_iUnk1 = -1;
		m_iUnk2 = -1;
		m_flScale = 1.333f;
		m_iUnk3 = 1;
		m_iUnk4 = 1;
	}
	const char* m_szImagePath;
	int m_iWidth;
	int m_iHeight;
	int m_iUnk1;
	int m_iUnk2;
	float m_flScale;
	char pad0[0x30];
	int m_iUnk3;
	char pad1[0x18];
	int m_iUnk4;
	char pad2[0x2C];
};

class CPanoramaTextureDx11 {
public:
	char pad0[0x10];
	ID3D11ShaderResourceView* m_pSRV_SRGB;
	ID3D11ShaderResourceView* m_pSRV_UNORM;
};

class CSource2UITexture {
public:
	class CData {
	public:
		CPanoramaTextureDx11* m_pDx11Texture;
	};
	char pad0[0x28];
	CData* m_pData;
	int m_iWidth;
	int m_iHeight;
};

class CImageProxySource {
public:
	CSource2UITexture* GetTextureID() {
		using Fn = CSource2UITexture*(__fastcall*)(CImageProxySource*);
		return (*reinterpret_cast<Fn**>(this))[4](this);
	}
	int GetWidth() {
		using Fn = int(__fastcall*)(CImageProxySource*);
		return (*reinterpret_cast<Fn**>(this))[5](this);
	}
	int GetHeight() {
		using Fn = int(__fastcall*)(CImageProxySource*);
		return (*reinterpret_cast<Fn**>(this))[6](this);
	}
	void AddRef() {
		using Fn = void(__fastcall*)(CImageProxySource*);
		(*reinterpret_cast<Fn**>(this))[10](this);
	}
	void Release() {
		using Fn = void(__fastcall*)(CImageProxySource*);
		(*reinterpret_cast<Fn**>(this))[11](this);
	}
	ID3D11ShaderResourceView* GetNativeTexture() {
		if (!this) return nullptr;
		auto* pTex = GetTextureID();
		if (!pTex) return nullptr;
		auto* pData = pTex->m_pData;
		if (!pData) return nullptr;
		auto* pDX11 = pData->m_pDx11Texture;
		if (!pDX11) return nullptr;
		return pDX11->m_pSRV_SRGB ? pDX11->m_pSRV_SRGB : pDX11->m_pSRV_UNORM;
	}
};

class CImageResourceManager {
public:
	CImageProxySource* LoadImageInternal(const char* szPath, EImageFormat fmt) {
		PanoramaImageData_t imageData{};
		imageData.m_szImagePath = szPath;
		using Fn = CImageProxySource*(__fastcall*)(CImageResourceManager*, void*, void*,
			const char*, EImageFormat, PanoramaImageData_t*);
		return (*reinterpret_cast<Fn**>(this))[0](
			this, nullptr, nullptr, szPath, fmt, &imageData);
	}
};

class CUIEngineSource2 {
public:
	// slot 24 = CImageResourceManager (Andromeda verified 2026-07-09)
	CImageResourceManager* GetResourceManager() {
		using Fn = CImageResourceManager*(__fastcall*)(CUIEngineSource2*);
		return (*reinterpret_cast<Fn**>(this))[24](this);
	}
};

class IPanoramaUIEngine {
public:
	CUIEngineSource2* AccessUIEngine() {
		using Fn = CUIEngineSource2*(__fastcall*)(IPanoramaUIEngine*);
		return (*reinterpret_cast<Fn**>(this))[13](this);
	}
};

// ── SEH wrappers (no C++ destructors — C2712) ───────────────────────────────

__declspec(noinline) static ID3D11ShaderResourceView* SEH_GetNativeTexture(void* pImg) {
	ID3D11ShaderResourceView* pOut = nullptr;
	__try { pOut = reinterpret_cast<CImageProxySource*>(pImg)->GetNativeTexture(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { pOut = nullptr; }
	return pOut;
}

__declspec(noinline) static void SEH_GetImageSize(void* pImg, float* pW, float* pH) {
	*pW = 0.f; *pH = 0.f;
	__try {
		auto* p = reinterpret_cast<CImageProxySource*>(pImg);
		*pW = (float)p->GetWidth();
		*pH = (float)p->GetHeight();
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static CUIEngineSource2* SEH_AccessUIEngine(IPanoramaUIEngine* p) {
	CUIEngineSource2* pOut = nullptr;
	__try { pOut = p->AccessUIEngine(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return pOut;
}

__declspec(noinline) static CImageResourceManager* SEH_GetResourceManager(CUIEngineSource2* p) {
	CImageResourceManager* pOut = nullptr;
	__try { pOut = p->GetResourceManager(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return pOut;
}

__declspec(noinline) static CImageProxySource* SEH_LoadImageInternal(CImageResourceManager* pMgr, const char* pPath) {
	CImageProxySource* pOut = nullptr;
	__try { pOut = pMgr->LoadImageInternal(pPath, EIMGFMT_RGBA8888); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return pOut;
}

__declspec(noinline) static void SEH_AddRef(void* pImg) {
	__try { reinterpret_cast<CImageProxySource*>(pImg)->AddRef(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void SEH_Release(void* pImg) {
	__try { reinterpret_cast<CImageProxySource*>(pImg)->Release(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Cache ───────────────────────────────────────────────────────────────────

// resolved s2r path → CImageProxySource* (nullptr = confirmed missing)
struct ImgCacheEntry {
	void* proxy = nullptr;
	float w = 0.f;
	float h = 0.f;
};
static std::unordered_map<std::string, ImgCacheEntry> g_imgCache;
static ID3D11Device* g_device = nullptr;
static bool g_lastWasLoading = false;
static float g_lastNativeW = 0.f;
static float g_lastNativeH = 0.f;

// #region agent log
static void Dbg(const char* hyp, const char* loc, const char* msg, const char* dataJson) {
	std::ofstream f(
		R"(C:\Users\Administrator\Desktop\cs2 project\TempleWare-CS2_[unknowncheats.me]_\debug-9cb1e6.log)",
		std::ios::app);
	if (!f) return;
	f << "{\"sessionId\":\"9cb1e6\",\"runId\":\"skin-preview\",\"hypothesisId\":\"" << hyp
		<< "\",\"location\":\"" << loc << "\",\"message\":\"" << msg
		<< "\",\"data\":" << (dataJson ? dataJson : "{}")
		<< ",\"timestamp\":" << (long long)GetTickCount64() << "}\n";
}
// #endregion

static std::string ResolveImagePath(const std::string& iconPath) {
	if (iconPath.empty()) return {};
	std::string path = iconPath;
	// vtex_c → vtex (Andromeda)
	if (path.size() > 2 && path.compare(path.size() - 2, 2, "_c") == 0)
		path.resize(path.size() - 2);
	if (path.compare(0, 6, "s2r://") != 0)
		path = "s2r://" + path;
	return path;
}

static ID3D11ShaderResourceView* LoadSrv(const std::string& iconPath) {
	g_lastWasLoading = false;
	g_lastNativeW = 0.f;
	g_lastNativeH = 0.f;
	if (iconPath.empty()) return nullptr;

	const std::string path = ResolveImagePath(iconPath);
	if (path.empty()) return nullptr;

	auto it = g_imgCache.find(path);
	if (it != g_imgCache.end()) {
		if (!it->second.proxy) return nullptr; // missing asset
		g_lastNativeW = it->second.w;
		g_lastNativeH = it->second.h;
		auto* srv = SEH_GetNativeTexture(it->second.proxy);
		if (!srv) {
			g_lastWasLoading = true;
			return nullptr;
		}
		// Refresh size if still unknown
		if (g_lastNativeW <= 0.f || g_lastNativeH <= 0.f) {
			SEH_GetImageSize(it->second.proxy, &it->second.w, &it->second.h);
			g_lastNativeW = it->second.w;
			g_lastNativeH = it->second.h;
		}
		return srv;
	}

	IPanoramaUIEngine* pPanorama = I::Get<IPanoramaUIEngine>("panorama.dll", "PanoramaUIEngine001");
	if (!pPanorama) {
		Dbg("P", "LoadSrv", "no_panorama", "{}");
		return nullptr;
	}

	CUIEngineSource2* pUI = SEH_AccessUIEngine(pPanorama);
	if (!pUI) {
		Dbg("P", "LoadSrv", "no_uiengine", "{}");
		return nullptr;
	}

	CImageResourceManager* pRes = SEH_GetResourceManager(pUI);
	if (!pRes) {
		Dbg("P", "LoadSrv", "no_resmgr", "{}");
		return nullptr;
	}

	CImageProxySource* pImage = SEH_LoadImageInternal(pRes, path.c_str());
	if (!pImage) {
		g_imgCache[path] = {};
		Dbg("P", "LoadSrv", "load_fail", "{}");
		return nullptr;
	}

	SEH_AddRef(pImage);
	ImgCacheEntry entry{};
	entry.proxy = pImage;
	SEH_GetImageSize(pImage, &entry.w, &entry.h);
	g_imgCache[path] = entry;
	g_lastNativeW = entry.w;
	g_lastNativeH = entry.h;

	auto* srv = SEH_GetNativeTexture(pImage);
	if (!srv) {
		g_lastWasLoading = true;
		Dbg("P", "LoadSrv", "decoding", "{}");
		return nullptr;
	}

	char data[96];
	sprintf_s(data, "{\"w\":%.0f,\"h\":%.0f}", entry.w, entry.h);
	Dbg("P", "LoadSrv", "ok", data);
	return srv;
}

} // namespace

void Init(ID3D11Device* device) {
	g_device = device;
	Dbg("P", "Init", "panorama_mode", device ? "{\"ok\":1}" : "{\"ok\":0}");
}

void Shutdown() {
	for (auto& kv : g_imgCache) {
		if (kv.second.proxy) SEH_Release(kv.second.proxy);
	}
	g_imgCache.clear();
	g_device = nullptr;
}

std::string PaintPath(const char* simpleName, const char* kitToken) {
	if (!simpleName || !*simpleName || !kitToken || !*kitToken) return {};
	char buf[512];
	if (sprintf_s(buf, "panorama/images/econ/default_generated/%s_%s_light_png.vtex_c",
			simpleName, kitToken) <= 0)
		return {};
	return buf;
}

std::string ModelPath(const char* simpleName) {
	if (!simpleName || !*simpleName) return {};
	char buf[512];
	if (sprintf_s(buf, "panorama/images/econ/weapons/base_weapons/%s_png.vtex_c", simpleName) <= 0)
		return {};
	return buf;
}

ImTextureID Get(const std::string& path) {
	(void)g_device;
	ID3D11ShaderResourceView* srv = LoadSrv(path);
	if (!srv) return ImTextureID_Invalid;
	return (ImTextureID)(std::uintptr_t)srv;
}

ImTextureID GetPaint(const char* simpleName, const char* kitToken, int paintKitId) {
	if (!simpleName || !*simpleName) return ImTextureID_Invalid;
	if (paintKitId > 0 && kitToken && *kitToken && std::strcmp(kitToken, "Vanilla") != 0) {
		ImTextureID t = Get(PaintPath(simpleName, kitToken));
		if (t != ImTextureID_Invalid || g_lastWasLoading)
			return t;
	}
	return Get(ModelPath(simpleName));
}

bool DrawPanel(ImTextureID tex, float maxH) {
	if (tex == ImTextureID_Invalid) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.65f, 0.9f));
		ImGui::TextWrapped(g_lastWasLoading ? "Loading preview..." : "No preview");
		ImGui::PopStyleColor();
		return false;
	}

	const float availW = ImGui::GetContentRegionAvail().x;
	const float boxW = availW;
	const float boxH = maxH > 0.f ? maxH : 150.f;

	// Fit inside box, keep aspect ratio (letterbox). CS2 econ icons are ~4:3 landscape.
	float nw = g_lastNativeW;
	float nh = g_lastNativeH;
	if (nw <= 1.f || nh <= 1.f) {
		nw = 512.f;
		nh = 384.f;
	}
	const float scaleW = boxW / nw;
	const float scaleH = boxH / nh;
	const float scale = (scaleW < scaleH) ? scaleW : scaleH;
	const float drawW = nw * scale;
	const float drawH = nh * scale;

	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(boxW, boxH));
	const float ox = cursor.x + (boxW - drawW) * 0.5f;
	const float oy = cursor.y + (boxH - drawH) * 0.5f;
	ImGui::GetWindowDrawList()->AddImage(
		ImTextureRef(tex),
		ImVec2(ox, oy),
		ImVec2(ox + drawW, oy + drawH));
	return true;
}

void DrawHover(ImTextureID tex, float size) {
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) return;
	ImGui::BeginTooltip();
	if (tex != ImTextureID_Invalid) {
		float nw = g_lastNativeW;
		float nh = g_lastNativeH;
		if (nw <= 1.f || nh <= 1.f) { nw = 512.f; nh = 384.f; }
		const float maxW = size;
		const float maxH = size * 0.75f;
		const float scaleW = maxW / nw;
		const float scaleH = maxH / nh;
		const float scale = (scaleW < scaleH) ? scaleW : scaleH;
		ImGui::Image(ImTextureRef(tex), ImVec2(nw * scale, nh * scale));
	} else {
		ImGui::TextDisabled(g_lastWasLoading ? "Loading preview..." : "No preview");
	}
	ImGui::EndTooltip();
}

} // namespace SkinPreview
