// Client-side agent changer — schema list + SetModel on local pawn (no inventory).
// Port notes vs Andromeda:
//   - Same schema offsets / #Type_CustomPlayer filter / team from ctm_/tm_
//   - Icons: prefer full model basename (customplayer_tm_leet_variantg_…); base without
//     _variant is fallback only (Andromeda FileExists order — most icons need the variant)
//   - Apply every frame while enabled: without inventory equip the game restores default
//     loadout model; one-shot SetModel does not stick
#include "agent_changer.h"
#include "skinchanger.h"

#include "../../config/config.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/vfunc/vfunc.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace SkinChanger {
namespace {

constexpr std::uintptr_t kOffSortedItemDefs = 0x128;
constexpr std::uintptr_t kOffDefIndex = 0x10;
constexpr std::uintptr_t kOffItemBaseName = 0x70;
constexpr std::uintptr_t kOffItemTypeName = 0x80;
constexpr std::uintptr_t kOffModelName = 0x148;
constexpr std::uintptr_t kOffIconName = 0x230;

struct ItemDefNode {
	int left;
	int right;
	int parent;
	int tag;
	int key;
	int pad;
	void* value;
};
static_assert(sizeof(ItemDefNode) == 32, "sorted item def node");

struct ItemDefMap {
	int size;
	int unknown;
	ItemDefNode* data;
	int root;
};

class ILocalize {
public:
	const char* FindSafe(const char* token) {
		if (!token) return "";
		const char* r = M::CallVFunc<const char*, 18U>(this, token);
		return r ? r : token;
	}
};

static std::vector<AgentInfo> g_agents;
static bool g_scanned = false;
static std::uint64_t g_lastHash = 0;
static float g_lastSpawn = 0.f;
static int g_lastTeam = 0;

static const char* ReadStr(void* def, std::uintptr_t off) {
	if (!def) return nullptr;
	__try {
		const char* s = *reinterpret_cast<const char**>((std::uint8_t*)def + off);
		if (s && Mem::IsReadable(s, 2) && s[0]) return s;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return nullptr;
}

static std::uint16_t ReadDefIndex(void* def) {
	if (!def) return 0;
	__try { return *reinterpret_cast<std::uint16_t*>((std::uint8_t*)def + kOffDefIndex); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool IsAgentType(void* def) {
	const char* type = ReadStr(def, kOffItemTypeName);
	return type && std::strcmp(type, "#Type_CustomPlayer") == 0;
}

static bool IsModelPath(const char* model) {
	if (!model || !*model) return false;
	return std::strstr(model, ".vmdl") || std::strstr(model, ".mdl");
}

static int ResolveTeam(const char* modelName) {
	if (!modelName || !*modelName) return 0;
	const char* base = strrchr(modelName, '/');
	if (!base) base = strrchr(modelName, '\\');
	base = base ? base + 1 : modelName;
	if (_strnicmp(base, "ctm_", 4) == 0) return 3;
	if (_strnicmp(base, "tm_", 3) == 0) return 2;
	if (std::strstr(modelName, "/ct/")) return 3;
	if (std::strstr(modelName, "/legacy/") || std::strstr(modelName, "/t/")) return 2;
	return 0;
}

static std::string ModelBasename(const char* modelPath) {
	if (!modelPath || !*modelPath) return {};
	const char* base = strrchr(modelPath, '/');
	if (!base) base = strrchr(modelPath, '\\');
	base = base ? base + 1 : modelPath;
	std::string skin = base;
	const auto dot = skin.find_last_of('.');
	if (dot != std::string::npos) skin.resize(dot);
	return skin;
}

static std::string StripVariant(std::string name) {
	const auto pos = name.find("_variant");
	if (pos != std::string::npos) name.resize(pos);
	return name;
}

// Andromeda: try base key, else full basename. Live scan_cache shows most agents
// publish as customplayer_<full_with_variant>_png.vtex_c — prefer full first.
static std::string BuildIconPath(const char* modelPath, const char* schemaIcon) {
	const std::string full = ModelBasename(modelPath);
	if (!full.empty()) {
		return "panorama/images/econ/characters/customplayer_" + full + "_png.vtex_c";
	}
	if (schemaIcon && *schemaIcon) {
		// Schema may give "econ/characters/customplayer_tm_x" or bare name
		std::string icon = schemaIcon;
		if (icon.find("panorama/") == 0) {
			if (icon.size() >= 7 && icon.compare(icon.size() - 7, 7, ".vtex_c") == 0)
				return icon;
			return icon + ".vtex_c";
		}
		if (icon.find("econ/") == 0)
			return "panorama/images/" + icon + "_png.vtex_c";
		return "panorama/images/econ/characters/customplayer_" + icon + "_png.vtex_c";
	}
	return {};
}

static std::string BuildIconPathFallback(const char* modelPath) {
	std::string base = StripVariant(ModelBasename(modelPath));
	if (base.empty()) return {};
	return "panorama/images/econ/characters/customplayer_" + base + "_png.vtex_c";
}

static const char* Loc(ILocalize* loc, const char* token) {
	if (!token || !*token) return "";
	if (!loc) return token;
	__try {
		const char* r = loc->FindSafe(token);
		if (r && Mem::IsReadable(r, 1) && r[0] && r[0] != '#') return r;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return token;
}

static bool TryGetItemDefMap(void* schema, ItemDefMap*& outMap) {
	outMap = nullptr;
	if (!schema) return false;
	__try {
		ItemDefMap* map = reinterpret_cast<ItemDefMap*>((std::uint8_t*)schema + kOffSortedItemDefs);
		if (!map || map->size <= 0 || map->size > 50000) return false;
		if (!map->data || !Mem::IsUserPtr(map->data)) return false;
		outMap = map;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* TryNodeValue(ItemDefMap* map, int i) {
	if (!map || i < 0 || i >= map->size) return nullptr;
	__try {
		void* def = map->data[i].value;
		if (!def || !Mem::IsUserPtr(def)) return nullptr;
		return def;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* FindDefByIndex(std::uint16_t defIdx) {
	auto findFn = Internal::FindItemDefFn();
	void* schema = Internal::ItemSchema();
	if (!schema || !findFn) return nullptr;
	__try {
		void* def = findFn(schema, (int)defIdx, 0);
		if (def && Mem::IsUserPtr(def)) return def;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return nullptr;
}

static const char* LiveModelPath(std::uint16_t defIdx) {
	void* def = FindDefByIndex(defIdx);
	if (!def) return nullptr;
	const char* model = ReadStr(def, kOffModelName);
	return IsModelPath(model) ? model : nullptr;
}

static bool PawnSkeletonReady(C_CSPlayerPawn* local) {
	if (!local) return false;
	__try {
		CGameSceneNode* node = local->m_pGameSceneNode();
		return node && Mem::IsUserPtr(node);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ScanAgents() {
	g_agents.clear();
	g_agents.push_back({ 0, 0, "Default", "", "" });

	void* schema = Internal::ItemSchema();
	if (!schema || !Mem::IsUserPtr(schema)) return;

	ItemDefMap* map = nullptr;
	if (!TryGetItemDefMap(schema, map) || !map) return;

	ILocalize* loc = I::Get<ILocalize>("localize.dll", "Localize_001");

	std::unordered_set<std::uint16_t> seenDefs;

	for (int i = 0; i < map->size; ++i) {
		void* def = TryNodeValue(map, i);
		if (!def) continue;

		if (!IsAgentType(def)) continue;

		const std::uint16_t defIdx = ReadDefIndex(def);
		if (defIdx == 0 || defIdx == 5036 || defIdx == 5037) continue;
		if (!seenDefs.insert(defIdx).second) continue;

		const char* baseToken = ReadStr(def, kOffItemBaseName);
		if (!baseToken) continue;
		if (std::strstr(baseToken, "map_based")) continue;

		const char* model = ReadStr(def, kOffModelName);
		if (!IsModelPath(model)) continue;

		const char* displayRaw = Loc(loc, baseToken);
		std::string display = (displayRaw && *displayRaw) ? displayRaw : baseToken;

		const char* schemaIcon = ReadStr(def, kOffIconName);

		AgentInfo info{};
		info.def_index = defIdx;
		info.team = ResolveTeam(model);
		info.display = std::move(display);
		info.model_path = model;
		info.icon_path = BuildIconPath(model, schemaIcon);
		info.icon_fallback = BuildIconPathFallback(model);
		if (info.icon_path.empty())
			info.icon_path = info.icon_fallback;
		g_agents.push_back(std::move(info));
	}

	std::sort(g_agents.begin() + 1, g_agents.end(),
		[](const AgentInfo& a, const AgentInfo& b) { return a.display < b.display; });

	g_scanned = true;
}

static std::uint64_t HashPath(const char* p) {
	if (!p) return 0;
	std::uint64_t h = 14695981039346656037ull;
	for (; *p; ++p) { h ^= (std::uint8_t)*p; h *= 1099511628211ull; }
	return h;
}

static const AgentInfo* FindByDef(std::uint16_t def) {
	if (!def) return nullptr;
	for (const auto& a : g_agents) {
		if (a.def_index == def) return &a;
	}
	return nullptr;
}

} // namespace

bool AgentsReady() {
	if (!g_scanned) ScanAgents();
	return g_scanned && g_agents.size() > 1;
}

const std::vector<AgentInfo>& Agents() {
	if (!g_scanned) ScanAgents();
	return g_agents;
}

std::vector<AgentInfo> AgentsForTeam(int team) {
	if (!g_scanned) ScanAgents();
	std::vector<AgentInfo> out;
	out.push_back(g_agents.empty() ? AgentInfo{ 0, 0, "Default", "", "" } : g_agents[0]);
	for (size_t i = 1; i < g_agents.size(); ++i) {
		const auto& a = g_agents[i];
		if (a.team == team || a.team == 0)
			out.push_back(a);
	}
	return out;
}

void InvalidateAgents() {
	g_lastHash = 0;
	g_lastSpawn = -1.f;
	g_lastTeam = 0;
}

void RunAgents(C_CSPlayerPawn* local) {
	using namespace Internal;
	if (!local || !SchemaReady()) return;
	if (!Config::agent_changer) {
		g_lastHash = 0;
		return;
	}
	if (local->getHealth() <= 0) return;

	if (!g_scanned) ScanAgents();

	const int team = (int)local->getTeam();
	const std::uint16_t wantDef = (team == 3) ? (std::uint16_t)Config::agent_ct_def
		: (team == 2) ? (std::uint16_t)Config::agent_t_def : 0;
	if (!wantDef) return;

	// Andromeda: if skeleton is gone (spawn / team / model swap), forget hash and wait
	if (!PawnSkeletonReady(local)) {
		g_lastHash = 0;
		return;
	}

	const char* liveModel = LiveModelPath(wantDef);
	const AgentInfo* info = FindByDef(wantDef);
	const char* model = liveModel;
	if (!model || !*model) {
		if (!info || info->model_path.empty()) return;
		model = info->model_path.c_str();
	}

	const float spawn = LastSpawnTime(local);
	const std::uint64_t hash = HashPath(model);
	const bool identityChanged = (hash != g_lastHash) || (spawn != g_lastSpawn) || (team != g_lastTeam);

	// Without inventory, the game reverts to default loadout agent — re-apply every frame.
	// Still track hash/spawn/team so InvalidateAgents forces an immediate path refresh.
	(void)identityChanged;
	SetPawnModel(local, model);

	g_lastHash = hash;
	g_lastSpawn = spawn;
	g_lastTeam = team;
}

} // namespace SkinChanger
