// Glove + weapon skins — nerv apply path (New folder (2)), offsets IDA-verified.
#include "skinchanger.h"
#include "knife_skins.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/vfunc/vfunc.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/handle.h"

#include <Windows.h>
#include <cstring>
#include <vector>

namespace SkinChanger {
namespace {

constexpr GloveInfo kGloves[] = {
	{ "Default",           0 },
	{ "Bloodhound",     5027 },
	{ "Sport",          5030 },
	{ "Driver",         5031 },
	{ "Hand Wraps",     5032 },
	{ "Moto",           5033 },
	{ "Specialist",     5034 },
	{ "Hydra",          5035 },
	{ "Broken Fang",    4725 },
};

constexpr int kGloveCount = (int)(sizeof(kGloves) / sizeof(kGloves[0]));

constexpr WeaponMenuInfo kWeaponMenu[] = {
	// Pistols
	{ "Glock-18", 4, 0 }, { "USP-S", 61, 0 }, { "P2000", 32, 0 }, { "Dual Berettas", 2, 0 },
	{ "P250", 36, 0 }, { "Tec-9", 30, 0 }, { "Five-SeveN", 3, 0 }, { "CZ75", 63, 0 },
	{ "Desert Eagle", 1, 0 }, { "R8 Revolver", 64, 0 },
	// Heavy
	{ "Nova", 35, 1 }, { "XM1014", 25, 1 }, { "MAG-7", 27, 1 }, { "Sawed-Off", 29, 1 },
	{ "M249", 14, 1 }, { "Negev", 28, 1 },
	// SMGs
	{ "MP9", 34, 2 }, { "MAC-10", 17, 2 }, { "MP7", 33, 2 }, { "UMP-45", 24, 2 },
	{ "P90", 19, 2 }, { "PP-Bizon", 26, 2 }, { "MP5-SD", 23, 2 },
	// Rifles
	{ "Galil AR", 13, 3 }, { "FAMAS", 10, 3 }, { "AK-47", 7, 3 }, { "M4A4", 16, 3 },
	{ "M4A1-S", 60, 3 }, { "AUG", 8, 3 }, { "SG 553", 39, 3 },
	// Snipers
	{ "SSG 08", 40, 4 }, { "AWP", 9, 4 }, { "G3SG1", 11, 4 }, { "SCAR-20", 38, 4 },
};

constexpr int kWeaponMenuCount = (int)(sizeof(kWeaponMenu) / sizeof(kWeaponMenu[0]));

// --- Glove state (nerv: clear 2 / update 4) ---
static bool  g_gloveShouldUpdate = false;
static int   g_gloveFrames = 0;
static int   g_gloveClear = 0;
static int   g_lastGloveDef = 0;
static int   g_lastGlovePaint = -1;
static float g_lastGloveWear = -1.f;
static int   g_lastGloveSeed = -1;
static int   g_lastGloveTeam = 0;
static float g_lastGloveSpawn = 0.f;

// --- Weapon state (nerv: update 6 on change) ---
static bool  g_weaponShouldUpdate = false;
static int   g_weaponFrames = 0;
static int   g_lastWeaponTeam = 0;
static float g_lastWeaponSpawn = 0.f;
static std::vector<std::uint16_t> g_lastWeaponDefs;

static void ProcessGloves(C_CSPlayerPawn* local)
{
	using namespace Internal;
	if (!local || !SchemaReady())
		return;
	if (local->getHealth() <= 0)
		return;

	if (!Config::glove_changer || Config::glove_index <= 0 || Config::glove_index >= kGloveCount) {
		g_gloveFrames = 0;
		g_gloveShouldUpdate = false;
		return;
	}

	void* gloveItem = EconGloves(local);
	if (!gloveItem || !Mem::IsUserPtr(gloveItem))
		return;

	const std::uint16_t selected = kGloves[Config::glove_index].def_index;
	if (!selected)
		return;

	int paintId = Config::glove_paint_kit_id;
	if (paintId <= 0)
		paintId = KnifeSkins::KitIdAt(selected, Config::glove_paint_kit);

	const float spawnTime = LastSpawnTime(local);
	const int team = (int)local->m_iTeamNum();

	std::uint16_t curDef = 0;
	bool initialized = false;
	__try {
		curDef = DefIndexOfItem(gloveItem);
		initialized = IsInitialized(gloveItem);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return; }

	bool* needRe = NeedReapplyGloves(local);
	const bool cfgChanged =
		(g_lastGloveDef != (int)selected) ||
		(g_lastGlovePaint != paintId) ||
		(g_lastGloveWear != Config::glove_wear) ||
		(g_lastGloveSeed != Config::glove_seed);
	const bool teamChanged = (team != g_lastGloveTeam) && g_lastGloveTeam != 0;
	const bool spawnChanged = (spawnTime != g_lastGloveSpawn);
	const bool engineReset = (curDef != selected) || !initialized || (needRe && *needRe);

	if (teamChanged)
		g_gloveClear = 2;
	if (cfgChanged || teamChanged || spawnChanged || engineReset || g_gloveShouldUpdate)
		g_gloveFrames = 4;

	if (g_gloveClear > 0) {
		__try {
			AttrWipeItem(gloveItem);
			SetDefIndex(gloveItem, 0);
			SetInitialized(gloveItem, false);
			if (needRe) *needRe = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
		--g_gloveClear;
		g_lastGloveDef = 0;
		g_lastGlovePaint = 0;
		g_lastGloveTeam = team;
		g_gloveShouldUpdate = false;
		return;
	}

	if (g_gloveFrames <= 0) {
		g_gloveShouldUpdate = false;
		return;
	}

	__try {
		SetDefIndex(gloveItem, selected);
		SetQualityUnusual(gloveItem);

		// nerv: wipe attrs then recreate paint/pattern/wear
		AttrWipeItem(gloveItem);
		if (paintId > 0)
			AttrApplyItem(gloveItem, paintId, Config::glove_wear, Config::glove_seed);

		// nerv identity spoof — keep low/full id, force high + account
		SpoofGloveIdentity(gloveItem, local);
		SetDisallowSOC(gloveItem, false);
		SetRestoreMat(gloveItem, true);
		SetInitialized(gloveItem, true);

		SetBodyGroupPawn(local);
		if (needRe) *needRe = true;

		g_lastGloveDef = selected;
		g_lastGlovePaint = paintId;
		g_lastGloveWear = Config::glove_wear;
		g_lastGloveSeed = Config::glove_seed;
		g_lastGloveSpawn = spawnTime;
		g_lastGloveTeam = team;
		--g_gloveFrames;
		g_gloveShouldUpdate = false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_gloveFrames = 0;
		g_gloveShouldUpdate = false;
	}
}

static void ApplyWeaponSkin(C_CSPlayerPawn* local, C_CSWeaponBase* w, void* item,
	int paintId, float wear, int seed)
{
	using namespace Internal;
	__try {
		// skichanger apply_skin order:
		// remove attrs → idHigh only → init flags → paint → create attrs → composite
		// → mesh(old?2:1) → skin → weapon_data
		AttrWipeItem(item);
		BustItemCache(item, local, DefIndexOf(w), paintId, seed);
		SetInitialized(item, true);
		SetDisallowSOC(item, false);
		SetRestoreMat(item, true);

		SetPaintFallback(w, paintId, wear, seed);
		AttrApplyItem(item, paintId, wear, seed);
		CompositeWeapon(w);

		const bool oldModel = KnifeSkins::UsesOldModel(ItemSchema(), paintId);
		const std::uint64_t mask = oldModel ? 2ull : 1ull;
		MeshWeapon(w, mask);
		MeshHudWeapon(w, local, mask);
		SkinWeapon(w);
		WeaponData(w);
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ProcessWeapons(C_CSPlayerPawn* local)
{
	using namespace Internal;
	if (!local || !SchemaReady() || !Config::weapon_skins)
		return;
	if (local->getHealth() <= 0)
		return;

	const float spawnTime = LastSpawnTime(local);
	const int team = (int)local->m_iTeamNum();
	const std::uint64_t steamId = LocalSteamId(local);

	const bool teamChanged = (team != g_lastWeaponTeam) && g_lastWeaponTeam != 0;
	const bool spawnChanged = (spawnTime != g_lastWeaponSpawn) && g_lastWeaponSpawn != 0.f;

	C_CSWeaponBase* weapons[64]{};
	int count = 0;
	std::uint16_t curDefs[64]{};
	int curDefCount = 0;
	if (CollectWeapons(local, weapons, 64, count)) {
		for (int i = 0; i < count; ++i) {
			const std::uint16_t d = DefIndexOf(weapons[i]);
			if (d && curDefCount < 64)
				curDefs[curDefCount++] = d;
		}
	}

	bool weaponChanged = (curDefCount != (int)g_lastWeaponDefs.size());
	if (!weaponChanged) {
		for (int i = 0; i < curDefCount; ++i) {
			if (g_lastWeaponDefs[i] != curDefs[i]) {
				weaponChanged = true;
				break;
			}
		}
	}
	g_lastWeaponDefs.assign(curDefs, curDefs + curDefCount);

	if (teamChanged || spawnChanged || weaponChanged || g_weaponShouldUpdate) {
		g_weaponFrames = 6;
		g_weaponShouldUpdate = false;
	}

	if (g_weaponFrames <= 0) {
		g_lastWeaponSpawn = spawnTime;
		g_lastWeaponTeam = team;
		return;
	}

	bool didUpdate = false;
	for (int i = 0; i < count; ++i) {
		C_CSWeaponBase* w = weapons[i];
		if (!w) continue;

		// Skip other players' dropped guns. XUID 0 = default loadout — still apply.
		if (steamId) {
			const std::uint64_t ox = OriginalOwnerXuid(w);
			if (ox != 0 && ox != steamId)
				continue;
		}

		const std::uint16_t def = DefIndexOf(w);
		if (def < 1 || def > 70) continue;

		const auto& skin = Config::weapon_skin[def];
		int paintId = skin.paint_kit_id;
		if (paintId <= 0 && skin.paint_kit > 0)
			paintId = KnifeSkins::KitIdAt(def, skin.paint_kit);
		if (paintId <= 0)
			continue;

		void* item = ItemViewFromWeapon(w);
		if (!item) continue;

		const float wear = skin.wear > 0.f ? skin.wear : 0.01f;
		ApplyWeaponSkin(local, w, item, paintId, wear, skin.seed);
		ClearHudIconFor(w);
		didUpdate = true;
	}

	if (didUpdate)
		RegenerateSkins();

	g_lastWeaponSpawn = spawnTime;
	g_lastWeaponTeam = team;
	--g_weaponFrames;
}

} // namespace

const char* WeaponCategoryName(int category) {
	static const char* names[] = { "Pistols", "Heavy", "SMGs", "Rifles", "Snipers" };
	if (category < 0 || category > 4) return "Other";
	return names[category];
}

const GloveInfo* Gloves() { return kGloves; }
int GloveCount() { return kGloveCount; }

bool GloveSkinsReady() {
	if (Config::glove_index <= 0 || Config::glove_index >= kGloveCount) return false;
	const auto def = kGloves[Config::glove_index].def_index;
	return !KnifeSkins::KitsFor(def).empty();
}

const std::vector<KnifePaintKit>& GlovePaintKits() {
	static std::vector<KnifePaintKit> empty;
	if (Config::glove_index <= 0 || Config::glove_index >= kGloveCount) return empty;
	const auto def = kGloves[Config::glove_index].def_index;
	KnifeSkins::EnsureKnife(def, Internal::ItemSchema(), Internal::FindItemDefFn());
	const auto& src = KnifeSkins::KitsFor(def);
	static std::vector<KnifePaintKit> out;
	out.clear();
	out.reserve(src.size());
	for (const auto& k : src)
		out.push_back({ k.id, k.name, k.token, k.rarity });
	return out;
}

int GlovePaintKitId(int listIndex) {
	if (Config::glove_index <= 0 || Config::glove_index >= kGloveCount) return 0;
	return KnifeSkins::KitIdAt(kGloves[Config::glove_index].def_index, listIndex);
}

const WeaponMenuInfo* WeaponMenu() { return kWeaponMenu; }
int WeaponMenuCount() { return kWeaponMenuCount; }

bool WeaponSkinsReady(std::uint16_t def) {
	return def >= 1 && def <= 70 && !KnifeSkins::KitsFor(def).empty();
}

const std::vector<KnifePaintKit>& WeaponPaintKits(std::uint16_t def) {
	static std::vector<KnifePaintKit> empty;
	if (def < 1 || def > 70) return empty;
	KnifeSkins::EnsureKnife(def, Internal::ItemSchema(), Internal::FindItemDefFn());
	const auto& src = KnifeSkins::KitsFor(def);
	static std::vector<KnifePaintKit> out;
	out.clear();
	out.reserve(src.size());
	for (const auto& k : src)
		out.push_back({ k.id, k.name, k.token, k.rarity });
	return out;
}

void RunGloveWeapon(C_CSPlayerPawn* local) {
	ProcessGloves(local);
	ProcessWeapons(local);
}

void OnRoundStartGloves() {
	g_gloveShouldUpdate = true;
	g_gloveFrames = 4;
	g_weaponShouldUpdate = true;
	g_weaponFrames = 6;
}

void InvalidateGloves() {
	g_lastGloveDef = 0;
	g_lastGlovePaint = -1;
	g_lastGloveWear = -1.f;
	g_lastGloveSeed = -1;
	g_gloveClear = 2;
	g_gloveFrames = 4;
	g_gloveShouldUpdate = true;
}

void InvalidateWeapons() {
	g_weaponShouldUpdate = true;
	g_weaponFrames = 6;
}

} // namespace SkinChanger
