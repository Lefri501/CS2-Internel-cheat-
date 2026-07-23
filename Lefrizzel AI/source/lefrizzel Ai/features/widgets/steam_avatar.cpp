#include "steam_avatar.h"

#include <Windows.h>
#include <unordered_map>
#include <vector>

namespace SteamAvatar {
namespace {

struct CacheEntry {
	ID3D11ShaderResourceView* srv = nullptr;
	DWORD nextTryMs = 0;
};

std::unordered_map<std::uint64_t, CacheEntry> g_cache;
void* g_friends = nullptr;
void* g_utils = nullptr;
bool g_ready = false;
bool g_tried = false;

// Friends017 / Utils010 vtable indices (stable enough for CS2 Steam client)
constexpr int kReqUserInfo = 9;
constexpr int kGetSmallAvatar = 36;
constexpr int kGetImageSize = 5;
constexpr int kGetImageRGBA = 6;

bool InitSteam() {
	if (g_tried)
		return g_ready;
	g_tried = true;

	HMODULE mod = GetModuleHandleA("steam_api64.dll");
	if (!mod)
		mod = GetModuleHandleA("steam_api.dll");
	if (!mod)
		return false;

	auto getUser = reinterpret_cast<int(__cdecl*)()>(
		GetProcAddress(mod, "SteamAPI_GetHSteamUser"));
	auto findIface = reinterpret_cast<void*(__cdecl*)(int, const char*)>(
		GetProcAddress(mod, "SteamInternal_FindOrCreateUserInterface"));
	if (!getUser || !findIface)
		return false;

	const int user = getUser();
	if (!user)
		return false;

	g_friends = findIface(user, "SteamFriends017");
	if (!g_friends)
		g_friends = findIface(user, "SteamFriends015");
	g_utils = findIface(user, "SteamUtils010");
	if (!g_utils)
		g_utils = findIface(user, "SteamUtils009");

	g_ready = (g_friends != nullptr && g_utils != nullptr);
	return g_ready;
}

void** VTable(void* iface) {
	return iface ? *reinterpret_cast<void***>(iface) : nullptr;
}

void RequestPersona(std::uint64_t steamId) {
	void** vt = VTable(g_friends);
	if (!vt || !vt[kReqUserInfo])
		return;
	using Fn = bool(__fastcall*)(void*, std::uint64_t, bool);
	__try { reinterpret_cast<Fn>(vt[kReqUserInfo])(g_friends, steamId, false); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

int GetSmallAvatar(std::uint64_t steamId) {
	void** vt = VTable(g_friends);
	if (!vt || !vt[kGetSmallAvatar])
		return 0;
	using Fn = int(__fastcall*)(void*, std::uint64_t);
	int img = 0;
	__try { img = reinterpret_cast<Fn>(vt[kGetSmallAvatar])(g_friends, steamId); }
	__except (EXCEPTION_EXECUTE_HANDLER) { img = 0; }
	return img;
}

bool GetImageSize(int image, std::uint32_t& w, std::uint32_t& h) {
	void** vt = VTable(g_utils);
	if (!vt || !vt[kGetImageSize] || image <= 0)
		return false;
	using Fn = bool(__fastcall*)(void*, int, std::uint32_t*, std::uint32_t*);
	w = h = 0;
	bool ok = false;
	__try { ok = reinterpret_cast<Fn>(vt[kGetImageSize])(g_utils, image, &w, &h); }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	return ok && w > 0 && h > 0 && w <= 256 && h <= 256;
}

bool GetImageRGBA(int image, std::uint8_t* dst, int bytes) {
	void** vt = VTable(g_utils);
	if (!vt || !vt[kGetImageRGBA] || image <= 0 || !dst || bytes <= 0)
		return false;
	using Fn = bool(__fastcall*)(void*, int, std::uint8_t*, int);
	bool ok = false;
	__try { ok = reinterpret_cast<Fn>(vt[kGetImageRGBA])(g_utils, image, dst, bytes); }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	return ok;
}

ID3D11ShaderResourceView* CreateSrv(ID3D11Device* device, const std::uint8_t* rgba, int w, int h) {
	if (!device || !rgba || w <= 0 || h <= 0)
		return nullptr;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA sub{};
	sub.pSysMem = rgba;
	sub.SysMemPitch = static_cast<UINT>(w * 4);

	ID3D11Texture2D* tex = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, &sub, &tex)) || !tex)
		return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	const HRESULT hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
	tex->Release();
	return SUCCEEDED(hr) ? srv : nullptr;
}

} // namespace

ImTextureID Get(std::uint64_t steamId, ID3D11Device* device) {
	if (!steamId || !device)
		return ImTextureID_Invalid;

	CacheEntry& slot = g_cache[steamId];
	if (slot.srv)
		return reinterpret_cast<ImTextureID>(slot.srv);

	const DWORD now = GetTickCount();
	if (slot.nextTryMs && now < slot.nextTryMs)
		return ImTextureID_Invalid;

	if (!InitSteam()) {
		slot.nextTryMs = now + 5000;
		return ImTextureID_Invalid;
	}

	RequestPersona(steamId);

	const int image = GetSmallAvatar(steamId);
	if (image <= 0) {
		slot.nextTryMs = now + 1500;
		return ImTextureID_Invalid;
	}

	std::uint32_t w = 0, h = 0;
	if (!GetImageSize(image, w, h)) {
		slot.nextTryMs = now + 1500;
		return ImTextureID_Invalid;
	}

	std::vector<std::uint8_t> rgba(static_cast<size_t>(w) * h * 4);
	if (!GetImageRGBA(image, rgba.data(), static_cast<int>(rgba.size()))) {
		slot.nextTryMs = now + 1500;
		return ImTextureID_Invalid;
	}

	slot.srv = CreateSrv(device, rgba.data(), static_cast<int>(w), static_cast<int>(h));
	slot.nextTryMs = 0;
	return slot.srv ? reinterpret_cast<ImTextureID>(slot.srv) : ImTextureID_Invalid;
}

void ClearCache() {
	for (auto& kv : g_cache) {
		if (kv.second.srv) {
			kv.second.srv->Release();
			kv.second.srv = nullptr;
		}
	}
	g_cache.clear();
}

} // namespace SteamAvatar
