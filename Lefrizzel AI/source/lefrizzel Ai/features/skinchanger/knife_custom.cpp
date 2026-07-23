// Custom knife mesh packs — same deploy/precache pattern as agent custom models.
// Drop: csgo/lefrizzel_models/knives/<pack>/*.vmdl_c
// Apply: SetModel on world knife + HUD viewmodel after stock knife identity.
#include "knife_custom.h"
#include "custom_assets.h"
#include "skinchanger.h"
#include "../../config/config.h"
#include "../../offsets/offsets.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace SkinChanger {
namespace {

std::vector<CustomKnifeInfo> g_knives;
bool g_scanned = false;
std::uint64_t g_precachedHash = 0;
int g_reapply = 0;
constexpr int kReapplyFrames = 48;
constexpr int kDeployRetryCd = 90;

// ---- Precache (same UC Insert path as agent_changer) ----
using FnCBufferPurge = void(__fastcall*)(void* buf, int);
using FnCBufferInsert = const char*(__fastcall*)(void* buf, int index, const char* str, int len, bool b);
using FnBlockingLoad = void*(__fastcall*)(void* rs, void* bufferString, const char* ext);

struct CBufferStringLite {
	int m_nLength;
	int m_nAllocatedSize;
	union {
		char* m_pString;
		char m_szString[8];
	};
};

static void* g_resourceSystem = nullptr;
static FnCBufferPurge g_bufPurge = nullptr;
static FnCBufferInsert g_bufInsert = nullptr;
static FnBlockingLoad g_blockingLoad = nullptr;
static bool g_precacheTried = false;

static void EnsurePrecacheFns() {
	if (g_precacheTried && g_resourceSystem && g_blockingLoad && g_bufInsert)
		return;
	g_precacheTried = true;
	g_resourceSystem = I::Get<void>("resourcesystem.dll", "ResourceSystem013");
	uintptr_t loadAddr = M::patternScan("resourcesystem",
		"40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA");
	if (!loadAddr)
		loadAddr = M::patternScan("resourcesystem",
			"40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA 48 8B D9 FF 90");
	if (loadAddr)
		g_blockingLoad = reinterpret_cast<FnBlockingLoad>(loadAddr);
	if (const HMODULE tier0 = GetModuleHandleA("tier0.dll")) {
		g_bufPurge = reinterpret_cast<FnCBufferPurge>(
			GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z"));
		g_bufInsert = reinterpret_cast<FnCBufferInsert>(
			GetProcAddress(tier0, "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z"));
	}
}

__declspec(noinline) static bool SehPrecacheInsert(const char* path) {
	if (!g_bufInsert || !g_resourceSystem || !path || !*path)
		return false;
	bool ok = false;
	__try {
		CBufferStringLite names{};
		names.m_nLength = 0;
		names.m_nAllocatedSize = static_cast<int>(0x80000000u | 0x40000000u | 8u);
		names.m_pString = nullptr;
		g_bufInsert(&names, 0, path, -1, false);
		if (g_blockingLoad)
			(void)g_blockingLoad(g_resourceSystem, &names, "");
		else if (g_resourceSystem) {
			void** vt = *reinterpret_cast<void***>(g_resourceSystem);
			if (vt && vt[41])
				(void)reinterpret_cast<FnBlockingLoad>(vt[41])(g_resourceSystem, &names, "");
		}
		ok = true;
		if (g_bufPurge)
			g_bufPurge(&names, 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	return ok;
}

static bool PrecacheModelPath(const char* path) {
	if (!path || !*path)
		return false;
	EnsurePrecacheFns();
	if (!g_resourceSystem)
		return false;
	return g_bufInsert && SehPrecacheInsert(path);
}

static std::uint64_t HashPath(const char* p) {
	if (!p) return 0;
	std::uint64_t h = 14695981039346656037ull;
	for (; *p; ++p) { h ^= (std::uint8_t)*p; h *= 1099511628211ull; }
	return h;
}

static bool PathEndsWithI(const std::string& s, const char* suffix) {
	const size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}

static bool IsCompiledVmdl(const fs::path& p) {
	return PathEndsWithI(p.filename().string(), ".vmdl_c");
}

static std::string CompiledAssetStem(const fs::path& p) {
	std::string name = p.filename().string();
	if (PathEndsWithI(name, ".vmdl_c"))
		name.resize(name.size() - 7);
	else
		name = p.stem().string();
	return name;
}

static bool IsFragmentKnifeStem(std::string stemLower) {
	for (char& ch : stemLower)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	static constexpr const char* kFrag[] = {
		"nohitbox", "no_hitbox", "gib", "phys", "ragdoll",
		"attach", "_part", "prefab", "_lod", "cloth",
		"player_model", "viewmodel_arms", "sleeve",
	};
	for (const char* m : kFrag) {
		if (stemLower.find(m) != std::string::npos)
			return true;
	}
	if (stemLower.empty())
		return true;
	return false;
}

static bool IsStockKnifePath(const std::string& relLower) {
	// Stock: weapons/models/knife/knife_xxx/weapon_knife_xxx.vmdl (one folder, weapon_ prefix)
	if (relLower.rfind("weapons/models/knife/", 0) != 0)
		return false;
	const std::string rest = relLower.substr(std::strlen("weapons/models/knife/"));
	const auto slash = rest.find('/');
	if (slash == std::string::npos)
		return false;
	const std::string file = rest.substr(slash + 1);
	if (file.find('/') != std::string::npos)
		return false; // nested author pack
	// Valve knives: weapon_knife_*.vmdl
	return file.rfind("weapon_knife", 0) == 0 || file.rfind("weapon_bayonet", 0) == 0;
}

// Map path/display/binary blob → Knives() index.
// LONGEST match wins — short tokens like "skeleton"/"m9" appear in many packs
// as shared junk; prefer weapon_knife_* / knife_* full ids.
//
// Binary .vmdl_c: NEVER use strlen — file is full of NULs; use size-aware scan.
static int InferStockFromBlob(const char* data, size_t len) {
	if (!data || !len)
		return 0;
	struct Tok { const char* needle; int idx; };
	// Indices match kKnives in skinchanger.cpp. Prefer long specific ids.
	static constexpr Tok kTok[] = {
		{ "weapon_knife_m9_bayonet", 6 },
		{ "knife_m9_bayonet", 6 },
		{ "m9_bayonet", 6 },
		{ "m9bayonet", 6 },
		{ "weapon_knife_survival_bowie", 9 },
		{ "knife_survival_bowie", 9 },
		{ "survival_bowie", 9 },
		{ "weapon_knife_gypsy_jackknife", 15 },
		{ "knife_gypsy_jackknife", 15 },
		{ "gypsy_jackknife", 15 },
		{ "weapon_knife_falchion_advanced", 8 },
		{ "knife_falchion_advanced", 8 },
		{ "falchion_advanced", 8 },
		{ "weapon_knife_butterfly", 10 },
		{ "knife_butterfly", 10 },
		{ "butterfly", 10 },
		{ "weapon_knife_karambit", 5 },
		{ "knife_karambit", 5 },
		{ "karambit", 5 },
		{ "weapon_knife_widowmaker", 18 },
		{ "knife_widowmaker", 18 },
		{ "widowmaker", 18 },
		{ "weapon_knife_stiletto", 17 },
		{ "knife_stiletto", 17 },
		{ "stiletto", 17 },
		{ "weapon_knife_skeleton", 19 },
		{ "knife_skeleton", 19 },
		// bare "skeleton" dropped — false positive in almost every custom pack
		{ "weapon_knife_outdoor", 16 },
		{ "knife_outdoor", 16 },
		{ "nomad", 16 },
		{ "weapon_knife_ursus", 14 },
		{ "knife_ursus", 14 },
		{ "ursus", 14 },
		{ "weapon_knife_canis", 13 },
		{ "knife_canis", 13 },
		{ "canis", 13 },
		{ "weapon_knife_cord", 12 },
		{ "knife_cord", 12 },
		{ "paracord", 12 },
		{ "weapon_knife_push", 11 },
		{ "knife_push", 11 },
		{ "shadow_daggers", 11 },
		{ "shadowdaggers", 11 },
		{ "weapon_knife_tactical", 7 },
		{ "knife_tactical", 7 },
		{ "huntsman", 7 },
		// ag2 bayonet packs embed weapon_knife_bayonet_ag2 — catch before bayonet
		{ "weapon_knife_bayonet_ag2", 1 },
		{ "knife_bayonet_ag2", 1 },
		{ "weapon_knife_bayonet", 1 },
		{ "knife_bayonet", 1 },
		{ "weapon_bayonet", 1 },
		{ "bayonet", 1 },
		{ "weapon_knife_css", 2 },
		{ "knife_css", 2 },
		{ "weapon_knife_flip", 3 },
		{ "knife_flip", 3 },
		{ "weapon_knife_gut", 4 },
		{ "knife_gut", 4 },
		{ "weapon_knife_kukri", 20 },
		{ "knife_kukri", 20 },
		{ "kukri", 20 },
		{ "weapon_knife_falchion", 8 },
		{ "falchion", 8 },
		{ "navaja", 15 },
		{ "jackknife", 15 },
		{ "talon", 18 },
		{ "bowie", 9 },
		// short tokens last — easy false positives
		{ "flip", 3 },
		{ "gut", 4 },
		{ "classic", 2 },
	};

	auto containsI = [](const char* hay, size_t hlen, const char* needle) -> bool {
		if (!hay || !hlen || !needle || !*needle) return false;
		const size_t n = std::strlen(needle);
		if (n > hlen) return false;
		for (size_t i = 0; i + n <= hlen; ++i) {
			bool ok = true;
			for (size_t j = 0; j < n; ++j) {
				char ca = hay[i + j];
				char cb = needle[j];
				if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
				if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
				if (ca != cb) { ok = false; break; }
			}
			if (ok) return true;
		}
		return false;
	};

	int bestIdx = 0;
	size_t bestLen = 0;
	for (const Tok& t : kTok) {
		const size_t n = std::strlen(t.needle);
		if (n <= bestLen)
			continue;
		if (containsI(data, len, t.needle)) {
			bestLen = n;
			bestIdx = t.idx;
		}
	}
	return bestIdx;
}

static int InferStockFromText(const char* a, const char* b) {
	int best = 0;
	if (a && *a) {
		const int s = InferStockFromBlob(a, std::strlen(a));
		if (s > 0) best = s;
	}
	if (b && *b) {
		const int s = InferStockFromBlob(b, std::strlen(b));
		// Longer needle already preferred inside; if both hit take non-zero max-specific via re-run
		if (s > 0) {
			// Prefer whichever came from longer match — re-scan combined for simplicity
			std::string both;
			if (a && *a) { both += a; both.push_back('\0'); }
			both += b;
			const int c = InferStockFromBlob(both.data(), both.size());
			if (c > 0) return c;
			return s;
		}
	}
	return best;
}

// Read .vmdl_c embeds for stock knife path tokens (best signal).
// Checks: csgo/<path>, then lefrizzel_models/knives/<path>, then recursive name hunt in knives drop.
static int InferStockFromCompiled(const std::string& gameVmdlPath) {
	if (gameVmdlPath.empty())
		return 0;
	std::string rel = gameVmdlPath;
	std::replace(rel.begin(), rel.end(), '\\', '/');
	if (PathEndsWithI(rel, ".vmdl")) {
		rel.resize(rel.size() - 5);
		rel += ".vmdl_c";
	} else if (!PathEndsWithI(rel, ".vmdl_c")) {
		return 0;
	}

	auto tryFile = [&](const std::string& fullIn) -> int {
		std::string full = fullIn;
		std::replace(full.begin(), full.end(), '/', '\\');
		std::error_code ec;
		if (!fs::exists(full, ec))
			return 0;
		std::ifstream in(full, std::ios::binary);
		if (!in)
			return 0;
		std::string data((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		if (data.empty())
			return 0;
		// Binary-safe: .vmdl_c is full of NULs — strlen would stop at header.
		return InferStockFromBlob(data.data(), data.size());
	};

	// 1) Deployed under csgo content root
	const std::string root = CsgoGameDir();
	if (!root.empty()) {
		if (const int s = tryFile(root + "\\" + rel))
			return s;
	}
	// 2) Still only in drop folder: knives/weapons/... mirror
	const std::string drop = LefrizzelKnivesDir();
	if (!drop.empty()) {
		if (const int s = tryFile(drop + "\\" + rel))
			return s;
		// filename-only hunt (loose packs)
		const auto slash = rel.find_last_of('/');
		const std::string fname = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
		std::error_code ec;
		if (fs::exists(drop, ec)) {
			const auto opts = fs::directory_options::skip_permission_denied;
			for (auto it = fs::recursive_directory_iterator(drop, opts, ec);
				it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) { ec.clear(); continue; }
				if (!it->is_regular_file(ec)) continue;
				if (_stricmp(it->path().filename().string().c_str(), fname.c_str()) != 0)
					continue;
				if (const int s = tryFile(it->path().string()))
					return s;
			}
		}
	}
	return 0;
}

static void AddKnifeEntry(
	std::unordered_set<std::string>& seen,
	std::string rel,
	const std::string& csgoRoot)
{
	std::replace(rel.begin(), rel.end(), '\\', '/');
	if (rel.find("/materials/") != std::string::npos
		|| rel.find("/mat/") != std::string::npos)
		return;
	if (!PathEndsWithI(rel, ".vmdl"))
		return;

	std::string relLower = rel;
	for (char& ch : relLower)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	if (IsStockKnifePath(relLower))
		return;
	if (!seen.insert(relLower).second)
		return;

	CustomKnifeInfo info{};
	std::string disp = rel;
	for (const char* pfx : { "weapons/models/knife/", "weapons/models/", "weapons/", "models/" }) {
		if (disp.rfind(pfx, 0) == 0) {
			disp.erase(0, std::strlen(pfx));
			break;
		}
	}
	if (disp.size() > 5 && disp.compare(disp.size() - 5, 5, ".vmdl") == 0)
		disp.resize(disp.size() - 5);
	if (!csgoRoot.empty() && !CustomModelDeployed(rel))
		disp = "[not deployed] " + disp;

	// Stock base: compiled embeds first (longest match), then path/display
	int stock = InferStockFromCompiled(rel);
	if (stock <= 0)
		stock = InferStockFromText(rel.c_str(), disp.c_str());
	// Never leave 0 — ProcessKnife skips Default; unknown packs → karambit
	if (stock <= 0)
		stock = 5; // Karambit
	info.stock_index = stock;

	// Show base knife in list so user sees auto-pick
	if (stock > 0 && stock < KnifeCount()) {
		const char* name = Knives()[stock].display;
		if (name && *name)
			disp = disp + "  [" + name + "]";
	}

	info.display = std::move(disp);
	info.model_path = std::move(rel);
	g_knives.push_back(std::move(info));
}

static std::string CsgoContentRoot() {
	return CsgoGameDir();
}

static void ScanCustomKnives() {
	g_knives.clear();
	g_scanned = true;
	EnsureLefrizzelModelsDir();
	SyncDroppedKnifePacks();

	const std::string dropStr = LefrizzelKnivesDir();
	const std::string csgoRoot = CsgoContentRoot();
	std::error_code ec;
	std::unordered_set<std::string> seen;
	const auto opts = fs::directory_options::skip_permission_denied;

	// 1) Already deployed under csgo/weapons (and models for odd packs)
	auto scanDeployed = [&](const fs::path& root, const char* pathMarker) {
		if (root.empty() || !fs::exists(root, ec))
			return;
		for (auto it = fs::recursive_directory_iterator(root, opts, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file(ec)) continue;
			const fs::path& path = it->path();
			if (!IsCompiledVmdl(path)) continue;
			{
				std::string check = path.generic_string();
				for (char& ch : check)
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				if (check.find("/materials/") != std::string::npos
					|| check.find("/mat/") != std::string::npos)
					continue;
			}
			if (IsFragmentKnifeStem(CompiledAssetStem(path)))
				continue;
			std::string full = path.string();
			std::replace(full.begin(), full.end(), '\\', '/');
			const auto hit = full.find(pathMarker);
			if (hit == std::string::npos) continue;
			std::string rel = full.substr(hit);
			if (PathEndsWithI(rel, ".vmdl_c")) {
				rel.resize(rel.size() - 7);
				rel += ".vmdl";
			}
			// Only custom: skip pure stock weapon_knife_* unless under custom author nest
			AddKnifeEntry(seen, std::move(rel), csgoRoot);
		}
	};

	if (!csgoRoot.empty()) {
		// Prefer weapons tree; also scan models for packs that bake there
		scanDeployed(fs::path(csgoRoot) / "weapons", "weapons/");
		// Custom knife packs sometimes land under models/weapons/...
		scanDeployed(fs::path(csgoRoot) / "models" / "weapons", "models/");
	}

	// 2) Drop folder (pre-deploy list)
	if (!dropStr.empty() && fs::exists(dropStr, ec)) {
		const fs::path drop(dropStr);
		for (auto it = fs::recursive_directory_iterator(drop, opts, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file(ec)) continue;
			const fs::path& path = it->path();
			if (!IsCompiledVmdl(path)) continue;
			{
				std::string check = path.generic_string();
				for (char& ch : check)
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				if (check.find("/materials/") != std::string::npos
					|| check.find("/mat/") != std::string::npos)
					continue;
			}
			if (IsFragmentKnifeStem(CompiledAssetStem(path)))
				continue;
			std::string rel = InferCustomModelGamePath(path, true);
			if (rel.empty()) continue;
			AddKnifeEntry(seen, std::move(rel), csgoRoot);
		}
	}

	std::sort(g_knives.begin(), g_knives.end(),
		[](const CustomKnifeInfo& a, const CustomKnifeInfo& b) {
			const bool aw = a.display.rfind("[not deployed]", 0) == 0;
			const bool bw = b.display.rfind("[not deployed]", 0) == 0;
			if (aw != bw) return !aw;
			return a.display < b.display;
		});
}

static bool IsKnifeWeapon(C_CSWeaponBase* w) {
	if (!w) return false;
	const std::uint16_t d = Internal::DefIndexOf(w);
	// CS2 knives 500-526 range + bayonet 500
	return (d >= 500 && d <= 526) || d == 42 || d == 59;
}

} // namespace

bool CustomKnivesReady() {
	if (!g_scanned)
		ScanCustomKnives();
	return !g_knives.empty();
}

const std::vector<CustomKnifeInfo>& CustomKnives() {
	if (!g_scanned)
		ScanCustomKnives();
	return g_knives;
}

void RefreshCustomKnives() {
	SyncDroppedKnifePacks();
	g_scanned = false;
	g_precachedHash = 0;
	ScanCustomKnives();
	if (Config::custom_knife_path[0]
		&& CustomKnifeIndexOf(Config::custom_knife_path) < 0) {
		Config::custom_knife_path[0] = '\0';
	}
}

int CustomKnifeIndexOf(const char* path) {
	if (!path || !*path)
		return -1;
	if (!g_scanned)
		ScanCustomKnives();
	for (int i = 0; i < (int)g_knives.size(); ++i) {
		if (_stricmp(g_knives[i].model_path.c_str(), path) == 0)
			return i;
	}
	return -1;
}

void InvalidateCustomKnife() {
	g_reapply = kReapplyFrames;
	g_precachedHash = 0;
}

// Default stock base when pack embeds nothing useful — ProcessKnife needs idx>0.
constexpr int kDefaultCustomStock = 5; // Karambit

int InferStockKnifeIndex(const char* modelPath, const char* display) {
	if ((!modelPath || !*modelPath) && (!display || !*display))
		return 0;
	// 1) Compiled .vmdl_c embeds first (weapon_knife_* paths — most reliable)
	if (modelPath && *modelPath) {
		const int fromBin = InferStockFromCompiled(modelPath);
		if (fromBin > 0)
			return fromBin;
		// 2) Cached scan entry
		const int idx = CustomKnifeIndexOf(modelPath);
		if (idx >= 0 && idx < (int)g_knives.size() && g_knives[idx].stock_index > 0)
			return g_knives[idx].stock_index;
	}
	// 3) Path / display tokens (longest match)
	return InferStockFromText(modelPath, display);
}

int AutoSelectStockKnifeForCustom(const char* modelPath, const char* display) {
	int stock = InferStockKnifeIndex(modelPath, display);
	// Always pick a real stock knife so ProcessKnife runs (idx 0 = Default = no-op).
	if (stock <= 0 || stock >= KnifeCount())
		stock = kDefaultCustomStock;

	const bool changed =
		(Config::knife_index != stock) || !Config::knife_changer;
	Config::knife_changer = true;
	Config::knife_index = stock;
	if (changed)
		Invalidate(); // open ProcessKnife apply window
	return stock;
}

const char* ActiveCustomKnifeModel() {
	if (!Config::custom_knife || !Config::custom_knife_path[0])
		return nullptr;
	// Deploy may lag — try one sync so first equip works
	if (!CustomModelDeployed(Config::custom_knife_path)) {
		SyncDroppedKnifePacks();
		if (!CustomModelDeployed(Config::custom_knife_path))
			return nullptr;
	}
	// Keep stock knife locked to pack base while custom mesh active
	(void)AutoSelectStockKnifeForCustom(Config::custom_knife_path, nullptr);
	return Config::custom_knife_path;
}

void RunCustomKnife(C_CSPlayerPawn* local) {
	if (!local || !Config::custom_knife || !Config::custom_knife_path[0])
		return;
	if (local->getHealth() <= 0)
		return;

	static char s_deployTried[260]{};
	static int s_deployCd = 0;

	const bool pathChanged =
		!s_deployTried[0]
		|| _stricmp(s_deployTried, Config::custom_knife_path) != 0;

	if (!CustomModelDeployed(Config::custom_knife_path)) {
		if (pathChanged || s_deployCd <= 0) {
			SyncDroppedKnifePacks();
			std::snprintf(s_deployTried, sizeof(s_deployTried),
				"%s", Config::custom_knife_path);
			s_deployCd = kDeployRetryCd;
			g_precachedHash = 0;
			g_reapply = kReapplyFrames;
			if (!CustomModelDeployed(Config::custom_knife_path)) {
				g_scanned = false;
				ScanCustomKnives();
				const char* want = Config::custom_knife_path;
				const char* slash = std::strrchr(want, '/');
				const char* stemFile = slash ? slash + 1 : want;
				for (const auto& m : g_knives) {
					if (_stricmp(m.model_path.c_str(), want) == 0
						|| m.model_path.find(stemFile) != std::string::npos) {
						if (CustomModelDeployed(m.model_path)) {
							std::snprintf(Config::custom_knife_path,
								sizeof(Config::custom_knife_path),
								"%s", m.model_path.c_str());
							break;
						}
					}
				}
			}
		} else {
			--s_deployCd;
		}
		if (!CustomModelDeployed(Config::custom_knife_path))
			return;
	} else if (pathChanged) {
		std::snprintf(s_deployTried, sizeof(s_deployTried),
			"%s", Config::custom_knife_path);
		g_reapply = kReapplyFrames;
		g_precachedHash = 0;
	}

	const char* model = Config::custom_knife_path;
	const std::uint64_t hash = HashPath(model);
	if (hash != g_precachedHash) {
		(void)PrecacheModelPath(model);
		g_precachedHash = hash;
	}

	// Collect knives from inventory and re-SetModel while window open
	if (g_reapply <= 0)
		g_reapply = kReapplyFrames;

	C_CSWeaponBase* weapons[16]{};
	int count = 0;
	if (!Internal::CollectWeapons(local, weapons, 16, count))
		return;

	// mask 1 = meshgroup 0 — custom packs only ship that group.
	// ProcessKnife may re-stamp stock model on NET_UPDATE; re-apply here on RENDER_END.
	constexpr std::uint64_t kCustomMeshMask = 1ull;
	for (int i = 0; i < count; ++i) {
		C_CSWeaponBase* w = weapons[i];
		if (!w || !IsKnifeWeapon(w))
			continue;
		Internal::SetModelWeaponAndHud(w, local, model, kCustomMeshMask);
	}

	if (g_reapply > 0)
		--g_reapply;
}

} // namespace SkinChanger
