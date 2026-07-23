// Skin image preview via Panorama (Andromeda approach) — not raw vtex decode.
// Path: panorama/.../foo_light_png.vtex_c → s2r://panorama/.../foo_light_png.vtex
#include "skin_preview.h"

#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"

#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <array>
#include <algorithm>
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
// Winning model path per simpleName — skip multi-root fanout after first resolve
static std::unordered_map<std::string, std::string> g_modelPathHit;
static ID3D11Device* g_device = nullptr;
static bool g_lastWasLoading = false;
static float g_lastNativeW = 0.f;
static float g_lastNativeH = 0.f;
// Panorama LoadImageInternal is expensive — cap new loads per Present frame.
// 1 was too low for knife model path fallbacks (4 roots × name variants).
static int g_loadsThisFrame = 0;
static DWORD g_loadFrameTick = 0;
constexpr int kMaxNewLoadsPerFrame = 3;

static void BeginFrameBudget()
{
	const DWORD t = GetTickCount();
	// ~16ms frame; reset budget when tick advances (Present rate)
	if (t != g_loadFrameTick) {
		g_loadFrameTick = t;
		g_loadsThisFrame = 0;
	}
}

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

	BeginFrameBudget();

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
		// Refresh size if still unknown (once, not every frame)
		if (g_lastNativeW <= 0.f || g_lastNativeH <= 0.f) {
			SEH_GetImageSize(it->second.proxy, &it->second.w, &it->second.h);
			g_lastNativeW = it->second.w;
			g_lastNativeH = it->second.h;
		}
		return srv;
	}

	// Budget: one new panorama load per frame — rest wait (pending, not fail)
	if (g_loadsThisFrame >= kMaxNewLoadsPerFrame) {
		g_lastWasLoading = true;
		return nullptr;
	}

	IPanoramaUIEngine* pPanorama = I::Get<IPanoramaUIEngine>("panorama.dll", "PanoramaUIEngine001");
	if (!pPanorama)
		return nullptr;

	CUIEngineSource2* pUI = SEH_AccessUIEngine(pPanorama);
	if (!pUI)
		return nullptr;

	CImageResourceManager* pRes = SEH_GetResourceManager(pUI);
	if (!pRes)
		return nullptr;

	++g_loadsThisFrame;
	CImageProxySource* pImage = SEH_LoadImageInternal(pRes, path.c_str());
	if (!pImage) {
		g_imgCache[path] = {}; // permanent miss
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
		return nullptr;
	}
	return srv;
}

} // namespace

void Init(ID3D11Device* device) {
	g_device = device;
}

void Shutdown() {
	for (auto& kv : g_imgCache) {
		if (kv.second.proxy) SEH_Release(kv.second.proxy);
	}
	g_imgCache.clear();
	g_modelPathHit.clear();
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
	// Primary CS2 base weapon / knife image
	if (sprintf_s(buf, "panorama/images/econ/weapons/base_weapons/%s_png.vtex_c", simpleName) <= 0)
		return {};
	return buf;
}

// Knife/gun base icons: try common panorama roots + short name (knife_m9_bayonet).
static const char* const kModelFmts[] = {
	"panorama/images/econ/weapons/base_weapons/%s_png.vtex_c",
	"panorama/images/econ/weapons/%s_png.vtex_c",
	"panorama/images/econ/default_generated/%s_light_png.vtex_c",
	"panorama/images/econ/default_generated/%s_png.vtex_c",
};

// weapon_knife_m9_bayonet → knife_m9_bayonet (some packs drop weapon_)
static void PushNameVariants(const char* simpleName, const char* out[4], int& n) {
	n = 0;
	if (!simpleName || !*simpleName) return;
	out[n++] = simpleName;
	if (std::strncmp(simpleName, "weapon_", 7) == 0 && simpleName[7]) {
		out[n++] = simpleName + 7; // knife_m9_bayonet / bayonet
	}
}

static ImTextureID GetModelAny(const char* simpleName) {
	if (!simpleName || !*simpleName) return ImTextureID_Invalid;

	// Fast path: already know which panorama root works
	auto hit = g_modelPathHit.find(simpleName);
	if (hit != g_modelPathHit.end()) {
		if (hit->second.empty())
			return ImTextureID_Invalid; // confirmed missing
		ImTextureID t = Get(hit->second);
		if (t != ImTextureID_Invalid || g_lastWasLoading)
			return t;
		// Cache stale (unloaded) — fall through and re-probe
		g_modelPathHit.erase(hit);
	}

	const char* names[4]{};
	int nn = 0;
	PushNameVariants(simpleName, names, nn);
	char buf[512];
	bool anyPending = false;
	for (int ni = 0; ni < nn; ++ni) {
		for (const char* fmt : kModelFmts) {
			if (sprintf_s(buf, fmt, names[ni]) <= 0)
				continue;
			ImTextureID t = Get(buf);
			if (t != ImTextureID_Invalid) {
				g_modelPathHit[simpleName] = buf;
				return t;
			}
			if (g_lastWasLoading)
				anyPending = true;
		}
	}
	// Don't cache miss while still downloading candidates
	if (!anyPending)
		g_modelPathHit[simpleName] = {};
	else
		g_lastWasLoading = true;
	return ImTextureID_Invalid;
}

ImTextureID Get(const std::string& path) {
	(void)g_device;
	ID3D11ShaderResourceView* srv = LoadSrv(path);
	if (!srv) return ImTextureID_Invalid;
	return (ImTextureID)(std::uintptr_t)srv;
}

ImTextureID GetPaint(const char* simpleName, const char* kitToken, int paintKitId) {
	if (!simpleName || !*simpleName) return ImTextureID_Invalid;

	// Prefer paint image when kit selected
	if (paintKitId > 0 && kitToken && *kitToken
		&& std::strcmp(kitToken, "Vanilla") != 0
		&& std::strcmp(kitToken, "vanilla") != 0) {
		ImTextureID t = Get(PaintPath(simpleName, kitToken));
		if (t != ImTextureID_Invalid)
			return t;
		// Loading or miss: still show base model so panel isn't blank/default CT knife
		ImTextureID m = GetModelAny(simpleName);
		if (m != ImTextureID_Invalid)
			return m;
		// Keep pending if paint still downloading
		if (g_lastWasLoading)
			return ImTextureID_Invalid;
	}

	return GetModelAny(simpleName);
}

bool PreviewPending() {
	return g_lastWasLoading;
}

static bool IsRgba8Format(DXGI_FORMAT fmt) {
	switch (fmt) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

static bool IsBgraFormat(DXGI_FORMAT fmt) {
	switch (fmt) {
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

bool SamplePaintPalette(const char* simpleName, const char* kitToken, int paintKitId, float outRgba[16]) {
	if (!outRgba || !simpleName || !*simpleName)
		return false;

	static std::unordered_map<int, std::array<float, 16>> s_paletteCache;
	if (paintKitId > 0) {
		auto it = s_paletteCache.find(paintKitId);
		if (it != s_paletteCache.end()) {
			std::memcpy(outRgba, it->second.data(), sizeof(float) * 16);
			return true;
		}
	}

	// Warm / resolve SRV (same path as menu preview)
	ImTextureID tex = GetPaint(simpleName, kitToken, paintKitId);
	if (tex == ImTextureID_Invalid) {
		Con::Rate("sp_noprev", 1500, "SamplePalette kit=%d no srv loading=%d",
			paintKitId, (int)g_lastWasLoading);
		return false;
	}

	auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>((std::uintptr_t)tex);
	if (!srv)
		return false;

	ID3D11Resource* res = nullptr;
	srv->GetResource(&res);
	if (!res)
		return false;

	ID3D11Texture2D* tex2d = nullptr;
	HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex2d);
	res->Release();
	if (FAILED(hr) || !tex2d) {
		Con::Rate("sp_not2d", 1500, "SamplePalette kit=%d QI Texture2D failed hr=%08X",
			paintKitId, (unsigned)hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	tex2d->GetDesc(&desc);
	if (desc.Width < 4 || desc.Height < 4) {
		tex2d->Release();
		return false;
	}

	// Panorama often serves BC1/BC3 compressed econ icons — cannot Map CPU.
	// Caller falls back to kit-id colour defaults (not stuck white Pending).
	if (!IsRgba8Format(desc.Format)) {
		Con::Rate("sp_fmt", 1500,
			"SamplePalette kit=%d unsupported fmt=%d %ux%u mips=%u (use defaults)",
			paintKitId, (int)desc.Format, desc.Width, desc.Height, desc.MipLevels);
		tex2d->Release();
		return false;
	}

	const bool bgra = IsBgraFormat(desc.Format);

	ID3D11Device* device = nullptr;
	tex2d->GetDevice(&device);
	if (!device) {
		tex2d->Release();
		return false;
	}
	ID3D11DeviceContext* ctx = nullptr;
	device->GetImmediateContext(&ctx);
	if (!ctx) {
		device->Release();
		tex2d->Release();
		return false;
	}

	// Staging: 1 mip. CopySubresourceRegion from source mip0 (CopyResource breaks on multi-mip).
	// Staging cannot use TYPELESS — map to UNORM.
	DXGI_FORMAT stageFmt = desc.Format;
	if (stageFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS)
		stageFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
	else if (stageFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS)
		stageFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
	else if (stageFmt == DXGI_FORMAT_B8G8R8X8_TYPELESS)
		stageFmt = DXGI_FORMAT_B8G8R8X8_UNORM;

	D3D11_TEXTURE2D_DESC stagingDesc{};
	stagingDesc.Width = desc.Width;
	stagingDesc.Height = desc.Height;
	stagingDesc.MipLevels = 1;
	stagingDesc.ArraySize = 1;
	stagingDesc.Format = stageFmt;
	stagingDesc.SampleDesc.Count = 1;
	stagingDesc.SampleDesc.Quality = 0;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;

	ID3D11Texture2D* staging = nullptr;
	hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
	if (FAILED(hr) || !staging) {
		Con::Rate("sp_stage", 1500, "SamplePalette kit=%d CreateTexture2D staging hr=%08X fmt=%d→%d",
			paintKitId, (unsigned)hr, (int)desc.Format, (int)stageFmt);
		device->Release();
		ctx->Release();
		tex2d->Release();
		return false;
	}

	ID3D11Texture2D* copySrc = tex2d;
	ID3D11Texture2D* resolved = nullptr;
	if (desc.SampleDesc.Count > 1) {
		D3D11_TEXTURE2D_DESC rd = stagingDesc;
		rd.Usage = D3D11_USAGE_DEFAULT;
		rd.CPUAccessFlags = 0;
		rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		hr = device->CreateTexture2D(&rd, nullptr, &resolved);
		if (FAILED(hr) || !resolved) {
			staging->Release();
			device->Release();
			ctx->Release();
			tex2d->Release();
			return false;
		}
		ctx->ResolveSubresource(resolved, 0, tex2d, 0, stageFmt);
		copySrc = resolved;
	}
	device->Release();

	// Copy only mip0 / slice0 — works regardless of source mip count
	ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, copySrc, 0, nullptr);
	ctx->Flush();

	D3D11_MAPPED_SUBRESOURCE mapped{};
	hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr) || !mapped.pData) {
		Con::Rate("sp_map", 1500, "SamplePalette kit=%d Map hr=%08X", paintKitId, (unsigned)hr);
		if (resolved) resolved->Release();
		staging->Release();
		ctx->Release();
		tex2d->Release();
		return false;
	}

	struct Bin {
		int count = 0;
		int r = 0, g = 0, b = 0;
	};
	// 4-bit/channel → 4096 bins (finer than old 3-bit)
	Bin bins[4096]{};
	const auto* base = static_cast<const uint8_t*>(mapped.pData);
	int sampled = 0;

	// Focus on weapon body — edges are dark letterbox / UI chrome that dominate greys
	const UINT x0 = desc.Width * 12u / 100u;
	const UINT x1 = desc.Width * 88u / 100u;
	const UINT y0 = desc.Height * 15u / 100u;
	const UINT y1 = desc.Height * 85u / 100u;
	const int stepX = (std::max)(1, (int)((x1 - x0) / 56u));
	const int stepY = (std::max)(1, (int)((y1 - y0) / 42u));

	for (UINT y = y0; y < y1; y += (UINT)stepY) {
		const uint8_t* row = base + y * mapped.RowPitch;
		for (UINT x = x0; x < x1; x += (UINT)stepX) {
			const uint8_t* p = row + x * 4;
			const int r = bgra ? p[2] : p[0];
			const int g = p[1];
			const int b = bgra ? p[0] : p[2];
			const int a = p[3];
			if (a < 50) continue;
			const int idx = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
			bins[idx].count++;
			bins[idx].r += r;
			bins[idx].g += g;
			bins[idx].b += b;
			++sampled;
		}
	}

	ctx->Unmap(staging, 0);
	if (resolved) resolved->Release();
	staging->Release();
	ctx->Release();
	tex2d->Release();

	if (sampled <= 0) {
		Con::Rate("sp_empty", 1500,
			"SamplePalette kit=%d empty read fmt=%d %ux%u mips=%u (copy failed?)",
			paintKitId, (int)desc.Format, desc.Width, desc.Height, desc.MipLevels);
		return false;
	}

	struct Cand {
		float r, g, b;
		float sat, val;
		int count;
		bool used;
	};
	Cand cands[4096];
	int nCand = 0;
	for (int i = 0; i < 4096; ++i) {
		if (bins[i].count <= 0) continue;
		Cand& c = cands[nCand++];
		c.r = (bins[i].r / (float)bins[i].count) / 255.f;
		c.g = (bins[i].g / (float)bins[i].count) / 255.f;
		c.b = (bins[i].b / (float)bins[i].count) / 255.f;
		c.count = bins[i].count;
		const float mx = (std::max)(c.r, (std::max)(c.g, c.b));
		const float mn = (std::min)(c.r, (std::min)(c.g, c.b));
		c.val = mx;
		c.sat = (mx > 1e-4f) ? (mx - mn) / mx : 0.f;
		c.used = false;
	}

	auto dist2 = [](const Cand& a, const Cand& b) {
		const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
		return dr * dr + dg * dg + db * db;
	};
	auto farEnough = [&](const Cand& c, const Cand* picked, int nPick) {
		for (int i = 0; i < nPick; ++i) {
			if (dist2(c, picked[i]) < 0.045f) // ~RGB Δ≈0.21
				return false;
		}
		return true;
	};

	Cand picked[4]{};
	int nPick = 0;
	const int minCount = (std::max)(2, sampled / 200);

	auto pickBest = [&](auto scoreFn) -> bool {
		int best = -1;
		float bestScore = -1.f;
		for (int i = 0; i < nCand; ++i) {
			if (cands[i].used || cands[i].count < minCount) continue;
			if (!farEnough(cands[i], picked, nPick)) continue;
			const float s = scoreFn(cands[i]);
			if (s > bestScore) {
				bestScore = s;
				best = i;
			}
		}
		if (best < 0) return false;
		cands[best].used = true;
		picked[nPick++] = cands[best];
		return true;
	};

	// 1-2: strongest chromatic accents (Asiimov orange, Doppler gem, etc.)
	auto accentScore = [](const Cand& c) {
		return c.sat * c.sat * (0.35f + std::log2f(1.f + (float)c.count));
	};
	pickBest(accentScore);
	pickBest(accentScore);

	// 3: lightest paint (white / ivory panels)
	pickBest([](const Cand& c) {
		if (c.val < 0.55f) return -1.f;
		return c.val * (1.f + 0.4f * c.sat) * std::log2f(1.f + (float)c.count);
	});

	// 4: mid-dark accent (not pure black — black multiplies albedo → dark gun)
	pickBest([](const Cand& c) {
		if (c.val > 0.55f || c.val < 0.18f) return -1.f;
		return (0.55f - std::fabs(c.val - 0.32f)) * (1.f + 0.35f * c.sat)
			* std::log2f(1.f + (float)c.count);
	});

	// Fill remaining with next-best distinct by population*sat
	while (nPick < 4) {
		if (!pickBest([](const Cand& c) {
			return (0.15f + c.sat) * std::log2f(1.f + (float)c.count);
		}))
			break;
	}
	if (nPick == 0)
		return false;

	for (int i = 0; i < 4; ++i) {
		const Cand& src = picked[(std::min)(i, nPick - 1)];
		outRgba[i * 4 + 0] = src.r;
		outRgba[i * 4 + 1] = src.g;
		outRgba[i * 4 + 2] = src.b;
		outRgba[i * 4 + 3] = 1.f;
	}

	Con::Ok("SamplePalette kit=%d fmt=%d %ux%u samples=%d c0=(%.2f,%.2f,%.2f) c1=(%.2f,%.2f,%.2f) c2=(%.2f,%.2f,%.2f) c3=(%.2f,%.2f,%.2f)",
		paintKitId, (int)desc.Format, desc.Width, desc.Height, sampled,
		outRgba[0], outRgba[1], outRgba[2],
		outRgba[4], outRgba[5], outRgba[6],
		outRgba[8], outRgba[9], outRgba[10],
		outRgba[12], outRgba[13], outRgba[14]);

	if (paintKitId > 0) {
		std::array<float, 16> cached{};
		std::memcpy(cached.data(), outRgba, sizeof(float) * 16);
		s_paletteCache[paintKitId] = cached;
	}
	return true;
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
