#include "skinchanger.h"
#include "knife_skins.h"
#include "agent_changer.h"
#include "knife_custom.h"

#include "custom_paint.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../aim/aim.h"
#include "../backtrack/backtrack.h"
#include "../prediction/prediction.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/vfunc/vfunc.h"


#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_EntityInstance/C_EntityInstance.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/handle.h"
#include "../../../lefrizzel Ai/interfaces/CUserCmd/CUserCmd.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
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
// Competitive loadout lands late after round_start — longer knife burst
constexpr int kUpdateFrames    = 24;
constexpr int kCfgUpdateFrames = 24;
constexpr int kRoundUpdateFrames = 64;

// -----------------------------------------------------------------------
// Function pointer types
// -----------------------------------------------------------------------
using FnAnimRebuild        = void(__fastcall*)(void*, std::uint8_t);
using FnUpdateSubclass     = void(__fastcall*)(void*);
using FnSetMeshGroupMask   = void(__fastcall*)(void*, std::uint64_t);
using FnSetModel           = void(__fastcall*)(void*, const char*);
using FnBuildBoneMerge     = char(__fastcall*)(void* childNode, void* parentNode, char full);
using FnInvalidateAttrCache= void(__fastcall*)(void*);
// C_EconItemView::construct_paint_kit — IDA 0x1810F1370 (VMT[3] kit id → schema paint kit*)
using FnConstructPaintKit  = void*(__fastcall*)(void* itemView);
using FnRegenerateSkins    = void(__fastcall*)();
// IDA 0x1807E1C50 — recreate HUD when weapon/hud model handles differ (DATA_UPDATE)
using FnWeaponHudModelSync = void(__fastcall*)(void* weapon, unsigned int updateType);
// nerv: HudWeaponSelection icon clear (bottom weapon strip)
using FnFindHudElement     = std::uintptr_t(__fastcall*)(const char* name);
using FnClearHudWeaponIcon = std::int64_t(__fastcall*)(std::uintptr_t hudWeapons, std::int32_t index, std::int64_t unk);
// IDA sub_180E49630 — full strip rebuild from live weapon entities (clear alone is not enough)
using FnUpdateWeaponSelection = void(__fastcall*)(void* hudWeaponSelection, void* localPawn);
// tier0: static CUtlStringToken CUtlStringToken::Make(const char*) — returns hash in RAX (MSVC x64)
using FnStringToken        = std::uint32_t(__fastcall*)(const char*);
// schema find: CEconItemDefinition* Find(schema, defIndex, bDefault) — IDA 0x1810939C0
using FnFindItemDef        = void*(__fastcall*)(void* schema, int defIdx, char allowDefault);
// CBaseModelEntity::SetBodyGroup(name, value) — IDA 0x18091F5B0
using FnSetBodyGroup       = void(__fastcall*)(void* entity, const char* name, unsigned int value);
// Material apply (Andromeda/IDA) — NOT weapon VMT[10]/[110]
// UpdateCompositeMaterial(compositeOwner@weapon+0x608, true)
using FnUpdateCompositeMat = void(__fastcall*)(void* compositeOwner, bool force);
// UpdateCompositeMaterialSet(weapon, false)
using FnUpdateCompositeSet = void(__fastcall*)(void* weapon, bool unk);
// UpdateSkin(weapon, true)
using FnUpdateSkin         = void(__fastcall*)(void* weapon, bool force);
// C_EconItemView::paintkit_prefab() — rebuild paint kit from fallbacks
using FnPaintkitPrefab     = void*(__fastcall*)(void* itemView);
// IDA ApplyEconCustomization 0x1807E2E20 — clientside_reload_custom_econ
using FnApplyEconCustomization = void(__fastcall*)(void* econEntity, char flags);
// IDA SetDynamicAttributeValue (float) 0x1810467B0
using FnSetDynamicAttrFloat = void(__fastcall*)(void* attrOwner, void* attrDef, float* value);
// IDA GetAttributeDefinitionByName 0x1810912C0
using FnGetAttrDefByName = void*(__fastcall*)(void* schema, const char* name);
// IDA GetCustomPaintKitIndex 0x181091B80 — Doppler/style index from itemView
using FnGetCustomPaintKitIndex = int(__fastcall*)(void* itemView);

// -----------------------------------------------------------------------
// Resolved pointers — all verified unique in live client.dll (IDA)
// -----------------------------------------------------------------------
FnUpdateSubclass      g_updateSubclass    = nullptr; // 0x180206430
FnSetMeshGroupMask    g_setMesh           = nullptr; // 0x180a6c550
FnSetModel            g_setModel          = nullptr; // 0x180920690
FnAnimRebuild         g_animRebuild       = nullptr; // IDA AnimGraphRebuild 0x1808F1280
FnBuildBoneMerge      g_boneMerge         = nullptr; // IDA BuildBoneMergeWork 0x180983900
FnInvalidateAttrCache g_invalidateCache   = nullptr; // 0x181100e60
FnConstructPaintKit   g_constructPaintKit = nullptr; // 0x1810F1370
FnRegenerateSkins     g_regenSkins        = nullptr; // 0x1807eb300
FnWeaponHudModelSync  g_weaponHudSync     = nullptr; // 0x1807E1C50
FnFindHudElement      g_findHudElement    = nullptr; // nerv find_hud_element
FnClearHudWeaponIcon  g_clearHudIcon      = nullptr; // nerv clear_hud_weapon_icon
FnUpdateWeaponSelection g_updateWeaponSel = nullptr; // IDA 0x180e49630
FnStringToken         g_makeToken         = nullptr; // tier0 ?Make@CUtlStringToken@@SA?AV1@PEBD@Z
FnSetBodyGroup        g_setBodyGroup      = nullptr; // 0x18091F5B0
FnUpdateCompositeMat  g_updateComposite   = nullptr; // 0x181420100
FnUpdateCompositeSet  g_updateCompositeSet = nullptr; // 0x1807c5ca0
FnUpdateSkin          g_updateSkin        = nullptr; // 0x180aaaa20
FnPaintkitPrefab      g_paintkitPrefab    = nullptr; // 0x1810a16b0
FnApplyEconCustomization g_applyEcon      = nullptr;
FnSetDynamicAttrFloat g_setDynAttrFloat   = nullptr;
FnGetAttrDefByName    g_getAttrDefByName  = nullptr;
FnGetCustomPaintKitIndex g_getCustomPaintKitIndex = nullptr;
// C_CSWeaponBase +0x608 = CCompositeMaterialOwner (IDA: 48 81 C1 08 06 00 00)
constexpr std::uint32_t kOffCompositeMaterial = 0x608;
// C_CSWeaponBase +0x1760 = CHandle → C_CS2HudModelWeapon (IDA sub_1807E1C50)
constexpr std::uint32_t kOffWeaponHudModel = 0x1760;

static void ClearHudNodraw(C_BaseEntity* e) {
    if (!e) return;
    __try {
        // C_BaseEntity::m_fEffects @ 0x52C — clear EF_NODRAW (bit 5) if set
        auto* fx = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(e) + 0x52C);
        *fx &= ~0x20u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

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
bool          g_forceRestore   = false; // config load disabled knife → restore next frame
std::uint32_t g_origSubclass   = 0;
std::uint16_t g_origDef        = 0;
void*         g_origWeapon     = nullptr;
std::uint8_t  g_lastTeam       = 0;
std::uint32_t g_lastActiveHdl  = 0;
int           g_lastKnifeCount = -1;
float         g_lastKnifeSpawn = 0.f;
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
// Debounce round_start cascade (same as weapons)
int           g_knifeRoundCooldown = 0;
// Death→respawn: event path often misses free'd pawn (LocalController null).
// Life-edge in OnFrameStage is the reliable clear + reapply arm.
static bool   g_wasLocalAlive = false;
// Skip composite/SetModel while inventory/HUD still half-built after spawn.
static int    g_spawnGraceFrames = 0;
// Frames to wait after first alive tick before skin writes (TDM crash window).
// TDM inventory / weapon services lag after respawn — 12 frames was too short
// (composite/SetModel on half-init pawn → crash).
// Short hold so inventory/scene exist; long grace delayed knife/weapon paint after respawn.
constexpr int kSpawnGraceFrames = 8;

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

// Per-itemView attr blocks we allocated. ONE global g_ourAttrBlock caused
// knife wipe to free pistol attrs (or vice versa) on respawn — knife paint
// bled onto pistol, knife lost skin.
static std::unordered_map<void*, void*> g_ourAttrByItem;
static void* GetItemSchema(); // forward — used by AttrApply econ path

static AttrVec* AttrVecPtr(void* itemView) {
	if (!itemView || !g_off.attrList) return nullptr;
	// CAttributeList::m_Attributes is at +0x08 (schema); never use 0 — that is the list header
	const std::uint32_t attrOff = g_off.attributes ? g_off.attributes : 0x8u;
	std::uint8_t* list = (std::uint8_t*)itemView + g_off.attrList;
	return reinterpret_cast<AttrVec*>(list + attrOff);
}

// nerv econ_item_attribute_manager: remove then create paint/pattern/wear.
// create() only allocates when vec empty — caller must wipe first on full apply.
static void AttrWipe(void* itemView); // forward

// ---- SEH helpers: NO C++ objects with destructors (Debug C2712) ----
static bool SehReadAttrVec(AttrVec* vec, int* outSz, void** outPtr)
{
	if (outSz) *outSz = 0;
	if (outPtr) *outPtr = nullptr;
	if (!vec)
		return false;
	__try {
		if (!Mem::IsUserPtr(vec))
			return false;
		const int sz32 = (int)(vec->size & 0xFFFFFFFFu);
		void* p = (void*)vec->ptr;
		if (outSz) *outSz = sz32;
		if (outPtr) *outPtr = p;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static void SehDetachAttrVec(AttrVec* vec)
{
	if (!vec)
		return;
	__try {
		if (!Mem::IsUserPtr(vec))
			return;
		vec->size = 0;
		vec->ptr = 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static bool SehSoftUpdateAttrs(void* block, int sz32, float paintF, float seedF, float wearF)
{
	if (!block || sz32 < 3 || sz32 >= 64)
		return false;
	bool hasPaint = false, hasPattern = false, hasWear = false;
	__try {
		if (!Mem::IsUserPtr(block))
			return false;
		auto* attrs = (EconItemAttribute*)block;
		for (int i = 0; i < sz32; ++i) {
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
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return hasPaint && hasPattern && hasWear;
}

static void SehZeroPaintAttrs(void* block, int sz32)
{
	if (!block || sz32 <= 0 || sz32 >= 64)
		return;
	__try {
		if (!Mem::IsUserPtr(block))
			return;
		auto* attrs = (EconItemAttribute*)block;
		for (int i = 0; i < sz32; ++i) {
			auto& a = attrs[i];
			if (a.def_index == kAttrPaint || a.def_index == kAttrPattern || a.def_index == kAttrWear) {
				a.value = a.init_value = 0.f;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static void SehSetDynAttrOne(void* schema, void* itemView, const char* name, float v)
{
	if (!schema || !itemView || !name || !g_getAttrDefByName || !g_setDynAttrFloat)
		return;
	void* def = nullptr;
	__try {
		def = g_getAttrDefByName(schema, name);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		def = nullptr;
	}
	if (!def)
		return;
	float tmp = v;
	__try {
		g_setDynAttrFloat(itemView, def, &tmp);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// Install private 3-attr block into vec (raw only). outBlock set for map ownership.
static bool SehInstallAttrBlock(
	AttrVec* vec, float paintF, float seedF, float wearF, void** outBlock)
{
	if (outBlock) *outBlock = nullptr;
	if (!vec || !g_gameAlloc)
		return false;
	constexpr std::size_t n = 3;
	auto* attrs = (EconItemAttribute*)g_gameAlloc(n * sizeof(EconItemAttribute));
	if (!attrs)
		return false;
	memset(attrs, 0, n * sizeof(EconItemAttribute));
	attrs[0].def_index = kAttrPaint;
	attrs[0].value = attrs[0].init_value = paintF;
	attrs[1].def_index = kAttrPattern;
	attrs[1].value = attrs[1].init_value = seedF;
	attrs[2].def_index = kAttrWear;
	attrs[2].value = attrs[2].init_value = wearF;
	__try {
		if (!Mem::IsUserPtr(vec))
			return false;
		vec->size = n;
		vec->ptr = (std::uintptr_t)attrs;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (outBlock)
		*outBlock = attrs;
	return true;
}

static bool SoftUpdateOurAttrs(void* itemView, AttrVec* vec, float paintF, float seedF, float wearF) {
	if (!itemView || !vec) return false;
	// Map ops outside SEH (Debug C2712)
	auto it = g_ourAttrByItem.find(itemView);
	if (it == g_ourAttrByItem.end() || !it->second)
		return false;
	int sz32 = 0;
	void* block = nullptr;
	if (!SehReadAttrVec(vec, &sz32, &block))
		return false;
	if (block != it->second)
		return false;
	return SehSoftUpdateAttrs(block, sz32, paintF, seedF, wearF);
}

static void AttrApply(void* itemView, int paintKit, float wear, int seed) {
	if (!itemView || paintKit <= 0) return;
	AttrVec* vec = AttrVecPtr(itemView);
	if (!vec) return;

	const float paintF = (float)paintKit;
	const float seedF = (float)(seed >= 0 ? seed : 0);
	const float wearF = ClampWear(wear);

	// Optional engine path (when resolved) — still fall through to vec create
	if (g_setDynAttrFloat && g_getAttrDefByName) {
		void* schema = GetItemSchema();
		if (schema) {
			// No lambda + __try (C2712) — plain SEH helpers
			SehSetDynAttrOne(schema, itemView, "set item texture preference", paintF);
			SehSetDynAttrOne(schema, itemView, "set item texture seed", seedF);
			SehSetDynAttrOne(schema, itemView, "set item texture wear", wearF);
		}
	}

	// Soft path ONLY on our own per-item block — never mutate game/shared attrs
	// in place (that was knife paint writing into pistol ItemView).
	if (SoftUpdateOurAttrs(itemView, vec, paintF, seedF, wearF))
		return;

	// Always wipe this itemView then create a private 3-attr block
	AttrWipe(itemView);
	vec = AttrVecPtr(itemView);
	if (!vec) return;
	SehDetachAttrVec(vec);
	void* block = nullptr;
	if (!SehInstallAttrBlock(vec, paintF, seedF, wearF, &block) || !block)
		return;
	// Map insert outside any __try
	g_ourAttrByItem[itemView] = block;
}

static void SehApplyEconCustomization(void* econEntity) {
	if (!econEntity || !g_applyEcon) return;
	__try { g_applyEcon(econEntity, 1); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Detach attr vector. NEVER free on wipe — respawn reuses/frees itemViews and
// free-after-detach double-freed → crash on death. Leak small 3×attr blocks only.
static void AttrWipe(void* itemView) {
	if (!itemView) return;
	// Drop ownership first so soft-update cannot touch a wiped view (STL outside SEH)
	g_ourAttrByItem.erase(itemView);
	AttrVec* vec = AttrVecPtr(itemView);
	if (!vec) return;
	SehDetachAttrVec(vec);
}

// Call on map load / local death — drop all ownership (entities gone).
static void ClearAllOurAttrs() {
	g_ourAttrByItem.clear();
}

// Vanilla restore — zero paint/pattern/wear in place, or free our per-item block.
static void AttrClearPaint(void* itemView) {
	if (!itemView) return;
	AttrVec* vec = AttrVecPtr(itemView);
	if (!vec) return;

	int sz32 = 0;
	void* block = nullptr;
	if (!SehReadAttrVec(vec, &sz32, &block))
		return;
	if (sz32 <= 0 || sz32 >= 64 || !block || !Mem::IsUserPtr(block))
		return;

	// Map find outside SEH — Debug C2712 if iterator lives in __try function
	auto it = g_ourAttrByItem.find(itemView);
	if (it != g_ourAttrByItem.end() && it->second && block == it->second) {
		AttrWipe(itemView);
		return;
	}

	SehZeroPaintAttrs(block, sz32);
}

// IDA sub_1810F2D20: HUD name from CEconItemDescription cached at itemView+0x200.
// Zeroing forces YieldingFillOutEconItemDescription on next strip rebuild
// → shows "★ Karambit | Doppler" from paint attr + def_index.
// skichanger: also write m_szCustomName when user set a tag.
static void BustItemNameCache(void* itemView) {
	if (!itemView) return;
	__try {
		*(std::uintptr_t*)((std::uint8_t*)itemView + 0x200) = 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ApplyItemName(void* itemView, const char* customName) {
	if (!itemView) return;
	__try {
		if (g_off.customName) {
			char* dst = (char*)((std::uint8_t*)itemView + g_off.customName);
			if (customName && customName[0])
				strncpy_s(dst, 161, customName, _TRUNCATE);
			else
				dst[0] = '\0';
		}
		// always bust description cache so HUD re-resolves skin name from paint kit
		*(std::uintptr_t*)((std::uint8_t*)itemView + 0x200) = 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ApplyCustomName(void* itemView) {
	// only user tag into m_szCustomName; paint kit name comes from description rebuild
	ApplyItemName(itemView, Config::knife_custom_name[0] ? Config::knife_custom_name : nullptr);
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

// Weapon entity safe for VMT composite/skin (not just ValidEntity).
// Half-init after respawn: scene/item missing → composite AV.
static bool WeaponSkinReady(void* w) {
    if (!w || !Mem::ValidEntity(w))
        return false;
    __try {
        CGameSceneNode* node = ((C_BaseEntity*)w)->m_pGameSceneNode();
        if (!node || !Mem::IsUserPtr(node))
            return false;
        void* item = ItemBase(w);
        if (!item || !Mem::IsUserPtr(item))
            return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
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

// Material refresh — Andromeda/IDA path (NOT weapon VMT[10]/[110]):
//   UpdateCompositeMaterial(weapon+0x608, true)
//   UpdateCompositeMaterialSet(weapon, false)
//   UpdateSkin(weapon, true)
// Wrong VMT → attrs/HUD update, paint never rebuilds on mesh.
// IDA UpdateCompositeMaterialSet (sub_1807C5CA0): full rebuild (g_vColor midhook path)
// only runs when material list counts at +0xAA8 and +0xAC0 are both <= 0.
// If lists already populated it jumps to force-update and never hits our midhook.
static void ClearCompositeListsForRebuild(void* w) {
    if (!w) return;
    __try {
        *(int*)((std::uint8_t*)w + 0xAA8) = 0;
        *(int*)((std::uint8_t*)w + 0xAC0) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SehComposite(void* w) {
    if (!w) return;
    __try {
        // nerv: vmt 10 update_composite(force) — primary path that works on all weapons
        M::CallVFunc<void*, 10U>(w, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Optional IDA material path when patterns resolved (extra force rebuild)
    if (g_updateComposite || g_updateCompositeSet) {
        __try {
            ClearCompositeListsForRebuild(w);
            if (g_updateComposite) {
                void* owner = (std::uint8_t*)w + kOffCompositeMaterial;
                g_updateComposite(owner, true);
            }
            if (g_updateCompositeSet)
                g_updateCompositeSet(w, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void SehSkin(void* w) {
    if (!w) return;
    // nerv: vmt 110 update_skin(force)
    __try { M::CallVFunc<void, 110U>(w, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_updateSkin) {
            __try { g_updateSkin(w, true); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

static void SehPaintkitPrefab(void* itemView) {
    if (!itemView || !g_paintkitPrefab) return;
    __try { g_paintkitPrefab(itemView); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void RefreshWeaponMaterials(void* w, void* itemView) {
    if (itemView) SehPaintkitPrefab(itemView);
    SehComposite(w);
    SehSkin(w);
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

// AnimGraphRebuild — IDA sub_1808F1280 (patterns.hpp AnimGraphRebuild).
// arg0 = CBaseAnimGraphController* (embedded @ body+0x510).
// mode 2 = tear graph instance only (A-pose / invisible if not recreated).
// mode 0xFF (-1) = soft: use controller+0x18 stored mode (callers prefer this).
// body = entity+0x30 (m_CBodyComponent).
static void* ResolveAnimController(void* entity) {
    if (!entity) return nullptr;
    constexpr std::uint32_t kBodyOff = 0x30u;
    constexpr std::uint32_t kAnimOffs[] = { 0x510u, 0x4E0u };
    __try {
        auto base = (std::uintptr_t)entity;
        auto body = SafeRead(base + kBodyOff);
        if (!body || !Mem::IsUserPtr((void*)body)) return nullptr;
        for (std::uint32_t animOff : kAnimOffs) {
            if (!Mem::IsReadable((void*)(body + animOff), 32))
                continue;
            return (void*)(body + animOff);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static void SehAnimRebuild(void* entity, std::uint8_t mode = 0xFFu) {
    if (!g_animRebuild || !entity) return;
    void* ctrl = ResolveAnimController(entity);
    if (!ctrl) return;
    __try { g_animRebuild(ctrl, mode); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
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

// IDA BuildBoneMergeWork 0x180983900 — rebuild HUD→arms bone merge after SetModel
static char SehBoneMergeHud(C_BaseEntity* hud, C_CSPlayerPawn* local) {
    if (!g_boneMerge || !hud || !local) return 0;
    char ok = 0;
    __try {
        C_BaseEntity* arms = GetHudArms(local);
        if (!arms) return 0;
        CGameSceneNode* child = hud->m_pGameSceneNode();
        CGameSceneNode* parent = arms->m_pGameSceneNode();
        if (!child || !parent || !Mem::IsUserPtr(child) || !Mem::IsUserPtr(parent))
            return 0;
        ok = g_boneMerge(child, parent, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
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

// Walk arms children whose m_hOwnerEntity == world weapon.
// Collect all matches — Addon can appear before C_CS2HudModelWeapon.
static int CollectHudFromArms(C_BaseEntity* world, C_CSPlayerPawn* local,
    C_BaseEntity** out, int maxOut) {
    int n = 0;
    if (!world || !local || !out || maxOut <= 0) return 0;
    if (!g_off.hudArms || !g_off.sceneChild || !g_off.sceneSibling)
        return 0;

    CBaseHandle h = *(CBaseHandle*)((std::uint8_t*)local + g_off.hudArms);
    if (!h.valid()) return 0;

    C_BaseEntity* arms = EntityByHandle(h);
    if (!arms) return 0;

    CGameSceneNode* armsNode = arms->m_pGameSceneNode();
    if (!armsNode || !Mem::IsUserPtr(armsNode)) return 0;

    auto add = [&](C_BaseEntity* e) {
        if (!e || !Mem::ValidEntity(e) || n >= maxOut) return;
        for (int i = 0; i < n; ++i)
            if (out[i] == e) return;
        out[n++] = e;
    };

    auto* child = *(CGameSceneNode**)((std::uint8_t*)armsNode + g_off.sceneChild);
    for (int g = 0; child && Mem::IsUserPtr(child) && g < 32; ++g) {
        auto* owner = *(C_BaseEntity**)((std::uint8_t*)child + g_off.sceneOwner);
        if (owner && Mem::IsUserPtr(owner)) {
            CBaseHandle oh = owner->m_hOwnerEntity();
            auto* owned = EntityByHandle(oh);
            if (owned == world)
                add(owner);
        }
        child = *(CGameSceneNode**)((std::uint8_t*)child + g_off.sceneSibling);
    }
    return n;
}

static C_BaseEntity* GetHudFromArms(C_BaseEntity* world, C_CSPlayerPawn* local) {
    C_BaseEntity* buf[8]{};
    const int n = CollectHudFromArms(world, local, buf, 8);
    return n > 0 ? buf[0] : nullptr;
}

// All FP knife HUD targets: weapon+0x1760 handle + every arms child owned by knife.
static int CollectKnifeHudTargets(C_BaseEntity* world, C_CSPlayerPawn* local,
    C_BaseEntity** out, int maxOut) {
    int n = 0;
    if (!out || maxOut <= 0) return 0;
    auto add = [&](C_BaseEntity* e) {
        if (!e || !Mem::ValidEntity(e) || n >= maxOut) return;
        for (int i = 0; i < n; ++i)
            if (out[i] == e) return;
        out[n++] = e;
    };
    // Prefer official HUD handle first (IDA WeaponHudModelSync @ +0x1760)
    add(GetHudFromWeaponHandle(world));
    C_BaseEntity* armsHud[8]{};
    const int an = CollectHudFromArms(world, local, armsHud, 8);
    for (int i = 0; i < an; ++i)
        add(armsHud[i]);
    return n;
}

// Prefer weapon+0x1760 (authoritative). Arms walk is fallback / secondary target.
static C_BaseEntity* GetHudWeapon(C_BaseEntity* world, C_CSPlayerPawn* local) {
    if (C_BaseEntity* byH = GetHudFromWeaponHandle(world))
        return byH;
    return GetHudFromArms(world, local);
}

static void ForceEntityOpaque(C_BaseEntity* e) {
    if (!e) return;
    __try {
        // C_BaseModelEntity::m_clrRender — agent FP hide uses same offset
        auto* clr = reinterpret_cast<std::uint8_t*>(e) + 0xC98;
        clr[0] = 255; clr[1] = 255; clr[2] = 255; clr[3] = 255;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SehWeaponHudSync(void* w) {
    if (!g_weaponHudSync || !w) return;
    __try { g_weaponHudSync(w, 1u); } // DATA_UPDATE path
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// skichanger item_schema.cpp 1:1:
//   find_hud_element("HudWeaponSelection")
//   base = hud - 0x98
//   data@base+0x58, count@base+0x50, handle@slot+0x38, stride 72
//   clear_hud_weapon_icon_for(weapon) only — no clear-all, no full rebuild spam
//   after any apply: regenerate_skins()
static void* g_lastLocalPawn = nullptr;

struct HudWeaponPanel {
    std::uintptr_t base = 0;
    std::uint8_t*  data = nullptr;
    int            count = 0;
};

static bool ResolveWeaponPanel(HudWeaponPanel& out) {
    out = {};
    if (!g_findHudElement) return false;
    const std::uintptr_t hud = g_findHudElement("HudWeaponSelection");
    if (!hud || !Mem::IsUserPtr((void*)hud)) return false;

    // skichanger primary: panel lives at FindHud - 0x98
    auto tryBase = [&](std::uintptr_t base) -> bool {
        if (!base || !Mem::IsUserPtr((void*)base)) return false;
        auto* data = *(std::uint8_t**)(base + 0x58);
        const int count = *(int*)(base + 0x50);
        if (!data || !Mem::IsUserPtr(data) || count <= 0 || count > 64) return false;
        out.base = base;
        out.data = data;
        out.count = count;
        return true;
    };

    if (tryBase(hud - 0x98)) return true;
    if (tryBase(hud)) return true; // Andromeda fallback if -0x98 invalid
    return false;
}

// skichanger clear_hud_weapon_icons — reverse wipe every slot
static void ClearHudWeaponIconsAll() {
    if (!g_clearHudIcon) return;
    __try {
        HudWeaponPanel panel;
        if (!ResolveWeaponPanel(panel)) return;
        for (int i = panel.count - 1; i >= 0; --i)
            g_clearHudIcon(panel.base, i, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// skichanger clear_hud_weapon_icon_for — match weapon handle only (no clear-all)
// Handle store is full CBaseHandle bits; resolve via GameEntitySystem::Get(handle)
// so serial is checked (index-only lookup recycled entities after death/round).
static void ClearHudWeaponIconFor(C_BaseEntity* weapon) {
    if (!g_clearHudIcon || !weapon) return;
    if (!I::GameEntity || !I::GameEntity->Instance) return;
    __try {
        HudWeaponPanel panel;
        if (!ResolveWeaponPanel(panel)) return;
        for (int i = panel.count - 1; i >= 0; --i) {
            const std::uint32_t raw = *(std::uint32_t*)(panel.data + 72 * i + 0x38);
            // -1 / invalid sentinel
            if (raw == 0xFFFFFFFFu) continue;
            const int entry = (int)(raw & 0x7FFFu);
            const int serial = (int)(raw >> 15);
            if (entry <= 0) continue;
            CBaseHandle h(entry, serial);
            if (!h.valid()) continue;
            void* ent = nullptr;
            __try { ent = I::GameEntity->Instance->Get(h); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ent = nullptr; }
            if (ent == weapon) {
                g_clearHudIcon(panel.base, i, 0);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void RegenerateSkinsSeh() {
    if (!g_regenSkins) return;
    __try { g_regenSkins(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// skichanger end-of-apply: clear matching strip icon only
static void ForceHudWeaponRefresh(void* weapon, void* /*localPawn*/ = nullptr) {
    if (weapon)
        ClearHudWeaponIconFor((C_BaseEntity*)weapon);
}

// Refresh arms anim graph — HudArmsRefresh sig is dead on live client,
// so we tear down the arms AG2 and let it rebind on next frame
static void RefreshHudArms(C_CSPlayerPawn* local) {
    C_BaseEntity* arms = GetHudArms(local);
    if (!arms) return;
    SehAnimRebuild(arms, 2u); // tear — engine rebinds next frame
}

// -----------------------------------------------------------------------
// Knife inventory walk
// -----------------------------------------------------------------------
struct KnifeList { C_CSWeaponBase* knives[8]{}; int count = 0; };

// skichanger my_weapons walk — only def-index knives (max 2 typical)
static KnifeList CollectKnives(C_CSPlayerPawn* local) {
    KnifeList out{};
    if (!local || !I::GameEntity || !I::GameEntity->Instance || !g_off.myWeapons) return out;
    CCSPlayer_WeaponServices* ws = local->GetWeaponServices();
    if (!ws || !Mem::IsUserPtr(ws)) return out;

    __try {
        auto* base = (std::uint8_t*)ws + g_off.myWeapons;
        if (!Mem::IsReadable(base, 16)) return out;
        // C_NetworkUtlVectorBase: size@+0, ptr@+8 (match live client)
        const int sz = *(int*)(base + 0);
        auto* elems = *(CBaseHandle**)(base + 8);
        if (sz <= 0 || sz > 64 || !elems || !Mem::IsUserPtr(elems)
            || !Mem::IsReadable(elems, static_cast<std::size_t>(sz) * sizeof(CBaseHandle))) {
            // swapped layout fallback
            const int sz2 = *(int*)(base + 8);
            auto* e2 = *(CBaseHandle**)(base + 0);
            if (sz2 <= 0 || sz2 > 64 || !e2 || !Mem::IsUserPtr(e2)
                || !Mem::IsReadable(e2, static_cast<std::size_t>(sz2) * sizeof(CBaseHandle)))
                return out;
            for (int i = 0; i < sz2 && out.count < 8; ++i) {
                if (!e2[i].valid()) continue;
                auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(e2[i]);
                if (!w || !Mem::ValidEntity(w)) continue;
                if (IsKnifeDef(DefIndexRef(w)))
                    out.knives[out.count++] = w;
            }
            return out;
        }
        for (int i = 0; i < sz && out.count < 8; ++i) {
            if (!elems[i].valid()) continue;
            auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(elems[i]);
            if (!w || !Mem::ValidEntity(w)) continue;
            if (IsKnifeDef(DefIndexRef(w)))
                out.knives[out.count++] = w;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = {};
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
    if (!local || !I::GameEntity || !I::GameEntity->Instance || !g_off.myWeapons) return 0;
    CCSPlayer_WeaponServices* ws = local->GetWeaponServices();
    if (!ws || !Mem::IsUserPtr(ws)) return 0;
    int found = 0;
    __try {
        auto* base = (std::uint8_t*)ws + g_off.myWeapons;
        if (!Mem::IsReadable(base, 16)) return 0;
        struct Try { CBaseHandle* elems; int sz; };
        Try tries[2] = {
            { *(CBaseHandle**)(base + 0), *(int*)(base + 8) },
            { *(CBaseHandle**)(base + 8), *(int*)(base + 0) },
        };
        for (auto& t : tries) {
            if (t.sz <= 0 || t.sz > 64 || !t.elems || !Mem::IsUserPtr(t.elems)) continue;
            if (!Mem::IsReadable(t.elems, static_cast<std::size_t>(t.sz) * sizeof(CBaseHandle)))
                continue;
            for (int i = 0; i < t.sz; ++i) {
                if (!t.elems[i].valid()) continue;
                auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(t.elems[i]);
                if (!w || !Mem::ValidEntity(w)) continue;
                if (IsKnife((C_BaseEntity*)w) || IsKnifeDef(DefIndexRef(w))) continue;
                const int idx = EntIndex(w);
                if (idx > 0) { found = idx; return found; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return found;
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

// Lookup paint list index from already-cached kits only (no EnsureKnife).
// paint_kit_id is source of truth for apply — index is menu UX only.
// No __try: KitsFor returns std::vector (Debug C2712 with SEH).
static int FindPaintListIndexCached(std::uint16_t def, int paintId) {
    if (!def || paintId <= 0) return 0;
    const auto& kits = KnifeSkins::KitsFor(def);
    for (int i = 0; i < (int)kits.size(); ++i) {
        if (kits[i].id == paintId)
            return i;
    }
    return 0;
}

// EnsureKnife only when schema ready — never on config load for all 70 weapons.
static void SehEnsureKnife(std::uint16_t def)
{
	if (!g_ready || !g_findItemDef)
		return;
	void* schema = GetItemSchema();
	if (!schema)
		return;
	__try {
		KnifeSkins::EnsureKnife(def, schema, g_findItemDef);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static int FindPaintListIndex(std::uint16_t def, int paintId) {
    if (!def || paintId <= 0) return 0;
    SehEnsureKnife(def);
    return FindPaintListIndexCached(def, paintId);
}

// Clamp indices + resolve paint_kit list index from paint_kit_id (and vice-versa).
// Safe on menu thread: no EnsureKnife flood, no game writes.
// No outer __try: uses std::vector / string (Debug C2712).
static void SanitizeSkinConfig() {
    const int nKnives = KnifeCount();
    if (nKnives <= 0) {
        Config::knife_index = 0;
    } else if (Config::knife_index < 0 || Config::knife_index >= nKnives) {
        Config::knife_index = 0;
    }
    if (Config::knife_index > 0 && Config::knife_index < nKnives) {
        const std::uint16_t def = kKnives[Config::knife_index].def_index;
        if (Config::knife_paint_kit_id > 0) {
            // keep paint_kit_id always; only update list index if kit list has it
            const int found = FindPaintListIndexCached(def, Config::knife_paint_kit_id);
            if (found > 0 || (found == 0 && KnifeSkins::KitIdAt(def, 0) == Config::knife_paint_kit_id))
                Config::knife_paint_kit = found;
            // if list not built yet, leave paint_kit as-is — apply uses paint_kit_id
        } else if (Config::knife_paint_kit > 0) {
            Config::knife_paint_kit_id = KnifeSkins::KitIdAt(def, Config::knife_paint_kit);
            if (Config::knife_paint_kit_id <= 0)
                Config::knife_paint_kit = 0;
        } else {
            Config::knife_paint_kit = 0;
            Config::knife_paint_kit_id = 0;
        }
    } else {
        Config::knife_index = 0;
        Config::knife_paint_kit = 0;
        Config::knife_paint_kit_id = 0;
    }
    if (Config::knife_wear < 0.0001f) Config::knife_wear = 0.0001f;
    if (Config::knife_wear > 1.f) Config::knife_wear = 1.f;
    if (Config::knife_seed < 0) Config::knife_seed = 0;
    if (Config::knife_seed > 1000) Config::knife_seed = 1000;
    Config::knife_custom_name[sizeof(Config::knife_custom_name) - 1] = '\0';

    const int nGloves = GloveCount();
    if (nGloves <= 0) {
        Config::glove_index = 0;
    } else if (Config::glove_index < 0 || Config::glove_index >= nGloves) {
        Config::glove_index = 0;
    }
    if (Config::glove_index > 0 && Config::glove_index < nGloves) {
        const std::uint16_t def = Gloves()[Config::glove_index].def_index;
        if (Config::glove_paint_kit_id > 0) {
            const int found = FindPaintListIndexCached(def, Config::glove_paint_kit_id);
            if (found > 0 || (found == 0 && KnifeSkins::KitIdAt(def, 0) == Config::glove_paint_kit_id))
                Config::glove_paint_kit = found;
        } else if (Config::glove_paint_kit > 0) {
            Config::glove_paint_kit_id = KnifeSkins::KitIdAt(def, Config::glove_paint_kit);
            if (Config::glove_paint_kit_id <= 0)
                Config::glove_paint_kit = 0;
        } else {
            Config::glove_paint_kit = 0;
            Config::glove_paint_kit_id = 0;
        }
    } else {
        Config::glove_index = 0;
        Config::glove_paint_kit = 0;
        Config::glove_paint_kit_id = 0;
    }
    if (Config::glove_wear < 0.0001f) Config::glove_wear = 0.0001f;
    if (Config::glove_wear > 1.f) Config::glove_wear = 1.f;
    if (Config::glove_seed < 0) Config::glove_seed = 0;
    if (Config::glove_seed > 1000) Config::glove_seed = 1000;

    if (Config::weapon_selected < 1 || Config::weapon_selected > 70)
        Config::weapon_selected = 7;
    for (int def = 1; def <= 70; ++def) {
        auto& s = Config::weapon_skin[def];
        // paint_kit_id is apply source of truth. Never invent an id from a
        // stale list index when kits aren't built (maps to wrong / "random" kit).
        if (s.paint_kit_id > 0) {
            const int found = FindPaintListIndexCached((std::uint16_t)def, s.paint_kit_id);
            if (found > 0 || (found == 0 && KnifeSkins::KitIdAt((std::uint16_t)def, 0) == s.paint_kit_id))
                s.paint_kit = found;
            // keep paint_kit_id even if list not ready
        } else if (s.paint_kit > 0) {
            // Migrate index→id only if kit list already built (size>1)
            if (KnifeSkins::KitsFor((std::uint16_t)def).size() > 1) {
                s.paint_kit_id = KnifeSkins::KitIdAt((std::uint16_t)def, s.paint_kit);
                if (s.paint_kit_id <= 0)
                    s.paint_kit = 0;
            }
            // else: leave index; ProcessWeapons migrates when EnsureKnife succeeds
        } else {
            s.paint_kit = 0;
            s.paint_kit_id = 0;
        }
        if (s.wear < 0.0001f) s.wear = 0.0001f;
        if (s.wear > 1.f) s.wear = 1.f;
        if (s.seed < 0) s.seed = 0;
        if (s.seed > 1000) s.seed = 1000;
    }

    if (Config::agent_ct_def < 0) Config::agent_ct_def = 0;
    if (Config::agent_t_def < 0) Config::agent_t_def = 0;
    // agent defs are item def indices — hard cap to avoid garbage from old configs
    if (Config::agent_ct_def > 100000) Config::agent_ct_def = 0;
    if (Config::agent_t_def > 100000) Config::agent_t_def = 0;
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

// Prefer game Find(schema, def, allowDefault); fallback walk sorted map @+0xF8
// nerv node 24B: left@0 right@4 value@8 flag@16 key@20 — def index also on value+0x10
static void* FindItemDef(std::uint16_t defIdx) {
    void* schema = GetItemSchema();
    if (!schema) return nullptr;
    if (g_findItemDef) {
        __try {
            void* def = g_findItemDef(schema, (int)defIdx, 1);
            if (def && Mem::IsUserPtr(def)) return def;
            def = g_findItemDef(schema, (int)defIdx, 0);
            if (def && Mem::IsUserPtr(def)) return def;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try {
        const auto* base = (std::uint8_t*)schema;
        const int count = *(int*)(base + 0xF8);
        if (count <= 0 || count > 20000) return nullptr;
        auto* nodes = *(std::uint8_t**)(base + 0x100);
        if (!nodes || !Mem::IsUserPtr(nodes)) return nullptr;
        for (int i = 0; i < count; ++i) {
            std::uint8_t* n = nodes + 24ull * (std::uint32_t)i;
            void* def = *(void**)(n + 8);
            if (!def || !Mem::IsUserPtr(def)) continue;
            // match node key OR definition_index on the def object
            if (*(int*)(n + 20) == (int)defIdx)
                return def;
            if (*(std::uint16_t*)((std::uint8_t*)def + 0x10) == defIdx)
                return def;
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
    // Custom knife mesh overrides stock path (drop packs under lefrizzel_models/knives).
    if (const char* custom = ActiveCustomKnifeModel())
        return custom;
    // skichanger uses live schema model_path (def+0x148). Prefer that; hardcoded fallback.
    if (const char* c = CachedModelForDef(k.def_index)) return c;
    if (const char* s = SchemaModelPath(weapon)) return s;
    if (k.model_path && *k.model_path) return k.model_path;
    return nullptr;
}

// Custom packs (nozb1 etc.) almost always only ship meshgroup 0.
// Stock knives: old model → mask 1 (group 0), new → mask 2 (group 1).
// Using mask 2 on a custom pack hides the mesh → invisible / pink / wrong.
static bool CustomKnifeMeshActive() {
    return Config::custom_knife && Config::custom_knife_path[0] != '\0';
}

static std::uint64_t ResolveKnifeMeshMask(int paintKitId) {
    if (CustomKnifeMeshActive())
        return 1ull; // group 0 — pack body
    bool oldModel = false;
    if (paintKitId > 0) {
        if (void* schema = GetItemSchema())
            oldModel = KnifeSkins::UsesOldModel(schema, paintKitId);
    }
    return oldModel ? 1ull : 2ull;
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
    const std::uint64_t mesh = ResolveKnifeMeshMask(paintKitId);

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
// Light rebind: UpdateSubclass / round recreate HUD entity after first paint.
// Without this, paint stays on world knife but FP viewmodel stays default.
static void RebindKnifeHud(C_CSWeaponBase* w, C_CSPlayerPawn* local, const KnifeInfo& k) {
    if (!w || !local || !k.def_index) return;
    const char* model = ResolveModelPath(w, k);
    if (!model || !*model) return;
    const int paintKitId = CurrentPaintKitId(k);
    const std::uint64_t mesh = ResolveKnifeMeshMask(paintKitId);
    SehSetModel(w, model);
    SehMeshEntity(w, mesh);
    C_BaseEntity* hudTargets[8]{};
    const int hn = CollectKnifeHudTargets((C_BaseEntity*)w, local, hudTargets, 8);
    for (int i = 0; i < hn; ++i) {
        SehSetModel(hudTargets[i], model);
        SehMeshEntity(hudTargets[i], mesh);
        ClearHudNodraw(hudTargets[i]);
    }
    if (hn > 0) {
        g_lastHudEnt = hudTargets[0];
        g_lastHudModel = model;
    }
    if (g_doHudSyncOnce) {
        SehWeaponHudSync(w);
        g_doHudSyncOnce = false;
    }
}

static bool ProcessKnife(C_CSWeaponBase* w, const KnifeInfo& k, C_CSPlayerPawn* local, bool force) {
    if (!w || !g_updateSubclass || !k.def_index) return false;
    // Half-init after respawn: VMT composite / SetModel on bad scene → crash
    if (!WeaponSkinReady(w))
        return false;

    // HARD GUARD — never touch non-knife (log had AK7/deagle1 rewritten to 507)
    const std::uint16_t curDef = DefIndexRef(w);
    if (!IsKnifeDef(curDef) && curDef != k.def_index)
        return false;
    // Extra: refuse if def is a normal gun range (1–70) that isn't stock knife 42/59
    if (curDef >= 1 && curDef <= 70 && curDef != 42 && curDef != 59)
        return false;

    if (g_itemSystemSingleton && !g_liveModelReady)
        CacheLiveModels();

    const std::uint32_t token = MakeKnifeToken(k);
    if (!token) return false;

    // Custom mesh ignores stock paint kit (pack has own materials).
    const bool customMesh = CustomKnifeMeshActive();
    const int paintKitId = customMesh ? 0 : CurrentPaintKitId(k);
    const float wear = customMesh ? 0.f : ClampWear(Config::knife_wear);
    const int seed = customMesh ? 0 : Config::knife_seed;

    CaptureOrig(w);

    static bool s_lastKnifeCustomColor = false;
    static bool s_lastKnifeColorsActive = false;
    static uint32_t s_lastKnifeColorHash = 0;
    static bool s_lastCustomKnife = false;
    static char s_lastCustomKnifePath[260]{};
    uint32_t colorHash = 0;
    if (!customMesh && Config::knife_custom_color && Config::knife_colors_active) {
        for (int i = 0; i < 16; ++i)
            colorHash = colorHash * 16777619u ^ (uint32_t)(Config::knife_colors[i] * 255.f);
    }
    const bool customColorChanged =
        !customMesh && (
        (s_lastKnifeCustomColor != Config::knife_custom_color) ||
        (s_lastKnifeColorsActive != Config::knife_colors_active) ||
        (Config::knife_custom_color && Config::knife_colors_active && colorHash != s_lastKnifeColorHash));
    s_lastKnifeColorsActive = Config::knife_colors_active;
    const bool customMeshChanged =
        (s_lastCustomKnife != Config::custom_knife)
        || (Config::custom_knife
            && _stricmp(s_lastCustomKnifePath, Config::custom_knife_path) != 0);
    s_lastCustomKnife = Config::custom_knife;
    if (Config::custom_knife)
        std::snprintf(s_lastCustomKnifePath, sizeof(s_lastCustomKnifePath),
            "%s", Config::custom_knife_path);
    else
        s_lastCustomKnifePath[0] = '\0';
    const bool configChanged =
        (g_lastPaintKitId != paintKitId) ||
        (g_lastWear != wear) ||
        (g_lastSeed != seed) ||
        (g_appliedIdx < 0) ||
        customColorChanged ||
        customMeshChanged;
    const int livePaint = (g_off.paintKit) ? PaintKitRef(w) : paintKitId;
    const bool engineWipe =
        (curDef != k.def_index) ||
        (!customMesh && paintKitId > 0 && livePaint != paintKitId);

    // Paint already correct — still rebind FP HUD while force/hold window open.
    // Round/team recreate HUD after first apply; early-out left viewmodel default.
    if (!configChanged && !engineWipe) {
        if (force || g_needHudPush || g_hudHoldFrames > 0)
            RebindKnifeHud(w, local, k);
        return false;
    }
    s_lastKnifeCustomColor = Config::knife_custom_color;
    s_lastKnifeColorHash = colorHash;

    void* itemView = ItemBase(w);

    // HUD strip ONLY on menu pick. Model swap after round (42→karambit) is NOT menu —
    // that was the every-round strip flicker.
    const bool refreshHud = g_doHudIconOnce;

    // 1. identity — skichanger: idHigh only (NOT low/full/account)
    // Quality Unusual (3) required so composite treats item as custom paint path.
    DefIndexRef(w)    = k.def_index;
    if (g_off.quality)
        QualityRef(w) = kQualityUnusual;
    ApplyItemIdHighOnly(itemView);
    InitializedRef(w) = true;
    DisallowSOCRef(w) = false;
    RestoreMatRef(w)  = true;

    // 2. model (world + every FP HUD child — not only first handle)
    const char* model = ResolveModelPath(w, k);
    C_BaseEntity* hudTargets[8]{};
    int hudN = CollectKnifeHudTargets((C_BaseEntity*)w, local, hudTargets, 8);
    if (model && *model) {
        SehSetModel(w, model);
        for (int i = 0; i < hudN; ++i)
            SehSetModel(hudTargets[i], model);
    }

    // 3. mesh before paint — custom packs only have group 0 (mask 1)
    const std::uint64_t kMesh = ResolveKnifeMeshMask(paintKitId);
    // HUD may appear after SetModel — re-collect
    hudN = CollectKnifeHudTargets((C_BaseEntity*)w, local, hudTargets, 8);
    SehMeshEntity(w, kMesh);
    for (int i = 0; i < hudN; ++i) {
        SehMeshEntity(hudTargets[i], kMesh);
        ClearHudNodraw(hudTargets[i]);
    }

    // 4. paint fallbacks + attrs
    // Custom mesh packs ship baked materials. Stock paint kit composite
    // (UpdateCompositeMaterial / UpdateSkin) rebinds mesh to econ paint mats
    // → pink/white/invisible. Keep paint attrs cleared and skip material rebuild.
    if (customMesh) {
        if (g_off.paintKit) PaintKitRef(w) = 0;
        if (g_off.wear)     WearRef(w) = 0.f;
        if (g_off.seed)     SeedRef(w) = 0;
        AttrClearPaint(itemView);
        SehInvalidateCache(itemView);
        SehSyncPaintKitFull(itemView, 0);
    } else {
        if (g_off.paintKit) PaintKitRef(w) = paintKitId;
        if (g_off.wear)     WearRef(w) = wear;
        if (g_off.seed)     SeedRef(w) = seed;
        if (paintKitId > 0)
            AttrApply(itemView, paintKitId, wear, seed);
        else
            AttrClearPaint(itemView);
        SehInvalidateCache(itemView);
        SehSyncPaintKitFull(itemView, paintKitId);
        SehApplyEconCustomization(w);
    }

    // 5. subclass (knife model swap needs VData before skin)
    SubclassRef(w) = token;
    SehSubclass(w);

    void* vdata = VDataPtr(w);
    if (!vdata) {
        if (g_haveOrig) {
            SubclassRef(w) = g_origSubclass;
            DefIndexRef(w) = g_origDef;
            SehSubclass(w);
        }
        g_spoofed = false;
        return false;
    }

    // 6. material + weapon_data
    // Skip composite/skin on custom mesh — preserves pack .vmat materials.
    if (!customMesh)
        RefreshWeaponMaterials(w, itemView);
    SehWeaponData(w);

    // HUD entity often recreated after subclass — rebind again
    RebindKnifeHud(w, local, k);

    // 7. name — always bust desc cache; clear strip icon ONLY on real skin/model change
    ApplyItemName(itemView, Config::knife_custom_name[0] ? Config::knife_custom_name : nullptr);
    if (refreshHud) {
        ClearHudWeaponIconFor((C_BaseEntity*)w);
        g_doHudIconOnce = false;
    }

    g_lastPaintKitId = paintKitId;
    g_lastWear = wear;
    g_lastSeed = seed;
    g_lastPaintIdx = Config::knife_paint_kit;
    g_spoofed = true;
    g_lastHudEnt = (hudN > 0) ? hudTargets[0] : GetHudWeapon((C_BaseEntity*)w, local);
    g_lastHudModel = model;
    g_needHudPush = true;
    if (g_hudHoldFrames < 12)
        g_hudHoldFrames = 12;
    return refreshHud; // true → caller may regenerate strip once
}

// -----------------------------------------------------------------------
// run — skichanger stage 7: 6 frames on team/spawn/weapon-list/config only
// -----------------------------------------------------------------------
static void RequestUpdate(const char* why, int frames = kUpdateFrames) {
    (void)why;
    if (frames > g_frames)
        g_frames = frames;
}

static void RunApply(C_CSPlayerPawn* local) {
    if (!g_ready || !local) return;

    if (!Config::knife_changer) {
        if (g_spoofed || g_haveOrig || g_forceRestore) {
            KnifeList kl = CollectKnives(local);
            if (kl.count > 0) RestoreKnife(kl.knives[0], "feature_off");
            RestoreKnivesForSelect(local);
            g_forceRestore = false;
        }
        g_appliedIdx = -1;
        g_lastCfgEn  = false;
        g_wasHoldingKnife = false;
        return;
    }

    const std::uint8_t team = local->m_iTeamNum();
    const float spawnTime = Internal::LastSpawnTime(local);
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
    const bool knifeAppeared = (kl.count > 0) && (s_lastDefN <= 0);

    // Custom knife mesh needs a real stock base (VData/anims). Auto-pick if Default.
    if (Config::custom_knife && Config::custom_knife_path[0]
        && (Config::knife_index <= 0 || Config::knife_index >= KnifeCount())) {
        const int s = AutoSelectStockKnifeForCustom(
            Config::custom_knife_path, nullptr);
        if (s > 0)
            Config::knife_index = s;
    }

    int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount()) {
        if (g_spoofed || g_haveOrig || g_forceRestore) {
            if (kl.count > 0) RestoreKnife(kl.knives[0], "default");
            RestoreKnivesForSelect(local);
            g_forceRestore = false;
        }
        g_appliedIdx = -1;
        g_wasHoldingKnife = false;
        return;
    }
    g_forceRestore = false;

    const KnifeInfo& k = kKnives[idx];
    const int wantPaint = CurrentPaintKitId(k);

    // Live paint / def reset after round — engine reverts knife without firing spawn.
    // Custom mesh: paint kit is forced 0 (pack mats) — never treat stock paint as wipe.
    const bool customMesh = CustomKnifeMeshActive();
    bool liveMismatch = false;
    for (int i = 0; i < kl.count && !liveMismatch; ++i) {
        C_CSWeaponBase* w = kl.knives[i];
        if (!w) continue;
        const std::uint16_t d = DefIndexRef(w);
        if (!IsKnifeDef(d) && d != k.def_index) continue;
        if (d != k.def_index) {
            liveMismatch = true;
            break;
        }
        if (!customMesh && wantPaint > 0 && g_off.paintKit) {
            const int live = PaintKitRef(w);
            if (live != wantPaint)
                liveMismatch = true;
        }
    }

    const bool knifeIdxChanged = (g_lastCfgIdx != idx) || !g_lastCfgEn;
    static bool s_lastCkEn = false;
    static char s_lastCkPath[260]{};
    const bool customKnifeCfg =
        (s_lastCkEn != Config::custom_knife)
        || (Config::custom_knife
            && _stricmp(s_lastCkPath, Config::custom_knife_path) != 0);
    s_lastCkEn = Config::custom_knife;
    if (Config::custom_knife)
        std::snprintf(s_lastCkPath, sizeof(s_lastCkPath), "%s", Config::custom_knife_path);
    else
        s_lastCkPath[0] = '\0';
    const bool skinCfgChanged =
        (g_lastPaintIdx != Config::knife_paint_kit) ||
        (g_lastPaintKitId != Config::knife_paint_kit_id && Config::knife_paint_kit_id != 0) ||
        (g_lastWear != Config::knife_wear) ||
        (g_lastSeed != Config::knife_seed) ||
        customKnifeCfg;
    const bool teamChanged = (g_lastTeam != 0 && team != g_lastTeam);
    const bool spawnChanged = (spawnTime != g_lastKnifeSpawn);
    // Menu change = real HUD refresh. Engine wipe = re-apply paint, no strip spam.
    const bool menuChange = knifeIdxChanged || skinCfgChanged;

    // Team swap: CT knife 42 ↔ T knife 59 — drop stale orig so CaptureOrig retargets.
    // Keep paint cache invalid so ProcessKnife does full path, not early rebind only.
    if (teamChanged) {
        g_haveOrig = false;
        g_origWeapon = nullptr;
        g_origDef = 0;
        g_origSubclass = 0;
        g_spoofed = false;
        g_appliedIdx = -1;
        g_lastPaintKitId = -1;
        g_lastHudEnt = nullptr;
        g_lastHudModel = nullptr;
        g_needHudPush = true;
        if (g_hudHoldFrames < 48)
            g_hudHoldFrames = 48;
        // New team knife entity may not exist yet — don't burn whole window empty
        s_lastDefN = -1;
    }

    if (menuChange || teamChanged || weaponChanged
        || spawnChanged || knifeAppeared || liveMismatch || g_appliedIdx != idx) {
        // HUD strip ONLY on menu knife/skin pick
        if (menuChange)
            g_doHudIconOnce = true;
        const int fr = (spawnChanged || knifeAppeared || teamChanged)
            ? kRoundUpdateFrames
            : (menuChange ? kUpdateFrames : 16);
        RequestUpdate(menuChange ? "menu" :
            (teamChanged ? "team" :
            (spawnChanged ? "spawn" :
            (liveMismatch ? "live" :
            (knifeAppeared ? "appear" :
            (weaponChanged ? "weapon_list" : "state"))))), fr);
        if (menuChange || teamChanged)
            g_lastPaintKitId = -1;
        if (spawnChanged || teamChanged || knifeAppeared) {
            g_needHudPush = true;
            if (g_hudHoldFrames < 32)
                g_hudHoldFrames = 32;
        }
    }

    for (int i = 0; i < kl.count && i < 8; ++i)
        s_lastDefs[i] = DefIndexRef(kl.knives[i]);
    s_lastDefN = kl.count;

    g_lastCfgIdx = idx;
    g_lastCfgEn  = true;
    g_lastTeam   = team;
    g_lastKnifeCount = kl.count;
    g_wasHoldingKnife = false;
    if (C_CSWeaponBase* act = local->GetActiveWeapon()) {
        if (WeaponSkinReady(act))
            g_wasHoldingKnife = IsKnifeDef(DefIndexRef(act));
    }

    if (g_knifeRoundCooldown > 0)
        --g_knifeRoundCooldown;

    // No knife entity yet — hold frames (competitive give is late)
    if (kl.count <= 0)
        return;

    if (g_frames > 0) {
        bool didHud = false;
        bool anyTouch = false;
        for (int i = 0; i < kl.count; ++i) {
            if (!IsKnifeDef(DefIndexRef(kl.knives[i])) && DefIndexRef(kl.knives[i]) != k.def_index)
                continue;
            anyTouch = true;
            if (ProcessKnife(kl.knives[i], k, local, true)) didHud = true;
        }

        if (anyTouch)
            g_appliedIdx = idx;
        // regenerate ONLY when ProcessKnife returned true (menu/model change)
        if (didHud) {
            RegenerateSkinsSeh();
            g_doHudIconOnce = false;
            g_menuSkinPending = false;
        }
        if (anyTouch)
            g_lastKnifeSpawn = spawnTime;

        --g_frames;
        if (g_hudHoldFrames > 0)
            --g_hudHoldFrames;
        if (g_frames <= 0 && g_hudHoldFrames <= 0)
            g_needHudPush = false;
    } else {
        // Idle — live wipe OR keep FP HUD rebind while hold window open
        if (liveMismatch || g_needHudPush || g_hudHoldFrames > 0) {
            for (int i = 0; i < kl.count; ++i) {
                C_CSWeaponBase* w = kl.knives[i];
                if (!w) continue;
                const std::uint16_t d = DefIndexRef(w);
                if (!IsKnifeDef(d) && d != k.def_index) continue;
                ProcessKnife(w, k, local, true);
            }
            if (g_hudHoldFrames > 0)
                --g_hudHoldFrames;
            if (g_hudHoldFrames <= 0 && !liveMismatch)
                g_needHudPush = false;
        }
        g_lastKnifeSpawn = spawnTime;
        g_appliedIdx = idx;
    }

    // Custom mesh applied on FRAME_RENDER_END (Tonyha7 UC SetModel timing)
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
    if (!KnifeSkins::EnsureKnife(kKnives[idx].def_index, schema, g_findItemDef))
        return false;
    // size>1 = real kits; Vanilla-only still "loading" so menu can retry
    return KnifeSkins::KitsFor(kKnives[idx].def_index).size() > 1;
}

const std::vector<KnifePaintKit>& KnifePaintKits() {
    static std::vector<KnifePaintKit> out;
    static std::uint16_t s_def = 0;
    static size_t s_srcN = 0;
    const int idx = Config::knife_index;
    if (idx <= 0 || idx >= KnifeCount()) {
        if (s_def != 0 || out.size() != 1) {
            out.clear();
            out.push_back({ 0, "Vanilla", "Vanilla", 1 });
            s_def = 0;
            s_srcN = 1;
        }
        return out;
    }
    const std::uint16_t def = kKnives[idx].def_index;
    // Kits only — EnsureKnife owned by KnifeSkinsReady (budgeted once/frame)
    const auto& kits = KnifeSkins::KitsFor(def);
    // Avoid rebuild+alloc every ImGui frame (was hitch on Knives subtab)
    if (def == s_def && kits.size() == s_srcN && !out.empty())
        return out;
    out.clear();
    out.reserve(kits.size() ? kits.size() : 1);
    for (const auto& k : kits)
        out.push_back({ k.id, k.name, k.token, k.rarity });
    if (out.empty())
        out.push_back({ 0, "Vanilla", "Vanilla", 1 });
    s_def = def;
    s_srcN = kits.size() ? kits.size() : 1;
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
    if (!p) p = M::FindPattern("client",
        "40 55 57 48 83 EC 28 4C 89 74 24 58 48 8B F9 80");
    if (p) g_animRebuild = (FnAnimRebuild)p;

    // BuildBoneMergeWork — IDA 0x180983900
    p = M::FindPattern("client",
        "40 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8D 6C 24 50 80 A1 06");
    if (!p) p = M::FindPattern("client",
        "40 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8D 6C 24 50 80 A1");
    if (p) g_boneMerge = (FnBuildBoneMerge)p;

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

    // skichanger clear_hud_weapon_icon: E8 ? ? ? ? 8B F8 C6 84 24 (call resolve)
    p = M::FindPattern("client", "E8 ? ? ? ? 8B F8 C6 84 24");
    if (p)
        g_clearHudIcon = (FnClearHudWeaponIcon)M::GetAbsoluteAddress(p, 1, 0);
    if (!g_clearHudIcon) {
        p = M::FindPattern("client", "4C 8B DC 55 57 48 83 EC ? 48 63 41 ? 48 8B F9");
        if (!p) p = M::FindPattern("client", "4C 8B DC 55 57 48 83 EC 48 48 63 41 68");
        if (p) g_clearHudIcon = (FnClearHudWeaponIcon)p;
    }

    // optional full rebuild (not used on hot path — skichanger only clears matching icon)
    p = M::FindPattern("client", "48 85 D2 0F 84 ? ? ? ? 48 8B C4 48 89 50 ? 56");
    if (p) g_updateWeaponSel = (FnUpdateWeaponSelection)p;

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

    // UpdateCompositeMaterial(compositeOwner, true) — IDA unique @ 0x181420100
    p = M::FindPattern("client",
        "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC 20 44 0F B6 F2");
    if (!p) {
        // fallback: call site inside UpdateCompositeMaterialSet
        p = M::FindPattern("client", "E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24");
        if (p) p = M::GetAbsoluteAddress(p, 1, 0);
    }
    if (p) g_updateComposite = (FnUpdateCompositeMat)p;

    // UpdateCompositeMaterialSet(weapon, false) — IDA unique @ 0x1807c5ca0
    p = M::FindPattern("client",
        "40 55 53 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 00 03 00 00");
    if (!p) p = M::FindPattern("client", "40 55 53 41 57 48 8D AC 24 00 FE ? ?");
    if (p) g_updateCompositeSet = (FnUpdateCompositeSet)p;

    // UpdateSkin(weapon, true) — IDA unique @ 0x180aaaa20
    p = M::FindPattern("client",
        "40 57 48 83 EC 40 48 89 5C 24 ? 48 8B F9 8B DA E8 ? ? ? ? F6 C3 01 48 8B 5C 24 ? 0F 84 ? ? ? ? F3 0F 10 87");
    if (!p) p = M::FindPattern("client",
        "40 57 48 83 EC 40 48 89 5C 24 50 48 8B F9 8B DA");
    if (p) g_updateSkin = (FnUpdateSkin)p;

    // C_EconItemView::paintkit_prefab — Andromeda
    p = M::FindPattern("client",
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B D9 48 81 C1");
    if (p) g_paintkitPrefab = (FnPaintkitPrefab)p;

    // ApplyEconCustomization — clientside_reload_custom_econ (IDA 0x1807E2E20)
    p = M::FindPattern("client",
        "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ? ? ? ? 48 8B CB E8 ? ? ? ? 48 85 C0 74");
    if (p) g_applyEcon = (FnApplyEconCustomization)p;

    // SetDynamicAttributeValue (float) — IDA 0x1810467B0 (uint @ 0x1810465A0, float +0x210)
    p = M::FindPattern("client",
        "48 89 6C 24 20 57 41 56 41 57 48 81 EC A0 00 00 00 48 8B FA C7 44 24 20 00 00 00 00 4D 8B F8");
    if (p)
        g_setDynAttrFloat = (FnSetDynamicAttrFloat)(p + 0x210);

    // GetAttributeDefinitionByName — IDA 0x1810912C0
    p = M::FindPattern("client",
        "48 89 5C 24 10 48 89 6C 24 18 57 41 56 41 57 48 83 EC 60 48");
    if (p) g_getAttrDefByName = (FnGetAttrDefByName)p;

    // GetCustomPaintKitIndex — Doppler/style (IDA 0x181091B80)
    p = M::FindPattern("client",
        "48 89 5C 24 08 57 48 83 EC 40 8B 15 ? ? ? ? 48 8B F9 65 48 8B 04 25 58 00 00 00 B9 68 00 00 00");
    if (p) g_getCustomPaintKitIndex = (FnGetCustomPaintKitIndex)p;

    if (HMODULE t0 = GetModuleHandleA("tier0.dll")) {
        g_gameAlloc = (FnGameAlloc)GetProcAddress(t0, "MemAlloc_AllocFunc");
        g_gameFree  = (FnGameFree)GetProcAddress(t0, "MemAlloc_FreeFunc");
    }

    if (g_itemSystemSingleton)
        CacheLiveModels();

    // Do not build paint kits at Init — lazy from menu (FS walk is heavy / fault-prone)

    InitKillfeed();
    Internal::InitGloveWeaponFns();
    CustomPaint::Install();

    Con::Info("SkinEcon applyEcon=%p setDyn=%p attrDef=%p paintIdx=%p",
        (void*)g_applyEcon, (void*)g_setDynAttrFloat,
        (void*)g_getAttrDefByName, (void*)g_getCustomPaintKitIndex);
    if (g_applyEcon)
        Con::Ok("SkinChanger: ApplyEconCustomization ready");
    else
        Con::OffsetMiss("SkinChanger::ApplyEconCustomization");

    g_ready = (g_updateSubclass != nullptr && g_setModel != nullptr && g_off.ready);
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
    g_ourAttrByItem.clear(); // drop ownership map; blocks freed with entities
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
    // Clamp only — no EnsureKnife flood, no entity writes from menu thread
    __try { SanitizeSkinConfig(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Config::knife_index = 0;
        Config::glove_index = 0;
    }

    // If new config disables knife / selects default, restore on next frame
    // (g_spoofed cleared below would otherwise skip restore)
    g_forceRestore = (!Config::knife_changer || Config::knife_index <= 0);

    // Drop apply state so next frame re-reads config cleanly.
    // Entity restore deferred to RunApply — not from ImGui click.
    g_haveOrig = false;
    g_origWeapon = nullptr;
    g_appliedIdx = -1;
    g_spoofed = false;
    g_lastPaintKitId = -1;
    g_lastWear = -1.f;
    g_lastSeed = -1;
    g_lastPaintIdx = -1;
    g_lastCfgIdx = -1;
    g_lastCfgEn = false;

    // Open knife/glove/weapon/agent apply windows
    Invalidate();
}

void InvalidateSkin(bool refreshHud) {
    // paint/wear/seed/nametag change — open apply window + force strip rebuild
    g_lastPaintKitId = -1;
    g_lastWear       = -1.f;
    g_lastSeed       = -1;
    g_lastPaintIdx   = -1;
    g_frames         = kUpdateFrames;
    g_hudHoldFrames  = refreshHud ? 30 : 0;
    g_doHudIconOnce  = true;   // was false — HUD kept old knife skin name
    g_wantSkinReequip = false;
    g_menuSkinPending = true;
    g_doHudSyncOnce  = refreshHud;
    g_menuHudPending = false;
    g_needHudPush    = refreshHud;
}

// Drop sticky skin state when local dies — do NOT free attr blocks (entities own them).
static void OnLocalDeathEdge() {
    ClearAllOurAttrs();
    g_frames = 0;
    g_appliedIdx = -1;
    g_lastHudEnt = nullptr;
    g_lastHudModel = nullptr;
    g_haveOrig = false;
    g_origWeapon = nullptr;
    g_origDef = 0;
    g_origSubclass = 0;
    g_spoofed = false;
    g_needHudPush = false;
    g_hudHoldFrames = 0;
    g_doHudIconOnce = false;
    g_doHudSyncOnce = false;
    g_reequip = RE_IDLE;
    g_reequipOther = 0;
    g_reequipKnife = 0;
    g_reequipWait = 0;
    g_lastKnifeSpawn = 0.f;
    g_lastPaintKitId = -1;
    g_spawnGraceFrames = 0;
    g_lastLocalPawn = nullptr;
    // Stop glove/weapon apply mid-death (stale C_CSWeaponBase*)
    HaltGloveWeaponApply();
    // Combat: drop locks / BT / pred that hold free'd pawn or bone data
    __try { Aimbot_Reset(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { Backtrack::Clear(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { Pred::Invalidate(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// True when pawn can safely take composite/SetModel (weapon services + scene node).
static bool PawnSkinReady(C_CSPlayerPawn* local) {
    if (!local || !Mem::ValidEntity(local))
        return false;
    CCSPlayer_WeaponServices* ws = nullptr;
    __try { ws = local->GetWeaponServices(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!ws || !Mem::IsUserPtr(ws))
        return false;
    // Scene node free mid-respawn → SetModel/mesh AV even with SEH sometimes
    __try {
        CGameSceneNode* node = ((C_BaseEntity*)local)->m_pGameSceneNode();
        if (!node || !Mem::IsUserPtr(node))
            return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void OnFrameStage(C_CSPlayerPawn* local, int stage) {
    if (Config::loading.load(std::memory_order_acquire))
        return;

    // Life-edge runs even when caller only passes alive pawn — also called from
    // hooks with dead/null via OnLifeTick. Here local is SafeLocalAlive only.
    if (!local || !Mem::ValidEntity(local))
        return;

    // Respawn window: skip if not fully alive
    {
        int hp = 0;
        std::uint8_t life = 1;
        __try {
            hp = local->m_iHealth();
            life = local->m_lifeState();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (hp <= 0 || hp > 200 || life != 0)
            return;
    }

    // First alive ticks after death: arm grace then reapply windows.
    // Without grace, ProcessKnife/Weapons VMT on half-init inventory crashes TDM.
    if (!g_wasLocalAlive) {
        g_wasLocalAlive = true;
        ClearAllOurAttrs(); // belt: drop any stale itemView keys from prior life
        g_spawnGraceFrames = kSpawnGraceFrames;
        g_haveOrig = false;
        g_origWeapon = nullptr;
        g_lastHudEnt = nullptr;
        g_lastHudModel = nullptr;
        g_appliedIdx = -1;
        g_lastPaintKitId = -1;
        g_lastKnifeSpawn = 0.f;
        // Soft arm — real apply after grace when inventory exists
        OnLocalPlayerSpawnWeapons();
        RequestUpdate("spawn_edge", kRoundUpdateFrames);
        g_needHudPush = true;
        if (g_hudHoldFrames < 48)
            g_hudHoldFrames = 48;
    }

    // Custom models: UC Tonyha7 SetModel on FRAME_RENDER_END only.
    if (stage == FRAME_RENDER_END) {
        if (g_spawnGraceFrames > 0 || !PawnSkinReady(local))
            return;
        __try { RunAgents(local); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { RunCustomKnife(local); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    if (stage != FRAME_NET_UPDATE_END) return;
    g_lastLocalPawn = local;

    if (g_spawnGraceFrames > 0) {
        --g_spawnGraceFrames;
        return; // hold windows open; no entity writes yet
    }
    if (!PawnSkinReady(local))
        return;

    // Weapons BEFORE knife — knife first shared attr block with pistol on respawn.
    __try { RunGloveWeapon(local); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { RunApply(local); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!(Config::custom_model && Config::custom_model_path[0])) {
        __try { RunAgents(local); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// Called every FSN NET_UPDATE from hooks — even when dead/null local.
// Reliable death clear when player_death event filter fails (free'd pawn).
void OnLifeTick() {
    C_CSPlayerPawn* p = H::SafeLocalPlayer();
    bool alive = false;
    if (p) {
        int hp = 0;
        std::uint8_t life = 1;
        __try {
            hp = p->m_iHealth();
            life = p->m_lifeState();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            p = nullptr;
        }
        alive = p && hp > 0 && hp <= 200 && life == 0;
    }
    if (g_wasLocalAlive && !alive) {
        g_wasLocalAlive = false;
        OnLocalDeathEdge();
    }
    // alive edge handled in OnFrameStage (needs apply path)
}

void OnCreateMove(C_CSPlayerPawn* local, CUserCmd* cmd) {
    if (!g_ready || !local || !cmd) return;
    if (!Mem::ValidEntity(local))
        return;
    if (Config::loading.load(std::memory_order_acquire))
        return;
    // Alive probe — dead pawn weapon services free (TDM) → skip reequip only
    {
        int hp = 0;
        std::uint8_t life = 1;
        __try {
            hp = local->m_iHealth();
            life = local->m_lifeState();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (hp <= 0 || hp > 200 || life != 0)
            return;
    }
    // Never reequip during spawn grace / half-init weapon services
    if (g_spawnGraceFrames > 0 || !PawnSkinReady(local))
        return;

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

// Local CCSPlayerController for event filters (spawn/purchase/killfeed).
static void* LocalController() {
    if (!I::GameEntity || !I::GameEntity->Instance)
        return nullptr;
    C_CSPlayerPawn* local = H::SafeLocalPlayer();
    if (!local || !Mem::ValidEntity(local))
        return nullptr;
    void* localCtrl = nullptr;
    __try {
        CBaseHandle hCtrl = local->m_hController();
        if (hCtrl.valid())
            localCtrl = I::GameEntity->Instance->Get(hCtrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (!localCtrl || !Mem::ValidEntity(localCtrl))
        return nullptr;
    return localCtrl;
}

// Event userid/controller equals local — kill/spawn of others must NOT reapply skins.
static bool EventIsLocalController(void* gameEvent, const char* key) {
    if (!gameEvent || !key || !g_evtGetController)
        return false;
    void* localCtrl = LocalController();
    if (!localCtrl)
        return false;
    EvtToken tok(key);
    void* ctrl = nullptr;
    __try { ctrl = g_evtGetController(gameEvent, &tok); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ctrl && ctrl == localCtrl;
}

// Reapply paint after loadout reset — NEVER set g_doHudIconOnce (that is menu-only).
static void OpenRoundSkinWindows() {
    if (g_knifeRoundCooldown > 0)
        return;
    g_knifeRoundCooldown = 64;

    OnRoundStartGloves();
    // Knife reapply window — force paint path + multi-frame HUD rebind
    RequestUpdate("round_evt", kRoundUpdateFrames);
    // Force full ProcessKnife once even if paint kit id still matches (new entity).
    g_appliedIdx = -1;
    g_lastPaintKitId = -1;
    g_lastHudEnt = nullptr;
    g_lastHudModel = nullptr;
    g_needHudPush = true;
    if (g_hudHoldFrames < 48)
        g_hudHoldFrames = 48;
    // Do NOT set g_doHudIconOnce / g_menuHudPending (menu-only strip thrash).
}

// Map load / team session reset — drop all sticky apply state so first round sticks.
void OnMapLoad() {
    ClearAllOurAttrs();
    g_wasLocalAlive = false;
    g_spawnGraceFrames = 0;
    g_knifeRoundCooldown = 0;
    g_frames = 0;
    g_appliedIdx = -1;
    g_spoofed = false;
    g_haveOrig = false;
    g_origWeapon = nullptr;
    g_origDef = 0;
    g_origSubclass = 0;
    g_lastTeam = 0;
    g_lastKnifeSpawn = 0.f;
    g_lastPaintKitId = -1;
    g_lastWear = -1.f;
    g_lastSeed = -1;
    g_lastPaintIdx = -1;
    g_lastHudEnt = nullptr;
    g_lastHudModel = nullptr;
    g_needHudPush = true;
    g_hudHoldFrames = 0;
    g_doHudIconOnce = false;
    g_doHudSyncOnce = false;
    g_menuHudPending = false;
    g_menuSkinPending = false;
    g_reequip = RE_IDLE;
    OpenRoundSkinWindows();
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

    // One reapply per round boundary (debounced inside OpenRoundSkinWindows).
    // Skip buytime_ended — often fires with round_start and doubled the HUD thrash.
    if (evHash == hash_32_fnv1a_const("round_start") ||
        evHash == hash_32_fnv1a_const("round_freeze_end")) {
        __try { OpenRoundSkinWindows(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    // Local spawn only — reapply paint, no HUD.
    // Separate from round_start debounce: TDM death mid-round must still re-open.
    if (evHash == hash_32_fnv1a_const("player_spawn")) {
        if (!g_evtGetController)
            return;
        if (!EventIsLocalController(gameEvent, "userid"))
            return;
        __try {
            // Old itemViews die with pawn — drop attr ownership before reapply
            ClearAllOurAttrs();
            // Hold entity writes until inventory/scene ready (TDM crash fix)
            // Grace only if not already counting down (alive edge may have set it)
            if (g_spawnGraceFrames <= 0)
                g_spawnGraceFrames = kSpawnGraceFrames;
            OnLocalPlayerSpawnWeapons();
            // Knife path: force reapply without eating round_start debounce
            RequestUpdate("player_spawn", kRoundUpdateFrames);
            g_appliedIdx = -1;
            g_lastPaintKitId = -1;
            g_lastHudEnt = nullptr;
            g_lastHudModel = nullptr;
            g_needHudPush = true;
            if (g_hudHoldFrames < 32)
                g_hudHoldFrames = 32;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    // Local death — clear sticky state (OnLifeTick is backup if controller already free)
    if (evHash == hash_32_fnv1a_const("player_death")) {
        if (g_evtGetController && EventIsLocalController(gameEvent, "userid")) {
            __try {
                g_wasLocalAlive = false;
                OnLocalDeathEdge();
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        // fall through to knife killfeed spoof below (attacker path)
    }

    // Buy mid-round — weapons only, no full round cascade / no HUD
    if (evHash == hash_32_fnv1a_const("item_purchase")) {
        if (!g_evtGetController)
            return;
        if (!EventIsLocalController(gameEvent, "userid"))
            return;
        __try { OnItemPurchaseWeapons(); }
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

    void* localCtrl = LocalController();
    if (!localCtrl)
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
    SehInvalidateCache(itemView);
}

void ApplyEconOnWeapon(void* weapon) {
    SehApplyEconCustomization(weapon);
}

int CustomPaintKitIndex(void* itemView) {
    if (!itemView || !g_getCustomPaintKitIndex)
        return 0;
    int idx = 0;
    __try { idx = g_getCustomPaintKitIndex(itemView); }
    __except (EXCEPTION_EXECUTE_HANDLER) { idx = 0; }
    return idx;
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
    // Guns: only this weapon's C_CS2HudModelWeapon (+0x1760).
    // CollectKnifeHudTargets also walks arms children — on respawn those can
    // still be owned by the knife and steal knife mesh/paint onto the pistol FP.
    if (!weapon) return;
    (void)local;
    C_BaseEntity* hud = GetHudFromWeaponHandle((C_BaseEntity*)weapon);
    if (!hud) return;
    SehMeshEntity(hud, mask);
    ClearHudNodraw(hud);
}

void SetModelWeaponAndHud(void* weapon, C_CSPlayerPawn* local,
    const char* modelPath, std::uint64_t meshMask) {
    if (!weapon || !modelPath || !*modelPath) return;
    SehSetModel(weapon, modelPath);
    SehMeshEntity(weapon, meshMask);
    if (!local) return;
    C_BaseEntity* hudTargets[8]{};
    const int n = CollectKnifeHudTargets((C_BaseEntity*)weapon, local, hudTargets, 8);
    for (int i = 0; i < n; ++i) {
        SehSetModel(hudTargets[i], modelPath);
        SehMeshEntity(hudTargets[i], meshMask);
        ClearHudNodraw(hudTargets[i]);
    }
}

void CompositeWeapon(void* weapon) {
    if (!weapon) return;
    // nerv: fallbacks already set — composite VMT[10] is enough
    // paintkit_prefab optional when pattern resolved
    if (g_paintkitPrefab)
        SehPaintkitPrefab(ItemBase(weapon));
    SehComposite(weapon);
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

void ApplyWeaponItemName(void* itemView, std::uint16_t /*def*/, int /*paintKitId*/) {
    // weapons: no user tag field — just bust description so strip shows paint kit name
    BustItemNameCache(itemView);
}

void ClearHudIconsAll() {
    ClearHudWeaponIconsAll();
}

void RequestHudHold(int frames) {
    // skichanger does not multi-frame hold — just open apply window
    (void)frames;
    g_frames = kUpdateFrames;
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

int GetPaintFallback(void* weapon) {
    if (!weapon || !g_off.paintKit) return 0;
    __try { return PaintKitRef(weapon); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool CollectWeapons(C_CSPlayerPawn* local, C_CSWeaponBase** out, int maxOut, int& count) {
    count = 0;
    if (!local || !out || maxOut <= 0 || !I::GameEntity || !I::GameEntity->Instance)
        return false;
    if (!g_off.myWeapons)
        return false;
    CCSPlayer_WeaponServices* ws = local->GetWeaponServices();
    if (!ws || !Mem::IsUserPtr(ws)) return false;

    // SEH: myWeapons vector freed mid-respawn (TDM) — bare size/ptr read AVs
    __try {
        auto* base = (std::uint8_t*)ws + g_off.myWeapons;
        if (!Mem::IsReadable(base, 16))
            return false;
        struct Try { CBaseHandle* elems; int sz; };
        Try tries[2] = {
            { *(CBaseHandle**)(base + 0), *(int*)(base + 8) },
            { *(CBaseHandle**)(base + 8), *(int*)(base + 0) },
        };
        for (auto& t : tries) {
            if (t.sz <= 0 || t.sz > 64 || !t.elems || !Mem::IsUserPtr(t.elems)) continue;
            if (!Mem::IsReadable(t.elems, static_cast<std::size_t>(t.sz) * sizeof(CBaseHandle)))
                continue;
            for (int i = 0; i < t.sz && count < maxOut; ++i) {
                if (!t.elems[i].valid()) continue;
                auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(t.elems[i]);
                if (!w || !Mem::ValidEntity(w)) continue;
                if (IsKnife((C_BaseEntity*)w) || IsKnifeDef(DefIndexRef(w))) continue;
                out[count++] = w;
            }
            if (count > 0) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0;
        return false;
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

void RebuildPawnAnimGraph(void* entity) {
    // mode=2 = tear-only → A/T-pose on custom models (engine never re-binds animgraph).
    // mode=0 = full reload path in IDA sub_1808F1280 (loc_1808F1393: calls init +
    // writes [rax+0FB0h] state). Safe for both stock agents and custom .vmdl packs.
    SehAnimRebuild(entity, 0u);
}

void SetHudArmsModel(C_CSPlayerPawn* local, const char* modelPath) {
    if (!local || !modelPath || !modelPath[0]) return;
    C_BaseEntity* arms = GetHudArms(local);
    if (!arms) return;
    SehSetModel(arms, modelPath);
    SehAnimRebuild(arms, 0xFFu); // soft after SetModel
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
