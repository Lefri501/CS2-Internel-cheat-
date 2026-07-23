#include "gamemode.h"

#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/console/console.h"
#include "../../config/config.h"
#include "../engine2/engine2.h"

#include <Windows.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cctype>

// Port of Andromeda CL_GameState:
//   1) mp_teammates_are_enemies convar (authoritative FFA)
//   2) IGameTypes type/mode (DM / training / workshop)
// Map-name + single-team kept as fallbacks only.

namespace GameMode {
namespace {

bool g_ffa = false;
bool g_haveMode = false;
char g_label[40] = "Unknown";
char g_map[128] = {};
std::uint64_t g_nextScanMs = 0;

// --- mp_teammates_are_enemies ------------------------------------------------
uintptr_t g_cvarTeammatesEnemies = 0;
bool g_cvarResolveTried = false;
bool g_cvarCached = false;
std::uint64_t g_cvarLastPollMs = 0;

constexpr uintptr_t kCvarDataOffset = 0x08;
constexpr uintptr_t kCvarValueOffset = 0x58;
constexpr char kConVarName[] = "mp_teammates_are_enemies";

// --- IGameTypes --------------------------------------------------------------
void** g_ppGameTypes = nullptr;
bool g_gameTypesTried = false;

// vtable: +0x98 idx19 GetGameType, +0xA0 idx20 GetGameMode
constexpr unsigned kVfnGetGameType = 19;
constexpr unsigned kVfnGetGameMode = 20;

uintptr_t ResolveRipRel3(uintptr_t insn) {
	const int32_t disp = *reinterpret_cast<const int32_t*>(insn + 3);
	return insn + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp));
}

uintptr_t FindCStringInRange(const char* sz, uintptr_t start, uintptr_t end) {
	const size_t len = std::strlen(sz) + 1;
	if (end <= start || end - start < len)
		return 0;
	for (uintptr_t p = start; p + len <= end; ++p) {
		if (std::memcmp(reinterpret_cast<const void*>(p), sz, len) == 0)
			return p;
	}
	return 0;
}

void ResolveConVar() {
	g_cvarResolveTried = true;
	g_cvarTeammatesEnemies = 0;

	HMODULE hClient = GetModuleHandleA("client.dll");
	if (!hClient)
		hClient = GetModuleHandleA("client");
	if (!hClient)
		return;

	auto* pDos = reinterpret_cast<PIMAGE_DOS_HEADER>(hClient);
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	auto* pNt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
		reinterpret_cast<uintptr_t>(pDos) + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE)
		return;

	const uintptr_t imageBase = reinterpret_cast<uintptr_t>(hClient);
	const uintptr_t imageEnd = imageBase + pNt->OptionalHeader.SizeOfImage;
	const uintptr_t codeStart = imageBase + pNt->OptionalHeader.BaseOfCode;
	const uintptr_t codeEnd = codeStart + pNt->OptionalHeader.SizeOfCode;

	const uintptr_t strAddr = FindCStringInRange(kConVarName, imageBase, imageEnd);
	if (!strAddr)
		return;

	// lea rdx, <name> ; lea rcx, <ConVar>
	for (uintptr_t p = codeStart; p + 14 <= codeEnd; ++p) {
		if (*reinterpret_cast<const uint8_t*>(p) != 0x48) continue;
		if (*reinterpret_cast<const uint8_t*>(p + 1) != 0x8D) continue;
		if (*reinterpret_cast<const uint8_t*>(p + 2) != 0x15) continue;
		if (ResolveRipRel3(p) != strAddr)
			continue;

		const uintptr_t leaRcx = p + 7;
		if (*reinterpret_cast<const uint8_t*>(leaRcx) != 0x48) continue;
		if (*reinterpret_cast<const uint8_t*>(leaRcx + 1) != 0x8D) continue;
		if (*reinterpret_cast<const uint8_t*>(leaRcx + 2) != 0x0D) continue;

		g_cvarTeammatesEnemies = ResolveRipRel3(leaRcx);
		return;
	}
}

bool AreTeammatesEnemies() {
	if (!g_cvarResolveTried)
		ResolveConVar();
	if (!g_cvarTeammatesEnemies)
		return false;

	const std::uint64_t now = GetTickCount64();
	if (g_cvarLastPollMs != 0 && now - g_cvarLastPollMs < 1000ull)
		return g_cvarCached;

	g_cvarLastPollMs = now;
	g_cvarCached = false;

	__try {
		const uintptr_t pData = *reinterpret_cast<uintptr_t*>(
			g_cvarTeammatesEnemies + kCvarDataOffset);
		if (pData < 0x10000)
			return false;
		g_cvarCached = (*reinterpret_cast<uint8_t*>(pData + kCvarValueOffset) != 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_cvarCached = false;
	}
	return g_cvarCached;
}

void ResolveGameTypes() {
	g_gameTypesTried = true;
	g_ppGameTypes = nullptr;

	// GetGameModeName prologue — unique in client.dll (Andromeda IDA-verified)
	auto* p = M::FindPattern("client.dll",
		"48 83 EC 28 48 8B 0D ? ? ? ? 48 8B 01 FF 90 C8 00 00 00");
	if (!p)
		p = M::FindPattern("client",
			"48 83 EC 28 48 8B 0D ? ? ? ? 48 8B 01 FF 90 C8 00 00 00");
	if (!p)
		return;

	// mov rcx, [g_pGameTypes] at p+4 → resolve to &g_pGameTypes
	auto* global = M::GetAbsoluteAddress(p + 4, 3);
	if (!global)
		return;
	g_ppGameTypes = reinterpret_cast<void**>(global);
}

int CallIntVfn(void** ppIface, unsigned index) {
	if (!ppIface)
		return -1;
	int result = -1;
	__try {
		void* pIface = *ppIface;
		if (reinterpret_cast<uintptr_t>(pIface) < 0x10000)
			return -1;
		using Fn = int(__fastcall*)(void*);
		auto fn = M::GetVFunc<Fn>(pIface, index);
		if (!fn)
			return -1;
		result = fn(pIface);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
	return result;
}

int GetGameType() {
	if (!g_gameTypesTried)
		ResolveGameTypes();
	return CallIntVfn(g_ppGameTypes, kVfnGetGameType);
}

int GetGameMode() {
	if (!g_gameTypesTried)
		ResolveGameTypes();
	return CallIntVfn(g_ppGameTypes, kVfnGetGameMode);
}

// True → treat as FFA (team-check OFF when Auto on)
bool ShouldTargetTeammates() {
	if (AreTeammatesEnemies())
		return true;

	const int type = GetGameType();
	const int mode = GetGameMode();
	if (type < 0)
		return false;

	// Deathmatch (gungame / type1 mode2)
	if (type == 1 && mode == 2)
		return true;
	// Training (2) + Custom/Workshop (3) — aim maps / community
	if (type == 2 || type == 3)
		return true;

	return false;
}

// --- fallbacks: map name / single-team ---------------------------------------

void ToLowerInPlace(char* s) {
	if (!s) return;
	for (; *s; ++s)
		*s = static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
}

const char* BaseMapName(const char* map) {
	if (!map || !map[0])
		return "";
	const char* slash = map;
	for (const char* p = map; *p; ++p) {
		if (*p == '/' || *p == '\\')
			slash = p + 1;
	}
	return slash;
}

bool MapLooksFfa(const char* mapIn) {
	if (!mapIn || !mapIn[0])
		return false;

	char buf[128]{};
	std::snprintf(buf, sizeof(buf), "%s", mapIn);
	ToLowerInPlace(buf);

	const char* base = BaseMapName(buf);
	if (!base || !base[0])
		base = buf;

	// workshop path almost always aim/FFA for this cheat's use case
	if (std::strstr(buf, "workshop"))
		return true;

	static const char* kPref[] = {
		"aim_", "awp_", "fy_", "aimbotz", "training_", "prefire_",
		"practice_", "yprac_", "recoil", "spray_", "dm_", "ffa_",
		"crashz", "botz", "peaim", "headshot_", "hs_", "1v1",
		"duel_", "aimmap", "range", "warmup_",
	};
	for (const char* p : kPref) {
		if (std::strstr(base, p))
			return true;
	}
	return false;
}

std::uint8_t ReadTeam(void* ent) {
	if (!ent)
		return 0;
	const std::uint32_t off = SchemaFinder::Get(
		hash_32_fnv1a_const("C_BaseEntity->m_iTeamNum"));
	if (!off)
		return 0;
	std::uint8_t t = 0;
	__try {
		t = *reinterpret_cast<std::uint8_t*>(
			reinterpret_cast<std::uint8_t*>(ent) + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return t;
}

bool SingleTeamLobby() {
	if (!I::GameEntity || !Mem::Valid(I::GameEntity->Instance, 0x2100))
		return false;

	const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0)
		return false;

	int teamsMask = 0;
	int players = 0;
	const int lim = (nMax < 4096) ? nMax : 4096;

	for (int i = 1; i <= lim; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity))
			continue;

		SchemaClassInfoData_t* cls = nullptr;
		__try {
			Entity->dump_class_info(&cls);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}
		if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
			continue;
		if (HASH(cls->szName) != HASH("CCSPlayerController"))
			continue;

		auto* ctrl = reinterpret_cast<CCSPlayerController*>(Entity);
		if (ctrl->IsLocalPlayer())
			continue;

		std::uint8_t team = ReadTeam(Entity);
		if (team != 2 && team != 3) {
			const CBaseHandle hPawn = ctrl->m_hPawn();
			if (hPawn.valid()) {
				auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(hPawn);
				if (Mem::ValidEntity(pawn))
					team = ReadTeam(pawn);
			}
		}
		if (team != 2 && team != 3)
			continue;

		teamsMask |= (1 << team);
		++players;
	}

	return players >= 1 && teamsMask != 0 && (teamsMask & (teamsMask - 1)) == 0;
}

void SetFfa(const char* label) {
	g_ffa = true;
	g_haveMode = true;
	std::snprintf(g_label, sizeof(g_label), "%s", label);
}

void SetTeam(const char* label) {
	g_ffa = false;
	g_haveMode = true;
	std::snprintf(g_label, sizeof(g_label), "%s", label);
}

void Scan() {
	g_ffa = false;
	g_haveMode = false;
	std::snprintf(g_label, sizeof(g_label), "Unknown");

	// 1) Authoritative: convar + IGameTypes (Andromeda path)
	if (AreTeammatesEnemies()) {
		SetFfa("FFA/cvar");
		return;
	}

	const int type = GetGameType();
	const int mode = GetGameMode();
	if (type >= 0) {
		if (type == 1 && mode == 2) {
			SetFfa("Deathmatch");
			return;
		}
		if (type == 2) {
			SetFfa("Training");
			return;
		}
		if (type == 3) {
			SetFfa("Workshop");
			return;
		}
		// Known team modes via IGameTypes
		// type 0 classic: 0 casual, 1 competitive, 2 wingman, 3 wpn-expert
		// type 1 gungame: 0 arms race, 1 demolition (2 = DM handled above)
		// type 4 co-op
		if (type == 0) {
			const char* names[] = { "Casual", "Competitive", "Wingman", "WpnExpert" };
			SetTeam(mode >= 0 && mode < 4 ? names[mode] : "Classic");
			return;
		}
		if (type == 1) {
			const char* names[] = { "ArmsRace", "Demolition", "Deathmatch" };
			SetTeam(mode >= 0 && mode < 3 ? names[mode] : "GunGame");
			return;
		}
		if (type == 4) {
			SetTeam("Coop");
			return;
		}
		// Unknown type but interface works — default team, still try fallbacks
	}

	// 2) Map name fallback (workshop / aim_ / …)
	if (MapLooksFfa(g_map)) {
		const char* base = BaseMapName(g_map);
		char lab[40];
		if (base && base[0])
			std::snprintf(lab, sizeof(lab), "Map:%s", base);
		else
			std::snprintf(lab, sizeof(lab), "Workshop");
		SetFfa(lab);
		return;
	}

	// 3) Single-team lobby
	if (SingleTeamLobby()) {
		SetFfa("Aim/FFA");
		return;
	}

	SetTeam("Team");
}

void SetMapCached(const char* mapName) {
	// Never wipe a good cached map on a bad/garbage pointer.
	// (Mis-hooked "LevelInit" used to pass junk a2 and clear g_map every call.)
	if (!mapName || !Mem::IsReadable(mapName, 1))
		return;
	char tmp[128]{};
	__try {
		if (!mapName[0] || mapName[0] <= 32)
			return;
		std::snprintf(tmp, sizeof(tmp), "%s", mapName);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	for (char* p = tmp; *p; ++p) {
		if (*p == '.') { *p = '\0'; break; }
	}
	const char* base = BaseMapName(tmp);
	if (!base || !base[0])
		base = tmp;
	if (!base[0] || base[0] == '#' || base[0] <= 32)
		return;
	if (!_stricmp(base, "dedicated") || !_stricmp(base, "Dedicated Server"))
		return;
	// Engine placeholder (IDA aEmpty_1) — never cache this as a map
	if (!_stricmp(base, "<empty>") || !_stricmp(base, "empty")
		|| !_stricmp(base, "none") || !_stricmp(base, "null"))
		return;
	std::snprintf(g_map, sizeof(g_map), "%s", base);
	ToLowerInPlace(g_map);
}

} // namespace

void OnLevelInit(const char* mapName) {
	SetMapCached(mapName);
	g_nextScanMs = 0;
	g_cvarLastPollMs = 0;
	Scan();
}

// --- map name resolution (IDA-verified) --------------------------------------
// engine2 GetLevelName @ 0x180075B50 — free fn, NetworkGameClient+0x210
// engine2 GetLevelNameShort @ 0x180075BB0 — free fn, NetworkGameClient+0x218
// client  GetMapBspName @ 0x180F11F10 — thunk: iface vt+0x208
// client  GetMapName @ 0x180F146C0 — same + #SFUI_Map_%s localize

static bool LooksLikeMapName(const char* s) {
	if (!s || !Mem::IsReadable(s, 2) || !s[0] || s[0] <= 32)
		return false;
	if (s[0] == '#') // localized token without resolve
		return false;
	// Engine placeholder before level name is ready (IDA: aEmpty_1 = "<empty>")
	if (!_stricmp(s, "<empty>") || !_stricmp(s, "empty") || !_stricmp(s, "none")
		|| !_stricmp(s, "null") || !_stricmp(s, "unknown"))
		return false;
	if (!_stricmp(s, "dedicated") || !_stricmp(s, "Dedicated Server"))
		return false;

	// Accept printable ASCII only
	int len = 0;
	for (const char* p = s; *p; ++p, ++len) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c < 32 || c > 126)
			return false;
		if (len > 120)
			return false;
	}
	if (len < 3)
		return false;

	// Prefer real map tokens (de_mirage, cs_office, maps/de_dust2, workshop/.../de_nuke)
	char lower[128]{};
	const int ncopy = (len < 127) ? len : 127;
	for (int i = 0; i < ncopy; ++i)
		lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
	lower[ncopy] = '\0';

	const char* base = lower;
	for (char* p = lower; *p; ++p) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}
	// strip extension already handled in SetMapCached; still tolerate
	for (char* p = lower; *p; ++p) {
		if (*p == '.') { *p = '\0'; break; }
	}
	// recompute base after strip
	base = lower;
	for (char* p = lower; *p; ++p) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}

	static const char* kPref[] = {
		"de_", "cs_", "ar_", "gd_", "aim_", "awp_", "fy_", "dz_", "graphics_",
	};
	for (const char* pref : kPref) {
		if (std::strncmp(base, pref, std::strlen(pref)) == 0)
			return true;
	}
	// workshop path containing a known prefix later in the string
	if (std::strstr(lower, "workshop") && std::strstr(lower, "de_"))
		return true;
	return false;
}

const char* CallNoArgCString(uintptr_t fn) {
	if (!fn)
		return nullptr;
	const char* raw = nullptr;
	__try {
		using Fn = const char*(__fastcall*)();
		raw = reinterpret_cast<Fn>(fn)();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return LooksLikeMapName(raw) ? raw : nullptr;
}

uintptr_t ResolveFnOnce(const char* mod, const char* pat, const char* tag) {
	uintptr_t addr = M::patternScan(mod, pat);
	if (addr)
		Con::Ok("GameMode %s @ 0x%p", tag, (void*)addr);
	else
		Con::OffsetMiss(tag);
	return addr;
}

const char* TryEngineLevelNameShort() {
	const char* s = Engine2::LevelNameShort();
	return LooksLikeMapName(s) ? s : nullptr;
}

const char* TryEngineLevelNameFull() {
	const char* s = Engine2::LevelName();
	return LooksLikeMapName(s) ? s : nullptr;
}

const char* TryClientMapBspName() {
	// IDA GetMapBSPName @ 0x180F11F10 — jmp [vt+0x208] (Source2EngineToClient)
	static uintptr_t s_fn = 0;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		// IDA: jmp qword ptr [rax+0x208] — unique vs generic vt thunks
		s_fn = ResolveFnOnce("client",
			"48 8B 0D ? ? ? ? 48 8B 01 48 FF A0 08 02 00 00",
			"GetMapBspName");
	}
	return CallNoArgCString(s_fn);
}

const char* TryClientMapName() {
	// IDA GetMapName @ 0x180F146C0 — call [vt+0x208] then #SFUI_Map_%s localize
	static uintptr_t s_fn = 0;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		s_fn = ResolveFnOnce("client",
			"48 83 EC 28 48 8B 0D ? ? ? ? 48 8B 01 FF 90 ? ? ? ? 48 8B C8 48 83 C4 28",
			"GetMapName");
	}
	return CallNoArgCString(s_fn);
}

const char* TryEngineClientVfunc() {
	if (!I::EngineClient)
		return nullptr;
	const char* s = nullptr;
	__try {
		s = I::EngineClient->get_level_name_short();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		s = nullptr;
	}
	if (LooksLikeMapName(s))
		return s;
	__try {
		s = I::EngineClient->get_level_name();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		s = nullptr;
	}
	return LooksLikeMapName(s) ? s : nullptr;
}

bool EnsureMap() {
	// Drop stale engine placeholder if we cached it earlier
	if (g_map[0] && !LooksLikeMapName(g_map))
		g_map[0] = '\0';
	if (g_map[0] && LooksLikeMapName(g_map))
		return true;

	Engine2::Init();

	// Need NetworkGameClient + signon>=2 before NGC+0x218 holds a real map
	const int signon = Engine2::SignonState();
	if (signon > 0 && signon < 2) {
		static std::uint64_t s_lastEarlyLog = 0;
		const std::uint64_t now = GetTickCount64();
		if (now - s_lastEarlyLog > 3000ull) {
			s_lastEarlyLog = now;
			Con::Info("GameMode map wait — signon=%d (need >= 2)", signon);
		}
	}

	const char* raw = TryEngineLevelNameShort();
	if (!raw)
		raw = TryEngineLevelNameFull();
	if (!raw)
		raw = TryClientMapBspName();
	if (!raw)
		raw = TryEngineClientVfunc();
	if (!raw)
		raw = TryClientMapName(); // localized last — may be display name
	if (!raw)
		return g_map[0] != '\0'; // keep prior good cache if any

	SetMapCached(raw);
	if (g_map[0] && LooksLikeMapName(g_map)) {
		Con::Ok("GameMode map resolved: %s (raw=%s signon=%d)", g_map, raw, Engine2::SignonState());
		return true;
	}
	// Rejected placeholder — clear so Tick keeps retrying
	g_map[0] = '\0';
	return false;
}

void Tick() {
	const std::uint64_t now = GetTickCount64();
	if (now < g_nextScanMs)
		return;
	g_nextScanMs = now + 500ull;
	// Keep retrying until we have a real map (not "<empty>")
	if (!g_map[0] || !LooksLikeMapName(g_map))
		EnsureMap();
	Scan();
}

bool IsFfa() {
	Tick();
	return g_ffa;
}

bool WantTeamCheck(bool userPref) {
	Tick();
	if (!Config::team_check_auto)
		return userPref;
	return !g_ffa;
}

const char* ModeLabel() {
	Tick();
	return g_label;
}

const char* MapName() {
	if (!g_map[0])
		EnsureMap();
	return g_map;
}

const char* BaseMap() {
	if (!g_map[0])
		EnsureMap();
	return g_map[0] ? g_map : "";
}

} // namespace GameMode
