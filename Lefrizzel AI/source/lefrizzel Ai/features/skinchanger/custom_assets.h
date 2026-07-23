#pragma once
#include <filesystem>
#include <string>

namespace SkinChanger {

// Drop folders under <csgo>/lefrizzel_models/
//   agents/  — custom player/agent packs
//   knives/  — custom knife mesh packs
// Refresh deploys to:
//   csgo/characters/... , csgo/models/... , csgo/weapons/...
// Legacy csgo/lefrizzel_models/ still accepted if new folder empty.
std::string LefrizzelModelsDir();
std::string LefrizzelAgentsDir();
std::string LefrizzelKnivesDir();
void EnsureLefrizzelModelsDir();

// Sync packs from drop folders. Returns packs deployed / updated.
int SyncDroppedAgentPacks();
int SyncDroppedKnifePacks();
// Agents + knives
int SyncDroppedCustomPacks();

// Game-relative path (characters|models|weapons/.../name.vmdl) for a
// drop-folder .vmdl_c. Empty if inference fails.
// preferKnives=true: look under lefrizzel_models/knives first.
std::string InferCustomModelGamePath(const std::filesystem::path& vmdlC,
	bool preferKnives = false);
// True if compiled .vmdl_c exists under csgo for that game-relative .vmdl path.
bool CustomModelDeployed(const std::string& gameVmdlPath);
// <csgo> content root (parent of lefrizzel_models).
std::string CsgoGameDir();

} // namespace SkinChanger
