#pragma once
#include <string>
#include <vector>

class C_CSPlayerPawn;
class C_CSWeaponBase;

namespace SkinChanger {

// Custom knife mesh packs under csgo/lefrizzel_models/knives/
struct CustomKnifeInfo {
	std::string display;    // pack stem / relative path
	std::string model_path; // weapons/... or models/... .vmdl
	int stock_index = 0;    // Knives() index to bind VData/anims (0 = unknown)
};

bool CustomKnivesReady();
const std::vector<CustomKnifeInfo>& CustomKnives();
void RefreshCustomKnives();
int CustomKnifeIndexOf(const char* path);
void InvalidateCustomKnife();

// Infer stock knife index (Knives[]) from custom path/display. 0 if unknown.
int InferStockKnifeIndex(const char* modelPath, const char* display = nullptr);
// Set Config::knife_index (+ enable knife_changer) from custom path. Returns stock idx.
int AutoSelectStockKnifeForCustom(const char* modelPath, const char* display = nullptr);

// Call from ProcessKnife after stock model resolve: override path if enabled.
// Returns custom path or nullptr (use stock).
const char* ActiveCustomKnifeModel();

// FRAME_RENDER_END re-apply while loadout stomps custom mesh.
void RunCustomKnife(C_CSPlayerPawn* local);

} // namespace SkinChanger
