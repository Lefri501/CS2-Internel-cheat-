#include "knife_skins.h"

#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/vfunc/vfunc.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace KnifeSkins {
namespace {

// nerv: CEconItemSchema::get_paint_kits @ +0x2F0
constexpr std::uintptr_t kOffPaintKits = 0x2F0;
// nerv: sorted item def map @ +0xF8 (24-byte nodes)
constexpr std::uintptr_t kOffSortedItemMap = 0xF8;
// nerv: c_econ_item_definition name fields
constexpr std::uintptr_t kOffSimpleWeaponName = 0x230;
constexpr std::uintptr_t kOffWeaponName = 0x248;
constexpr std::uintptr_t kOffItemName = 0x260; // get_item_name — used for panorama kit path
// nerv: c_paint_kit::uses_old_model @ +0xAE
constexpr std::uintptr_t kOffUsesOldModel = 0xAE;
// def index on CEconItemDefinition (after vtable + kv)
constexpr std::uintptr_t kOffDefIndex = 0x10;

struct PaintKit {
	int id;
	const char* name;
	const char* description_string;
	const char* description_tag;
	std::uint8_t pad[0x24];
	int rarity;
};

// Paint kit map — 32-byte nodes (nerv c_utl_map<int, c_paint_kit*>)
struct PaintKitNode {
	int left;
	int right;
	int pad8;
	int pad12;
	int key;
	int pad20;
	PaintKit* value;
};
static_assert(sizeof(PaintKitNode) == 32, "paint kit utlmap node");

struct PaintKitMap {
	int count;
	int capacity_flags;
	PaintKitNode* elements;
};

// Sorted item def map — 24-byte nodes (nerv c_utl_map generic)
struct ItemDefNode {
	int left;
	int right;
	void* value; // c_econ_item_definition*
	int flag;
	int key;
};
static_assert(sizeof(ItemDefNode) == 24, "item def utlmap node");

struct ItemDefMap {
	int count;
	int capacity_flags;
	ItemDefNode* elements;
};

class IFileSystem {};

// IDA filesystem_stdio ??_7CBaseFileSystem@@6B@ @ 0x1801AE6D0:
// FileExists sub_18005A4A0 @ 0x1801AE778 → slot (0xA8/8)=21
static constexpr int kFsExistsVmt = 21;

static bool FsExists(void* fs, const char* file, const char* pathId) {
	if (!fs || !file) return false;
	using Fn = bool(__fastcall*)(void*, const char*, const char*);
	__try {
		void** vt = *reinterpret_cast<void***>(fs);
		if (!vt || !Mem::IsUserPtr(vt)) return false;
		auto fn = reinterpret_cast<Fn>(vt[kFsExistsVmt]);
		if (!fn || !Mem::IsUserPtr((void*)fn)) return false;
		if (fn(fs, file, pathId))
			return true;
		// pathId null = search all paths (some mounts ignore "GAME")
		if (pathId)
			return fn(fs, file, nullptr);
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

class ILocalize {
public:
	const char* FindSafe(const char* token) {
		if (!token) return "";
		const char* r = M::CallVFunc<const char*, 18U>(this, token);
		return r ? r : token;
	}
};

static IFileSystem* g_fs = nullptr;
static ILocalize* g_loc = nullptr;
static bool g_fsTried = false;
static bool g_locTried = false;
static std::unordered_map<std::uint16_t, std::vector<PaintKitInfo>> g_kits;
static std::unordered_map<std::uint16_t, std::string> g_simpleNames;
// Prevent EnsureKnife FS rebuild every frame when only Vanilla (schema/FS not ready)
static std::unordered_map<std::uint16_t, ULONGLONG> g_ensureCooldownMs;
// VPK FileExists is expensive — memoize path results across builds/tabs
static std::unordered_map<std::string, bool> g_fsExistsCache;
// At most one chunk of FS work per Present tick (menu may call EnsureKnife 4×/frame)
static DWORD g_chunkFrameTick = 0;
static int g_chunksThisFrame = 0;
constexpr int kMaxChunksPerFrame = 1;
static const std::vector<PaintKitInfo> g_emptyDefault{ { 0, "Vanilla", "Vanilla", 1 } };
constexpr ULONGLONG kEnsureRetryMs = 2000;

static void BeginChunkBudget()
{
	const DWORD t = GetTickCount();
	if (t != g_chunkFrameTick) {
		g_chunkFrameTick = t;
		g_chunksThisFrame = 0;
	}
}

static bool TakeChunkBudget()
{
	BeginChunkBudget();
	if (g_chunksThisFrame >= kMaxChunksPerFrame)
		return false;
	++g_chunksThisFrame;
	return true;
}

static void EnsureIfaces() {
	if (!g_fsTried) {
		g_fsTried = true;
		// nerv: filesystem_stdio / VFileSystem017, exists @ vmt 21
		g_fs = I::Get<IFileSystem>("filesystem_stdio.dll", "VFileSystem017");
		if (!g_fs)
			g_fs = I::Get<IFileSystem>("filesystem_stdio", "VFileSystem017");
	}
	if (!g_locTried) {
		g_locTried = true;
		g_loc = I::Get<ILocalize>("localize.dll", "Localize_001");
		if (!g_loc)
			g_loc = I::Get<ILocalize>("localize", "Localize_001");
	}
}

static const char* Loc(const char* token) {
	if (!token || !Mem::IsReadable(token, 1) || !token[0]) return "";
	if (!g_loc) return token;
	__try {
		const char* r = g_loc->FindSafe(token);
		if (r && Mem::IsReadable(r, 1) && r[0]) return r;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return token;
}

static bool FileExists(const char* path) {
	if (!g_fs || !path || !path[0]) return false;
	auto it = g_fsExistsCache.find(path);
	if (it != g_fsExistsCache.end())
		return it->second;
	const bool hit = FsExists(g_fs, path, "GAME");
	g_fsExistsCache.emplace(path, hit);
	return hit;
}

// Prefer fields that look like "weapon_ak47" (panorama path token).
// ItemName @0x260 is often a localize token (#SFUI_…) — useless for images.
static bool LooksLikeWeaponToken(const char* n) {
	if (!n || !Mem::IsReadable(n, 8) || !n[0]) return false;
	if (n[0] == '#') return false; // localize token
	// weapon_* or glove_* / studded_*
	if (std::strncmp(n, "weapon_", 7) == 0) return true;
	if (std::strncmp(n, "glove_", 6) == 0) return true;
	if (std::strncmp(n, "studded_", 8) == 0) return true;
	if (std::strncmp(n, "sporty_", 7) == 0) return true;
	if (std::strncmp(n, "motorcycle_", 11) == 0) return true;
	if (std::strncmp(n, "specialist_", 11) == 0) return true;
	if (std::strncmp(n, "handwrap_", 9) == 0) return true;
	if (std::strncmp(n, "slick_", 6) == 0) return true;
	if (std::strncmp(n, "leather_handwraps", 17) == 0) return true;
	if (std::strncmp(n, "bloodhound_", 11) == 0) return true;
	if (std::strncmp(n, "hydra_", 6) == 0) return true;
	if (std::strncmp(n, "brokenfang_", 11) == 0) return true;
	return false;
}

static const char* ReadItemName(void* def) {
	if (!def || !Mem::IsUserPtr(def)) return nullptr;
	// SimpleWeaponName first — real panorama base name
	static const std::uintptr_t kOffs[] = {
		kOffSimpleWeaponName, kOffWeaponName, kOffItemName
	};
	const char* fallback = nullptr;
	for (std::uintptr_t off : kOffs) {
		__try {
			const char* n = *reinterpret_cast<const char**>((std::uint8_t*)def + off);
			if (!n || !Mem::IsReadable(n, 4) || !n[0]) continue;
			if (LooksLikeWeaponToken(n))
				return n;
			if (!fallback)
				fallback = n;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return fallback;
}

// Static fallback when schema name not cached yet (menu opens before EnsureKnife).
// Keys match panorama/images/econ/weapons/base_weapons/<name>_png.vtex_c
static const char* StaticSimpleName(std::uint16_t defIndex) {
	switch (defIndex) {
	// Guns
	case 1:  return "weapon_deagle";
	case 2:  return "weapon_elite";
	case 3:  return "weapon_fiveseven";
	case 4:  return "weapon_glock";
	case 7:  return "weapon_ak47";
	case 8:  return "weapon_aug";
	case 9:  return "weapon_awp";
	case 10: return "weapon_famas";
	case 11: return "weapon_g3sg1";
	case 13: return "weapon_galilar";
	case 14: return "weapon_m249";
	case 16: return "weapon_m4a1";
	case 17: return "weapon_mac10";
	case 19: return "weapon_p90";
	case 23: return "weapon_mp5sd";
	case 24: return "weapon_ump45";
	case 25: return "weapon_xm1014";
	case 26: return "weapon_bizon";
	case 27: return "weapon_mag7";
	case 28: return "weapon_negev";
	case 29: return "weapon_sawedoff";
	case 30: return "weapon_tec9";
	case 32: return "weapon_hkp2000";
	case 33: return "weapon_mp7";
	case 34: return "weapon_mp9";
	case 35: return "weapon_nova";
	case 36: return "weapon_p250";
	case 38: return "weapon_scar20";
	case 39: return "weapon_sg556";
	case 40: return "weapon_ssg08";
	case 60: return "weapon_m4a1_silencer";
	case 61: return "weapon_usp_silencer";
	case 63: return "weapon_cz75a";
	case 64: return "weapon_revolver";
	// Knives
	case 500: return "weapon_bayonet";
	case 503: return "weapon_knife_css";
	case 505: return "weapon_knife_flip";
	case 506: return "weapon_knife_gut";
	case 507: return "weapon_knife_karambit";
	case 508: return "weapon_knife_m9_bayonet";
	case 509: return "weapon_knife_tactical";
	case 512: return "weapon_knife_falchion";
	case 514: return "weapon_knife_survival_bowie";
	case 515: return "weapon_knife_butterfly";
	case 516: return "weapon_knife_push";
	case 517: return "weapon_knife_cord";
	case 518: return "weapon_knife_canis";
	case 519: return "weapon_knife_ursus";
	case 520: return "weapon_knife_gypsy_jackknife";
	case 521: return "weapon_knife_outdoor";
	case 522: return "weapon_knife_stiletto";
	case 523: return "weapon_knife_widowmaker";
	case 525: return "weapon_knife_skeleton";
	case 526: return "weapon_knife_kukri";
	// Gloves (econ image path uses these tokens under default_generated / weapons)
	case 4725: return "sporty_gloves";          // Broken Fang family often sporty path
	case 5027: return "bloodhound_gloves";
	case 5030: return "sporty_gloves";
	case 5031: return "slick_gloves";
	case 5032: return "leather_handwraps";
	case 5033: return "motorcycle_gloves";
	case 5034: return "specialist_gloves";
	case 5035: return "sporty_gloves";          // Hydra
	default: return nullptr;
	}
}

static PaintKitMap* ReadPaintMap(void* itemSchema) {
	if (!itemSchema || !Mem::IsUserPtr(itemSchema)) return nullptr;
	__try {
		auto* map = reinterpret_cast<PaintKitMap*>((std::uint8_t*)itemSchema + kOffPaintKits);
		if (!map) return nullptr;
		if (map->count <= 0 || map->count > 20000) return nullptr;
		if (!map->elements || !Mem::IsUserPtr(map->elements)) return nullptr;
		if (!Mem::IsReadable(map->elements, sizeof(PaintKitNode))) return nullptr;
		return map;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static ItemDefMap* ReadItemMap(void* itemSchema) {
	if (!itemSchema || !Mem::IsUserPtr(itemSchema)) return nullptr;
	__try {
		auto* map = reinterpret_cast<ItemDefMap*>((std::uint8_t*)itemSchema + kOffSortedItemMap);
		if (!map) return nullptr;
		if (map->count <= 0 || map->count > 20000) return nullptr;
		if (!map->elements || !Mem::IsUserPtr(map->elements)) return nullptr;
		if (!Mem::IsReadable(map->elements, sizeof(ItemDefNode))) return nullptr;
		return map;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Kit belongs to weapon only if econ preview asset exists for this pair.
// NEVER return true without FS — unfiltered = every paint kit in CS2.
static bool KitFitsItem(const char* simpleName, PaintKit* kit) {
	if (!simpleName || !simpleName[0] || !kit) return false;
	if (!kit->name || !Mem::IsReadable(kit->name, 1) || !kit->name[0]) return false;
	if (!g_fs) return false;

	// Primary CS2 path (Andromeda / nerv)
	char path[512];
	if (sprintf_s(path, "panorama/images/econ/default_generated/%s_%s_light_png.vtex_c",
			simpleName, kit->name) > 0 && FileExists(path))
		return true;
	// Rare kits without _light suffix
	if (sprintf_s(path, "panorama/images/econ/default_generated/%s_%s_png.vtex_c",
			simpleName, kit->name) > 0 && FileExists(path))
		return true;
	return false;
}

static PaintKit* ReadNodeValue(PaintKitMap* map, int i) {
	__try {
		if (!map || i < 0 || i >= map->count) return nullptr;
		return map->elements[i].value;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// nerv: walk sorted map by m_definition_index on value, not node key
static void* FindDefByIndex(void* itemSchema, std::uint16_t defIndex,
	void* (*findItemDef)(void*, int, char))
{
	if (findItemDef && itemSchema) {
		__try {
			void* d = findItemDef(itemSchema, (int)defIndex, 1);
			if (d && Mem::IsUserPtr(d)) return d;
			d = findItemDef(itemSchema, (int)defIndex, 0);
			if (d && Mem::IsUserPtr(d)) return d;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	ItemDefMap* map = ReadItemMap(itemSchema);
	if (!map) return nullptr;
	__try {
		for (int i = 0; i < map->count; ++i) {
			void* def = map->elements[i].value;
			if (!def || !Mem::IsUserPtr(def)) continue;
			const std::uint16_t di =
				*reinterpret_cast<std::uint16_t*>((std::uint8_t*)def + kOffDefIndex);
			if (di == defIndex)
				return def;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return nullptr;
}

static PaintKitMap* GetMap(void* itemSchema) {
	return ReadPaintMap(itemSchema);
}

// Knife list size sanity: real M9 ~80–120 kits. Unfiltered dump is 1000+.
constexpr size_t kMaxSaneKits = 400;

// Time-sliced build state: map->count can be 5000+ and each pass calls
// g_fs->FileExists (VPK lookup) → 5000 sync FS calls on the render thread
// froze the UI for ~1s on first knife/weapon tab click. We now process a
// bounded chunk per call and let the caller re-poll each frame.
struct BuildState {
	int cursor = 0;                    // next PaintKitMap index to scan
	std::vector<PaintKitInfo> partial; // accumulating output (index 0 = Vanilla)
	std::string simple;                // resolved panorama simple name
	bool started = false;
};
static std::unordered_map<std::uint16_t, BuildState> g_builds;

// Kits per chunk. FileExists is VPK-sync — large chunks freeze tab open.
// 28 × light-path FS (budget 1 chunk/frame) keeps Present smooth; "Loading..." spans frames.
constexpr int kBuildChunkKits = 28;

// Resolve panorama simple name. Knives/guns: STATIC first (schema often returns
// generic "weapon_knife" for all knife defs → wrong path → default CT knife icon).
static const char* ResolveSimpleName(std::uint16_t defIndex, void* def) {
	if (const char* st = StaticSimpleName(defIndex))
		return st;
	const char* schema = ReadItemName(def);
	if (schema && LooksLikeWeaponToken(schema))
		return schema;
	return nullptr;
}

// Process ONE chunk of the paint-kit scan for `defIndex`. Returns:
//    1  build fully complete this call (final sort + commit done)
//    0  build still in progress (call again next frame)
//   -1  hard error (caller sets cooldown, retries later)
static int BuildForDefChunk(std::uint16_t defIndex, void* def, PaintKitMap* map) {
	if (!map || !g_fs) return -1;

	BuildState& bs = g_builds[defIndex];

	// First-call init: seed Vanilla, resolve simple name, park cursor at 0.
	if (!bs.started) {
		bs.partial.clear();
		bs.partial.push_back({ 0, "Vanilla", "Vanilla", 1 });
		bs.cursor = 0;
		if (const char* simple = ResolveSimpleName(defIndex, def)) {
			bs.simple = simple;
			g_simpleNames[defIndex] = simple;
		}
		bs.started = true;
	}

	// No simple name → can't filter. Ship Vanilla only, done.
	if (bs.simple.empty()) {
		g_kits[defIndex] = std::move(bs.partial);
		g_builds.erase(defIndex);
		return 1;
	}

	const int end = (bs.cursor + kBuildChunkKits < map->count)
		? bs.cursor + kBuildChunkKits : map->count;
	const int rarityBase = 6;

	for (; bs.cursor < end; ++bs.cursor) {
		PaintKit* kit = ReadNodeValue(map, bs.cursor);
		if (!kit || !Mem::IsUserPtr(kit) || !Mem::IsReadable(kit, sizeof(PaintKit)))
			continue;
		if (kit->id <= 0) continue;
		if (!KitFitsItem(bs.simple.c_str(), kit)) continue;

		const char* display = Loc(kit->description_tag);
		if (!display || !display[0]) display = kit->name;
		if (!display || !display[0]) continue;

		int finalRarity = rarityBase + kit->rarity - 1;
		if (finalRarity < 0) finalRarity = 0;
		if (kit->rarity == 7) {
			if (finalRarity > 7) finalRarity = 7;
		} else {
			if (finalRarity > 6) finalRarity = 6;
		}
		const char* tok = (kit->name && Mem::IsReadable(kit->name, 1)) ? kit->name : "";
		bs.partial.push_back({ kit->id, display, tok, finalRarity });

		// Runaway safety: abort if we've already blown past sane cap mid-scan.
		if (bs.partial.size() > kMaxSaneKits + 8) {
			g_kits[defIndex] = g_emptyDefault;
			g_builds.erase(defIndex);
			return -1;
		}
	}

	if (bs.cursor < map->count)
		return 0; // more chunks to go — caller re-polls next frame

	// Finalize: cap + sort + commit atomically.
	auto& out = g_kits[defIndex];
	if (bs.partial.size() > kMaxSaneKits) {
		out = g_emptyDefault; // safety — filter failed
	} else {
		out = std::move(bs.partial);
		if (out.size() > 1) {
			std::sort(out.begin() + 1, out.end(),
				[](const PaintKitInfo& a, const PaintKitInfo& b) {
					if (a.rarity != b.rarity) return a.rarity > b.rarity;
					return a.name < b.name;
				});
		}
	}
	g_builds.erase(defIndex);
	return 1;
}

// SEH shell — schema reads can AV on hot-swap; treat as recoverable retry.
static int BuildForDefChunkSeh(std::uint16_t defIndex, void* def, PaintKitMap* map) {
	__try {
		return BuildForDefChunk(defIndex, def, map);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_builds.erase(defIndex);
		return -1;
	}
}

// Sync bulk build — used by EnsureReady (menu-init warm of ~20 knife defs at
// a moment where a UI freeze is acceptable). Drains chunks until complete.
static int BuildForDefSyncSeh(std::uint16_t defIndex, void* def, PaintKitMap* map) {
	int r = 0;
	while ((r = BuildForDefChunkSeh(defIndex, def, map)) == 0) { /* drain */ }
	return r > 0 ? 1 : 0;
}

} // namespace

bool EnsureReady(void* itemSchema, void* (*findItemDef)(void* schema, int defIdx, char allowDefault)) {
	if (g_kits.size() > 4) return true;
	if (!itemSchema || !Mem::IsUserPtr(itemSchema))
		return false;

	EnsureIfaces();
	PaintKitMap* map = GetMap(itemSchema);
	if (!map) return false;

	static constexpr std::uint16_t kDefs[] = {
		500, 503, 505, 506, 507, 508, 509, 512, 514, 515,
		516, 517, 518, 519, 520, 521, 522, 523, 525, 526
	};

	for (std::uint16_t def : kDefs) {
		void* itemDef = FindDefByIndex(itemSchema, def, findItemDef);
		if (!itemDef) continue;
		// Menu-init warm — drain sync (one-time cost off the tab-click path)
		if (!BuildForDefSyncSeh(def, itemDef, map))
			g_kits[def] = g_emptyDefault;
	}

	return !g_kits.empty();
}

bool EnsureKnife(std::uint16_t defIndex, void* itemSchema,
	void* (*findItemDef)(void* schema, int defIdx, char allowDefault))
{
	if (defIndex == 0) return false;

	// Always pin static simple name for known defs (preview + filter path)
	if (const char* st = StaticSimpleName(defIndex))
		g_simpleNames[defIndex] = st;

	auto it = g_kits.find(defIndex);
	// Polluted cache from old unfiltered dump — force rebuild once
	if (it != g_kits.end() && it->second.size() > kMaxSaneKits) {
		g_kits.erase(it);
		g_ensureCooldownMs.erase(defIndex);
		g_builds.erase(defIndex);
		it = g_kits.end();
	}
	// Cache hit with sane real kits
	if (it != g_kits.end() && it->second.size() > 1 && it->second.size() <= kMaxSaneKits)
		return true;

	// In-progress chunked build: never cooldown-block, always advance a chunk.
	const bool inProgress = g_builds.find(defIndex) != g_builds.end();

	const ULONGLONG now = GetTickCount64();
	if (!inProgress) {
		auto cd = g_ensureCooldownMs.find(defIndex);
		if (cd != g_ensureCooldownMs.end() && now < cd->second)
			return it != g_kits.end();
	}

	if (!itemSchema || !Mem::IsUserPtr(itemSchema))
		return false;

	EnsureIfaces();
	if (!g_fs) {
		// Retry later when FS iface up — never build unfiltered
		g_ensureCooldownMs[defIndex] = now + 500;
		return false;
	}

	PaintKitMap* map = GetMap(itemSchema);
	if (!map)
		return false;

	// Menu calls Ready+PaintKits on left+right cards → 4 EnsureKnife/frame.
	// Cap to one chunk so tab click never runs multi-chunk FS on Present.
	if (!TakeChunkBudget())
		return false;

	void* itemDef = FindDefByIndex(itemSchema, defIndex, findItemDef);
	// itemDef optional — chunked builder uses StaticSimpleName first.
	// Result: 1 done this call, 0 more chunks pending, -1 hard error.
	const int r = BuildForDefChunkSeh(defIndex, itemDef, map);
	if (r < 0) {
		g_ensureCooldownMs[defIndex] = now + kEnsureRetryMs;
		return false;
	}
	if (r == 0) {
		// Progress made; UI keeps showing "Loading paint kits..." for a few
		// frames. No cooldown — we want the next frame to advance immediately.
		return false;
	}

	auto it2 = g_kits.find(defIndex);
	if (it2 == g_kits.end() || it2->second.size() <= 1)
		g_ensureCooldownMs[defIndex] = now + kEnsureRetryMs;
	else if (it2->second.size() > kMaxSaneKits) {
		// Still polluted → wipe and back off
		g_kits[defIndex] = g_emptyDefault;
		g_ensureCooldownMs[defIndex] = now + kEnsureRetryMs;
		return false;
	} else {
		g_ensureCooldownMs.erase(defIndex);
	}
	return it2 != g_kits.end() && it2->second.size() > 1;
}

bool Ready() {
	return !g_kits.empty();
}

const std::vector<PaintKitInfo>& KitsFor(std::uint16_t defIndex) {
	auto it = g_kits.find(defIndex);
	return it != g_kits.end() ? it->second : g_emptyDefault;
}

int KitIdAt(std::uint16_t defIndex, int listIndex) {
	const auto& kits = KitsFor(defIndex);
	if (listIndex < 0 || listIndex >= (int)kits.size()) return 0;
	return kits[listIndex].id;
}

bool UsesOldModel(void* itemSchema, int paintKitId) {
	if (!itemSchema || paintKitId <= 0) return false;
	PaintKitMap* map = GetMap(itemSchema);
	if (!map) return false;
	__try {
		for (int i = 0; i < map->count; ++i) {
			if (map->elements[i].key != paintKitId) continue;
			PaintKit* kit = map->elements[i].value;
			if (!kit || !Mem::IsUserPtr(kit)) return false;
			return *reinterpret_cast<bool*>((std::uint8_t*)kit + kOffUsesOldModel);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return false;
}

const char* SimpleNameFor(std::uint16_t defIndex) {
	// Static first for knives/guns — schema often returns generic "weapon_knife"
	if (const char* s = StaticSimpleName(defIndex))
		return s;
	auto it = g_simpleNames.find(defIndex);
	if (it != g_simpleNames.end() && !it->second.empty()
		&& LooksLikeWeaponToken(it->second.c_str()))
		return it->second.c_str();
	if (it != g_simpleNames.end() && !it->second.empty())
		return it->second.c_str();
	return "";
}

} // namespace KnifeSkins
