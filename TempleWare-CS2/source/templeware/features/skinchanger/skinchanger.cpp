#include "skinchanger.h"
#include "knife_skins.h"
#include "agent_changer.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/vfunc/vfunc.h"
#include "../../utils/console/console.h"

#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/handle.h"
#include "../../../templeware/interfaces/CUserCmd/CUserCmd.h"

#include <Windows.h>
#include <cmath>
#include <cstring>
#include <vector>

// CS2 m_flFallbackWear is normalized 0..1 (0 = Factory New, 1 = Battle-Scarred).
// Clamp out-of-range / NaN so a tampered config or stale slider can't break skin rendering.
static float ClampWear(float w) {
    if (!std::isfinite(w) || w < 0.0001f) return 0.0001f;
    if (w > 1.0f) return 1.0f;
    return w;
}
namespace SkinChanger {
namespace {

// -----------------------------------------------------------------------
// Schema offsets — resolved at Init via SchemaFinder (no hardcodes)
// Non-schema: CEconItemDefinition model ptrs (layout, not client schema)
// -----------------------------------------------------------------------
struct SchemaOffs {
    std::uint32_t subclassID    = 0; // C_BaseEntity->m_nSubclassID
    std::uint32_t subclassVData = 0; // m_nSubclassID + 8 (VData ptr, not a named field)
    std::uint32_t attrMgr       = 0; // C_EconEntity->m_AttributeManager
    std::uint32_t paintKit      = 0; // C_EconEntity->m_nFallbackPaintKit
    std::uint32_t seed          = 0; // C_EconEntity->m_nFallbackSeed
    std::uint32_t wear          = 0; // C_EconEntity->m_flFallbackWear
    std::uint32_t item          = 0; // C_AttributeContainer->m_Item
    std::uint32_t restoreMat    = 0; // C_EconItemView->m_bRestoreCustomMaterialAfterPrecache
    std::uint32_t defIndex      = 0; // C_EconItemView->m_iItemDefinitionIndex
    std::uint32_t quality       = 0; // C_EconItemView->m_iEntityQuality
    std::uint32_t itemID        = 0; // C_EconItemView->m_iItemID
    std::uint32_t itemIDHigh    = 0; // C_EconItemView->m_iItemIDHigh
    std::uint32_t itemIDLow     = 0; // C_EconItemView->m_iItemIDLow
    std::uint32_t accountID     = 0; // C_EconItemView->m_iAccountID
    std::uint32_t initialized   = 0; // C_EconItemView->m_bInitialized
    std::uint32_t disallowSOC   = 0; // C_EconItemView->m_bDisallowSOC
    std::uint32_t attrList      = 0; // C_EconItemView->m_AttributeList
    std::uint32_t attributes    = 0; // CAttributeList->m_Attributes
    std::uint32_t customName    = 0; // C_EconItemView->m_szCustomName
    std::uint32_t hudArms       = 0; // C_CSPlayerPawn->m_hHudModelArms
    std::uint32_t myWeapons     = 0; // CPlayer_WeaponServices->m_hMyWeapons
    std::uint32_t sceneOwner    = 0; // CGameSceneNode->m_pOwner
    std::uint32_t sceneChild    = 0; // CGameSceneNode->m_pChild
    std::uint32_t sceneSibling  = 0; // CGameSceneNode->m_pNextSibling
    std::uint32_t bodyComponent = 0; // C_BaseEntity->m_CBodyComponent
    std::uint32_t animCtrl      = 0; // CBodyComponentBaseAnimGraph->m_animationController
    std::uint32_t ownerEntity   = 0; // C_BaseEntity->m_hOwnerEntity
    // Gloves — C_CSPlayerPawn (IDA schema dump a2bd4708)
    std::uint32_t needReapplyGloves = 0; // m_bNeedToReApplyGloves @ 0x1685
    std::uint32_t econGloves        = 0; // m_EconGloves @ 0x1688
    std::uint32_t lastSpawnTime     = 0; // C_CSPlayerPawnBase->m_flLastSpawnTimeIndex @ 0x1404
    std::uint32_t ownerXuidLow      = 0; // C_EconEntity->m_OriginalOwnerXuidLow @ 0x1678
    std::uint32_t ownerXuidHigh     = 0; // C_EconEntity->m_OriginalOwnerXuidHigh @ 0x167C
    // CEconItemDefinition — not in client schema (nerv layout)
    std::uint32_t defModelName  = 0x148;
    std::uint32_t defViewModel  = 0x348;
    bool ready = false;
};

static SchemaOffs g_off{};

static std::uint32_t Sch(const char* field) {
    return SchemaFinder::Get(hash_32_fnv1a_const(field));
}

static bool ResolveSchemaOffs() {
    g_off.subclassID    = Sch("C_BaseEntity->m_nSubclassID");
    g_off.subclassVData = g_off.subclassID ? g_off.subclassID + 8 : 0;
    g_off.attrMgr       = Sch("C_EconEntity->m_AttributeManager");
    g_off.paintKit      = Sch("C_EconEntity->m_nFallbackPaintKit");
    g_off.seed          = Sch("C_EconEntity->m_nFallbackSeed");
    g_off.wear          = Sch("C_EconEntity->m_flFallbackWear");
    // Prefer known client dump offsets — schema miss OR stale hash must not skip paint writes
    if (!g_off.paintKit || g_off.paintKit < 0x1000) g_off.paintKit = 0x1680;
    if (!g_off.seed     || g_off.seed < 0x1000)     g_off.seed = 0x1684;
    if (!g_off.wear     || g_off.wear < 0x1000)     g_off.wear = 0x1688;
    g_off.item          = Sch("C_AttributeContainer->m_Item");
    g_off.restoreMat    = Sch("C_EconItemView->m_bRestoreCustomMaterialAfterPrecache");
    g_off.defIndex      = Sch("C_EconItemView->m_iItemDefinitionIndex");
    g_off.quality       = Sch("C_EconItemView->m_iEntityQuality");
    g_off.itemID        = Sch("C_EconItemView->m_iItemID");
    g_off.itemIDHigh    = Sch("C_EconItemView->m_iItemIDHigh");
    g_off.itemIDLow     = Sch("C_EconItemView->m_iItemIDLow");
    g_off.accountID     = Sch("C_EconItemView->m_iAccountID");
    g_off.initialized   = Sch("C_EconItemView->m_bInitialized");
    g_off.disallowSOC   = Sch("C_EconItemView->m_bDisallowSOC");
    g_off.attrList      = Sch("C_EconItemView->m_AttributeList");
    g_off.attributes    = Sch("CAttributeList->m_Attributes");
    g_off.customName    = Sch("C_EconItemView->m_szCustomName");
    // Hard fallbacks from current client schema dump (a2x / s2v) if SchemaFinder misses
    if (!g_off.attrList)   g_off.attrList = 0x208;
    if (!g_off.attributes) g_off.attributes = 0x8;
    if (!g_off.customName) g_off.customName = 0x2F8;
    g_off.hudArms       = Sch("C_CSPlayerPawn->m_hHudModelArms");
    g_off.myWeapons     = Sch("CPlayer_WeaponServices->m_hMyWeapons");
    g_off.sceneOwner    = Sch("CGameSceneNode->m_pOwner");
    g_off.sceneChild    = Sch("CGameSceneNode->m_pChild");
    g_off.sceneSibling  = Sch("CGameSceneNode->m_pNextSibling");
    g_off.bodyComponent = Sch("C_BaseEntity->m_CBodyComponent");
    g_off.animCtrl      = Sch("CBodyComponentBaseAnimGraph->m_animationController");
    g_off.ownerEntity   = Sch("C_BaseEntity->m_hOwnerEntity");
    g_off.needReapplyGloves = Sch("C_CSPlayerPawn->m_bNeedToReApplyGloves");
    g_off.econGloves        = Sch("C_CSPlayerPawn->m_EconGloves");
    g_off.lastSpawnTime     = Sch("C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
    g_off.ownerXuidLow      = Sch("C_EconEntity->m_OriginalOwnerXuidLow");
    g_off.ownerXuidHigh     = Sch("C_EconEntity->m_OriginalOwnerXuidHigh");
    // IDA schema field dump (a2bd4708)
    if (!g_off.needReapplyGloves) g_off.needReapplyGloves = 0x1685;
    if (!g_off.econGloves)        g_off.econGloves = 0x1688;
    if (!g_off.lastSpawnTime)     g_off.lastSpawnTime = 0x1404;
    if (!g_off.ownerXuidLow)      g_off.ownerXuidLow = 0x1678;
    if (!g_off.ownerXuidHigh)     g_off.ownerXuidHigh = 0x167C;

    // minimum set required for knife apply
    g_off.ready =
        g_off.subclassID && g_off.attrMgr && g_off.item &&
        g_off.defIndex && g_off.itemIDHigh && g_off.initialized &&
        g_off.hudArms && g_off.myWeapons &&
        g_off.sceneOwner && g_off.sceneChild && g_off.sceneSibling;
    return g_off.ready;
}

constexpr int kQualityUnusual = 3;
constexpr int kUpdateFrames   = 6;   // same burst as weapon skins
constexpr int kCfgUpdateFrames = 6;  // knife model/skin menu — match weapons (was 24)

// -----------------------------------------------------------------------
// Function pointer types
// -----------------------------------------------------------------------
using FnUpdateSubclass     = void(__fastcall*)(void*);
using FnSetMeshGroupMask   = void(__fastcall*)(void*, std::uint64_t);
using FnSetModel           = void(__fastcall*)(void*, const char*);
using FnAnimRebuild        = void(__fastcall*)(void*, std::uint8_t);
using FnInvalidateAttrCache= void(__fastcall*)(void*);
// C_EconItemView::construct_paint_kit — IDA 0x1810F1370 (VMT[3] kit id → schema paint kit*)
using FnConstructPaintKit  = void*(__fastcall*)(void* itemView);
using FnRegenerateSkins    = void(__fastcall*)();
// IDA 0x1807E1C50 — recreate HUD when weapon/hud model handles differ (DATA_UPDATE)
using FnWeaponHudModelSync = void(__fastcall*)(void* weapon, unsigned int updateType);
// nerv: HudWeaponSelection icon clear (bottom weapon strip)
using FnFindHudElement     = std::uintptr_t(__fastcall*)(const char* name);
using FnClearHudWeaponIcon = std::int64_t(__fastcall*)(std::uintptr_t hudWeapons, std::int32_t index, std::int64_t unk);
// tier0: static CUtlStringToken CUtlStringToken::Make(const char*) — returns hash in RAX (MSVC x64)
using FnStringToken        = std::uint32_t(__fastcall*)(const char*);
// schema find: CEconItemDefinition* Find(schema, defIndex, bDefault) — IDA 0x1810939C0
using FnFindItemDef        = void*(__fastcall*)(void* schema, int defIdx, char allowDefault);
// CBaseModelEntity::SetBodyGroup(name, value) — IDA 0x18091F5B0
using FnSetBodyGroup       = void(__fastcall*)(void* entity, const char* name, unsigned int value);

// -----------------------------------------------------------------------
// Resolved pointers — all verified unique in live client.dll (IDA)
// -----------------------------------------------------------------------
FnUpdateSubclass      g_updateSubclass    = nullptr; // 0x180206430
FnSetMeshGroupMask    g_setMesh           = nullptr; // 0x180a6c550
FnSetModel            g_setModel          = nullptr; // 0x180920690
FnAnimRebuild         g_animRebuild       = nullptr; // 0x1808f0e30
FnInvalidateAttrCache g_invalidateCache   = nullptr; // 0x181100e60
FnConstructPaintKit   g_constructPaintKit = nullptr; // 0x1810F1370
FnRegenerateSkins     g_regenSkins        = nullptr; // 0x1807eb300
FnWeaponHudModelSync  g_weaponHudSync     = nullptr; // 0x1807E1C50
FnFindHudElement      g_findHudElement    = nullptr; // nerv find_hud_element
FnClearHudWeaponIcon  g_clearHudIcon      = nullptr; // nerv clear_hud_weapon_icon
FnStringToken         g_makeToken         = nullptr; // tier0 ?Make@CUtlStringToken@@SA?AV1@PEBD@Z
FnSetBodyGroup        g_setBodyGroup      = nullptr; // 0x18091F5B0
// C_CSWeaponBase +0x1760 = CHandle → C_CS2HudModelWeapon (IDA sub_1807E1C50)
constexpr std::uint32_t kOffWeaponHudModel = 0x1760;

// Item schema — CEconItemDefinition* by def index (IDA: schema+0xF8 map)
// NOT GetStaticData VMT13: that tail-calls def VMT[0x148] (not def*)
void**         g_itemSystemSingleton = nullptr; // CCStrike15ItemSystem**
void*          g_itemSchema          = nullptr;
FnFindItemDef  g_findItemDef         = nullptr;

using FnGameAlloc = void*(__cdecl*)(std::size_t);
using FnGameFree  = void(__cdecl*)(void*);
FnGameAlloc g_gameAlloc = nullptr;
FnGameFree  g_gameFree  = nullptr;

bool g_ready          = false;

// Live model paths from schema (+0x148); game strings, stable for process life
static const char* g_liveModel[32]{};
static bool        g_liveModelReady = false;

// Skin apply tracking (nerv m_last_knife_*)
static int   g_lastPaintKitId = -1;
static float g_lastWear       = -1.f;
static int   g_lastSeed       = -1;
static int   g_lastPaintIdx   = -1;

// -----------------------------------------------------------------------
// Per-run state
// -----------------------------------------------------------------------
int           g_frames        = 0;
int           g_appliedIdx    = -1;
bool          g_spoofed        = false;
bool          g_haveOrig       = false;
std::uint32_t g_origSubclass   = 0;
std::uint16_t g_origDef        = 0;
void*         g_origWeapon     = nullptr;
std::uint8_t  g_lastTeam       = 0;
std::uint32_t g_lastActiveHdl  = 0;
int           g_lastKnifeCount = -1;
    int           g_lastCfgIdx     = -1;
bool          g_lastCfgEn      = false;
// HUD viewmodel is recreated after UpdateSubclass — push model when it appears
void*         g_lastHudEnt     = nullptr;
const char*   g_lastHudModel   = nullptr;
bool          g_needHudPush    = false;
bool          g_doHudSyncOnce  = false; // WeaponHudSync once per menu knife change
bool          g_doHudIconOnce  = false; // clear strip icon on model/skin menu change
bool          g_wantSkinReequip = false; // soft reequip when paint kit changes
int           g_hudHoldFrames  = 0;     // keep SetModel on HUD after window ends
// Mid-round HUD rebuild: switch off knife then back (engine create path)
enum { RE_IDLE = 0, RE_AWAY, RE_WAIT, RE_BACK };
int           g_reequip        = RE_IDLE;
int           g_reequipOther   = 0; // entity index of non-knife
int           g_reequipKnife   = 0; // entity index of knife
int           g_reequipWait    = 0;
// Track equip state — spoof only while knife is active so slot/key select works
bool          g_wasHoldingKnife = false;
// Menu changed knife model/skin — full HUD once while held (or on next equip)
bool          g_menuHudPending = false;
// Menu changed paint only — strip/icon refresh, no WeaponHudSync
bool          g_menuSkinPending = false;

// -----------------------------------------------------------------------
// Knife table
// -----------------------------------------------------------------------
constexpr KnifeInfo kKnives[] = {
    { "Default",        "weapon_knife",                 0,   nullptr },
    { "Bayonet",        "weapon_bayonet",               500, "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl" },
    { "Classic",        "weapon_knife_css",             503, "weapons/models/knife/knife_css/weapon_knife_css.vmdl" },
    { "Flip",           "weapon_knife_flip",            505, "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl" },
    { "Gut",            "weapon_knife_gut",             506, "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl" },
    { "Karambit",       "weapon_knife_karambit",        507, "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl" },
    { "M9 Bayonet",     "weapon_knife_m9_bayonet",      508, "weapons/models/knife/knife_m9_bayonet/weapon_knife_m9_bayonet.vmdl" },
    { "Huntsman",       "weapon_knife_tactical",        509, "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl" },
    { "Falchion",       "weapon_knife_falchion",        512, "weapons/models/knife/knife_falchion_advanced/weapon_knife_falchion_advanced.vmdl" },
    { "Bowie",          "weapon_knife_survival_bowie",  514, "weapons/models/knife/knife_survival_bowie/weapon_knife_survival_bowie.vmdl" },
    { "Butterfly",      "weapon_knife_butterfly",       515, "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl" },
    { "Shadow Daggers", "weapon_knife_push",            516, "weapons/models/knife/knife_push/weapon_knife_push.vmdl" },
    { "Paracord",       "weapon_knife_cord",            517, "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl" },
    { "Survival",       "weapon_knife_canis",           518, "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl" },
    { "Ursus",          "weapon_knife_ursus",           519, "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl" },
    { "Navaja",         "weapon_knife_gypsy_jackknife", 520, "weapons/models/knife/knife_gypsy_jackknife/weapon_knife_gypsy_jackknife.vmdl" },
    { "Nomad",          "weapon_knife_outdoor",         521, "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl" },
    { "Stiletto",       "weapon_knife_stiletto",        522, "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl" },
    { "Talon",          "weapon_knife_widowmaker",      523, "weapons/models/knife/knife_widowmaker/weapon_knife_widowmaker.vmdl" },
    { "Skeleton",       "weapon_knife_skeleton",        525, "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl" },
    { "Kukri",          "weapon_knife_kukri",           526, "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl" },
};

// -----------------------------------------------------------------------
// Token hash — Murmur2 seed 0x31415926 (CUtlStringToken / Source 2)
// Subclass VData keyed by weapon name: "weapon_knife_karambit"
// -----------------------------------------------------------------------
static std::uint32_t Murmur2Token(const char* name) {
    if (!name || !*name) return 0;
    char buf[128]{}; int len = 0;
    for (; name[len] && len < 127; ++len) {
        unsigned char c = (unsigned char)name[len];
        buf[len] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    const std::uint32_t m = 0x5bd1e995u; const int r = 24;
    std::uint32_t h = 0x31415926u ^ (std::uint32_t)len;
    const auto* d = (const unsigned char*)buf; int n = len;
    while (n >= 4) {
        std::uint32_t k = 0; std::memcpy(&k, d, 4);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k; d += 4; n -= 4;
    }
    switch (n) {
    case 3: h ^= d[2] << 16; [[fallthrough]];
    case 2: h ^= d[1] << 8;  [[fallthrough]];
    case 1: h ^= d[0]; h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

// Source2 CUtlStringToken = MurmurHash2 lowercased, seed 0x31415926 (ttapp string_token_hash).
// Do NOT call tier0 Make@CUtlStringToken via wrong ABI — RAX garbage was 0x6E10BE48 for
// EVERY knife def (log: 506/507/508 all same tok → same VData → broken model/anim).
static std::uint32_t MakeToken(const char* name) {
    return Murmur2Token(name);
}

// VData registry key is decimal def-index string (live: T-knife def=59 → hash("59")).
static std::uint32_t MakeKnifeToken(const KnifeInfo& k) {
    if (!k.def_index) return 0;
    char buf[16];
    sprintf_s(buf, "%d", (int)k.def_index);
    return MakeToken(buf);
}

// -----------------------------------------------------------------------
// Item field accessors (schema-resolved)
// -----------------------------------------------------------------------
static std::uint8_t* ItemBase(void* w) {
    return (std::uint8_t*)w + g_off.attrMgr + g_off.item;
}
static std::uint32_t& SubclassRef(void* w) {
    return *(std::uint32_t*)((std::uint8_t*)w + g_off.subclassID);
}
static void* VDataPtr(void* w) {
    return *(void**)((std::uint8_t*)w + g_off.subclassVData);
}
static std::uint16_t& DefIndexRef(void* w)  { return *(std::uint16_t*)(ItemBase(w) + g_off.defIndex);   }
static std::int32_t&  QualityRef(void* w)   { return *(std::int32_t*)(ItemBase(w) + g_off.quality);     }
static std::uint64_t& ItemIDRef(void* w)    { return *(std::uint64_t*)(ItemBase(w) + g_off.itemID);      }
static std::uint32_t& ItemIDHighRef(void* w){ return *(std::uint32_t*)(ItemBase(w) + g_off.itemIDHigh);  }
static std::uint32_t& ItemIDLowRef(void* w) { return *(std::uint32_t*)(ItemBase(w) + g_off.itemIDLow);   }
static std::uint32_t& AccountIDRef(void* w) { return *(std::uint32_t*)(ItemBase(w) + g_off.accountID);   }
static bool& InitializedRef(void* w)  { return *(bool*)(ItemBase(w) + g_off.initialized); }
static bool& DisallowSOCRef(void* w)  { return *(bool*)(ItemBase(w) + g_off.disallowSOC); }
static bool& RestoreMatRef(void* w)   { return *(bool*)(ItemBase(w) + g_off.restoreMat);  }
static std::int32_t& PaintKitRef(void* w) { return *(std::int32_t*)((std::uint8_t*)w + g_off.paintKit); }
static std::int32_t& SeedRef(void* w)     { return *(std::int32_t*)((std::uint8_t*)w + g_off.seed); }
static float& WearRef(void* w)            { return *(float*)((std::uint8_t*)w + g_off.wear); }

// nerv econ_item_attribute_manager — paint/pattern/wear attrs on C_EconItemView
struct EconItemAttribute {
	char     pad0[0x30];
	std::uint16_t def_index;
	char     pad32[2];
	float    value;
	float    init_value;
	std::int32_t refundable_currency;
	bool     set_bonus;
	char     pad41[7];
};
static_assert(sizeof(EconItemAttribute) == 0x48, "CEconItemAttribute size");

struct AttrVec {
	std::uint64_t size;
	std::uintptr_t ptr;
};

enum : std::uint16_t {
	kAttrPaint   = 6,
	kAttrPattern = 7,
	kAttrWear    = 8,
};

// Attrs we allocated via g_gameAlloc — safe to detach on Vanilla. Never null game-owned vecs.
static void* g_ourAttrBlock = nullptr;

static AttrVec* AttrVecPtr(void* itemView) {
	if (!itemView || !g_off.attrList) return nullptr;
	// CAttributeList::m_Attributes is at +0x08 (schema); never use 0 — that is the list header
	const std::uint32_t attrOff = g_off.attributes ? g_off.attributes : 0x8u;
	std::uint8_t* list = (std::uint8_t*)itemView + g_off.attrList;
	return reinterpret_cast<AttrVec*>(list + attrOff);
}

// skichanger econ_item_attribute_manager: remove then create paint/pattern/wear.
// In-place update when paint attr already present; otherwise wipe + alloc fresh.
// Knives often ship with a non-empty attr list that lacks def 6/7/8 — old path
// returned early and never created paint attrs.
static void AttrWipe(void* itemView); // forward

static void AttrApply(void* itemView, int paintKit, float wear, int seed) {
	if (!itemView || paintKit <= 0) return;
	AttrVec* vec = AttrVecPtr(itemView);
	if (!vec) return;

	const float paintF = (float)paintKit;
	const float seedF = (float)(seed >= 0 ? seed : 0);
	const float wearF = ClampWear(wear);

	__try {
		const std::uint64_t rawSize = vec->size;
		const int sz32 = (int)(rawSize & 0xFFFFFFFFu);
		const int nExist = (sz32 > 0 && sz32 < 64) ? sz32 : 0;

		if (nExist > 0 && vec->ptr && Mem::IsUserPtr((void*)vec->ptr)) {
			auto* attrs = (EconItemAttribute*)vec->ptr;
			bool hasPaint = false, hasPattern = false, hasWear = false;
			for (int i = 0; i < nExist; ++i) {
				auto& a = attrs[i];
				if (a.def_index == kAttrPaint) {
					a.value = a.init_value = paintF;
					hasPaint = true;
				} else if (a.def_index == kAttrPattern) {
					a.value = a.init_value = seedF;
					hasPattern = true;
				} else if (a.def_index == kAttrWear) {
					a.value = a.init_value = wearF;
					hasWear = true;
				}
			}
			if (hasPaint && hasPattern && hasWear)
				return;
			// Missing paint/pattern/wear slots — detach and recreate like skichanger remove+create
			AttrWipe(itemView);
			vec = AttrVecPtr(itemView);
			if (!vec) return;
		}

		// create only when empty (skichanger create)
		if (vec->size != 0 || vec->ptr != 0) return;
		if (!g_gameAlloc) return;

		constexpr std::size_t n = 3;
		auto* attrs = (EconItemAttribute*)g_gameAlloc(n * sizeof(EconItemAttribute));
		if (!attrs) return;
		memset(attrs, 0, n * sizeof(EconItemAttribute));
		attrs[0].def_index = kAttrPaint;
		attrs[0].value = attrs[0].init_value = paintF;
		attrs[1].def_index = kAttrPattern;
		attrs[1].value = attrs[1].init_value = seedF;
		attrs[2].def_index = kAttrWear;
		attrs[2].value = attrs[2].init_value = wearF;
		vec->size = n;
		vec->ptr = (std::uintptr_t)attrs;
		g_ourAttrBlock = attrs;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Detach attr vector (skichanger remove). Free only our alloc.
static void AttrWipe(void* itemView) {
	if (!itemView) return;
	AttrVec* vec = AttrVecPtr(itemView);
	if (!vec) return;
	__try {
		const int sz32 = (int)(vec->size & 0xFFFFFFFFu);
		if (sz32 <= 0 && vec->ptr == 0) return;
		void* ptr = (void*)vec->ptr;
		vec->size = 0;
		vec->ptr = 0;
		if (ptr && g_ourAttrBlock && ptr == g_ourAttrBlock) {
			g_ourAttrBlock = nullptr;
			if (g_gameFree) {
				__try { g_gameFree(ptr); }
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
		}
		// game-owned: detached only — never free foreign memory
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Vanilla restore — zero paint/pattern/wear in place, or detach our block.
static void AttrClearPaint(void* itemView) {
	if (!itemView) return;
	AttrVec* vec = AttrVecPtr(itemView);
	if (!vec) return;
	__try {
		const int sz32 = (int)(vec->size & 0xFFFFFFFFu);
		if (sz32 <= 0 || sz32 >= 64 || !vec->ptr || !Mem::IsUserPtr((void*)vec->ptr))
			return;

		if (g_ourAttrBlock && (void*)vec->ptr == g_ourAttrBlock) {
			AttrWipe(itemView);
			return;
		}

		auto* attrs = (EconItemAttribute*)vec->ptr;
		for (int i = 0; i < sz32; ++i) {
			auto& a = attrs[i];
			if (a.def_index == kAttrPaint || a.def_index == kAttrPattern || a.def_index == kAttrWear) {
				a.value = a.init_value = 0.f;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ApplyCustomName(void* itemView) {
	if (!itemView || !g_off.customName || !Config::knife_custom_name[0]) return;
	char* dst = (char*)((std::uint8_t*)itemView + g_off.customName);
	__try {
		strncpy_s(dst, 161, Config::knife_custom_name, _TRUNCATE);
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// -----------------------------------------------------------------------
// Safe pointer read
// -----------------------------------------------------------------------
static std::uintptr_t SafeRead(std::uintptr_t a) {
    __try { return *(std::uintptr_t*)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// -----------------------------------------------------------------------
// Entity helpers
// -----------------------------------------------------------------------
static const char* ClassName(C_BaseEntity* e) {
    if (!e) return "?";
    SchemaClassInfoData_t* ci = nullptr;
    e->dump_class_info(&ci);
    if (ci && ci->szName && Mem::IsReadable(ci->szName, 2)) return ci->szName;
    return "?";
}

static const char* DesignerName(C_BaseEntity* e) {
    if (!e) return nullptr;
    CEntityIdentity* id = e->m_pEntityIdentity();
    if (!id || !Mem::IsUserPtr(id)) return nullptr;
    const char* n = *(const char**)((std::uint8_t*)id + 0x20);
    if (n && Mem::IsReadable(n, 2)) return n;
    return nullptr;
}

// skichanger: knife ONLY by def_index (42/59/500-526). Never class-name —
// false positives rewrote AK (7) / deagle (1) into knife and killed HUD/viewmodel.
static bool IsKnifeDef(std::uint16_t d) {
    return d == 42 || d == 59 || (d >= 500 && d <= 526);
}

static bool IsKnife(C_BaseEntity* e) {
    if (!e) return false;
    return IsKnifeDef(DefIndexRef(e));
}

// -----------------------------------------------------------------------
// SEH wrappers
// -----------------------------------------------------------------------
static void SehSubclass(void* w) {
    if (!g_updateSubclass || !w) return;
    __try { g_updateSubclass(w); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SehSetModel(void* w, const char* p) {
    if (!g_setModel || !w || !p || !*p) return;
    __try { g_setModel(w, p); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SehSetMesh(void* node, std::uint64_t mask) {
    if (!g_setMesh || !node) return;
    __try { g_setMesh(node, mask); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// mesh via scene node
static void SehMeshEntity(void* ent, std::uint64_t mask) {
    if (!ent) return;
    __try {
        CGameSceneNode* node = ((C_BaseEntity*)ent)->m_pGameSceneNode();
        if (node && Mem::IsUserPtr(node)) SehSetMesh(node, mask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Match working ttapp (Downloads/New folder c_econ_entity):
//   update_composite → VMT[10], update_skin → VMT[110]
// (11/111 are adjacent slots; wrong index leaves knife mesh/anim half-applied)
static void SehComposite(void* w) {
    __try { M::CallVFunc<void*, 10U>(w, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SehSkin(void* w) {
    __try { M::CallVFunc<void, 110U>(w, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// nerv update_weapon_data — refreshes HUD / weapon presentation after skin
static void SehWeaponData(void* w) {
    __try { M::CallVFunc<void*, 195U>(w); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// InvalidateAttributeCache — ttapp after attr create, before visuals
static void SehInvalidateCache(void* item) {
    if (!g_invalidateCache || !item) return;
    __try { g_invalidateCache(item); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void* GetItemSchema(); // defined below

// ttapp sync_constructed_paint_kit: construct_paint_kit() then copy id/name from schema kit.
// Without this, material system can keep the previous kit binding even when fallbacks/attrs are correct.
static void* FindPaintKitById(int paintKitId) {
    if (paintKitId <= 0) return nullptr;
    void* schema = GetItemSchema();
    if (!schema || !Mem::IsUserPtr(schema)) return nullptr;
    // CEconItemSchema::get_paint_kits @ +0x2F0 (same as knife_skins)
    struct Node { int left, right, p8, p12, key, p20; void* value; };
    struct Map { int count, capacity_flags; Node* elements; };
    __try {
        auto* map = reinterpret_cast<Map*>((std::uint8_t*)schema + 0x2F0);
        if (!map || map->count <= 0 || map->count > 20000 || !map->elements || !Mem::IsUserPtr(map->elements))
            return nullptr;
        for (int i = 0; i < map->count; ++i) {
            if (map->elements[i].key != paintKitId) continue;
            void* kit = map->elements[i].value;
            return (kit && Mem::IsUserPtr(kit)) ? kit : nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static void SehSyncPaintKitFull(void* itemView, int paintKitId) {
    if (!itemView || !g_constructPaintKit) return;
    static const char kDefaultName[] = "default";
    __try {
        void* kit = g_constructPaintKit(itemView);
        if (!kit || !Mem::IsUserPtr(kit)) return;
        // c_paint_kit: m_id @0, m_name @8
        if (paintKitId <= 0) {
            *(int*)kit = 0;
            *(const char**)((std::uint8_t*)kit + 8) = kDefaultName;
            return;
        }
        if (void* desired = FindPaintKitById(paintKitId)) {
            *(int*)kit = *(int*)desired;
            *(const char**)((std::uint8_t*)kit + 8) =
                *(const char**)((std::uint8_t*)desired + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// skichanger: ONLY m_iItemIDHigh = 0xFFFFFFFF. Leave low/full/account alone.
// Fresh low + full rewrite breaks inventory/material cache and kills knife skins.
static void ApplyItemIdHighOnly(void* itemView)
{
    if (!itemView || !g_off.itemIDHigh) return;
    __try {
        *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDHigh) = 0xFFFFFFFFu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// AnimGraphRebuild (IDA sub_1808F0E30): arg0 = CBaseAnimGraphController*, mode=2 tears graph.
// body = entity+0x30 (m_CBodyComponent). Controller offset:
//   schema dump CBodyComponentBaseAnimGraph::m_animationController = 0x510
//   ttapp uses 0x4E0 (still works on some builds) — try both; graph @ +0x448 / +0x450
static void SehAnimRebuild(void* entity) {
    if (!g_animRebuild || !entity) return;
    constexpr std::uint32_t kBodyOff = 0x30u;
    constexpr std::uint32_t kAnimOffs[] = { 0x510u, 0x4E0u };
    constexpr std::uint32_t kGraphOffs[] = { 0x450u, 0x448u };
    __try {
        auto base = (std::uintptr_t)entity;
        auto body = SafeRead(base + kBodyOff);
        if (!body || !Mem::IsUserPtr((void*)body)) return;
        for (std::uint32_t animOff : kAnimOffs) {
            for (std::uint32_t graphOff : kGraphOffs) {
                if (!Mem::IsReadable((void*)(body + animOff + graphOff), sizeof(void*)))
                    continue;
                const auto controller = body + animOff;
                const auto graph = SafeRead(controller + graphOff);
                if (!graph || !Mem::IsUserPtr((void*)graph))
                    continue;
                g_animRebuild((void*)controller, 2u);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// HUD / viewmodel entities use indices well above 2048.
// CGameEntitySystem::Get() clamps to Mem::kMaxEntityIndex — bypass via ogGetBaseEntity.
static C_BaseEntity* EntityByIndex(int idx) {
    if (idx < 0 || idx > 0x7FFE) return nullptr;
    if (!H::ogGetBaseEntity || !I::GameEntity || !I::GameEntity->Instance) return nullptr;
    void* raw = nullptr;
    __try { raw = H::ogGetBaseEntity(I::GameEntity->Instance, idx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (!Mem::ValidEntity(raw)) return nullptr;
    return reinterpret_cast<C_BaseEntity*>(raw);
}

static C_BaseEntity* EntityByHandle(CBaseHandle h) {
    if (!h.valid()) return nullptr;
    return EntityByIndex(h.index());
}

static C_BaseEntity* GetHudArms(C_CSPlayerPawn* local) {
    if (!local || !g_off.hudArms) return nullptr;
    CBaseHandle h = *(CBaseHandle*)((std::uint8_t*)local + g_off.hudArms);
    return EntityByHandle(h);
}

// IDA: C_CSWeaponBase+0x1760 is CHandle to this weapon's C_CS2HudModelWeapon
static C_BaseEntity* GetHudFromWeaponHandle(C_BaseEntity* world) {
    if (!world) return nullptr;
    __try {
        CBaseHandle h = *(CBaseHandle*)((std::uint8_t*)world + kOffWeaponHudModel);
        if (!h.valid()) return nullptr;
        C_BaseEntity* hud = EntityByHandle(h);
        if (!hud || !Mem::IsUserPtr(hud)) return nullptr;
        return hud;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// skichanger get_hud_weapon 1:1 — walk arms children, owner_entity == world weapon
// (no designer-name filter; wrong filter can bind the wrong HUD child)
static C_BaseEntity* GetHudFromArms(C_BaseEntity* world, C_CSPlayerPawn* local) {
    if (!world || !local || !g_off.hudArms || !g_off.sceneChild || !g_off.sceneSibling)
        return nullptr;

    CBaseHandle h = *(CBaseHandle*)((std::uint8_t*)local + g_off.hudArms);
    if (!h.valid()) return nullptr;

    C_BaseEntity* arms = EntityByHandle(h);
    if (!arms) return nullptr;

    CGameSceneNode* armsNode = arms->m_pGameSceneNode();
    if (!armsNode || !Mem::IsUserPtr(armsNode)) return nullptr;

    auto* child = *(CGameSceneNode**)((std::uint8_t*)armsNode + g_off.sceneChild);
    for (int g = 0; child && Mem::IsUserPtr(child) && g < 32; ++g) {
        auto* owner = *(C_BaseEntity**)((std::uint8_t*)child + g_off.sceneOwner);
        if (owner && Mem::IsUserPtr(owner)) {
            CBaseHandle oh = owner->m_hOwnerEntity();
            auto* owned = EntityByHandle(oh);
            if (owned == world) return owner;
        }
        child = *(CGameSceneNode**)((std::uint8_t*)child + g_off.sceneSibling);
    }
    return nullptr;
}

// Prefer arms walk (ttapp get_hud_weapon — designer cs2_hudmodel_weapon);
// weapon+0x1760 handle is secondary (can lag one frame after SetModel).
static C_BaseEntity* GetHudWeapon(C_BaseEntity* world, C_CSPlayerPawn* local) {
    if (C_BaseEntity* byArms = GetHudFromArms(world, local))
        return byArms;
    return GetHudFromWeaponHandle(world);
}

static void SehWeaponHudSync(void* w) {
    if (!g_weaponHudSync || !w) return;
    __try { g_weaponHudSync(w, 1u); } // DATA_UPDATE path
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// nerv item_schema.cpp 1:1 — HudWeaponSelection strip icon
// FindHudElement("HudWeaponSelection") → panel base = hud-0x98
// slots: count@+0x50, data@+0x58, stride 72, handle@+0x38
static bool ResolveWeaponPanel(std::uintptr_t& base, std::uint8_t*& data, int& count) {
    base = 0; data = nullptr; count = 0;
    if (!g_findHudElement) return false;
    const std::uintptr_t hud = g_findHudElement("HudWeaponSelection");
    if (!hud || !Mem::IsUserPtr((void*)hud)) return false;
    base = hud - 0x98;
    data = *(std::uint8_t**)(base + 0x58);
    count = *(int*)(base + 0x50);
    return data && Mem::IsUserPtr(data) && count > 0 && count <= 64;
}

// nerv clear_hud_weapon_icons — wipe every slot so strip rebuilds from current def_index
static void ClearHudWeaponIconsAll() {
    if (!g_clearHudIcon) return;
    __try {
        std::uintptr_t base = 0; std::uint8_t* data = nullptr; int count = 0;
        if (!ResolveWeaponPanel(base, data, count)) return;
        for (int i = count - 1; i >= 0; --i)
            g_clearHudIcon(base, i, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// nerv clear_hud_weapon_icon_for — match weapon handle only (no clear-all fallback;
// wiping every slot made the strip fall back to generic "Knife")
static void ClearHudWeaponIconFor(C_BaseEntity* weapon) {
    if (!g_clearHudIcon || !weapon) return;
    if (!I::GameEntity || !I::GameEntity->Instance || !H::ogGetBaseEntity) return;
    __try {
        std::uintptr_t base = 0; std::uint8_t* data = nullptr; int count = 0;
        if (!ResolveWeaponPanel(base, data, count)) return;
        for (int i = count - 1; i >= 0; --i) {
            const int handle = *(int*)(data + 72 * i + 0x38);
            if (handle < 0) continue;
            void* ent = nullptr;
            __try { ent = H::ogGetBaseEntity(I::GameEntity->Instance, handle & 0x7FFF); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ent = nullptr; }
            if (ent == weapon) {
                g_clearHudIcon(base, i, 0);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Refresh arms anim graph — HudArmsRefresh sig is dead on live client,
// so we tear down the arms AG2 and let it rebind on next frame
static void RefreshHudArms(C_CSPlayerPawn* local) {
    C_BaseEntity* arms = GetHudArms(local);
    if (!arms) return;
    SehAnimRebuild(arms);
}

// -----------------------------------------------------------------------
// Knife inventory walk
// -----------------------------------------------------------------------
struct KnifeList { C_CSWeaponBase* knives[8]{}; int count = 0; };

// skichanger my_weapons walk — only def-index knives (max 2 typical)
static KnifeList CollectKnives(C_CSPlayerPawn* local) {
    KnifeList out{};
    if (!local || !I::GameEntity || !I::GameEntity->Instance) return out;
    CCSPlayer_WeaponServices* ws = local->GetWeaponServices();
    if (!ws || !Mem::IsUserPtr(ws)) return out;

    auto* base = (std::uint8_t*)ws + g_off.myWeapons;
    // C_NetworkUtlVectorBase: size@+0, ptr@+8 (match live client)
    const int sz = *(int*)(base + 0);
    auto* elems = *(CBaseHandle**)(base + 8);
    if (sz <= 0 || sz > 64 || !elems || !Mem::IsUserPtr(elems)) {
        // swapped layout fallback
        const int sz2 = *(int*)(base + 8);
        auto* e2 = *(CBaseHandle**)(base + 0);
        if (sz2 <= 0 || sz2 > 64 || !e2 || !Mem::IsUserPtr(e2)) return out;
        for (int i = 0; i < sz2 && out.count < 8; ++i) {
            if (!e2[i].valid()) continue;
            auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(e2[i]);
            if (!w) continue;
            if (IsKnifeDef(DefIndexRef(w)))
                out.knives[out.count++] = w;
        }
        return out;
    }
    for (int i = 0; i < sz && out.count < 8; ++i) {
        if (!elems[i].valid()) continue;
        auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(elems[i]);
        if (!w) continue;
        if (IsKnifeDef(DefIndexRef(w)))
            out.knives[out.count++] = w;
    }
    return out;
}

static int EntIndex(void* e) {
    if (!e) return 0;
    __try { return (int)((C_BaseEntity*)e)->get_entity_by_handle(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// First non-knife in inventory (for mid-round switch-away)
static int FindNonKnifeIndex(C_CSPlayerPawn* local) {
    if (!local || !I::GameEntity || !I::GameEntity->Instance) return 0;
    CCSPlayer_WeaponServices* ws = local->GetWeaponServices();
    if (!ws || !Mem::IsUserPtr(ws)) return 0;
    auto* base = (std::uint8_t*)ws + g_off.myWeapons;
    struct Try { CBaseHandle* elems; int sz; };
    Try tries[2] = {
        { *(CBaseHandle**)(base + 0), *(int*)(base + 8) },
        { *(CBaseHandle**)(base + 8), *(int*)(base + 0) },
    };
    for (auto& t : tries) {
        if (t.sz <= 0 || t.sz > 64 || !t.elems || !Mem::IsUserPtr(t.elems)) continue;
        for (int i = 0; i < t.sz; ++i) {
            if (!t.elems[i].valid()) continue;
            auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(t.elems[i]);
            if (!w) continue;
            if (IsKnife((C_BaseEntity*)w) || IsKnifeDef(DefIndexRef(w))) continue;
            const int idx = EntIndex(w);
            if (idx > 0) return idx;
        }
    }
    return 0;
}

static void RequestReequip(C_CSPlayerPawn* local, C_CSWeaponBase* knife) {
    if (!local || !knife || g_reequip != RE_IDLE) return;
    const int ki = EntIndex(knife);
    const int other = FindNonKnifeIndex(local);
    if (ki <= 0 || other <= 0) return;
    g_reequipKnife = ki;
    g_reequipOther = other;
    g_reequip = RE_AWAY;
    g_reequipWait = 0;
}

// -----------------------------------------------------------------------
// Orig capture / restore
// -----------------------------------------------------------------------
static void CaptureOrig(C_CSWeaponBase* w) {
    if (!w) return;
    // Only capture real default knife (CT 42 / T 59) — never a already-spoofed knife
    const std::uint16_t d = DefIndexRef(w);
    if (d != 42 && d != 59) return;
    if (g_haveOrig && g_origWeapon == w) return;
    g_origDef      = d;
    g_origSubclass = SubclassRef(w);
    g_origWeapon   = w;
    g_haveOrig     = true;
}

static void RestoreKnife(C_CSWeaponBase* w, const char* /*why*/) {
    if (!w || !g_haveOrig || !g_updateSubclass) return;
    SubclassRef(w) = g_origSubclass;
    DefIndexRef(w) = g_origDef;
    // Selection-safe identity: clear paint fallback so default knife is valid to equip
    if (g_off.paintKit) PaintKitRef(w) = 0;
    if (g_off.seed)     SeedRef(w) = 0;
    if (g_off.wear)     WearRef(w) = 0.0001f;
    AttrClearPaint(ItemBase(w));
    SehSubclass(w);
    g_spoofed = false;
}

static std::uint16_t DefaultKnifeDefFor(C_CSPlayerPawn* local) {
    if (!local) return 42;
    // T = 2 → weapon_knife_t (59), else CT knife (42)
    return local->m_iTeamNum() == 2 ? (std::uint16_t)59 : (std::uint16_t)42;
}

static std::uint32_t TokenForDefIndex(std::uint16_t def) {
    if (!def) return 0;
    char buf[16];
    sprintf_s(buf, "%d", (int)def);
    return MakeToken(buf);
}

// Holster path: put default knife def/subclass back so slot3 / lastinv can find it.
// Spoofed 500+ def_index while holstered is what blocks "switch to knife" on CS2.
static void RestoreKnivesForSelect(C_CSPlayerPawn* local) {
    if (!local) return;
    const std::uint16_t wantDef = g_haveOrig ? g_origDef : DefaultKnifeDefFor(local);
    const std::uint32_t wantSub = g_haveOrig && g_origSubclass
        ? g_origSubclass : TokenForDefIndex(wantDef);
    if (!wantDef) return;

    KnifeList kl = CollectKnives(local);
    for (int i = 0; i < kl.count; ++i) {
        C_CSWeaponBase* w = kl.knives[i];
        if (!w) continue;
        const std::uint16_t d = DefIndexRef(w);
        // Already default CT/T knife — leave def, fix subclass if needed
        if (d == 42 || d == 59) {
            if (wantSub && SubclassRef(w) != wantSub)
                SubclassRef(w) = wantSub;
            continue;
        }
        if (!IsKnifeDef(d) && !IsKnife((C_BaseEntity*)w))
            continue;
        SubclassRef(w) = wantSub;
        DefIndexRef(w) = wantDef;
        if (g_off.paintKit) PaintKitRef(w) = 0;
        if (g_off.seed)     SeedRef(w) = 0;
        if (g_off.wear)     WearRef(w) = 0.0001f;
        AttrClearPaint(ItemBase(w));
        SehSubclass(w);
        if (!g_haveOrig) {
            g_origDef = wantDef;
            g_origSubclass = wantSub;
            g_origWeapon = w;
            g_haveOrig = true;
        }
    }
    g_spoofed = false;
    g_appliedIdx = -1;
}

static void* GetItemSchema(); // defined below

static int FindPaintListIndex(std::uint16_t def, int paintId) {
    if (!def || paintId <= 0) return 0;
    void* schema = GetItemSchema();
    if (schema && g_findItemDef)
        KnifeSkins::EnsureKnife(def, schema, g_findItemDef);
    const auto& kits = KnifeSkins::KitsFor(def);
    for (int i = 0; i < (int)kits.size(); ++i) {
        if (kits[i].id == paintId)
            return i;
    }
    return 0;
}

// Clamp indices + resolve paint_kit list index from paint_kit_id (and vice-versa)
static void SanitizeSkinConfig() {
    const int nKnives = KnifeCount();
    if (Config::knife_index < 0 || Config::knife_index >= nKnives)
        Config::knife_index = 0;
    if (Config::knife_index > 0) {
        const std::uint16_t def = kKnives[Config::knife_index].def_index;
        if (Config::knife_paint_kit_id > 0) {
            const int found = FindPaintListIndex(def, Config::knife_paint_kit_id);
            // only update index when kit list actually contains the id
            if (found > 0 || (found == 0 && KnifeSkins::KitIdAt(def, 0) == Config::knife_paint_kit_id))
                Config::knife_paint_kit = found;
        } else if (Config::knife_paint_kit > 0) {
            Config::knife_paint_kit_id = KnifeSkins::KitIdAt(def, Config::knife_paint_kit);
            if (Config::knife_paint_kit_id > 0) {
                const int found = FindPaintListIndex(def, Config::knife_paint_kit_id);
                if (found > 0)
                    Config::knife_paint_kit = found;
            }
        }
    } else {
        Config::knife_paint_kit = 0;
        Config::knife_paint_kit_id = 0;
    }
    if (Config::knife_wear < 0.0001f) Config::knife_wear = 0.0001f;
    if (Config::knife_wear > 1.f) Config::knife_wear = 1.f;
    if (Config::knife_seed < 0) Config::knife_seed = 0;

    const int nGloves = GloveCount();
    if (Config::glove_index < 0 || Config::glove_index >= nGloves)
        Config::glove_index = 0;
    if (Config::glove_index > 0) {
        const std::uint16_t def = Gloves()[Config::glove_index].def_index;
        if (Config::glove_paint_kit_id > 0) {
            Config::glove_paint_kit = FindPaintListIndex(def, Config::glove_paint_kit_id);
        } else if (Config::glove_paint_kit > 0) {
            Config::glove_paint_kit_id = KnifeSkins::KitIdAt(def, Config::glove_paint_kit);
            if (Config::glove_paint_kit_id > 0)
                Config::glove_paint_kit = FindPaintListIndex(def, Config::glove_paint_kit_id);
        }
    } else {
        Config::glove_paint_kit = 0;
        Config::glove_paint_kit_id = 0;
    }
    if (Config::glove_wear < 0.0001f) Config::glove_wear = 0.0001f;
    if (Config::glove_wear > 1.f) Config::glove_wear = 1.f;
    if (Config::glove_seed < 0) Config::glove_seed = 0;

    if (Config::weapon_selected < 1 || Config::weapon_selected > 70)
        Config::weapon_selected = 7;
    for (int def = 1; def <= 70; ++def) {
        auto& s = Config::weapon_skin[def];
        if (s.paint_kit_id > 0) {
            s.paint_kit = FindPaintListIndex((std::uint16_t)def, s.paint_kit_id);
        } else if (s.paint_kit > 0) {
            s.paint_kit_id = KnifeSkins::KitIdAt((std::uint16_t)def, s.paint_kit);
            if (s.paint_kit_id > 0)
                s.paint_kit = FindPaintListIndex((std::uint16_t)def, s.paint_kit_id);
        }
        if (s.wear < 0.0001f) s.wear = 0.0001f;
        if (s.wear > 1.f) s.wear = 1.f;
        if (s.seed < 0) s.seed = 0;
    }

    if (Config::agent_ct_def < 0) Config::agent_ct_def = 0;
    if (Config::agent_t_def < 0) Config::agent_t_def = 0;
}

// Item system singleton → schema (CCStrike15ItemSystem+0x8)
// GetStaticData body (IDA 0x1810F33A0): mov rbx,[rip] singleton; schema=*(sys+8)
static void* GetItemSchema() {
    if (g_itemSchema && Mem::IsUserPtr(g_itemSchema)) return g_itemSchema;
    if (!g_itemSystemSingleton) return nullptr;
    __try {
        void* sys = *g_itemSystemSingleton;
        if (!sys || !Mem::IsUserPtr(sys)) return nullptr;
        void* schema = *(void**)((std::uint8_t*)sys + 0x8);
        if (!schema || !Mem::IsUserPtr(schema)) return nullptr;
        g_itemSchema = schema;
        return schema;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static int CurrentPaintKitId(const KnifeInfo& k) {
    if (!k.def_index) return 0;
    // paint_kit_id is source of truth (menu/config). Index alone is unreliable
    // when kit list isn't built yet (FindPaintListIndex returns 0 → vanilla).
    if (Config::knife_paint_kit_id > 0)
        return Config::knife_paint_kit_id;
    if (Config::knife_paint_kit > 0)
        return KnifeSkins::KitIdAt(k.def_index, Config::knife_paint_kit);
    return 0;
}

// Prefer game Find(schema, def, 0); fallback walk map @+0xF8
// Node 24B: key@0 value@8 chain@16 (IDA sub_1810939C0 / sub_1807FC9E0)
static void* FindItemDef(std::uint16_t defIdx) {
    void* schema = GetItemSchema();
    if (!schema) return nullptr;
    if (g_findItemDef) {
        __try {
            void* def = g_findItemDef(schema, (int)defIdx, 0);
            if (def && Mem::IsUserPtr(def)) return def;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try {
        const auto* base = (std::uint8_t*)schema;
        const int count = *(int*)(base + 0xF8);
        const int capFlags = *(int*)(base + 0xFC);
        if (count <= 0 || count > 20000) return nullptr;
        if ((capFlags & 0x7FFFFFFF) == 0) return nullptr;
        auto* nodes = *(std::uint8_t**)(base + 0x100);
        if (!nodes || !Mem::IsUserPtr(nodes)) return nullptr;
        for (int i = 0; i < count; ++i) {
            std::uint8_t* n = nodes + 24ull * (std::uint32_t)i;
            if (*(int*)(n + 16) < -1) continue;
            if (*(int*)n != (int)defIdx) continue;
            void* def = *(void**)(n + 8);
            if (def && Mem::IsUserPtr(def)) return def;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// CEconItemDefinition::m_pszModelName @ +0x148 / view @ +0x348
// MUST run AFTER def_index write so lookup key matches selected knife.
static const char* SchemaModelPath(void* weapon) {
    __try {
        const std::uint16_t defIdx = DefIndexRef(weapon);
        if (!defIdx) return nullptr;
        void* def = FindItemDef(defIdx);
        if (!def) return nullptr;
        const char* model = *(const char**)((std::uint8_t*)def + g_off.defModelName);
        if (model && Mem::IsReadable(model, 8) && model[0] && std::strstr(model, ".vmdl"))
            return model;
        const char* vm = *(const char**)((std::uint8_t*)def + g_off.defViewModel);
        if (vm && Mem::IsReadable(vm, 8) && vm[0] && std::strstr(vm, ".vmdl"))
            return vm;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// Prefer def+0x348 for HUD/viewmodel when present and different from world path
static const char* SchemaViewModelPath(void* weapon) {
    __try {
        const std::uint16_t defIdx = DefIndexRef(weapon);
        if (!defIdx) return nullptr;
        void* def = FindItemDef(defIdx);
        if (!def) return nullptr;
        const char* vm = *(const char**)((std::uint8_t*)def + g_off.defViewModel);
        if (vm && Mem::IsReadable(vm, 8) && vm[0] && std::strstr(vm, ".vmdl"))
            return vm;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static const char* CachedModelForDef(std::uint16_t defIdx) {
    const int n = KnifeCount();
    for (int i = 0; i < n && i < 32; ++i) {
        if (kKnives[i].def_index == defIdx && g_liveModel[i] && g_liveModel[i][0])
            return g_liveModel[i];
    }
    return nullptr;
}

static void CacheLiveModels() {
    if (!GetItemSchema()) return;
    const int n = KnifeCount();
    int hit = 0;
    for (int i = 0; i < n && i < 32; ++i) {
        g_liveModel[i] = nullptr;
        if (!kKnives[i].def_index) continue;
        void* def = FindItemDef(kKnives[i].def_index);
        if (!def) continue;
        __try {
            const char* model = *(const char**)((std::uint8_t*)def + g_off.defModelName);
            if (model && Mem::IsReadable(model, 8) && model[0] && std::strstr(model, ".vmdl")) {
                g_liveModel[i] = model;
                ++hit;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (hit > 0) g_liveModelReady = true;
}

static const char* ResolveModelPath(void* weapon, const KnifeInfo& k) {
    // skichanger uses live schema model_path (def+0x148). Prefer that; hardcoded fallback.
    if (const char* c = CachedModelForDef(k.def_index)) return c;
    if (const char* s = SchemaModelPath(weapon)) return s;
    if (k.model_path && *k.model_path) return k.model_path;
    return nullptr;
}

static void SetModelBoth(void* world, C_BaseEntity* hud, const char* model) {
    if (!model || !*model) return;
    SehSetModel(world, model);
    if (hud) SehSetModel(hud, model);
}

static void SetMeshBoth(void* world, C_BaseEntity* hud, std::uint64_t mesh) {
    SehMeshEntity(world, mesh);
    if (hud) SehMeshEntity(hud, mesh);
}

// Light HUD rebind — SetModel only when HUD entity/path changes.
// Never AnimGraphRebuild here (tears idle/draw). skichanger only set_model+mesh.
static bool PushHudOnly(C_CSWeaponBase* w, C_CSPlayerPawn* local, const KnifeInfo& k) {
    if (!w || !local || !k.def_index) return false;
    const char* model = ResolveModelPath(w, k);
    if (!model || !*model) return false;

    C_BaseEntity* hud = GetHudWeapon((C_BaseEntity*)w, local);
    if (!hud) return false;

    const int paintKitId = CurrentPaintKitId(k);
    bool oldModel = false;
    if (paintKitId > 0) {
        if (void* schema = GetItemSchema())
            oldModel = KnifeSkins::UsesOldModel(schema, paintKitId);
    }
    const std::uint64_t mesh = oldModel ? 1ull : 2ull;

    if (hud != g_lastHudEnt || model != g_lastHudModel) {
        SehSetModel(hud, model);
        g_lastHudEnt = hud;
        g_lastHudModel = model;
    }
    SehMeshEntity(hud, mesh);
    return true;
}

// -----------------------------------------------------------------------
// process_knife — EXACT skichanger (Downloads/skichanger/features/skin_changer):
// def+quality+idHigh → init flags → set_model(world+HUD) → mesh → paint
// → composite → attrs → name → subclass → skin → weapon_data → clear_hud
// -----------------------------------------------------------------------
static bool ProcessKnife(C_CSWeaponBase* w, const KnifeInfo& k, C_CSPlayerPawn* local, bool force) {
    if (!w || !g_updateSubclass || !k.def_index) return false;

    // HARD GUARD — never touch non-knife (log had AK7/deagle1 rewritten to 507)
    const std::uint16_t curDef = DefIndexRef(w);
    if (!IsKnifeDef(curDef) && curDef != k.def_index)
        return false;

    if (g_itemSystemSingleton && !g_liveModelReady)
        CacheLiveModels();

    const std::uint32_t token = MakeKnifeToken(k);
    if (!token) return false;

    const int paintKitId = CurrentPaintKitId(k);
    const float wear = ClampWear(Config::knife_wear);
    const int seed = Config::knife_seed;

    CaptureOrig(w);

    const bool configChanged =
        (g_lastPaintKitId != paintKitId) ||
        (g_lastWear != wear) ||
        (g_lastSeed != seed) ||
        (g_appliedIdx < 0);
    if (curDef == k.def_index && !configChanged && !force)
        return false;

    void* itemView = ItemBase(w);

    // 1. identity — skichanger: idHigh only (NOT low/full/account)
    DefIndexRef(w)    = k.def_index;
    QualityRef(w)     = kQualityUnusual;
    ApplyItemIdHighOnly(itemView);
    InitializedRef(w) = true;
    DisallowSOCRef(w) = false;
    RestoreMatRef(w)  = true;

    // 2. model
    const char* model = CachedModelForDef(k.def_index);
    if (!model) model = SchemaModelPath(w);
    if (!model) model = k.model_path;

    C_BaseEntity* hud = GetHudWeapon((C_BaseEntity*)w, local);
    if (model && *model) {
        SehSetModel(w, model);
        if (hud) SehSetModel(hud, model);
    }

    // 3. mesh before paint (skichanger knife: old?1:2)
    bool oldModel = false;
    if (paintKitId > 0) {
        if (void* schema = GetItemSchema())
            oldModel = KnifeSkins::UsesOldModel(schema, paintKitId);
    }
    const std::uint64_t kMesh = oldModel ? 1ull : 2ull;
    hud = GetHudWeapon((C_BaseEntity*)w, local);
    SehMeshEntity(w, kMesh);
    if (hud) SehMeshEntity(hud, kMesh);

    // 4. paint fallbacks
    if (g_off.paintKit) PaintKitRef(w) = paintKitId;
    if (g_off.wear)     WearRef(w) = wear;
    if (g_off.seed)     SeedRef(w) = seed;

    // 5. composite BEFORE attrs (skichanger)
    SehComposite(w);

    // 6. attrs after composite — create only (no invalidate / construct_paint_kit write)
    if (paintKitId > 0)
        AttrApply(itemView, paintKitId, wear, seed);
    else
        AttrClearPaint(itemView);

    // 7. custom name
    ApplyCustomName(itemView);

    // 8. subclass after composite+attrs
    SubclassRef(w) = token;
    SehSubclass(w);

    void* vdata = VDataPtr(w);
    if (!vdata) {
        Con::Warn("knife VData miss def=%u tok=0x%08X", (unsigned)k.def_index, token);
        if (g_haveOrig) {
            SubclassRef(w) = g_origSubclass;
            DefIndexRef(w) = g_origDef;
            SehSubclass(w);
        }
        g_spoofed = false;
        return false;
    }

    // 9. skin + weapon_data — no post-skin HUD rebind/AnimRebuild
    SehSkin(w);
    SehWeaponData(w);
    ClearHudWeaponIconFor((C_BaseEntity*)w);

    int postPaint = 0;
    int attrSz = -1;
    int attrPaint = -1;
    int attrSeed = -1;
    float attrWear = -1.f;
    std::uint32_t idLow = 0, idHigh = 0;
    __try {
        if (g_off.paintKit) postPaint = PaintKitRef(w);
        if (AttrVec* av = AttrVecPtr(itemView)) {
            attrSz = (int)(av->size & 0xFFFFFFFFu);
            if (attrSz > 0 && av->ptr && Mem::IsUserPtr((void*)av->ptr)) {
                auto* attrs = (EconItemAttribute*)av->ptr;
                for (int i = 0; i < attrSz && i < 16; ++i) {
                    if (attrs[i].def_index == kAttrPaint)   attrPaint = (int)attrs[i].value;
                    if (attrs[i].def_index == kAttrPattern) attrSeed  = (int)attrs[i].value;
                    if (attrs[i].def_index == kAttrWear)    attrWear  = attrs[i].value;
                }
            }
        }
        if (g_off.itemIDLow)  idLow  = *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDLow);
        if (g_off.itemIDHigh) idHigh = *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDHigh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Con::Info(
        "knife %s def=%u→%u tok=0x%08X paint=%d post=%d attrN=%d a6=%d a7=%d a8=%.4f mesh=%llu old=%d "
        "idH=0x%08X idL=0x%08X model=%s vdata=%p hud=%p",
        k.display ? k.display : "?",
        (unsigned)curDef, (unsigned)DefIndexRef(w), token, paintKitId, postPaint, attrSz,
        attrPaint, attrSeed, attrWear, (unsigned long long)kMesh, oldModel ? 1 : 0,
        idHigh, idLow, model ? model : "(null)", vdata, (void*)hud);

    g_lastPaintKitId = paintKitId;
    g_lastWear = wear;
    g_lastSeed = seed;
    g_lastPaintIdx = Config::knife_paint_kit;
    g_spoofed = true;
    g_lastHudEnt = hud;
    g_lastHudModel = model;
    return true;
}

// -----------------------------------------------------------------------
// run — skichanger stage 7: 6 frames on team/spawn/weapon-list/config only
// -----------------------------------------------------------------------
static void RequestUpdate(const char* why, int frames = kUpdateFrames) {
    if (frames > g_frames) {
        Con::Info("knife window +%d why=%s", frames, why ? why : "?");
        g_frames = frames;
    }
}

static void RunApply(C_CSPlayerPawn* local) {
    if (!g_ready || !local) return;

    if (!Config::knife_changer) {
        if (g_spoofed || g_haveOrig) {
            KnifeList kl = CollectKnives(local);
            if (kl.count > 0) RestoreKnife(kl.knives[0], "feature_off");
            RestoreKnivesForSelect(local);
        }
        g_appliedIdx = -1;
        g_lastCfgEn  = false;
        g_wasHoldingKnife = false;
        return;
    }

    const int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount()) {
        if (g_spoofed || g_haveOrig) {
            KnifeList kl = CollectKnives(local);
            if (kl.count > 0) RestoreKnife(kl.knives[0], "default");
            RestoreKnivesForSelect(local);
        }
        g_appliedIdx = -1;
        g_wasHoldingKnife = false;
        return;
    }

    const std::uint8_t team = local->m_iTeamNum();
    KnifeList kl = CollectKnives(local);

    // skichanger weapon_changed = def-index list of inventory knives
    static std::uint16_t s_lastDefs[8]{};
    static int s_lastDefN = -1;
    bool weaponChanged = (s_lastDefN != kl.count);
    if (!weaponChanged) {
        for (int i = 0; i < kl.count; ++i) {
            if (s_lastDefs[i] != DefIndexRef(kl.knives[i])) {
                weaponChanged = true;
                break;
            }
        }
    }

    const bool knifeIdxChanged = (g_lastCfgIdx != idx) || !g_lastCfgEn;
    const bool skinCfgChanged =
        (g_lastPaintIdx != Config::knife_paint_kit) ||
        (g_lastPaintKitId != Config::knife_paint_kit_id) ||
        (g_lastWear != Config::knife_wear) ||
        (g_lastSeed != Config::knife_seed);
    const bool teamChanged = (g_lastTeam != 0 && team != g_lastTeam);

    if (knifeIdxChanged || skinCfgChanged || teamChanged || weaponChanged || g_appliedIdx != idx)
        RequestUpdate(knifeIdxChanged ? "menu" : (weaponChanged ? "weapon_list" : "state"), kUpdateFrames);

    for (int i = 0; i < kl.count && i < 8; ++i)
        s_lastDefs[i] = DefIndexRef(kl.knives[i]);
    s_lastDefN = kl.count;

    g_lastCfgIdx = idx;
    g_lastCfgEn  = true;
    g_lastTeam   = team;
    g_lastKnifeCount = kl.count;
    g_wasHoldingKnife = local->GetActiveWeapon() &&
        IsKnifeDef(DefIndexRef(local->GetActiveWeapon()));

    if (kl.count <= 0 || g_frames <= 0)
        return;

    const KnifeInfo& k = kKnives[idx];
    bool did = false;
    // skichanger: only real knife entities, force=true during window
    for (int i = 0; i < kl.count; ++i) {
        if (!IsKnifeDef(DefIndexRef(kl.knives[i])) && DefIndexRef(kl.knives[i]) != k.def_index)
            continue;
        if (ProcessKnife(kl.knives[i], k, local, true)) did = true;
    }

    if (did) {
        g_appliedIdx = idx;
        if (g_regenSkins) {
            __try { g_regenSkins(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    --g_frames;
}

} // namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
const KnifeInfo* Knives()   { return kKnives; }
int              KnifeCount(){ return (int)(sizeof(kKnives) / sizeof(kKnives[0])); }

bool KnifeSkinsReady() {
    const int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount()) return false;
    void* schema = GetItemSchema();
    if (!schema || !g_findItemDef) return false;
    return KnifeSkins::EnsureKnife(kKnives[idx].def_index, schema, g_findItemDef);
}

const std::vector<KnifePaintKit>& KnifePaintKits() {
    static std::vector<KnifePaintKit> out;
    out.clear();
    const int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount()) {
        out.push_back({ 0, "Vanilla", "Vanilla", 1 });
        return out;
    }
    KnifeSkinsReady();
    const auto& kits = KnifeSkins::KitsFor(kKnives[idx].def_index);
    out.reserve(kits.size());
    for (const auto& k : kits)
        out.push_back({ k.id, k.name, k.token, k.rarity });
    if (out.empty())
        out.push_back({ 0, "Vanilla", "Vanilla", 1 });
    return out;
}

const char* SimpleNameFor(std::uint16_t defIndex) {
    return KnifeSkins::SimpleNameFor(defIndex);
}

void PrefetchDef(std::uint16_t defIndex) {
    if (!defIndex) return;
    void* schema = GetItemSchema();
    if (!schema || !g_findItemDef) return;
    KnifeSkins::EnsureKnife(defIndex, schema, g_findItemDef);
}

int KnifePaintKitId(int listIndex) {
    const int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount()) return 0;
    return KnifeSkins::KitIdAt(kKnives[idx].def_index, listIndex);
}

bool Init() {
    if (!ResolveSchemaOffs()) {
        g_ready = false;
        return false;
    }

    if (HMODULE t0 = GetModuleHandleA("tier0.dll"))
        g_makeToken = (FnStringToken)GetProcAddress(t0, "?Make@CUtlStringToken@@SA?AV1@PEBD@Z");

    auto* p = M::FindPattern("client",
        "4C 8B DC 53 48 81 EC ? ? ? ? 48 8B 41 10 48 8B D9 8B 50 30 C1 EA 04");
    if (!p) p = M::FindPattern("client", "48 8B 41 10 48 8B D9 8B 50 30 C1 EA 04");
    if (p) g_updateSubclass = (FnUpdateSubclass)p;

    p = M::FindPattern("client",
        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71");
    if (p) g_setMesh = (FnSetMeshGroupMask)p;

    p = M::FindPattern("client",
        "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 40");
    if (!p) p = M::FindPattern("client",
        "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24");
    if (p) g_setModel = (FnSetModel)p;

    p = M::FindPattern("client",
        "40 55 57 48 83 EC 28 4C 89 74 24 58 48 8B F9 80 FA FF 75 04 0F B6 51 18");
    if (!p) p = M::FindPattern("client",
        "40 55 57 48 83 EC 28 4C 89 74 24 58 48 8B F9 80 FA FF");
    if (p) g_animRebuild = (FnAnimRebuild)p;

    p = M::FindPattern("client",
        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D B9 ? ? ? ? 48 8B F1 48 8B 1F 48 85 DB 0F 84");
    if (p) g_invalidateCache = (FnInvalidateAttrCache)p;

    // construct_paint_kit: 48 89 5C 24 10 56 48 83 EC 20 48 8B 01 FF 50 18
    p = M::FindPattern("client",
        "48 89 5C 24 ? 56 48 83 EC ? 48 8B 01 FF 50 18");
    if (!p) p = M::FindPattern("client",
        "48 89 5C 24 ? 56 48 83 EC ? 48 8B 01 FF 50");
    if (p) g_constructPaintKit = (FnConstructPaintKit)p;

    p = M::FindPattern("client",
        "48 83 EC ? E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 10");
    if (p) g_regenSkins = (FnRegenerateSkins)p;

    p = M::FindPattern("client", "40 55 56 48 83 EC 28 8B EA 48 8B F1 83 FA 01");
    if (p) g_weaponHudSync = (FnWeaponHudModelSync)p;

    p = M::FindPattern("client",
        "40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24 ? 48 8D 88 58 02 00 00");
    if (!p) p = M::FindPattern("client",
        "40 53 48 83 EC ? 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24 ? 48 8D 88 58 02 00 00");
    if (p) g_findHudElement = (FnFindHudElement)p;

    p = M::FindPattern("client", "E8 ? ? ? ? 8B F8 C6 84 24");
    if (p) {
        g_clearHudIcon = (FnClearHudWeaponIcon)M::GetAbsoluteAddress(p, 1, 0);
    } else {
        p = M::FindPattern("client", "4C 8B DC 55 57 48 83 EC 48 48 63 41 68");
        if (p) g_clearHudIcon = (FnClearHudWeaponIcon)p;
    }

    p = M::FindPattern("client",
        "40 56 48 83 EC 20 48 89 5C 24 30 48 8B F1 48 8B 1D ? ? ? ? 48 85 DB 75 ? B9 10 00 00 00");
    if (!p) p = M::FindPattern("client",
        "40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? 48 85 DB 75 ? B9 10 00 00 00");
    if (p)
        g_itemSystemSingleton = (void**)M::GetAbsoluteAddress(p + 0x0E, 3, 0);

    p = M::FindPattern("client",
        "48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 89 54 24 ? 48 81 C1 D8 00 00 00");
    if (!p) p = M::FindPattern("client",
        "48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 89 54 24");
    if (p) g_findItemDef = (FnFindItemDef)p;

    if (HMODULE t0 = GetModuleHandleA("tier0.dll")) {
        g_gameAlloc = (FnGameAlloc)GetProcAddress(t0, "MemAlloc_AllocFunc");
        g_gameFree  = (FnGameFree)GetProcAddress(t0, "MemAlloc_FreeFunc");
    }

    if (g_itemSystemSingleton)
        CacheLiveModels();

    // Do not build paint kits at Init — lazy from menu (FS walk is heavy / fault-prone)

    InitKillfeed();
    Internal::InitGloveWeaponFns();

    g_ready = (g_updateSubclass != nullptr && g_setModel != nullptr && g_off.ready);
    Con::Info("skin fns: subclass=%p setmodel=%p invcache=%p constructpk=%p regen=%p alloc=%p paintOff=0x%X attrList=0x%X item=0x%X attrMgr=0x%X",
        (void*)g_updateSubclass, (void*)g_setModel,
        (void*)g_invalidateCache, (void*)g_constructPaintKit, (void*)g_regenSkins,
        (void*)g_gameAlloc, g_off.paintKit, g_off.attrList, g_off.item, g_off.attrMgr);
    return g_ready;
}

bool Ready() { return g_ready; }

void Invalidate() {
    // If we still know orig knife, leave haveOrig so holster restore works after menu toggle
    g_appliedIdx    = -1;
    g_spoofed       = false;
    g_frames        = kCfgUpdateFrames;
    g_lastCfgIdx    = -1;
    g_lastCfgEn     = false;
    g_lastPaintKitId = -1;
    g_lastWear       = -1.f;
    g_lastSeed       = -1;
    g_lastPaintIdx   = -1;
    g_lastHudEnt    = nullptr;
    g_lastHudModel  = nullptr;
    g_needHudPush   = true;
    g_doHudSyncOnce = true;
    g_doHudIconOnce = true;
    g_hudHoldFrames = 30;
    g_wantSkinReequip = false;
    g_menuHudPending = true;   // full HUD when held / next equip
    g_menuSkinPending = false;
    g_ourAttrBlock  = nullptr; // weapon recreate; don't free foreign/stale ptr
    g_reequip       = RE_IDLE;
    g_reequipOther  = 0;
    g_reequipKnife  = 0;
    g_reequipWait   = 0;
    // keep g_wasHoldingKnife — menu edit while knife out must not look like a fresh equip

    // Glove + weapon + agent state
    InvalidateGloves();
    InvalidateWeapons();
    InvalidateAgents();
}

void OnConfigLoaded() {
    SanitizeSkinConfig();

    // Restore spoofed knife to default before wiping apply state (config may change model)
    if (H::oGetLocalPlayer) {
        C_CSPlayerPawn* local = nullptr;
        __try { local = H::oGetLocalPlayer(0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { local = nullptr; }
        if (local)
            RestoreKnivesForSelect(local);
    }

    // Full re-apply window for knife/gloves/weapons/agents
    g_haveOrig = false;
    g_origWeapon = nullptr;
    Invalidate();
}

void InvalidateSkin(bool refreshHud) {
    // Paint/wear/seed — apply while held; strip refresh; full HudSync only if asked / vanilla
    g_lastPaintKitId = -1;
    g_lastWear       = -1.f;
    g_lastSeed       = -1;
    g_lastPaintIdx   = -1;
    g_frames         = kUpdateFrames;
    g_hudHoldFrames  = 12;
    g_doHudIconOnce  = true;
    g_wantSkinReequip = false;
    g_menuSkinPending = true;
    // Vanilla →0 or explicit refreshHud needs WeaponHudSync mid-round
    if (refreshHud || Config::knife_paint_kit_id <= 0 || Config::knife_paint_kit <= 0) {
        g_doHudSyncOnce = true;
        g_menuHudPending = true;
        g_hudHoldFrames = 24;
    } else {
        g_doHudSyncOnce = false;
    }
}

void OnFrameStage(C_CSPlayerPawn* local, int stage) {
    if (stage != FRAME_NET_UPDATE_END) return;
    RunApply(local);
    RunGloveWeapon(local);
    RunAgents(local);
}

void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd) {
    if (!g_ready || !local || !cmd) return;

    // Drive weapon select state machine (mid-round HUD rebuild).
    // MUST set BASE_BITS_WEAPON_SELECT or protobuf drops nWeaponSelect.
    if (g_reequip != RE_IDLE) {
        CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
        if (base) {
            if (g_reequip == RE_AWAY) {
                if (g_reequipOther > 0) {
                    base->nWeaponSelect = g_reequipOther;
                    base->SetBits(EBaseCmdBits::BASE_BITS_WEAPON_SELECT);
                }
                g_reequip = RE_WAIT;
                g_reequipWait = 12;
            } else if (g_reequip == RE_WAIT) {
                C_CSWeaponBase* act = local->GetActiveWeapon();
                const bool stillKnife = act &&
                    (IsKnife((C_BaseEntity*)act) || IsKnifeDef(DefIndexRef(act)));
                if (!stillKnife || --g_reequipWait <= 0)
                    g_reequip = RE_BACK;
            } else if (g_reequip == RE_BACK) {
                if (g_reequipKnife > 0) {
                    base->nWeaponSelect = g_reequipKnife;
                    base->SetBits(EBaseCmdBits::BASE_BITS_WEAPON_SELECT);
                }
                g_reequip = RE_IDLE;
                g_doHudSyncOnce = true;
                g_hudHoldFrames = 90;
                RequestUpdate("reequip_back", kCfgUpdateFrames);
            }
        } else {
            g_reequip = RE_IDLE;
        }
    }
}

// -----------------------------------------------------------------------
// Killfeed — nerv FireEventClientSide player_death weapon rewrite
// -----------------------------------------------------------------------
using FnEvtGetName       = const char*(__fastcall*)(void*);
using FnEvtGetString     = const char*(__fastcall*)(void*, void*, void*);
using FnEvtSetString     = const char*(__fastcall*)(void*, void*, const char*, int);
using FnEvtGetController = void*(__fastcall*)(void*, void*);

static FnEvtGetName       g_evtGetName       = nullptr;
static FnEvtGetString     g_evtGetString     = nullptr;
static FnEvtSetString     g_evtSetString     = nullptr;
static FnEvtGetController g_evtGetController = nullptr;

// CUtlStringToken layout nerv uses (16 bytes)
struct EvtToken {
    std::uint32_t hash = 0;
    std::uint32_t pad  = 0xFFFFFFFFu;
    const char*   name = nullptr;
    explicit EvtToken(const char* s) : name(s) {
        hash = Murmur2Token(s); // same seed 0x31415926
    }
};

static std::uint32_t FnvRuntime(const char* s) {
    std::uint32_t h = val_32_const;
    for (; s && *s; ++s)
        h = (h ^ (std::uint8_t)*s) * prime_32_const;
    return h;
}

static bool EventWeaponIsKnife(const char* name) {
    if (!name || !*name) return false;
    if (std::strncmp(name, "weapon_", 7) == 0) name += 7;
    if (!std::strcmp(name, "knife") || !std::strcmp(name, "knife_t"))
        return true;
    const int n = KnifeCount();
    for (int i = 1; i < n; ++i) {
        const char* sub = kKnives[i].subclass;
        if (!sub) continue;
        if (!std::strncmp(sub, "weapon_", 7) && !std::strcmp(name, sub + 7))
            return true;
    }
    return false;
}

bool InitKillfeed() {
    auto* p = M::FindPattern("client",
        "8B 41 14 0F BA E0 1E 73 05 48 8D 41 18 C3");
    if (p) g_evtGetName = (FnEvtGetName)p;

    p = M::FindPattern("client",
        "48 83 EC 38 8B 02 48 83 C1 58 89 44 24 20 8B 42 04 89 44 24 24 48 8B 42 08 48 8D 54 24 20 48 89 44 24 28 E8 ? ? ? ? 48 83 C4 38 C3 CC CC CC 33 C9");
    if (p) g_evtGetString = (FnEvtGetString)p;

    p = M::FindPattern("client",
        "48 83 EC 38 8B 02 48 83 C1 58 89 44 24 20 41 B1 1A");
    if (p) g_evtSetString = (FnEvtSetString)p;

    p = M::FindPattern("client",
        "48 83 EC 38 8B 02 4C 8D 44 24 20");
    if (p) g_evtGetController = (FnEvtGetController)p;

    return g_evtGetName && g_evtGetString && g_evtSetString && g_evtGetController;
}

void OnFireEvent(void* gameEvent) {
    if (!gameEvent || !Mem::IsUserPtr(gameEvent) || !g_evtGetName)
        return;

    const char* evName = nullptr;
    __try { evName = g_evtGetName(gameEvent); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!evName || !Mem::IsReadable(evName, 1))
        return;

    const std::uint32_t evHash = FnvRuntime(evName);
    if (evHash == hash_32_fnv1a_const("round_start") ||
        evHash == hash_32_fnv1a_const("item_purchase")) {
        __try { OnRoundStartGloves(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    if (!g_evtGetString || !g_evtSetString || !g_evtGetController)
        return;
    if (!Config::knife_changer)
        return;

    const int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount() || !kKnives[idx].subclass)
        return;

    if (evHash != hash_32_fnv1a_const("player_death"))
        return;

    // local controller only — handle lookup by full CBaseHandle (not raw index)
    if (!H::oGetLocalPlayer || !I::GameEntity || !I::GameEntity->Instance)
        return;
    C_CSPlayerPawn* local = nullptr;
    __try { local = H::oGetLocalPlayer(0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!local || !Mem::ValidEntity(local))
        return;

    void* localCtrl = nullptr;
    __try {
        CBaseHandle hCtrl = local->m_hController();
        if (hCtrl.valid())
            localCtrl = I::GameEntity->Instance->Get(hCtrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!localCtrl || !Mem::ValidEntity(localCtrl))
        return;

    EvtToken atkTok("attacker");
    void* attacker = nullptr;
    __try { attacker = g_evtGetController(gameEvent, &atkTok); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!attacker || attacker != localCtrl)
        return;

    EvtToken wepTok("weapon");
    const char* wep = nullptr;
    __try { wep = g_evtGetString(gameEvent, &wepTok, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!wep || !Mem::IsReadable(wep, 1))
        return;
    if (!EventWeaponIsKnife(wep))
        return;

    const char* newName = kKnives[idx].subclass; // e.g. weapon_knife_karambit
    if (!newName || !Mem::IsReadable(newName, 1))
        return;
    EvtToken setTok("weapon");
    __try { g_evtSetString(gameEvent, &setTok, newName, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// -----------------------------------------------------------------------
// Internal bridge for glove_weapon.cpp
// -----------------------------------------------------------------------
namespace Internal {

bool SchemaReady() { return g_off.ready; }

void* ItemViewFromWeapon(void* weapon) {
    if (!weapon || !g_off.attrMgr || !g_off.item) return nullptr;
    return ItemBase(weapon);
}

void AttrApplyItem(void* itemView, int paintKit, float wear, int seed) {
    AttrApply(itemView, paintKit, wear, seed);
}

void AttrClearItem(void* itemView) {
    AttrClearPaint(itemView);
}

void AttrWipeItem(void* itemView) {
    AttrWipe(itemView);
}

void MeshWeapon(void* weapon, std::uint64_t mask) {
    SehMeshEntity(weapon, mask);
}

void MeshHudWeapon(void* weapon, C_CSPlayerPawn* local, std::uint64_t mask) {
    if (!weapon || !local) return;
    C_BaseEntity* hud = GetHudWeapon((C_BaseEntity*)weapon, local);
    if (hud) SehMeshEntity(hud, mask);
}

void CompositeWeapon(void* weapon) {
    if (weapon) SehComposite(weapon);
}

void SkinWeapon(void* weapon) {
    if (weapon) SehSkin(weapon);
}

void WeaponData(void* weapon) {
    if (weapon) SehWeaponData(weapon);
}

void InvalidateItemCache(void* itemView) {
    SehInvalidateCache(itemView);
}

void SyncConstructedPaintKit(void* itemView, int paintKitId) {
    SehSyncPaintKitFull(itemView, paintKitId);
}

void BustItemCache(void* itemView, C_CSPlayerPawn* /*local*/, std::uint16_t /*defIndex*/, int /*paintKitId*/, int /*seed*/) {
    ApplyItemIdHighOnly(itemView);
}

void RegenerateSkins() {
    if (!g_regenSkins) return;
    __try { g_regenSkins(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ClearHudIconFor(void* weapon) {
    ClearHudWeaponIconFor((C_BaseEntity*)weapon);
}

void* ItemSchema() {
    return GetItemSchema();
}

void* (*FindItemDefFn())(void*, int, char) {
    return g_findItemDef;
}

std::uint16_t DefIndexOf(void* weapon) {
    if (!weapon) return 0;
    __try { return DefIndexRef(weapon); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

std::uint16_t DefIndexOfItem(void* itemView) {
    if (!itemView || !g_off.defIndex) return 0;
    __try { return *(std::uint16_t*)((std::uint8_t*)itemView + g_off.defIndex); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool IsInitialized(void* itemView) {
    if (!itemView || !g_off.initialized) return false;
    __try { return *(bool*)((std::uint8_t*)itemView + g_off.initialized); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SetDefIndex(void* itemView, std::uint16_t def) {
    if (!itemView || !g_off.defIndex) return;
    __try { *(std::uint16_t*)((std::uint8_t*)itemView + g_off.defIndex) = def; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetQualityUnusual(void* itemView) {
    if (!itemView || !g_off.quality) return;
    __try { *(std::int32_t*)((std::uint8_t*)itemView + g_off.quality) = kQualityUnusual; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetItemIdsMax(void* itemView) {
    // skichanger: only m_iItemIDHigh = 0xFFFFFFFF
    ApplyItemIdHighOnly(itemView);
}

void SetItemIdHigh(void* itemView, std::uint32_t v) {
    if (!itemView || !g_off.itemIDHigh) return;
    __try { *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDHigh) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetInitialized(void* itemView, bool v) {
    if (!itemView || !g_off.initialized) return;
    __try { *(bool*)((std::uint8_t*)itemView + g_off.initialized) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetRestoreMat(void* itemView, bool v) {
    if (!itemView || !g_off.restoreMat) return;
    __try { *(bool*)((std::uint8_t*)itemView + g_off.restoreMat) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetDisallowSOC(void* itemView, bool v) {
    if (!itemView || !g_off.disallowSOC) return;
    __try { *(bool*)((std::uint8_t*)itemView + g_off.disallowSOC) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

std::uint64_t LocalSteamId(C_CSPlayerPawn* local) {
    if (!local || !I::GameEntity || !I::GameEntity->Instance) return 0;
    __try {
        CBaseHandle h = local->m_hController();
        if (!h.valid()) return 0;
        auto* ctrl = reinterpret_cast<CCSPlayerController*>(
            I::GameEntity->Instance->Get(h.index()));
        if (!ctrl || !Mem::IsUserPtr(ctrl)) return 0;
        return ctrl->m_steamID();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void SpoofGloveIdentity(void* itemView, C_CSPlayerPawn* local) {
    if (!itemView) return;
    __try {
        const std::uint32_t low = g_off.itemIDLow
            ? *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDLow) : 0;
        const std::uint64_t full = g_off.itemID
            ? *(std::uint64_t*)((std::uint8_t*)itemView + g_off.itemID) : 0;
        if (g_off.itemIDHigh)
            *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDHigh) = 0xFFFFFFFFu;
        if (g_off.itemIDLow)
            *(std::uint32_t*)((std::uint8_t*)itemView + g_off.itemIDLow) = low;
        if (g_off.itemID)
            *(std::uint64_t*)((std::uint8_t*)itemView + g_off.itemID) = full;
        if (g_off.accountID && local) {
            const std::uint64_t sid = LocalSteamId(local);
            *(std::uint32_t*)((std::uint8_t*)itemView + g_off.accountID) = (std::uint32_t)sid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetPaintFallback(void* weapon, int paint, float wear, int seed) {
    if (!weapon) return;
    __try {
        if (g_off.paintKit) PaintKitRef(weapon) = paint;
        if (g_off.seed)     SeedRef(weapon) = paint > 0 ? seed : 0;
        if (g_off.wear)     WearRef(weapon) = ClampWear(wear);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool CollectWeapons(C_CSPlayerPawn* local, C_CSWeaponBase** out, int maxOut, int& count) {
    count = 0;
    if (!local || !out || maxOut <= 0 || !I::GameEntity || !I::GameEntity->Instance)
        return false;
    CCSPlayer_WeaponServices* ws = local->GetWeaponServices();
    if (!ws || !Mem::IsUserPtr(ws)) return false;

    auto* base = (std::uint8_t*)ws + g_off.myWeapons;
    struct Try { CBaseHandle* elems; int sz; };
    Try tries[2] = {
        { *(CBaseHandle**)(base + 0), *(int*)(base + 8) },
        { *(CBaseHandle**)(base + 8), *(int*)(base + 0) },
    };
    for (auto& t : tries) {
        if (t.sz <= 0 || t.sz > 64 || !t.elems || !Mem::IsUserPtr(t.elems)) continue;
        for (int i = 0; i < t.sz && count < maxOut; ++i) {
            if (!t.elems[i].valid()) continue;
            auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(t.elems[i]);
            if (!w) continue;
            if (IsKnife((C_BaseEntity*)w) || IsKnifeDef(DefIndexRef(w))) continue;
            out[count++] = w;
        }
        if (count > 0) return true;
    }
    return count > 0;
}

void SetBodyGroupPawn(C_CSPlayerPawn* pawn) {
    if (!pawn || !g_setBodyGroup) return;
    // nerv default: "first_or_third_person", 1
    __try { g_setBodyGroup(pawn, "first_or_third_person", 1u); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        __try { g_setBodyGroup(pawn, "default_gloves", 1u); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void SetPawnModel(void* entity, const char* modelPath) {
    if (!entity || !modelPath || !modelPath[0]) return;
    SehSetModel(entity, modelPath);
}

void* EconGloves(C_CSPlayerPawn* pawn) {
    if (!pawn || !g_off.econGloves) return nullptr;
    return (std::uint8_t*)pawn + g_off.econGloves;
}

bool* NeedReapplyGloves(C_CSPlayerPawn* pawn) {
    if (!pawn || !g_off.needReapplyGloves) return nullptr;
    return (bool*)((std::uint8_t*)pawn + g_off.needReapplyGloves);
}

float LastSpawnTime(C_CSPlayerPawn* pawn) {
    if (!pawn || !g_off.lastSpawnTime) return 0.f;
    __try { return *(float*)((std::uint8_t*)pawn + g_off.lastSpawnTime); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

std::uint64_t OriginalOwnerXuid(void* weapon) {
    if (!weapon || !g_off.ownerXuidLow) return 0;
    __try {
        const std::uint32_t lo = *(std::uint32_t*)((std::uint8_t*)weapon + g_off.ownerXuidLow);
        const std::uint32_t hi = g_off.ownerXuidHigh
            ? *(std::uint32_t*)((std::uint8_t*)weapon + g_off.ownerXuidHigh) : 0;
        return (std::uint64_t)lo | ((std::uint64_t)hi << 32);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool InitGloveWeaponFns() {
    // Unique call site → CBaseModelEntity::SetBodyGroup(name) @ 0x18091F5B0 (IDA a2bd4708)
    auto* p = M::FindPattern("client", "E8 ? ? ? ? EB 0C 48 8B CF");
    if (p)
        g_setBodyGroup = (FnSetBodyGroup)M::GetAbsoluteAddress(p, 1, 0);
    if (!g_setBodyGroup) {
        p = M::FindPattern("client",
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 41 8B F8 48 8B F2 48 8B D9");
        if (p) g_setBodyGroup = (FnSetBodyGroup)p;
    }
    return g_setBodyGroup != nullptr;
}

} // namespace Internal

} // namespace SkinChanger
