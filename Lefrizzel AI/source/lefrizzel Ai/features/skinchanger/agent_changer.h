#pragma once
#include <cstdint>
#include <string>
#include <vector>

class C_CSPlayerPawn;

namespace SkinChanger {

struct AgentInfo {
	std::uint16_t def_index = 0;
	int team = 0; // 2 = T, 3 = CT, 0 = unknown
	std::string display;
	std::string model_path;
	std::string icon_path;      // preferred panorama vtex_c
	std::string icon_fallback;  // stripped-_variant path if primary missing
};

// Disk .vmdl_c under csgo/characters/models or csgo/models (custom packs)
struct CustomModelInfo {
	std::string display;     // pack-relative stem (or "[wrong path] ...")
	std::string model_path;  // characters/models/... or models/... .vmdl
};

bool AgentsReady();
// All scanned agents (Default entry at index 0 with def 0)
const std::vector<AgentInfo>& Agents();
// Filtered view: team 2 or 3; includes Default as first entry
std::vector<AgentInfo> AgentsForTeam(int team);

void InvalidateAgents();
void RunAgents(C_CSPlayerPawn* local);

// Custom model list (disk scan)
bool CustomModelsReady();
const std::vector<CustomModelInfo>& CustomModels();
void RefreshCustomModels();
// Index into CustomModels() for Config::custom_model_path, or -1
int CustomModelIndexOf(const char* path);

} // namespace SkinChanger
