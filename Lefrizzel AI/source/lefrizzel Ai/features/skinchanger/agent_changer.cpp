// Client-side agent changer — schema list + SetModel + AnimGraphRebuild on local pawn.
// Also: custom .vmdl from csgo/characters/models (disk scan + ResourceSystem precache
// incl. sibling .vmat deps).
// Port notes vs Andromeda:
//   - Same schema offsets / #Type_CustomPlayer filter / team from ctm_/tm_
//   - Icons: prefer full model basename (customplayer_tm_leet_variantg_…); base without
//     _variant is fallback only (Andromeda FileExists order — most icons need the variant)
//   - Apply every frame while enabled: without inventory equip the game restores default
//     loadout model; one-shot SetModel does not stick
#include "agent_changer.h"
#include "skinchanger.h"
#include "custom_assets.h"
#include "../../config/config.h"
#include "../../keybinds/keybinds.h"
#include "../../offsets/offsets.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/vfunc/vfunc.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
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

static std::vector<CustomModelInfo> g_customModels;

static bool g_customScanned = false;

static std::uint64_t g_precachedHash = 0;
// UC Tonyha7 / celerity: CBufferString::Insert (tier0) + BlockingLoad.
// Init-only path left paths truncated → BlockingLoad null → hard abort → no SetModel.
using FnCBufferStringInit = char(__fastcall*)(void* buf, const char* str);
using FnCBufferPurge = void(__fastcall*)(void* buf, int);
using FnCBufferInsert = const char*(__fastcall*)(void* buf, int index, const char* str, int len, bool b);
using FnBlockingLoad = void*(__fastcall*)(void* rs, void* bufferString, const char* ext);

// Mirror UC/celerity layout for Insert path (must match tier0 CBufferString).
struct CBufferStringLite {
	int m_nLength;
	int m_nAllocatedSize;
	union {
		char* m_pString;
		char m_szString[8];
	};
};

constexpr std::size_t kCBufferStringBytes = 256;
constexpr std::uint32_t kCBufferAllocEmpty = 0xC0000088u;
// UC Tonyha7: NeedSetModel on FRAME_RENDER_END.
// Loadout stomps model after one SetModel — re-apply while path mismatches
// for a short window (not infinite loop). Menu pick / enable / spawn re-arms.
constexpr int kCustomReapplyFrames = 48; // ~0.75s @ 64 tick of re-SetModel
// Deploy/sync is expensive (full tree copy) — once per path only.
constexpr int kDeployRetryCooldown = 90;

static void* g_resourceSystem = nullptr;
static FnCBufferStringInit g_bufInit = nullptr;
static FnCBufferPurge g_bufPurge = nullptr;
static FnCBufferInsert g_bufInsert = nullptr;
static FnBlockingLoad g_blockingLoad = nullptr;
static bool g_precacheTried = false;
static int g_customReapplyFrames = 0;
// SEH helpers must be noinline / no C++ objects with dtors (MSVC C2712).

__declspec(noinline) static FnBlockingLoad SehBindBlockingLoadVt(void* rs) {
	FnBlockingLoad out = nullptr;
	if (!rs)
		return out;
	__try {
		void** table = *reinterpret_cast<void***>(rs);
		if (table && table[41])
			out = reinterpret_cast<FnBlockingLoad>(table[41]);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out = nullptr;
	}
	return out;
}

__declspec(noinline) static char SehBufInit(void* buf, const char* path) {
	char ok = 0;
	if (!g_bufInit || !buf || !path) return 0;
	__try {
		ok = g_bufInit(buf, path);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = 0;
	}
	return ok;
}

__declspec(noinline) static void* SehBlockingLoad(void* buf) {
	void* result = nullptr;
	if (!buf || !g_resourceSystem) return nullptr;
	__try {
		if (g_blockingLoad)
			result = g_blockingLoad(g_resourceSystem, buf, "");
		else {
			void** vt = *reinterpret_cast<void***>(g_resourceSystem);
			if (vt && vt[41])
				result = reinterpret_cast<FnBlockingLoad>(vt[41])(g_resourceSystem, buf, "");
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		result = nullptr;
	}
	return result;
}

__declspec(noinline) static void SehBufPurge(void* buf) {
	if (!g_bufPurge || !buf) return;
	__try {
		g_bufPurge(buf, 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

__declspec(noinline) static bool SehPrecacheInsert(const char* path) {
	if (!g_bufInsert || !g_resourceSystem || !path || !*path)
		return false;
	bool ok = false;
	__try {
		CBufferStringLite names{};
		names.m_nLength = 0;
		// UC/celerity: 0x80000000 | 0x40000000 | 8
		names.m_nAllocatedSize = static_cast<int>(0x80000000u | 0x40000000u | 8u);
		names.m_pString = nullptr;
		g_bufInsert(&names, 0, path, -1, false);
		void* result = nullptr;
		if (g_blockingLoad)
			result = g_blockingLoad(g_resourceSystem, &names, "");
		else {
			void** vt = *reinterpret_cast<void***>(g_resourceSystem);
			if (vt && vt[41])
				result = reinterpret_cast<FnBlockingLoad>(vt[41])(g_resourceSystem, &names, "");
		}
		// celerity never gates on result — Insert path still warms the resource.
		ok = true;
		(void)result;
		if (g_bufPurge)
			g_bufPurge(&names, 0);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	return ok;
}

static void EnsurePrecacheFns() {
	if (g_precacheTried && g_resourceSystem && g_blockingLoad
		&& (g_bufInsert || g_bufInit))
		return;
	g_precacheTried = true;
	g_resourceSystem = I::Get<void>("resourcesystem.dll", "ResourceSystem013");
	// UC / celerity pattern (shorter, more stable across builds)
	uintptr_t loadAddr = M::patternScan("resourcesystem",
		"40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA");
	if (!loadAddr)
		loadAddr = M::patternScan("resourcesystem",
			"40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA 48 8B D9 FF 90");
	if (loadAddr)
		g_blockingLoad = reinterpret_cast<FnBlockingLoad>(loadAddr);
	else if (g_resourceSystem)
		g_blockingLoad = SehBindBlockingLoadVt(g_resourceSystem);
	const uintptr_t bufInit = M::patternScan("client",
		"48 89 5C 24 10 57 48 83 EC 30 8B 41 04 48 8D 79");
	if (bufInit)
		g_bufInit = reinterpret_cast<FnCBufferStringInit>(bufInit);
	if (const HMODULE tier0 = GetModuleHandleA("tier0.dll")) {
		g_bufPurge = reinterpret_cast<FnCBufferPurge>(
			GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z"));
		// UC Tonyha7 primary path
		g_bufInsert = reinterpret_cast<FnCBufferInsert>(
			GetProcAddress(tier0, "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z"));
	}
}

// Soft precache: always try Insert (UC), fall back Init. Returns true if we
// attempted a load — callers must NOT hard-abort SetModel on false (celerity).
static bool PrecacheModelPath(const char* path) {
	if (!path || !*path)
		return false;
	EnsurePrecacheFns();
	if (!g_resourceSystem || !g_blockingLoad)
		return false;
	// 1) UC/celerity Insert — preferred
	if (g_bufInsert && SehPrecacheInsert(path))
		return true;
	// 2) Init fallback (older LefrizzelAi path)
	if (!g_bufInit)
		return false;
	alignas(16) unsigned char storage[kCBufferStringBytes];
	std::memset(storage, 0, sizeof(storage));
	*reinterpret_cast<std::int32_t*>(storage + 0) = 0;
	*reinterpret_cast<std::uint32_t*>(storage + 4) = kCBufferAllocEmpty;
	if (!SehBufInit(storage, path)) {
		SehBufPurge(storage);
		return false;
	}
	void* result = SehBlockingLoad(storage);
	SehBufPurge(storage);
	return result != nullptr;
}

// Source2 compiled assets use double extensions: foo.vmdl_c / bar.vmat_c.
// std::filesystem::path::extension() only returns the last segment (".c") —
// never match with extension() == ".vmdl_c".
static bool PathEndsWithI(const std::string& s, const char* suffix) {
	const size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}
static bool IsCompiledVmdl(const fs::path& p) {
	return PathEndsWithI(p.filename().string(), ".vmdl_c");
}
static bool IsCompiledVmat(const fs::path& p) {
	return PathEndsWithI(p.filename().string(), ".vmat_c");
}
// Stem of "deadpool.vmdl_c" → "deadpool" (not "deadpool.vmdl").
static std::string CompiledAssetStem(const fs::path& p) {
	std::string name = p.filename().string();
	if (PathEndsWithI(name, ".vmdl_c")) name.resize(name.size() - 7);
	else if (PathEndsWithI(name, ".vmat_c")) name.resize(name.size() - 7);
	else {
		const auto stem = p.stem().string();
		return stem;
	}
	return name;
}

static std::string CharactersModelsDir() {
	char mod[MAX_PATH]{};
	HMODULE h = GetModuleHandleA("client.dll");
	if (!h || !GetModuleFileNameA(h, mod, MAX_PATH))
		return {};
	std::string p = mod;
	// .../game/csgo/bin/win64/client.dll → .../game/csgo/characters/models
	auto pos = p.find("\\bin\\win64");
	if (pos == std::string::npos)
		pos = p.find("/bin/win64");
	if (pos == std::string::npos)
		return {};
	return p.substr(0, pos) + "\\characters\\models";
}

static std::string CsgoContentRoot() {
	const std::string models = CharactersModelsDir();
	if (models.empty()) return {};
	fs::path p(models);
	// .../game/csgo/characters/models → .../game/csgo
	return p.parent_path().parent_path().string();
}

static bool FileExistsRelVmat(const std::string& vmatRel) {
	const std::string root = CsgoContentRoot();
	if (root.empty() || vmatRel.empty()) return false;
	std::string full = root + "\\" + vmatRel;
	std::replace(full.begin(), full.end(), '/', '\\');
	const auto pos = full.rfind(".vmat");
	if (pos == std::string::npos) return false;
	full.replace(pos, 5, ".vmat_c");
	std::error_code ec;
	return fs::exists(full, ec);
}
// Compiled .vmdl_c embeds absolute material paths. Wrong install folder → checkerboard.

static bool ModelMaterialsResolvable(const fs::path& vmdlC) {
	std::error_code ec;
	if (!fs::exists(vmdlC, ec)) return false;
	std::ifstream in(vmdlC, std::ios::binary);
	if (!in) return false;
	std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (data.empty()) return false;
	int refs = 0, ok = 0;
	// Packs may embed vmat refs under either content root:
	//   characters/models/<pack>/materials/*.vmat  (Valve-style)
	//   models/<pack>/materials/*.vmat             (otakku-style)
	auto scanPrefix = [&](const char* pfx, size_t plen, size_t minLen) {
		for (size_t i = 0; i + minLen < data.size(); ++i) {
			if (data[i] != pfx[0] || data.compare(i, plen, pfx) != 0)
				continue;
			size_t end = i;
			while (end < data.size()) {
				const unsigned char ch = static_cast<unsigned char>(data[end]);
				if (ch < 32 || ch > 126) break;
				++end;
			}
			if (end < i + minLen) continue;
			std::string s = data.substr(i, end - i);
			const auto vmat = s.find(".vmat");
			if (vmat == std::string::npos) continue;
			s.resize(vmat + 5);
			// Must live under the pack root we expect for this prefix.
			const bool underPack =
				(plen == 11 && s.rfind("characters/models/", 0) == 0) ||
				(plen == 7 && s.rfind("models/", 0) == 0);
			if (!underPack) continue;
			// Stock shared/arms mats live in VPK — only require pack-local mats on disk
			if (s.find("/shared/") != std::string::npos)
				continue;
			++refs;
			if (FileExistsRelVmat(s)) ++ok;
		}
	};
	scanPrefix("characters/", 11, 16);
	scanPrefix("models/", 7, 12);
	if (refs > 0)
		return ok >= refs;
	const fs::path dir = vmdlC.parent_path();
	for (auto it = fs::recursive_directory_iterator(dir, ec);
		it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) { ec.clear(); continue; }
		if (it->is_regular_file(ec) && IsCompiledVmat(it->path()))
			return true;
	}
	return false;
}

// Warm .vmdl + pack mats ONCE per path. Full recursive BlockingLoad of every
// .vmat was the FPS killer (called every frame while path mismatched).
// UC/celerity: precache the .vmdl itself; engine pulls mats from disk on SetModel.
static bool PrecacheModelDeps(const char* modelVmdl) {
	if (!modelVmdl || !*modelVmdl) return false;
	// Primary: the body mesh resource
	(void)PrecacheModelPath(modelVmdl);

	const std::string csgoRoot = CsgoContentRoot();
	if (csgoRoot.empty()) return true;

	// Light: only embed refs from the .vmdl_c (cap count — no recursive dir walk)
	std::string compiled = csgoRoot + "\\" + modelVmdl;
	std::replace(compiled.begin(), compiled.end(), '/', '\\');
	if (PathEndsWithI(compiled, ".vmdl")) {
		compiled.resize(compiled.size() - 5);
		compiled += ".vmdl_c";
	}
	std::error_code ec;
	if (!fs::exists(compiled, ec))
		return true;

	std::ifstream in(compiled, std::ios::binary);
	if (!in)
		return true;
	std::string data((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	int warmed = 0;
	constexpr int kMaxWarm = 32; // hard cap — never scan whole pack tree
	auto tryPrecacheEmbed = [&](const char* pfx, size_t plen) {
		for (size_t i = 0; i + plen + 6 < data.size() && warmed < kMaxWarm; ++i) {
			if (data[i] != pfx[0] || data.compare(i, plen, pfx) != 0)
				continue;
			size_t end = i;
			while (end < data.size()) {
				const unsigned char ch = static_cast<unsigned char>(data[end]);
				if (ch < 32 || ch > 126) break;
				++end;
			}
			if (end < i + plen + 4) continue;
			std::string s = data.substr(i, end - i);
			bool keep = false;
			for (const char* ext : { ".vmat", ".vmdl" }) {
				const auto e = s.find(ext);
				if (e != std::string::npos) {
					s.resize(e + std::strlen(ext));
					keep = true;
					break;
				}
			}
			if (!keep) continue;
			if (s.find("/shared/") != std::string::npos) continue;
			if (s.find("_arms.vmdl") != std::string::npos
				|| s.find("_arm.vmdl") != std::string::npos)
				continue;
			if (PrecacheModelPath(s.c_str()))
				++warmed;
		}
	};
	tryPrecacheEmbed("characters/", 11);
	tryPrecacheEmbed("models/", 7);
	return true;
}

static bool g_customFpHidden = false;

// UC Tonyha7 never touches render alpha. Zeroing m_clrRender alpha hides the
// WHOLE model (Deadpool invisible). We keep alpha at 255 and rely on the
// engine's own first_person meshgroups; if a pack lacks them the head clips
// in FP, but the model stays visible. Hiding must be done via meshgroup mask,
// not entity alpha.

static void ApplyCustomFpVisibility(C_CSPlayerPawn* local) {
	if (!local) return;
	__try {
		const std::uint32_t off = Offset::m_clrRender();
		if (!off) return;
		auto* clr = reinterpret_cast<std::uint8_t*>(
			reinterpret_cast<std::uint8_t*>(local) + off);
		clr[0] = 255;
		clr[1] = 255;
		clr[2] = 255;
		clr[3] = 255; // never hide — alpha=0 makes the whole pawn invisible
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	g_customFpHidden = false;
}

static void RestoreCustomFpVisibility(C_CSPlayerPawn* local) {
	if (!local) return;
	__try {
		const std::uint32_t off = Offset::m_clrRender();
		if (!off) return;
		auto* clr = reinterpret_cast<std::uint8_t*>(
			reinterpret_cast<std::uint8_t*>(local) + off);
		clr[0] = 255; clr[1] = 255; clr[2] = 255; clr[3] = 255;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	g_customFpHidden = false;
}

static bool IsStockValveAgentPath(const std::string& relLower) {
	// Valve agents: characters/models/ctm_xxx/file.vmdl or tm_xxx/ (one folder).
	// Nested characters/models/author/... kept as custom packs.
	if (relLower.rfind("characters/models/shared/", 0) == 0)
		return true;
	constexpr const char* pfx = "characters/models/";
	if (relLower.rfind(pfx, 0) != 0)
		return false;
	const std::string rest = relLower.substr(std::strlen(pfx));
	const auto slash = rest.find('/');
	if (slash == std::string::npos)
		return false;
	const std::string folder = rest.substr(0, slash);
	const std::string file = rest.substr(slash + 1);
	if (file.find('/') != std::string::npos)
		return false; // nested under author/pack = custom
	return folder.rfind("ctm_", 0) == 0 || folder.rfind("tm_", 0) == 0;
}

static bool IsFragmentModelStem(std::string stemLower) {
	for (char& ch : stemLower)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	// Partial / attach vmdls crash SetModel — keep body packs.
	// Use _arms/_arm suffix (not bare "arm") so names like "charm" still list.
	static constexpr const char* kFrag[] = {
		"nohitbox", "no_hitbox", "nohbox",
		"cloth", "swim", "prefab",
		"_lod", "gib", "phys", "ragdoll",
		"attach", "_part", "helmet", "hat", "mask", "prop",
		"player_model", "dress",
		"sk2model", "gfl2", "sleeve",
		"_arms", "_arm", "arms_", "arm_",
		"viewmodel", "weapon",
	};
	for (const char* m : kFrag) {
		if (stemLower.find(m) != std::string::npos)
			return true;
	}
	// Bare "arms" / "arm" as whole token (deadpool_arms already hit _arms)
	if (stemLower == "arms" || stemLower == "arm")
		return true;
	if (stemLower.empty())
		return true;
	if (std::isdigit(static_cast<unsigned char>(stemLower[0])))
		return true;
	return false;
}

// UC Tonyha7 ScanModels: recursive .vmdl_c under csgo/characters/models,
// path = "characters/..." + ".vmdl". Also list lefrizzel drop packs (pre-deploy).
static void AddCustomModelEntry(
	std::unordered_set<std::string>& seen,
	std::string rel,
	const fs::path& compiledPath,
	const std::string& csgoRoot)
{
	std::replace(rel.begin(), rel.end(), '\\', '/');
	if (rel.find("/materials/") != std::string::npos)
		return;
	if (!PathEndsWithI(rel, ".vmdl"))
		return;

	std::string relLower = rel;
	for (char& ch : relLower)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	if (IsStockValveAgentPath(relLower))
		return;
	if (!seen.insert(relLower).second)
		return;

	CustomModelInfo info{};
	{
		std::string disp = rel;
		const char* pfxA = "characters/models/";
		const char* pfxB = "models/";
		if (disp.rfind(pfxA, 0) == 0) disp.erase(0, std::strlen(pfxA));
		else if (disp.rfind(pfxB, 0) == 0) disp.erase(0, std::strlen(pfxB));
		if (disp.size() > 5 && disp.compare(disp.size() - 5, 5, ".vmdl") == 0)
			disp.resize(disp.size() - 5);
		if (!csgoRoot.empty() && !CustomModelDeployed(rel))
			disp = "[not deployed] " + disp;
		else if (!ModelMaterialsResolvable(compiledPath))
			disp = "[wrong path] " + disp;
		info.display = std::move(disp);
	}
	info.model_path = std::move(rel);
	g_customModels.push_back(std::move(info));
}

static void ScanCustomModels() {
	g_customModels.clear();
	g_customScanned = true;
	EnsureLefrizzelModelsDir();
	// One deploy pass on refresh only (not per-frame)
	SyncDroppedAgentPacks();

	const std::string dropStr = LefrizzelAgentsDir();
	const std::string csgoRoot = CsgoContentRoot();
	const std::string charModels = CharactersModelsDir();
	std::error_code ec;
	std::unordered_set<std::string> seen;
	const auto opts = fs::directory_options::skip_permission_denied;

	// 1) UC Tonyha7 path: already deployed under csgo/characters/models
	if (!charModels.empty() && fs::exists(charModels, ec)) {
		for (auto it = fs::recursive_directory_iterator(charModels, opts, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file(ec)) continue;
			const fs::path& path = it->path();
			if (!IsCompiledVmdl(path)) continue;
			{
				std::string check = path.generic_string();
				for (char& ch : check)
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				if (check.find("/materials/") != std::string::npos)
					continue;
			}
			const std::string stem = CompiledAssetStem(path);
			if (IsFragmentModelStem(stem))
				continue;
			// full path → characters/models/.../name.vmdl
			std::string full = path.string();
			std::replace(full.begin(), full.end(), '\\', '/');
			const auto hit = full.find("characters/");
			if (hit == std::string::npos) continue;
			std::string rel = full.substr(hit);
			if (PathEndsWithI(rel, ".vmdl_c")) {
				rel.resize(rel.size() - 7);
				rel += ".vmdl";
			}
			AddCustomModelEntry(seen, std::move(rel), path, csgoRoot);
		}
	}

	// 2) Drop folder packs (may still need deploy)
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
				if (check.find("/materials/") != std::string::npos)
					continue;
			}
			if (IsFragmentModelStem(CompiledAssetStem(path)))
				continue;
			std::string rel = InferCustomModelGamePath(path);
			if (rel.empty()) continue;
			AddCustomModelEntry(seen, std::move(rel), path, csgoRoot);
		}
	}

	std::sort(g_customModels.begin(), g_customModels.end(),
		[](const CustomModelInfo& a, const CustomModelInfo& b) {
			const bool aw = a.display.rfind("[wrong path]", 0) == 0
				|| a.display.rfind("[not deployed]", 0) == 0;
			const bool bw = b.display.rfind("[wrong path]", 0) == 0
				|| b.display.rfind("[not deployed]", 0) == 0;
			if (aw != bw) return !aw;
			return a.display < b.display;
		});
}

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
	g_precachedHash = 0;
	g_customReapplyFrames = kCustomReapplyFrames; // force re-SetModel after menu pick
}

bool CustomModelsReady() {
	if (!g_customScanned)
		ScanCustomModels();
	return !g_customModels.empty();
}

const std::vector<CustomModelInfo>& CustomModels() {
	if (!g_customScanned)
		ScanCustomModels();
	return g_customModels;
}

void RefreshCustomModels() {
	SyncDroppedAgentPacks();
	g_customScanned = false;
	g_precachedHash = 0;
	ScanCustomModels();
	// Dropped pack gone → clear dead selection so combo doesn't show ghost path
	if (Config::custom_model_path[0]
		&& CustomModelIndexOf(Config::custom_model_path) < 0) {
		Config::custom_model_path[0] = '\0';
	}
}

int CustomModelIndexOf(const char* path) {
	if (!path || !*path)
		return -1;
	if (!g_customScanned)
		ScanCustomModels();
	for (int i = 0; i < (int)g_customModels.size(); ++i) {
		if (_stricmp(g_customModels[i].model_path.c_str(), path) == 0)
			return i;
	}
	return -1;
}
// UC Tonyha7: PrecacheResource (Insert) + SetModel on FRAME_RENDER_END.
// Celerity: re-SetModel for k_skin_reapply_frames after path/spawn change
// (loadout stomps one-shot). No AnimGraphRebuild on custom (can A-pose).

// CSkeletonInstance + CModelState::m_ModelName (CUtlSymbolLarge = const char*)
static const char* ReadPawnModelPath(C_CSPlayerPawn* local) {
	if (!local) return nullptr;
	const char* name = nullptr;
	__try {
		CGameSceneNode* node = local->m_pGameSceneNode();
		if (!node || !Mem::IsUserPtr(node)) return nullptr;
		// CSkeletonInstance::m_modelState + CModelState::m_ModelName (schema)
		auto* p = reinterpret_cast<const char**>(
			reinterpret_cast<std::uint8_t*>(node) + Offset::SkeletonModelNameOff());
		if (!p || !Mem::IsUserPtr(p)) return nullptr;
		name = *p;
		if (!name || !Mem::IsUserPtr(const_cast<char*>(name)) || !*name)
			return nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return name;
}

// Engine may store / or \, optional leading junk — compare normalized.
static bool ModelPathEq(const char* a, const char* b) {
	if (!a || !b || !*a || !*b) return false;
	// Skip common "models/" vs full path mismatches by comparing tail after last
	// characters/models or models/
	auto normTail = [](const char* p) -> const char* {
		const char* hit = std::strstr(p, "characters/models/");
		if (hit) return hit;
		hit = std::strstr(p, "characters\\models\\");
		if (hit) return hit;
		hit = std::strstr(p, "models/");
		if (hit) return hit;
		hit = std::strstr(p, "models\\");
		if (hit) return hit;
		return p;
	};
	const char* sa = normTail(a);
	const char* sb = normTail(b);
	for (;;) {
		char ca = *sa++;
		char cb = *sb++;
		if (ca == '\\') ca = '/';
		if (cb == '\\') cb = '/';
		if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
		if (ca != cb) return false;
		if (!ca) return true;
	}
}

static const char* TeamDefaultAgentModel(int team) {
	if (team == 3) return "characters/models/ctm_sas/ctm_sas.vmdl";
	if (team == 2) return "characters/models/tm_phoenix/tm_phoenix.vmdl";
	return nullptr;
}

// Resolve stock agent model for current team from config def index.
// Prefer live schema path; fall back to scanned AgentInfo::model_path.
// NOTE: do NOT gate on SchemaReady() — that is skin offset init, not ItemSchema.
static const char* ResolveAgentModel(int team) {
	const std::uint16_t wantDef = (team == 3) ? (std::uint16_t)Config::agent_ct_def
		: (team == 2) ? (std::uint16_t)Config::agent_t_def : 0;
	if (!wantDef)
		return nullptr;
	if (!g_scanned)
		ScanAgents();
	// Live econ path first (handles post-update renames)
	if (const char* live = LiveModelPath(wantDef)) {
		if (IsModelPath(live))
			return live;
	}
	if (const AgentInfo* info = FindByDef(wantDef)) {
		if (!info->model_path.empty())
			return info->model_path.c_str();
	}
	return nullptr;
}

void RunAgents(C_CSPlayerPawn* local) {
	using namespace Internal;
	if (!local) return;

	// Track applies so disable can restore pre-change / team-default model.
	static bool s_hadCustom = false;
	static char s_preCustomModel[260]{};
	static bool s_hadAgent = false;
	static char s_preAgentModel[260]{};
	static char s_lastAppliedModel[260]{};
	static int s_agentReapply = 0;
	static int s_deployRetryCd = 0;
	static char s_deployTriedPath[260]{};
	// UC Tonyha7: re-fire one-shot after respawn (his sample only did menu select)
	static float s_customSpawnLatch = -1.f;

	const bool useCustom = Config::custom_model && Config::custom_model_path[0] != '\0';
	const bool agentOn = Config::agent_changer && !useCustom;

	if (local->getHealth() <= 0) {
		RestoreCustomFpVisibility(local);
		g_lastHash = 0;
		g_customReapplyFrames = 0;
		s_agentReapply = 0;
		s_lastAppliedModel[0] = '\0';
		// Arm re-apply once on next alive frame (spawn)
		if (useCustom)
			g_customReapplyFrames = kCustomReapplyFrames;
		return;
	}
	if (!PawnSkeletonReady(local)) {
		g_lastHash = 0;
		return;
	}

	const int team = (int)local->getTeam();
	const float spawn = LastSpawnTime(local);
	const char* model = nullptr;
	const bool leavingCustom = s_hadCustom && !useCustom;
	bool agentActive = false;
	const char* agentModel = nullptr;

	// ── Custom model (UC Tonyha7 ChangeModelNow) ─────────────────────────
	// Select → g_NeedSetModel. RENDER_END → Precache + SetModel once → clear.
	// No continuous pathMismatch loop. No AnimGraphRebuild. No mat spam.
	if (useCustom) {
		if (!s_hadCustom) {
			if (const char* cur = ReadPawnModelPath(local)) {
				if (!ModelPathEq(cur, Config::custom_model_path))
					std::snprintf(s_preCustomModel, sizeof(s_preCustomModel), "%s", cur);
			}
			s_hadCustom = true;
			g_customReapplyFrames = kCustomReapplyFrames; // == g_NeedSetModel
			s_agentReapply = 0;
			s_deployTriedPath[0] = '\0';
			s_customSpawnLatch = spawn;
		}
		// Respawn → one more apply (Tonyha7 didn't; we need mesh after death)
		if (spawn != s_customSpawnLatch) {
			s_customSpawnLatch = spawn;
			g_customReapplyFrames = kCustomReapplyFrames;
		}

		if (!g_customScanned)
			ScanCustomModels();

		// Deploy once per selected path (not every frame)
		const bool pathChanged =
			!s_deployTriedPath[0]
			|| _stricmp(s_deployTriedPath, Config::custom_model_path) != 0;
		if (!CustomModelDeployed(Config::custom_model_path)) {
			if (pathChanged || s_deployRetryCd <= 0) {
				// Force full re-sync (fixed InferBakedDir may place pack correctly now)
				SyncDroppedAgentPacks();
				std::snprintf(s_deployTriedPath, sizeof(s_deployTriedPath),
					"%s", Config::custom_model_path);
				s_deployRetryCd = kDeployRetryCooldown;
				g_precachedHash = 0;
				g_customReapplyFrames = kCustomReapplyFrames;
				// Path may have been wrong (old bake) — re-resolve from list
				if (!CustomModelDeployed(Config::custom_model_path)) {
					g_customScanned = false;
					ScanCustomModels();
					// If selection still missing, try match by stem in scanned list
					const char* want = Config::custom_model_path;
					const char* slash = std::strrchr(want, '/');
					const char* stemFile = slash ? slash + 1 : want;
					for (const auto& m : g_customModels) {
						if (_stricmp(m.model_path.c_str(), want) == 0
							|| m.model_path.find(stemFile) != std::string::npos) {
							if (CustomModelDeployed(m.model_path)) {
								std::snprintf(Config::custom_model_path,
									sizeof(Config::custom_model_path),
									"%s", m.model_path.c_str());
								break;
							}
						}
					}
				}
			} else {
				--s_deployRetryCd;
			}
			if (!CustomModelDeployed(Config::custom_model_path))
				return; // missing on disk — wait for Refresh
		} else if (pathChanged) {
			std::snprintf(s_deployTriedPath, sizeof(s_deployTriedPath),
				"%s", Config::custom_model_path);
			g_customReapplyFrames = kCustomReapplyFrames;
			g_precachedHash = 0;
		}

		model = Config::custom_model_path;
		const std::uint64_t hash = HashPath(model);

		const char* livePath = ReadPawnModelPath(local);
		const bool pathMismatch = !livePath || !ModelPathEq(livePath, model);

		// Idle only when live model already matches.
		if (!pathMismatch) {
			g_customReapplyFrames = 0;
			g_lastHash = hash;
			ApplyCustomFpVisibility(local);
			g_lastSpawn = spawn;
			g_lastTeam = team;
			return;
		}

		// Mismatch: keep re-applying while path wrong (loadout stomps).
		// Re-arm every window so one failed SetModel doesn't stick forever.
		if (g_customReapplyFrames <= 0)
			g_customReapplyFrames = kCustomReapplyFrames;

		// ChangeModelNow(): PrecacheResource + SetModel
		if (hash != g_precachedHash) {
			(void)PrecacheModelDeps(model);
			g_precachedHash = hash;
		} else {
			(void)PrecacheModelPath(model);
		}
		SetPawnModel(local, model);
		// First few frames after path change: rebuild anim so custom mesh binds
		// (mode=0 full reload — not tear-only A-pose). After that, only SetModel.
		if (pathChanged || g_customReapplyFrames >= (kCustomReapplyFrames - 2))
			RebuildPawnAnimGraph(local);

		std::snprintf(s_lastAppliedModel, sizeof(s_lastAppliedModel), "%s", model);
		if (g_customReapplyFrames > 0)
			--g_customReapplyFrames;
		g_lastHash = hash;
		g_lastSpawn = spawn;
		g_lastTeam = team;
		ApplyCustomFpVisibility(local);
		return;
	}

	// ── Stock agents / restore ───────────────────────────────────────────
	RestoreCustomFpVisibility(local);
	s_deployTriedPath[0] = '\0';
	s_customSpawnLatch = -1.f;

	if (agentOn) {
		agentModel = ResolveAgentModel(team);
		if (agentModel && *agentModel)
			agentActive = true;
	}

	if (agentActive) {
		if (!s_hadAgent) {
			if (const char* cur = ReadPawnModelPath(local)) {
				if (!ModelPathEq(cur, agentModel))
					std::snprintf(s_preAgentModel, sizeof(s_preAgentModel), "%s", cur);
			}
			if (!s_preAgentModel[0] && s_preCustomModel[0])
				std::snprintf(s_preAgentModel, sizeof(s_preAgentModel), "%s", s_preCustomModel);
			s_hadAgent = true;
			s_agentReapply = 12; // short window for loadout stomp on stock agents
		}
		if (g_customReapplyFrames > 0) {
			s_agentReapply = (std::max)(s_agentReapply, g_customReapplyFrames);
			g_customReapplyFrames = 0;
		}
		model = agentModel;
	} else {
		const bool leavingAgent = s_hadAgent && !agentActive;
		if (leavingCustom || leavingAgent) {
			if (leavingCustom && s_preCustomModel[0])
				model = s_preCustomModel;
			else if (leavingAgent && s_preAgentModel[0])
				model = s_preAgentModel;
			else if (leavingCustom && s_preAgentModel[0])
				model = s_preAgentModel;
			else
				model = TeamDefaultAgentModel(team);
		}
		s_agentReapply = 0;
	}

	if (!model || !*model) {
		if (leavingCustom) {
			s_hadCustom = false;
			s_preCustomModel[0] = '\0';
		}
		if (s_hadAgent && !agentActive) {
			s_hadAgent = false;
			s_preAgentModel[0] = '\0';
		}
		g_lastHash = 0;
		s_lastAppliedModel[0] = '\0';
		return;
	}

	const std::uint64_t hash = HashPath(model);
	const bool leavingAgent = s_hadAgent && !agentActive && !useCustom;
	const bool oneShotRestore =
		(leavingCustom && !agentActive) || leavingAgent;

	if (agentActive && (hash != g_lastHash || spawn != g_lastSpawn || team != g_lastTeam))
		s_agentReapply = 12;

	const char* livePath = ReadPawnModelPath(local);
	const bool pathMismatch = !livePath || !ModelPathEq(livePath, model);

	if (!oneShotRestore && !pathMismatch) {
		s_agentReapply = 0;
		g_lastHash = hash;
		g_lastSpawn = spawn;
		g_lastTeam = team;
		if (!s_lastAppliedModel[0] || !ModelPathEq(s_lastAppliedModel, model))
			std::snprintf(s_lastAppliedModel, sizeof(s_lastAppliedModel), "%s", model);
		return;
	}

	const bool needSetModel = oneShotRestore
		|| (agentActive && (s_agentReapply > 0 || pathMismatch));

	if (needSetModel) {
		if (hash != g_precachedHash) {
			PrecacheModelPath(model);
			g_precachedHash = hash;
		}
		SetPawnModel(local, model);
		const bool pathChanged = !s_lastAppliedModel[0]
			|| !ModelPathEq(s_lastAppliedModel, model)
			|| spawn != g_lastSpawn;
		if (pathChanged)
			RebuildPawnAnimGraph(local);
		std::snprintf(s_lastAppliedModel, sizeof(s_lastAppliedModel), "%s", model);
		if (s_agentReapply > 0)
			--s_agentReapply;
	}

	if (leavingCustom) {
		s_hadCustom = false;
		s_preCustomModel[0] = '\0';
	}
	if (leavingAgent) {
		s_hadAgent = false;
		s_preAgentModel[0] = '\0';
		s_lastAppliedModel[0] = '\0';
	}

	if (oneShotRestore) {
		g_lastHash = 0;
		g_lastSpawn = spawn;
		g_lastTeam = team;
		g_customReapplyFrames = 0;
		s_agentReapply = 0;
		s_lastAppliedModel[0] = '\0';
		return;
	}

	g_lastHash = hash;
	g_lastSpawn = spawn;
	g_lastTeam = team;
}

} // namespace SkinChanger
