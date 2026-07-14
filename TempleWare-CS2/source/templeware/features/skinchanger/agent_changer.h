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

bool AgentsReady();
// All scanned agents (Default entry at index 0 with def 0)
const std::vector<AgentInfo>& Agents();
// Filtered view: team 2 or 3; includes Default as first entry
std::vector<AgentInfo> AgentsForTeam(int team);

void InvalidateAgents();
void RunAgents(C_CSPlayerPawn* local);

} // namespace SkinChanger
