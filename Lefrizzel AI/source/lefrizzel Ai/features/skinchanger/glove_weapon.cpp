// Glove + weapon skins — nerv apply path (New folder (2)), offsets IDA-verified.
#include "skinchanger.h"
#include "knife_skins.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../sdk_prio_a/sdk_prio_a.h"
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
#include <algorithm>
#include <cmath>
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
// Competitive loadout lands late after round_start / freezetime — long window,
// and never burn frames while inventory empty (was main "skins gone after round").
static bool  g_weaponShouldUpdate = false;
static int   g_weaponFrames = 0;
static int   g_lastWeaponTeam = 0;
static float g_lastWeaponSpawn = 0.f;
static std::vector<std::uint16_t> g_lastWeaponDefs;
// Detect paint/seed/wear/custom-color config drift (defs alone miss seed-only edits)
static int   g_lastAppliedPaint[71]{};
static int   g_lastAppliedSeed[71]{};
static float g_lastAppliedWear[71]{};
static bool  g_lastAppliedCustom[71]{};
static uint32_t g_lastAppliedColorHash[71]{};
// Force full material rebuild even when m_nFallbackPaintKit still matches
static bool  g_weaponForceFull = false;
static int   g_weaponSustain = 0; // soft composite refresh after hard apply
static int   g_weaponRegenCooldown = 0; // throttle global regenerate_weapon_skins
// HUD strip only after menu skin pick — NEVER on round/spawn/buy reapply
static bool  g_weaponMenuHud = false;
// Debounce round_start / freeze_end / buytime triple-fire
static int   g_roundOpenCooldown = 0;
// Stay open across death→respawn until inventory painted once (TDM / late give).
static bool  g_pendingSpawnApply = false;

static void ProcessGloves(C_CSPlayerPawn* local)
{
	using namespace Internal;
	if (!local || !SchemaReady())
		return;
	if (local->getHealth() <= 0)
		return;
	// Dead/respawn: glove item on free'd pawn — skip until services live
	if (!local->GetWeaponServices())
		return;

	if (!Config::glove_changer || Config::glove_index <= 0 || Config::glove_index >= kGloveCount) {
		// config load / feature off: still run clear if InvalidateGloves scheduled it
		if (g_gloveClear > 0) {
			void* gloveItem = EconGloves(local);
			if (gloveItem && Mem::IsUserPtr(gloveItem)) {
				__try {
					AttrWipeItem(gloveItem);
					SetDefIndex(gloveItem, 0);
					SetInitialized(gloveItem, false);
					bool* needRe = NeedReapplyGloves(local);
					if (needRe) *needRe = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			--g_gloveClear;
			g_lastGloveDef = 0;
			g_lastGlovePaint = 0;
		}
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
	static bool s_lastGloveCustomColor = false;
	static uint32_t s_lastGloveColorHash = 0;
	uint32_t gloveColorHash = 0;
	if (Config::glove_custom_color && Config::glove_colors_active) {
		for (int i = 0; i < 16; ++i)
			gloveColorHash = gloveColorHash * 16777619u ^ (uint32_t)(Config::glove_colors[i] * 255.f);
	}
	const bool gloveColorChanged =
		(s_lastGloveCustomColor != Config::glove_custom_color) ||
		(Config::glove_custom_color && gloveColorHash != s_lastGloveColorHash);
	const bool cfgChanged =
		(g_lastGloveDef != (int)selected) ||
		(g_lastGlovePaint != paintId) ||
		(g_lastGloveWear != Config::glove_wear) ||
		(g_lastGloveSeed != Config::glove_seed) ||
		gloveColorChanged;
	s_lastGloveCustomColor = Config::glove_custom_color;
	s_lastGloveColorHash = gloveColorHash;
	const bool teamChanged = (team != g_lastGloveTeam) && g_lastGloveTeam != 0;
	const bool spawnChanged = (spawnTime != g_lastGloveSpawn);
	const bool engineReset = (curDef != selected) || !initialized || (needRe && *needRe);

	if (teamChanged)
		g_gloveClear = 2;
	if (cfgChanged || teamChanged || spawnChanged || engineReset || g_gloveShouldUpdate) {
		// Competitive late loadout — longer than nerv 4
		if (g_gloveFrames < 16)
			g_gloveFrames = 16;
	}

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

		// Priority A: engine glove composite refresh if dump pattern resolved
		if (auto fn = SdkPrioA::GloveApplyPerTick()) {
			__try { fn(local); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_gloveFrames = 0;
		g_gloveShouldUpdate = false;
	}
}

// nerv apply_skin path (New folder (5)/skin_changer.cpp):
//   wipe attrs → idHigh → flags → fallbacks → create attrs → composite →
//   mesh (weapon: old?2:1) → skin → weapon_data
// Knife mesh is inverted (old?1:2) — ProcessKnife handles that.
// Always fullWipe attrs for this itemView so knife/pistol never share a paint block.
static void ApplyWeaponSkin(C_CSPlayerPawn* local, C_CSWeaponBase* w, void* item,
	int paintId, float wear, int seed, bool fullWipe, bool refreshHud)
{
	using namespace Internal;
	if (!w || !item || !Mem::ValidEntity(w) || !Mem::IsUserPtr(item))
		return;
	// Respawn half-init: no scene node → composite/mesh VMT crashes
	__try {
		CGameSceneNode* node = ((C_BaseEntity*)w)->m_pGameSceneNode();
		if (!node || !Mem::IsUserPtr(node))
			return;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	__try {
		// Always wipe THIS weapon's attrs first. Soft in-place update across
		// entities caused knife kit to land on pistol after respawn.
		(void)fullWipe;
		AttrWipeItem(item);

		const std::uint16_t def = DefIndexOf(w);
		// Never paint guns with knife def path (def 0 / knife slot leakage)
		if (def < 1 || def > 70)
			return;
		if (def == 42 || def == 59 || (def >= 500 && def <= 526))
			return;

		SetQualityUnusual(item);
		BustItemCache(item, local, def, paintId, seed);
		SetInitialized(item, true);
		SetDisallowSOC(item, false);
		SetRestoreMat(item, true);

		SetPaintFallback(w, paintId, wear, seed);
		AttrApplyItem(item, paintId, wear, seed);
		SyncConstructedPaintKit(item, paintId);

		// nerv: update_composite(true) then mesh — weapon mask is INVERTED vs knife
		CompositeWeapon(w);
		const bool oldModel = KnifeSkins::UsesOldModel(ItemSchema(), paintId);
		const std::uint64_t mask = oldModel ? 2ull : 1ull;
		MeshWeapon(w, mask);
		// FP HUD for THIS gun only (+0x1760) — never arms walk (knife bleed)
		MeshHudWeapon(w, local, mask);

		SkinWeapon(w);
		WeaponData(w);
		// modern paint path after legacy composite (verified_features)
		ApplyEconOnWeapon(w);
		ApplyWeaponItemName(item, def, paintId);
		if (refreshHud)
			ClearHudIconFor(w);
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ClearWeaponAppliedCache() {
	std::memset(g_lastAppliedPaint, 0, sizeof(g_lastAppliedPaint));
	std::memset(g_lastAppliedSeed, 0, sizeof(g_lastAppliedSeed));
	std::memset(g_lastAppliedWear, 0, sizeof(g_lastAppliedWear));
	std::memset(g_lastAppliedCustom, 0, sizeof(g_lastAppliedCustom));
	std::memset(g_lastAppliedColorHash, 0, sizeof(g_lastAppliedColorHash));
}

static uint32_t HashWeaponColors(const Config::WeaponSkinSlot& s) {
	if (!s.custom_color || !s.colors_active)
		return 0;
	uint32_t h = 2166136261u;
	for (int i = 0; i < 16; ++i)
		h = (h ^ (uint32_t)(s.colors[i] * 255.f)) * 16777619u;
	return h;
}

// forceFull: wipe applied cache + hard material path.
// menuHud: allow ClearHud + RegenerateSkins once (menu only).
static void OpenWeaponWindow(int frames, bool forceFull, bool menuHud = false) {
	if (frames > g_weaponFrames)
		g_weaponFrames = frames;
	if (forceFull) {
		g_weaponForceFull = true;
		ClearWeaponAppliedCache();
	}
	if (menuHud)
		g_weaponMenuHud = true;
	g_weaponShouldUpdate = false;
	// Late loadout hold — short; do not re-composite forever
	if (forceFull && g_weaponSustain < 24)
		g_weaponSustain = 24;
}

// Buy / new def in inventory: only invalidate those slots (keep others)
static void InvalidateDefsNotIn(const std::vector<std::uint16_t>& sortedLast,
	const std::uint16_t* cur, int curN)
{
	for (int i = 0; i < curN; ++i) {
		const std::uint16_t d = cur[i];
		if (d < 1 || d > 70) continue;
		bool known = false;
		for (std::uint16_t old : sortedLast) {
			if (old == d) { known = true; break; }
		}
		if (!known) {
			g_lastAppliedPaint[d] = 0;
			g_lastAppliedSeed[d] = -1;
			g_lastAppliedWear[d] = -1.f;
		}
	}
}

static void ProcessWeapons(C_CSPlayerPawn* local)
{
	using namespace Internal;
	if (!local || !SchemaReady())
		return;
	if (!Config::weapon_skins) {
		g_weaponShouldUpdate = false;
		g_weaponFrames = 0;
		g_weaponSustain = 0;
		g_weaponForceFull = false;
		return;
	}
	if (local->getHealth() <= 0)
		return;

	const float spawnTime = LastSpawnTime(local);
	const int team = (int)local->m_iTeamNum();
	const std::uint64_t steamId = LocalSteamId(local);

	const bool teamChanged = (team != g_lastWeaponTeam) && g_lastWeaponTeam != 0;
	// First spawn + every respawn (spawn time changes when pawn re-enters round).
	const bool spawnChanged = (spawnTime != g_lastWeaponSpawn);

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

	// Multiset compare (sorted) — inventory handle order can shuffle every tick
	std::vector<std::uint16_t> sortedCur(curDefs, curDefs + curDefCount);
	std::sort(sortedCur.begin(), sortedCur.end());
	std::vector<std::uint16_t> sortedLast = g_lastWeaponDefs;
	std::sort(sortedLast.begin(), sortedLast.end());
	const bool weaponChanged = (sortedCur != sortedLast);
	// Detect inventory empty→non-empty (buy / late competitive give)
	const bool inventoryAppeared =
		(curDefCount > 0) && g_lastWeaponDefs.empty();
	// New gun bought mid-round — invalidate only new defs so paint reapplies
	if (weaponChanged && curDefCount > 0)
		InvalidateDefsNotIn(sortedLast, curDefs, curDefCount);
	g_lastWeaponDefs.assign(curDefs, curDefs + curDefCount);

	// Resolve wanted paint per def once.
	// ONLY paint_kit_id applies. List index alone maps to wrong kit when kit list
	// rebuilds / FS-filter changes order (looked like "random skin on unselected").
	int wantPaint[71]{};
	int wantSeed[71]{};
	float wantWear[71]{};
	bool wantCustom[71]{};
	uint32_t wantColorHash[71]{};
	for (int i = 0; i < curDefCount; ++i) {
		const std::uint16_t d = curDefs[i];
		if (d < 1 || d > 70) continue;
		auto& s = Config::weapon_skin[d];
		int pid = s.paint_kit_id;
		// One-shot migrate: old configs stored list index only. Resolve once when
		// real kit list exists; never apply index while Vanilla-only placeholder.
		if (pid <= 0 && s.paint_kit > 0) {
			if (KnifeSkins::EnsureKnife(d, ItemSchema(), FindItemDefFn())
				&& KnifeSkins::KitsFor(d).size() > 1) {
				pid = KnifeSkins::KitIdAt(d, s.paint_kit);
				if (pid > 0)
					s.paint_kit_id = pid;
				else
					s.paint_kit = 0; // stale index → treat as no skin
			} else {
				pid = 0; // kits not ready — skip apply, retry next frame
			}
		}
		// Explicit vanilla / no selection
		if (pid <= 0) {
			s.paint_kit = 0;
			s.paint_kit_id = 0;
			wantPaint[d] = 0;
			continue;
		}
		wantPaint[d] = pid;
		wantSeed[d] = s.seed;
		wantWear[d] = s.wear > 0.f ? s.wear : 0.01f;
		wantCustom[d] = s.custom_color && s.colors_active;
		wantColorHash[d] = HashWeaponColors(s);
	}

	// Config paint/seed/wear/custom-color vs last applied
	bool configDrift = false;
	for (int i = 0; i < curDefCount && !configDrift; ++i) {
		const std::uint16_t d = curDefs[i];
		if (d < 1 || d > 70 || wantPaint[d] <= 0) continue;
		if (g_lastAppliedPaint[d] != wantPaint[d] || g_lastAppliedSeed[d] != wantSeed[d]
			|| std::fabs(g_lastAppliedWear[d] - wantWear[d]) > 1e-5f
			|| g_lastAppliedCustom[d] != wantCustom[d]
			|| g_lastAppliedColorHash[d] != wantColorHash[d])
			configDrift = true;
	}

	// Live engine paint vs wanted — competitive loadout reset after our window ends
	// was the main "skins gone on spawn / HUD flicker" bug.
	bool liveMismatch = false;
	for (int i = 0; i < count && !liveMismatch; ++i) {
		C_CSWeaponBase* w = weapons[i];
		if (!w) continue;
		const std::uint16_t def = DefIndexOf(w);
		if (def < 1 || def > 70 || wantPaint[def] <= 0) continue;
		if (steamId) {
			const std::uint64_t ox = OriginalOwnerXuid(w);
			if (ox != 0 && ox != steamId) continue;
		}
		const int live = GetPaintFallback(w);
		if (live != wantPaint[def])
			liveMismatch = true;
	}

	// Spawn/team: reapply paint — NO menu HUD flag.
	// Team swap often keeps same def multiset but new entities — wipe apply cache.
	if (spawnChanged || teamChanged) {
		if (teamChanged)
			g_lastWeaponDefs.clear();
		ClearWeaponAppliedCache();
		g_pendingSpawnApply = true;
		OpenWeaponWindow(64, true, false);
	}
	// Inventory just appeared after spawn — force hard reapply (new weapon entities)
	if (g_pendingSpawnApply && inventoryAppeared) {
		ClearWeaponAppliedCache();
		OpenWeaponWindow(48, true, false);
	}
	// Buy / live wipe / config: open apply window. Hard wipe only when paint wrong.
	if (weaponChanged || inventoryAppeared || configDrift || liveMismatch
		|| g_weaponShouldUpdate || g_pendingSpawnApply) {
		const bool hard = liveMismatch || inventoryAppeared || configDrift
			|| g_weaponShouldUpdate || g_pendingSpawnApply;
		OpenWeaponWindow(hard ? 32 : 16, hard, false);
	}

	// No inventory yet — hold window, do NOT burn frames (round_start before give).
	// Do NOT commit g_lastWeaponSpawn — spawn must stay "changed" until guns exist.
	if (count <= 0) {
		g_lastWeaponTeam = team;
		if (g_roundOpenCooldown > 0) --g_roundOpenCooldown;
		return;
	}

	// Always re-check live paint even outside window — buy can land after window ends
	if (g_weaponFrames <= 0 && g_weaponSustain <= 0) {
		if (liveMismatch || g_pendingSpawnApply) {
			OpenWeaponWindow(32, true, false);
		} else {
			// Only commit spawn when inventory present and paints match
			g_lastWeaponSpawn = spawnTime;
			g_lastWeaponTeam = team;
			g_weaponForceFull = false;
			if (g_roundOpenCooldown > 0) --g_roundOpenCooldown;
			return;
		}
	}

	const bool forceFull = g_weaponForceFull || spawnChanged || g_pendingSpawnApply;

	bool didUpdate = false;
	bool anyHudRefresh = false;
	bool anyWanted = false;
	bool allWantedOk = true;
	for (int i = 0; i < count; ++i) {
		C_CSWeaponBase* w = weapons[i];
		if (!w) continue;

		// Skip other players' dropped guns. XUID 0 = default / not yet stamped — still apply.
		// Never skip on pending spawn: XUID often wrong for first frames after respawn.
		if (steamId && !g_pendingSpawnApply) {
			const std::uint64_t ox = OriginalOwnerXuid(w);
			if (ox != 0 && ox != steamId)
				continue;
		}

		const std::uint16_t def = DefIndexOf(w);
		if (def < 1 || def > 70) continue;

		const int paintId = wantPaint[def];
		if (paintId <= 0)
			continue; // no user skin for this gun — leave engine loadout alone
		anyWanted = true;

		void* item = ItemViewFromWeapon(w);
		if (!item) {
			allWantedOk = false;
			continue;
		}

		const int live = GetPaintFallback(w);
		const bool paintChanged = (g_lastAppliedPaint[def] != paintId);
		const bool seedWearChanged =
			(g_lastAppliedSeed[def] != wantSeed[def])
			|| (std::fabs(g_lastAppliedWear[def] - wantWear[def]) > 1e-5f)
			|| (g_lastAppliedCustom[def] != wantCustom[def])
			|| (g_lastAppliedColorHash[def] != wantColorHash[def]);
		const bool alreadyApplied =
			live == paintId
			&& g_lastAppliedPaint[def] == paintId
			&& !seedWearChanged;

		// Already correct on this entity — skip (no HUD thrash)
		if (alreadyApplied && !forceFull)
			continue;

		// Engine wiped kit (live 0 or wrong) or menu/spawn force
		const bool fullWipe =
			forceFull
			|| paintChanged
			|| (live == 0)
			|| (live != paintId);

		// HUD ONLY when user picked skin in menu — never round/spawn/buy
		const bool refreshHud = g_weaponMenuHud && fullWipe;

		const float wear = wantWear[def];
		ApplyWeaponSkin(local, w, item, paintId, wear, wantSeed[def], fullWipe, refreshHud);
		g_lastAppliedPaint[def] = paintId;
		g_lastAppliedSeed[def] = wantSeed[def];
		g_lastAppliedWear[def] = wear;
		g_lastAppliedCustom[def] = wantCustom[def];
		g_lastAppliedColorHash[def] = wantColorHash[def];
		if (refreshHud)
			anyHudRefresh = true;
		didUpdate = true;

		// Verify write stuck (engine can race on spawn)
		if (GetPaintFallback(w) != paintId)
			allWantedOk = false;
	}

	// Spawn reapply done when every configured skin matches live paint
	if (g_pendingSpawnApply && anyWanted && allWantedOk && !liveMismatch) {
		// Confirm all wanted defs actually match
		bool ok = true;
		for (int i = 0; i < count && ok; ++i) {
			C_CSWeaponBase* w = weapons[i];
			if (!w) continue;
			const std::uint16_t def = DefIndexOf(w);
			if (def < 1 || def > 70 || wantPaint[def] <= 0) continue;
			if (GetPaintFallback(w) != wantPaint[def])
				ok = false;
		}
		if (ok)
			g_pendingSpawnApply = false;
	} else if (g_pendingSpawnApply && !anyWanted) {
		// No configured skins in inventory — nothing to wait for
		g_pendingSpawnApply = false;
	}

	// One strip rebuild per menu pick — never per round
	if (didUpdate && anyHudRefresh && g_weaponMenuHud) {
		if (g_weaponRegenCooldown <= 0) {
			RegenerateSkins();
			g_weaponRegenCooldown = 16;
		}
		g_weaponMenuHud = false;
		g_weaponForceFull = false;
	}
	if (g_weaponRegenCooldown > 0)
		--g_weaponRegenCooldown;
	if (g_roundOpenCooldown > 0)
		--g_roundOpenCooldown;

	// Commit spawn only after we had guns and (no pending OR paints ok)
	if (!g_pendingSpawnApply)
		g_lastWeaponSpawn = spawnTime;
	g_lastWeaponTeam = team;
	if (g_weaponFrames > 0)
		--g_weaponFrames;
	else if (g_weaponSustain > 0)
		--g_weaponSustain;
	// Keep forceFull while spawn pending so new entities always full-wipe
	if (g_weaponFrames <= 0 && !g_pendingSpawnApply)
		g_weaponForceFull = false;
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
	// Must call EnsureKnife — KitsFor alone never builds (stuck "Loading...").
	if (!KnifeSkins::EnsureKnife(def, Internal::ItemSchema(), Internal::FindItemDefFn()))
		return false;
	return KnifeSkins::KitsFor(def).size() > 1;
}

const std::vector<KnifePaintKit>& GlovePaintKits() {
	static std::vector<KnifePaintKit> out;
	static std::uint16_t s_def = 0;
	static size_t s_srcN = 0;
	if (Config::glove_index <= 0 || Config::glove_index >= kGloveCount) {
		if (s_def != 0 || out.size() != 1) {
			out.clear();
			out.push_back({ 0, "Vanilla", "Vanilla", 1 });
			s_def = 0;
			s_srcN = 1;
		}
		return out;
	}
	const auto def = kGloves[Config::glove_index].def_index;
	// Kits only — EnsureKnife owned by GloveSkinsReady (budgeted once/frame)
	const auto& src = KnifeSkins::KitsFor(def);
	if (def == s_def && src.size() == s_srcN && !out.empty())
		return out;
	out.clear();
	out.reserve(src.size() ? src.size() : 1);
	for (const auto& k : src)
		out.push_back({ k.id, k.name, k.token, k.rarity });
	if (out.empty())
		out.push_back({ 0, "Vanilla", "Vanilla", 1 });
	s_def = def;
	s_srcN = src.size() ? src.size() : 1;
	return out;
}

int GlovePaintKitId(int listIndex) {
	if (Config::glove_index <= 0 || Config::glove_index >= kGloveCount) return 0;
	return KnifeSkins::KitIdAt(kGloves[Config::glove_index].def_index, listIndex);
}

const WeaponMenuInfo* WeaponMenu() { return kWeaponMenu; }
int WeaponMenuCount() { return kWeaponMenuCount; }

bool WeaponSkinsReady(std::uint16_t def) {
	if (def < 1 || def > 70) return false;
	// Build on demand; Ready means real kits loaded (not just Vanilla placeholder).
	if (!KnifeSkins::EnsureKnife(def, Internal::ItemSchema(), Internal::FindItemDefFn()))
		return false;
	return KnifeSkins::KitsFor(def).size() > 1;
}

const std::vector<KnifePaintKit>& WeaponPaintKits(std::uint16_t def) {
	static std::vector<KnifePaintKit> out;
	static std::uint16_t s_def = 0;
	static size_t s_srcN = 0;
	if (def < 1 || def > 70) {
		if (s_def != 0 || out.size() != 1) {
			out.clear();
			out.push_back({ 0, "Vanilla", "Vanilla", 1 });
			s_def = 0;
			s_srcN = 1;
		}
		return out;
	}
	// Kits only — EnsureKnife owned by WeaponSkinsReady (budgeted once/frame)
	const auto& src = KnifeSkins::KitsFor(def);
	if (def == s_def && src.size() == s_srcN && !out.empty())
		return out;
	out.clear();
	out.reserve(src.size() ? src.size() : 1);
	for (const auto& k : src)
		out.push_back({ k.id, k.name, k.token, k.rarity });
	if (out.empty())
		out.push_back({ 0, "Vanilla", "Vanilla", 1 });
	s_def = def;
	s_srcN = src.size() ? src.size() : 1;
	return out;
}

void RunGloveWeapon(C_CSPlayerPawn* local) {
	ProcessGloves(local);
	ProcessWeapons(local);
}

void OnRoundStartGloves() {
	// Debounce: round_start + freeze_end often fire together
	if (g_roundOpenCooldown > 0)
		return;
	g_roundOpenCooldown = 64; // ~1s at 64 tick — ignore duplicate round events

	g_gloveShouldUpdate = true;
	g_gloveFrames = 24; // competitive late glove reapply
	g_gloveClear = 0;   // don't wipe mid-round if clear was pending from menu
	// Reapply paint when loadout lands — NO menu HUD
	g_weaponShouldUpdate = true;
	g_pendingSpawnApply = true;
	// Drop applied cache so new entities re-paint even if paint id matches
	ClearWeaponAppliedCache();
	g_lastWeaponDefs.clear();
	// Do not commit spawn stamp — wait until inventory painted
	g_lastWeaponSpawn = 0.f;
	OpenWeaponWindow(64, true, false);
}

// Local player_spawn (TDM death mid-round) — always re-open even if round_start
// already consumed the debounce. Without this, skins miss after respawn.
void OnLocalPlayerSpawnWeapons() {
	// Don't touch entities here — FireEvent may run while old pawn weapons free.
	// Only arm windows; ProcessWeapons/Gloves apply next NET_UPDATE when alive.
	g_gloveShouldUpdate = true;
	if (g_gloveFrames < 32)
		g_gloveFrames = 32;
	g_weaponShouldUpdate = true;
	g_pendingSpawnApply = true;
	ClearWeaponAppliedCache();
	g_lastWeaponDefs.clear();
	g_lastWeaponSpawn = 0.f;
	// Wider apply window after respawn — competitive loadout lags
	if (g_weaponFrames < 96)
		g_weaponFrames = 96;
	g_weaponForceFull = true;
	g_weaponMenuHud = false;
	if (g_weaponSustain < 48)
		g_weaponSustain = 48;
}

// Buy mid-round: open apply only (no full round cascade, no HUD)
void OnItemPurchaseWeapons() {
	g_weaponShouldUpdate = true;
	OpenWeaponWindow(32, true, false);
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
	// Menu selected new weapon skin — apply + one HUD rebuild
	g_weaponShouldUpdate = true;
	OpenWeaponWindow(16, true, true);
	g_weaponRegenCooldown = 0;
}

// Death / map unload — stop entity writes (no menu HUD open).
void HaltGloveWeaponApply() {
	g_gloveClear = 0;
	g_gloveFrames = 0;
	g_gloveShouldUpdate = false;
	g_weaponShouldUpdate = false;
	g_pendingSpawnApply = false;
	g_weaponFrames = 0;
	g_weaponForceFull = false;
	g_weaponMenuHud = false;
	ClearWeaponAppliedCache();
	g_lastWeaponDefs.clear();
}

} // namespace SkinChanger