// Drop folders for agent packs under csgo/lefrizzel_models/ (legacy: lefrizzel_models).
// Deploy to baked characters/ paths the .vmdl expects.
#include "custom_assets.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
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

static std::string CsgoRoot() {
	char mod[MAX_PATH]{};
	HMODULE h = GetModuleHandleA("client.dll");
	if (!h || !GetModuleFileNameA(h, mod, MAX_PATH))
		return {};
	std::string p = mod;
	auto pos = p.find("\\bin\\win64");
	if (pos == std::string::npos) pos = p.find("/bin/win64");
	if (pos == std::string::npos) return {};
	return p.substr(0, pos);
}

// foo.vmdl_c → extension() is ".c". Always match full suffix.
static bool PathEndsWithI(const std::string& s, const char* suffix) {
	const size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}
static bool IsCompiledVmdlName(const fs::path& p) {
	return PathEndsWithI(p.filename().string(), ".vmdl_c");
}
static std::string CompiledVmdlStem(const fs::path& vmdlC) {
	std::string name = vmdlC.filename().string();
	if (PathEndsWithI(name, ".vmdl_c"))
		name.resize(name.size() - 7);
	else
		name = vmdlC.stem().string();
	return name;
}

static std::string ReadFileBin(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool IsGameAssetPrefix(const std::string& s) {
	return s.rfind("characters/", 0) == 0
		|| s.rfind("models/", 0) == 0
		|| s.rfind("weapons/", 0) == 0;
}

// Collect printable asset-path strings (characters/ models/ weapons/).
// Binary .vmdl_c embeds vmdl/vmat/vtex refs in ASCII; other bytes break the run.
static void CollectAssetPaths(const std::string& data, std::vector<std::string>& out) {
	auto try_start = [&](size_t i, const char* pfx, size_t plen) {
		if (i + plen >= data.size()) return;
		if (data.compare(i, plen, pfx) != 0) return;
		size_t end = i;
		while (end < data.size()) {
			const unsigned char chb = static_cast<unsigned char>(data[end]);
			if (chb < 32 || chb > 126) break;
			++end;
		}
		if (end < i + plen + 2) return;
		std::string s = data.substr(i, end - i);
		for (const char* ext : { ".vmdl", ".vmat", ".vtex", ".vanmgrph" }) {
			const auto e = s.find(ext);
			if (e != std::string::npos) {
				s.resize(e + std::strlen(ext));
				break;
			}
		}
		if (!IsGameAssetPrefix(s)) return;
		out.push_back(std::move(s));
	};
	for (size_t i = 0; i + 12 < data.size(); ++i) {
		if (data[i] == 'c') try_start(i, "characters/", 11);
		else if (data[i] == 'm') try_start(i, "models/", 7);
		else if (data[i] == 'w') try_start(i, "weapons/", 8);
	}
}

static std::string DirnameSlash(std::string p) {
	std::replace(p.begin(), p.end(), '\\', '/');
	const auto slash = p.find_last_of('/');
	if (slash == std::string::npos) return {};
	return p.substr(0, slash);
}

// Normalize a vmdl-embedded asset dir to the game's baked root by stripping
// trailing material folder segments:
//   characters/models/kolka/deadpool/materials -> characters/models/kolka/deadpool
//   characters/models/kolka/2026/megumin/mat   -> characters/models/kolka/2026/megumin
//   models/otakku/asuka_langley/materials      -> models/otakku/asuka_langley
// Path kept VERBATIM (characters/... or models/...) — never force-prefix.
static std::string BakeDirFromAssetDir(std::string d) {
	std::replace(d.begin(), d.end(), '\\', '/');
	// Strip trailing empty + mat folder once so pack root = model folder.
	// e.g. .../megumin/mat -> .../megumin ; .../deadpool/materials -> .../deadpool
	while (!d.empty() && d.back() == '/')
		d.pop_back();
	const auto slash = d.find_last_of('/');
	if (slash != std::string::npos) {
		const std::string seg = d.substr(slash + 1);
		if (seg == "materials" || seg == "mat" || seg == "mats"
			|| seg == "textures" || seg == "texture")
			d.resize(slash);
	}
	return d; // verbatim — may be characters/... or models/...
}

// Infer pack install folder from .vmdl_c embeds.
// Critical: many packs embed a BARE pack dir (no extension), e.g.
//   "characters/models/kolka/2026/megumin"
// Old code did DirnameSlash on that → parent ".../2026" → wrong deploy path
// → CustomModelDeployed fails → select does nothing.
// Prefer: bare .../stem dir, then .../stem/file.vmat, never parent of stem.
static std::string InferBakedDir(const fs::path& vmdlC, const std::string& data) {
	const std::string stem = CompiledVmdlStem(vmdlC);
	if (stem.empty())
		return {};
	std::vector<std::string> paths;
	CollectAssetPaths(data, paths);

	auto lastSeg = [](std::string d) -> std::string {
		std::replace(d.begin(), d.end(), '\\', '/');
		while (!d.empty() && d.back() == '/')
			d.pop_back();
		const auto slash = d.find_last_of('/');
		return (slash == std::string::npos) ? d : d.substr(slash + 1);
	};
	auto endsWithStemDir = [&](const std::string& d) -> bool {
		return _stricmp(lastSeg(d).c_str(), stem.c_str()) == 0;
	};

	// 1) Bare pack dir embed: path last segment == stem, no file extension.
	//    characters/models/kolka/2026/megumin  → bake = that path (NOT parent)
	for (const auto& p : paths) {
		std::string d = p;
		std::replace(d.begin(), d.end(), '\\', '/');
		while (!d.empty() && d.back() == '/')
			d.pop_back();
		if (!IsGameAssetPrefix(d))
			continue;
		// no '.' in last segment → directory, not file.ext
		const std::string seg = lastSeg(d);
		if (seg.find('.') != std::string::npos)
			continue;
		if (_stricmp(seg.c_str(), stem.c_str()) == 0)
			return d;
	}

	// 2) File under pack whose bake dir ends with stem
	//    .../megumin/mat/bodt.vmat → BakeDir strip mat → .../megumin
	//    .../megumin.vmdl → Dirname → .../ (parent) only accept if ends stem
	for (const auto& p : paths) {
		if (p.find(stem) == std::string::npos)
			continue;
		std::string d = BakeDirFromAssetDir(DirnameSlash(p));
		if (!d.empty() && endsWithStemDir(d))
			return d;
		// file named stem.ext sitting in pack folder already ends with parent≠stem;
		// if path is .../stem/stem.vmdl, Dirname is .../stem — handled above.
	}

	// 3) Any vmat whose bake dir ends with stem (materials strip)
	for (const auto& p : paths) {
		if (p.find(".vmat") == std::string::npos)
			continue;
		std::string d = BakeDirFromAssetDir(DirnameSlash(p));
		if (!d.empty() && endsWithStemDir(d))
			return d;
	}

	// 4) Last resort: first stem-containing bake (may be wrong parent — better
	//    than empty so fallback pack-folder path can still list something)
	for (const auto& p : paths) {
		if (p.find(stem) == std::string::npos)
			continue;
		std::string d = BakeDirFromAssetDir(DirnameSlash(p));
		if (!d.empty())
			return d;
	}
	for (const auto& p : paths) {
		if (p.find(".vmat") == std::string::npos)
			continue;
		std::string d = BakeDirFromAssetDir(DirnameSlash(p));
		if (!d.empty())
			return d;
	}
	return {};
}

static void CopyTree(const fs::path& src, const fs::path& dst) {
	std::error_code ec;
	fs::create_directories(dst, ec);
	for (auto it = fs::recursive_directory_iterator(src, ec);
		it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) { ec.clear(); continue; }
		const fs::path rel = fs::relative(it->path(), src, ec);
		if (ec) { ec.clear(); continue; }
		const fs::path out = dst / rel;
		if (it->is_directory(ec)) {
			fs::create_directories(out, ec);
		} else if (it->is_regular_file(ec)) {
			fs::create_directories(out.parent_path(), ec);
			fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
		}
	}
}

// Pack root already mirrors characters/... models/... and/or weapons/... layout
static int SyncMirroredTrees(const fs::path& drop, const fs::path& csgo) {
	std::error_code ec;
	int n = 0;
	const fs::path charRoot = drop / "characters";
	if (fs::exists(charRoot, ec)) {
		CopyTree(charRoot, csgo / "characters");
		++n;
	}
	// otakku-style packs ship models/<author>/<name>/...
	const fs::path modelsRoot = drop / "models";
	if (fs::exists(modelsRoot, ec)) {
		CopyTree(modelsRoot, csgo / "models");
		++n;
	}
	// Knife packs: weapons/models/knife/...
	const fs::path weaponsRoot = drop / "weapons";
	if (fs::exists(weaponsRoot, ec)) {
		CopyTree(weaponsRoot, csgo / "weapons");
		++n;
	}
	return n;
}

// Loose pack folders: find body .vmdl_c, infer baked dir from embedded paths.
// Prefer the body mesh (skip _arms / nohitbox fragments) so packRoot is the
// real pack folder, not a nested materials tree.
static bool IsBodyVmdlStem(const std::string& stem) {
	std::string s = stem;
	for (char& ch : s)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	if (s.empty()) return false;
	static constexpr const char* kSkip[] = {
		"_arms", "_arm", "nohitbox", "no_hitbox", "arms_", "arm_",
		"viewmodel", "gib", "phys", "ragdoll",
	};
	for (const char* m : kSkip) {
		if (s.find(m) != std::string::npos)
			return false;
	}
	return true;
}

// preferWeaponsFallback: loose knife packs → weapons/models/knife/<pack>/<stem>
static int SyncLooseVmdls(const fs::path& drop, const fs::path& csgo,
	std::unordered_set<std::string>& done, bool preferWeaponsFallback) {
	std::error_code ec;
	int n = 0;
	// Collect body vmdls first so fragment meshes don't "claim" a pack root.
	std::vector<fs::path> bodyVmdls;
	for (auto it = fs::recursive_directory_iterator(drop, ec);
		it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) { ec.clear(); continue; }
		if (!it->is_regular_file(ec)) continue;
		const fs::path& p = it->path();
		if (!IsCompiledVmdlName(p)) continue;

		std::string rel = fs::relative(p, drop, ec).string();
		if (ec) { ec.clear(); continue; }
		std::replace(rel.begin(), rel.end(), '\\', '/');
		if (rel.rfind("characters/", 0) == 0) continue;
		if (rel.rfind("models/", 0) == 0) continue;
		if (rel.rfind("weapons/", 0) == 0) continue;
		// Never treat materials trees as pack roots
		if (rel.find("/materials/") != std::string::npos) continue;

		if (!IsBodyVmdlStem(CompiledVmdlStem(p)))
			continue;
		bodyVmdls.push_back(p);
	}

	for (const fs::path& p : bodyVmdls) {
		const fs::path packRoot = p.parent_path();
		std::string key = packRoot.string();
		std::transform(key.begin(), key.end(), key.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		if (!done.insert(key).second) continue;

		const std::string data = ReadFileBin(p);
		if (data.empty()) continue;
		std::string baked = InferBakedDir(p, data);
		if (baked.empty()) {
			const std::string packName = packRoot.filename().string();
			const std::string stem = CompiledVmdlStem(p);
			if (!packName.empty() && !stem.empty()) {
				if (preferWeaponsFallback)
					baked = std::string("weapons/models/knife/") + packName;
				else
					baked = std::string("characters/models/") + packName;
			}
		}
		const bool validRoot =
			!baked.empty() &&
			(baked.rfind("characters/", 0) == 0
				|| baked.rfind("models/", 0) == 0
				|| baked.rfind("weapons/", 0) == 0);
		if (!validRoot)
			continue;
		std::replace(baked.begin(), baked.end(), '/', '\\');
		CopyTree(packRoot, csgo / baked);
		++n;
	}
	return n;
}

static void WriteAgentDropReadme(const fs::path& dir) {
	std::error_code ec;
	const fs::path readme = dir / "README.txt";
	if (fs::exists(readme, ec)) return;
	std::ofstream out(readme, std::ios::trunc);
	if (!out) return;
	out << "Lefrizzel AI — custom AGENT / player packs\n"
		<< "========================================\n"
		<< "Drop player/agent pack folders here (any layout).\n"
		<< "In-menu \"Refresh models\" deploys packs to:\n"
		<< "  csgo\\characters\\models\\...  (Valve-style)\n"
		<< "  csgo\\models\\...             (otakku-style)\n"
		<< "Accepted layouts:\n"
		<< "  1) Loose folder with *.vmdl_c (auto-infers baked path)\n"
		<< "  2) Full mirror: characters\\... or models\\... tree\n";
}

static void WriteKnifeDropReadme(const fs::path& dir) {
	std::error_code ec;
	const fs::path readme = dir / "README.txt";
	if (fs::exists(readme, ec)) return;
	std::ofstream out(readme, std::ios::trunc);
	if (!out) return;
	out << "Lefrizzel AI — custom KNIFE packs\n"
		<< "=================================\n"
		<< "Drop knife mesh pack folders here (any layout).\n"
		<< "In-menu Knives → Custom Model → Refresh deploys to:\n"
		<< "  csgo\\weapons\\models\\knife\\...\n"
		<< "  (or whatever path the .vmdl embeds)\n"
		<< "Accepted layouts:\n"
		<< "  1) Loose folder with *.vmdl_c (auto-infers baked path)\n"
		<< "  2) Full mirror: weapons\\models\\knife\\... tree\n"
		<< "Pick a stock knife first (e.g. Karambit) so anims/VData bind,\n"
		<< "then enable Custom Model and select your pack.\n";
}

static int SyncDropFolder(const fs::path& drop, const fs::path& csgo, bool preferWeapons) {
	std::error_code ec;
	if (!fs::exists(drop, ec)) return 0;
	int n = SyncMirroredTrees(drop, csgo);
	std::unordered_set<std::string> done;
	n += SyncLooseVmdls(drop, csgo, done, preferWeapons);
	return n;
}

} // namespace

std::string LefrizzelModelsDir() {
	const std::string root = CsgoRoot();
	if (root.empty()) return {};
	const std::string neu = root + "\\lefrizzel_models";
	const std::string legacy = root + "\\lefrizzel_models";
	std::error_code ec;
	// Prefer new brand path; fall back if only legacy exists
	if (fs::exists(neu, ec) || !fs::exists(legacy, ec))
		return neu;
	return legacy;
}

std::string LefrizzelAgentsDir() {
	const std::string root = LefrizzelModelsDir();
	if (root.empty()) return {};
	return root + "\\agents";
}

std::string LefrizzelKnivesDir() {
	const std::string root = LefrizzelModelsDir();
	if (root.empty()) return {};
	return root + "\\knives";
}

void EnsureLefrizzelModelsDir() {
	// Always create branded folder so menu path matches disk
	const std::string csgo = CsgoRoot();
	if (csgo.empty()) return;
	const std::string parent = csgo + "\\lefrizzel_models";
	const std::string agents = parent + "\\agents";
	const std::string knives = parent + "\\knives";
	std::error_code ec;
	fs::create_directories(agents, ec);
	fs::create_directories(knives, ec);

	const fs::path parentReadme = fs::path(parent) / "README.txt";
	if (!fs::exists(parentReadme, ec)) {
		std::ofstream out(parentReadme, std::ios::trunc);
		if (out) {
			out << "Lefrizzel AI custom models\n"
				<< "========================\n"
				<< "  agents\\  — player/agent packs\n"
				<< "  knives\\  — knife mesh packs\n"
				<< "Use the matching Refresh button in the menu.\n"
				<< "Legacy path lefrizzel_models\\ still loads if present.\n";
		}
	}
	WriteAgentDropReadme(fs::path(agents));
	WriteKnifeDropReadme(fs::path(knives));
}

int SyncDroppedAgentPacks() {
	EnsureLefrizzelModelsDir();
	const std::string dropStr = LefrizzelAgentsDir();
	const std::string csgoStr = CsgoRoot();
	if (dropStr.empty() || csgoStr.empty()) return 0;
	return SyncDropFolder(fs::path(dropStr), fs::path(csgoStr), false);
}

int SyncDroppedKnifePacks() {
	EnsureLefrizzelModelsDir();
	const std::string dropStr = LefrizzelKnivesDir();
	const std::string csgoStr = CsgoRoot();
	if (dropStr.empty() || csgoStr.empty()) return 0;
	return SyncDropFolder(fs::path(dropStr), fs::path(csgoStr), true);
}

int SyncDroppedCustomPacks() {
	return SyncDroppedAgentPacks() + SyncDroppedKnifePacks();
}

std::string CsgoGameDir() {
	return CsgoRoot();
}

std::string InferCustomModelGamePath(const fs::path& vmdlC, bool preferKnives) {
	std::error_code ec;
	if (!fs::exists(vmdlC, ec) || !IsCompiledVmdlName(vmdlC))
		return {};

	const std::string stem = CompiledVmdlStem(vmdlC);
	if (stem.empty())
		return {};

	// Prefer path relative to the matching drop when pack mirrors game layout
	const std::string dropStr = preferKnives ? LefrizzelKnivesDir() : LefrizzelAgentsDir();
	if (!dropStr.empty()) {
		const fs::path drop(dropStr);
		const fs::path rel = fs::relative(vmdlC, drop, ec);
		if (!ec) {
			std::string r = rel.generic_string();
			std::replace(r.begin(), r.end(), '\\', '/');
			if (r.rfind("characters/", 0) == 0
				|| r.rfind("models/", 0) == 0
				|| r.rfind("weapons/", 0) == 0) {
				if (PathEndsWithI(r, ".vmdl_c")) {
					r.resize(r.size() - 7);
					r += ".vmdl";
					return r;
				}
			}
		}
		ec.clear();
	}

	// Baked path from embedded refs
	const std::string data = ReadFileBin(vmdlC);
	if (!data.empty()) {
		std::string baked = InferBakedDir(vmdlC, data);
		const bool validRoot =
			!baked.empty() &&
			(baked.rfind("characters/", 0) == 0
				|| baked.rfind("models/", 0) == 0
				|| baked.rfind("weapons/", 0) == 0);
		if (validRoot)
			return baked + "/" + stem + ".vmdl";
	}

	// Fallback: loose pack folder
	const fs::path packFolder = vmdlC.parent_path().filename();
	const std::string pack = packFolder.string();
	if (!pack.empty() && pack != "." && pack != ".."
		&& pack != "agents" && pack != "knives") {
		if (preferKnives)
			return std::string("weapons/models/knife/") + pack + "/" + stem + ".vmdl";
		return std::string("characters/models/") + pack + "/" + stem + ".vmdl";
	}

	return {};
}

bool CustomModelDeployed(const std::string& gameVmdlPath) {
	const std::string root = CsgoRoot();
	if (root.empty() || gameVmdlPath.empty())
		return false;
	std::string rel = gameVmdlPath;
	std::replace(rel.begin(), rel.end(), '\\', '/');
	if (PathEndsWithI(rel, ".vmdl")) {
		rel.resize(rel.size() - 5);
		rel += ".vmdl_c";
	} else if (!PathEndsWithI(rel, ".vmdl_c")) {
		return false;
	}
	std::string full = root + "\\" + rel;
	std::replace(full.begin(), full.end(), '/', '\\');
	std::error_code ec;
	return fs::exists(full, ec) && fs::is_regular_file(full, ec);
}

} // namespace SkinChanger
