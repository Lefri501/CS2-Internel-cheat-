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
// nerv: CEconItemDefinition::get_item_name @ +0x260
constexpr std::uintptr_t kOffItemName = 0x260;
// nerv: c_paint_kit::uses_old_model @ +0xAE
constexpr std::uintptr_t kOffUsesOldModel = 0xAE;

struct PaintKit {
	int id;
	const char* name;
	const char* description_string;
	const char* description_tag;
	std::uint8_t pad[0x24];
	int rarity;
};

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

// Raw game interface — do NOT add data members (object starts with vtable)
class IFileSystem {};

static int g_existsIndex = -1;

static bool FsExists(void* fs, const char* file, const char* pathId) {
	if (!fs || !file || g_existsIndex < 0) return false;
	using Fn = bool(__fastcall*)(void*, const char*, const char*);
	__try {
		void** vt = *reinterpret_cast<void***>(fs);
		if (!vt || !Mem::IsUserPtr(vt)) return false;
		auto fn = reinterpret_cast<Fn>(vt[g_existsIndex]);
		if (!fn || !Mem::IsUserPtr((void*)fn)) return false;
		return fn(fs, file, pathId);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static int ProbeExistsIndex(void* fs) {
	if (!fs || !Mem::IsUserPtr(fs)) return -1;
	static const char* kProbes[] = {
		"cfg/gamemode_competitive.cfg",
		"cfg/gameinfo.gi",
		"scripts/items/items_game.txt",
		"panorama/layout/base.xml",
	};
	using Fn = bool(__fastcall*)(void*, const char*, const char*);
	void** vt = nullptr;
	__try { vt = *reinterpret_cast<void***>(fs); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	if (!vt || !Mem::IsUserPtr(vt)) return -1;

	const int order[] = { 21, 22, 20, 23, 24, 25, 18, 19, 26, 27, 28, 30, 40, 50, 60, 70, 80, 88, 90 };
	for (int idx : order) {
		__try {
			auto fn = reinterpret_cast<Fn>(vt[idx]);
			if (!fn || !Mem::IsUserPtr((void*)fn)) continue;
			for (const char* p : kProbes) {
				if (fn(fs, p, "GAME")) return idx;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
	}
	return -1;
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
static const std::vector<PaintKitInfo> g_emptyDefault{ { 0, "Vanilla", "Vanilla", 1 } };

static void EnsureIfaces() {
	if (!g_fsTried) {
		g_fsTried = true;
		g_fs = I::Get<IFileSystem>("filesystem_stdio.dll", "VFileSystem017");
		if (g_fs) {
			g_existsIndex = ProbeExistsIndex(g_fs);
			if (g_existsIndex < 0) g_fs = nullptr;
		}
	}
	if (!g_locTried) {
		g_locTried = true;
		g_loc = I::Get<ILocalize>("localize.dll", "Localize_001");
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
	if (!g_fs || !path) return false;
	return FsExists(g_fs, path, "GAME");
}

// SEH-only helpers (no C++ objects — required for MSVC C2712)
static const char* ReadItemName(void* def) {
	__try {
		const char* n = *reinterpret_cast<const char**>((std::uint8_t*)def + kOffItemName);
		if (n && Mem::IsReadable(n, 2) && n[0]) return n;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return nullptr;
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

static bool KitFitsItem(const char* simpleName, PaintKit* kit, bool requireFs) {
	if (!simpleName || !simpleName[0] || !kit) return false;
	if (!kit->name || !Mem::IsReadable(kit->name, 1) || !kit->name[0]) return false;
	if (!requireFs || !g_fs) return true; // unfiltered fallback so skins still apply

	char path[512];
	if (sprintf_s(path, "panorama/images/econ/default_generated/%s_%s_light_png.vtex_c",
			simpleName, kit->name) <= 0)
		return false;
	return FileExists(path);
}

static const char* ItemSimpleName(void* def) {
	if (!def || !Mem::IsUserPtr(def)) return nullptr;
	// nerv: +0x260 get_item_name, +0x230 get_simple_weapon_name, +0x248 get_weapon_name
	static const std::uintptr_t kOffs[] = { 0x260, 0x230, 0x248 };
	for (std::uintptr_t off : kOffs) {
		__try {
			const char* n = *reinterpret_cast<const char**>((std::uint8_t*)def + off);
			// Knives/weapons use weapon_*; gloves use leather_/sporty_/studded_/etc.
			if (n && Mem::IsReadable(n, 8) && n[0])
				return n;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return ReadItemName(def);
}

static PaintKit* ReadNodeValue(PaintKitMap* map, int i) {
	__try {
		if (!map || i < 0 || i >= map->count) return nullptr;
		return map->elements[i].value;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static PaintKitMap* GetMap(void* itemSchema) {
	return ReadPaintMap(itemSchema);
}

static void AppendKits(std::vector<PaintKitInfo>& out, const char* simple, PaintKitMap* map, bool requireFs) {
	const int rarityBase = 6;
	for (int i = 0; i < map->count; ++i) {
		PaintKit* kit = ReadNodeValue(map, i);
		if (!kit || !Mem::IsUserPtr(kit) || !Mem::IsReadable(kit, sizeof(PaintKit)))
			continue;
		if (kit->id <= 0) continue;
		if (!KitFitsItem(simple, kit, requireFs)) continue;

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

		out.push_back({ kit->id, display, kit->name ? kit->name : "", finalRarity });
	}
}

static void BuildForDef(std::uint16_t defIndex, void* def, PaintKitMap* map) {
	auto& out = g_kits[defIndex];
	out.clear();
	out.push_back({ 0, "Vanilla", "Vanilla", 1 });

	const char* simple = ItemSimpleName(def);
	if (simple && simple[0])
		g_simpleNames[defIndex] = simple;
	if (!simple || !map) return;

	// Prefer FS-filtered list (nerv). If that yields nothing, include all kits so apply works.
	AppendKits(out, simple, map, true);
	if (out.size() <= 1)
		AppendKits(out, simple, map, false);

	if (out.size() > 1) {
		std::sort(out.begin() + 1, out.end(), [](const PaintKitInfo& a, const PaintKitInfo& b) {
			if (a.rarity != b.rarity) return a.rarity > b.rarity;
			return a.name < b.name;
		});
	}
}

// SEH wrapper — MSVC C2712 prevents __try inside range-for or with C++ locals
static void* FindItemDefSeh(void* schema, int defIdx, char allowDefault,
                            void* (*fn)(void*, int, char)) {
	__try { return fn(schema, defIdx, allowDefault); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static int BuildForDefSeh(std::uint16_t defIndex, void* def, PaintKitMap* map) {
	__try {
		BuildForDef(defIndex, def, map);
		return 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

} // namespace

bool EnsureReady(void* itemSchema, void* (*findItemDef)(void* schema, int defIdx, char allowDefault)) {
	if (!g_kits.empty()) return true;
	if (!itemSchema || !findItemDef || !Mem::IsUserPtr(itemSchema))
		return false;

	EnsureIfaces();
	if (!g_fs) {
		g_kits[0] = g_emptyDefault;
		return false;
	}

	PaintKitMap* map = GetMap(itemSchema);
	if (!map) return false;

	static constexpr std::uint16_t kDefs[] = {
		500, 503, 505, 506, 507, 508, 509, 512, 514, 515,
		516, 517, 518, 519, 520, 521, 522, 523, 525, 526
	};

	for (std::uint16_t def : kDefs) {
		void* itemDef = FindItemDefSeh(itemSchema, (int)def, 1, findItemDef);
		if (!itemDef || !Mem::IsUserPtr(itemDef)) continue;
		if (!BuildForDefSeh(def, itemDef, map))
			g_kits[def] = g_emptyDefault;
	}

	return !g_kits.empty();
}

bool EnsureKnife(std::uint16_t defIndex, void* itemSchema,
	void* (*findItemDef)(void* schema, int defIdx, char allowDefault))
{
	if (defIndex == 0) return false;
	if (g_kits.find(defIndex) != g_kits.end()) return true;
	if (!itemSchema || !findItemDef || !Mem::IsUserPtr(itemSchema)) return false;

	EnsureIfaces();
	PaintKitMap* map = GetMap(itemSchema);
	if (!map) {
		g_kits[defIndex] = g_emptyDefault;
		return false;
	}

	void* itemDef = FindItemDefSeh(itemSchema, (int)defIndex, 1, findItemDef);

	if (!itemDef || !Mem::IsUserPtr(itemDef)) {
		g_kits[defIndex] = g_emptyDefault;
		return false;
	}

	if (!BuildForDefSeh(defIndex, itemDef, map))
		g_kits[defIndex] = g_emptyDefault;
	return true;
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
	auto it = g_simpleNames.find(defIndex);
	return it != g_simpleNames.end() ? it->second.c_str() : "";
}

} // namespace KnifeSkins
