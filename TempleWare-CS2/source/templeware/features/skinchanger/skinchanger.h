#pragma once
#include <cstdint>
#include <string>
#include <vector>

class C_CSPlayerPawn;
class C_CSWeaponBase;
class CUserCmd;

namespace SkinChanger {

struct KnifeInfo {
	const char* display;
	const char* subclass; // e.g. "weapon_knife_karambit" (display/debug only)
	std::uint16_t def_index;
	const char* model_path; // vmdl for set_model; null for Default
};

struct GloveInfo {
	const char* display;
	std::uint16_t def_index;
};

struct WeaponMenuInfo {
	const char* display;
	std::uint16_t def_index;
	int category; // 0 Pistols | 1 Heavy | 2 SMGs | 3 Rifles | 4 Snipers
};

const char* WeaponCategoryName(int category);

struct KnifePaintKit {
	int id;
	std::string name;
	std::string token; // paint kit name for panorama path
	int rarity;
};

// Schema simple weapon name for preview images (e.g. "weapon_ak47")
const char* SimpleNameFor(std::uint16_t defIndex);
// Warm paint-kit + simple-name cache for any def (knife/glove/weapon)
void PrefetchDef(std::uint16_t defIndex);

const KnifeInfo* Knives();
int KnifeCount();

const GloveInfo* Gloves();
int GloveCount();
bool GloveSkinsReady();
const std::vector<KnifePaintKit>& GlovePaintKits();
int GlovePaintKitId(int listIndex);

const WeaponMenuInfo* WeaponMenu();
int WeaponMenuCount();
bool WeaponSkinsReady(std::uint16_t def);
const std::vector<KnifePaintKit>& WeaponPaintKits(std::uint16_t def);

// Schema + filesystem filtered kits for current Config::knife_index knife
bool KnifeSkinsReady();
const std::vector<KnifePaintKit>& KnifePaintKits();
int KnifePaintKitId(int listIndex);

bool Init();
bool Ready();

// Primary apply path — FRAME_NET_UPDATE_END (stage 7, matches working ttapp)
void OnFrameStage(C_CSPlayerPawn* local, int stage);

// CreateMove: force switch-away/back so engine rebuilds HUD mid-round
void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd);

// FireEventClientSide — rewrite player_death weapon for killfeed knife icon
void OnFireEvent(void* gameEvent);

void Invalidate();
// After ConfigManager::Load — clamp indices, resync paint list from kit ids, force re-apply
void OnConfigLoaded();
// Paint / wear / seed / nametag — refreshHud=true soft-reequips so viewmodel rebuilds
void InvalidateSkin(bool refreshHud = false);
void InvalidateGloves();
void InvalidateWeapons();

// Killfeed helpers resolved at Init (nerv patterns)
bool InitKillfeed();

// Called from skinchanger.cpp OnFrameStage / FireEvent
void RunGloveWeapon(C_CSPlayerPawn* local);
void OnRoundStartGloves();

} // namespace SkinChanger

// Internal bridge used by glove_weapon.cpp (defined in skinchanger.cpp)
namespace SkinChanger {
namespace Internal {
	bool SchemaReady();
	void* ItemViewFromWeapon(void* weapon);
	void AttrApplyItem(void* itemView, int paintKit, float wear, int seed);
	void AttrClearItem(void* itemView);
	void AttrWipeItem(void* itemView); // nerv remove — zero attr vec then recreate
	void MeshWeapon(void* weapon, std::uint64_t mask);
	void MeshHudWeapon(void* weapon, C_CSPlayerPawn* local, std::uint64_t mask);
	void CompositeWeapon(void* weapon);
	void SkinWeapon(void* weapon);
	void WeaponData(void* weapon);
	void InvalidateItemCache(void* itemView);
	void SyncConstructedPaintKit(void* itemView, int paintKitId);
	void BustItemCache(void* itemView, C_CSPlayerPawn* local, std::uint16_t defIndex, int paintKitId, int seed);
	void RegenerateSkins();
	void ClearHudIconFor(void* weapon);
	void* ItemSchema();
	void* (*FindItemDefFn())(void*, int, char);
	std::uint16_t DefIndexOf(void* weapon);
	std::uint16_t DefIndexOfItem(void* itemView);
	bool IsInitialized(void* itemView);
	void SetDefIndex(void* itemView, std::uint16_t def);
	void SetQualityUnusual(void* itemView);
	void SetItemIdsMax(void* itemView);
	void SetItemIdHigh(void* itemView, std::uint32_t v);
	void SetInitialized(void* itemView, bool v);
	void SetRestoreMat(void* itemView, bool v);
	void SetDisallowSOC(void* itemView, bool v);
	void SpoofGloveIdentity(void* itemView, C_CSPlayerPawn* local);
	void SetPaintFallback(void* weapon, int paint, float wear, int seed);
	bool CollectWeapons(C_CSPlayerPawn* local, C_CSWeaponBase** out, int maxOut, int& count);
	void SetBodyGroupPawn(C_CSPlayerPawn* pawn);
	void SetPawnModel(void* entity, const char* modelPath);
	void* EconGloves(C_CSPlayerPawn* pawn);
	bool* NeedReapplyGloves(C_CSPlayerPawn* pawn);
	float LastSpawnTime(C_CSPlayerPawn* pawn);
	std::uint64_t LocalSteamId(C_CSPlayerPawn* local);
	std::uint64_t OriginalOwnerXuid(void* weapon);
	bool InitGloveWeaponFns();
}
}

