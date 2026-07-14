#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace KnifeSkins {

struct PaintKitInfo {
	int id = 0;
	std::string name;
	std::string token;
	int rarity = 1;
};

// Full build for all knives (optional). Prefer EnsureKnife for menu safety.
bool EnsureReady(void* itemSchema, void* (*findItemDef)(void* schema, int defIdx, char allowDefault));

// Lazy build for one knife def-index only (called from menu).
bool EnsureKnife(std::uint16_t defIndex, void* itemSchema,
	void* (*findItemDef)(void* schema, int defIdx, char allowDefault));

bool Ready();

const std::vector<PaintKitInfo>& KitsFor(std::uint16_t defIndex);
int KitIdAt(std::uint16_t defIndex, int listIndex);
bool UsesOldModel(void* itemSchema, int paintKitId);

// Item schema simple name used for panorama image paths (e.g. "weapon_ak47")
const char* SimpleNameFor(std::uint16_t defIndex);

} // namespace KnifeSkins
