#include "visuals.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
#include <Windows.h>
#include "../../hooks/hooks.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/gaa/gaa.h"
#include "../../../../external/imgui/imgui.h"
#include "../../interfaces/interfaces.h"
#include "../../config/config.h"
#include "../../menu/menu.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../bones/bones.h"
#include "../trace/trace.h"
#include "../glow/glow.h"
#include "../gamemode/gamemode.h"
#include "../nade_pred/nade_pred.h"
#include "../widgets/steam_avatar.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/security/vacdetect.h"
#include "assets/weapon_icons.hpp"

extern ID3D11Device* pDevice;

using namespace Esp;

LocalPlayerCached cached_local;
std::vector<PlayerCache> cached_players;
std::vector<WorldCache> cached_world;
PlantedBombInfo g_plantedBomb;

static Vector_t GetAbsOrigin(C_CSPlayerPawn* pawn) {
	if (!pawn)
		return {};
	CGameSceneNode* node = pawn->m_pGameSceneNode();
	if (node)
		return node->m_vecAbsOrigin();
	return pawn->m_vOldOrigin();
}

static bool IsDormant(C_CSPlayerPawn* pawn) {
	if (!pawn)
		return true;
	CGameSceneNode* node = pawn->m_pGameSceneNode();
	return node && node->m_bDormant();
}

static bool ResolvePawn(const CBaseHandle& h, C_CSPlayerPawn** out) {
	*out = nullptr;
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return false;

	// Prefer full-handle Get (validates index); then confirm slot identity.
	auto* pawn = I::GameEntity->Instance->Get<C_CSPlayerPawn>(h);
	if (!pawn)
		return false;

	CBaseHandle actual = pawn->handle();
	if (!actual.valid()
		|| actual.index() != h.index()
		|| actual.serial_number() != h.serial_number())
		return false;

	*out = pawn;
	return true;
}

static const char* StripWeaponPrefix(const char* nm) {
	if (!nm || !nm[0])
		return nm;
	const char* p = nm;
	if (strncmp(p, "weapon_", 7) == 0)
		p += 7;
	else if (strncmp(p, "C_Weapon", 8) == 0)
		p += 8;
	else if (strncmp(p, "CWeapon", 7) == 0)
		p += 7;
	else if (p[0] == 'C' && p[1] == '_')
		p += 2;
	else if (p[0] == 'C' && p[1] >= 'A' && p[1] <= 'Z')
		p += 1;
	return p;
}

static void CopyCleanWeaponName(const char* nm, char* buf, size_t n) {
	if (!buf || n == 0)
		return;
	buf[0] = '\0';
	if (!nm || !nm[0])
		return;

	// "weapon_ak47" / "C_WeaponAWP" / "CAK47" → short label
	const char* p = StripWeaponPrefix(nm);
	if (!p)
		return;

	size_t i = 0;
	for (; i + 1 < n && p[i]; ++i) {
		char c = p[i];
		if (c == '_') c = ' ';
		buf[i] = c;
	}
	buf[i] = '\0';
}

// Icon lookup key: lowercase, underscores (ak47 / m4a1_silencer / knife_karambit)
static void MakeWeaponIconKey(const char* nm, char* buf, size_t n) {
	if (!buf || n == 0)
		return;
	buf[0] = '\0';
	if (!nm || !nm[0])
		return;

	const char* p = StripWeaponPrefix(nm);
	if (!p)
		return;

	size_t i = 0;
	for (; i + 1 < n && p[i]; ++i) {
		unsigned char c = static_cast<unsigned char>(p[i]);
		if (c == ' ' || c == '-')
			c = '_';
		else
			c = static_cast<unsigned char>(std::tolower(c));
		buf[i] = static_cast<char>(c);
	}
	buf[i] = '\0';
}

static const char* ResolveWeaponIconGlyph(const char* key) {
	if (!key || !key[0])
		return nullptr;
	auto it = weapon_icons::icon_table.find(key);
	if (it != weapon_icons::icon_table.end() && !it->second.empty())
		return it->second.c_str();

	// knife_* fallback
	if (strncmp(key, "knife", 5) == 0) {
		it = weapon_icons::icon_table.find("knife");
		if (it != weapon_icons::icon_table.end() && !it->second.empty())
			return it->second.c_str();
	}
	return nullptr;
}

// Centered weapon glyph — same visual band as name/weapon text ESP
// Returns advance so next line sits like text-under-text (no extra gap)
static float DrawWeaponIconCentered(ImDrawList* dl, float cx, float y, ImU32 col, const char* key) {
	if (!dl || !g_WeaponIconFont)
		return 0.f;
	const char* glyph = ResolveWeaponIconGlyph(key);
	if (!glyph || !glyph[0])
		return 0.f;

	// Match default font metrics (name / "knife t" / "13m")
	const float textSz = ImGui::GetFontSize();
	const float textH = ImGui::CalcTextSize("A").y;
	const float targetH = (textH > 1.f) ? textH : textSz * 0.85f;

	// Start at text size; only scale DOWN if glyph is taller than text (never upscale)
	float iconSz = textSz * 0.95f;
	ImVec2 sz = g_WeaponIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, glyph);
	if (sz.y > targetH && sz.y > 0.5f) {
		iconSz *= targetH / sz.y;
		sz = g_WeaponIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, glyph);
	}
	if (sz.x <= 1.f)
		return 0.f;

	// Top-align like AddText for name ESP (same y origin as DrawCenteredText)
	const float x = floorf(cx - sz.x * 0.5f);
	const float drawY = floorf(y);

	const ImU32 shadow = IM_COL32(0, 0, 0, 200);
	for (int ox = -1; ox <= 1; ++ox) {
		for (int oy = -1; oy <= 1; ++oy) {
			if (ox == 0 && oy == 0) continue;
			dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x + (float)ox, drawY + (float)oy), shadow, glyph);
		}
	}
	dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x, drawY), col, glyph);

	// Advance by text line height so icon → label spacing = label → distance
	return textSz;
}

static void ReadWeaponEntityName(C_CSWeaponBase* wep, char* label, size_t labelN, char* key, size_t keyN) {
	if (label && labelN)
		label[0] = '\0';
	if (key && keyN)
		key[0] = '\0';
	if (!wep)
		return;
	__try {
		const char* raw = nullptr;
		if (H::oGetWeaponData > 0) {
			CCSWeaponBaseVData* data = wep->Data();
			if (data) {
				const char* nm = data->m_szName();
				if (nm && nm[0])
					raw = nm;
			}
		}
		if (!raw) {
			auto* inst = reinterpret_cast<CEntityInstance*>(wep);
			SchemaClassInfoData_t* cls = nullptr;
			if (inst)
				inst->dump_class_info(&cls);
			if (cls && cls->szName)
				raw = cls->szName;
		}
		if (!raw || !raw[0])
			return;
		if (label && labelN)
			CopyCleanWeaponName(raw, label, labelN);
		if (key && keyN)
			MakeWeaponIconKey(raw, key, keyN);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		if (label && labelN)
			label[0] = '\0';
		if (key && keyN)
			key[0] = '\0';
	}
}

static void ReadWeaponName(C_CSPlayerPawn* pawn, char* label, size_t labelN, char* key, size_t keyN) {
	if (label && labelN)
		label[0] = '\0';
	if (key && keyN)
		key[0] = '\0';
	if (!pawn)
		return;
	__try {
		ReadWeaponEntityName(pawn->GetActiveWeapon(), label, labelN, key, keyN);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		if (label && labelN)
			label[0] = '\0';
		if (key && keyN)
			key[0] = '\0';
	}
}

static bool AnyWorldEspEnabled() {
	// nade_warn forces world nade scan so badges share the same reliable entity pass
	// widget_bomb forces planted-C4 scan for HUD even when world bomb ESP is off
	return Config::nade_warn
		|| Config::widget_bomb
		|| Config::world_esp_weapons || Config::world_esp_bomb
		|| Config::world_esp_smoke || Config::world_esp_molotov
		|| Config::world_esp_he || Config::world_esp_flash
		|| Config::world_esp_decoy
		|| Config::glow_world_weapons || Config::glow_world_bomb
		|| Config::glow_world_grenades;
}

static Vector_t GetEntityAbsOrigin(C_BaseEntity* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return {};
	constexpr uint32_t kSceneNode = 0x330;
	constexpr uint32_t kAbsOrigin = 0xC8;
	CGameSceneNode* node = nullptr;
	if (!Mem::ReadField(ent, kSceneNode, node) || !node || !Mem::Valid(node, kAbsOrigin + 12)) {
		node = ent->m_pGameSceneNode();
		if (!node || !Mem::Valid(node, kAbsOrigin + 12))
			return {};
	}
	Vector_t o{};
	if (Mem::ReadField(node, kAbsOrigin, o)
		&& std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z))
		return o;
	return node->m_vecAbsOrigin();
}

static bool IsEntityDormant(C_BaseEntity* ent) {
	if (!ent)
		return true;
	CGameSceneNode* node = ent->m_pGameSceneNode();
	return node && node->m_bDormant();
}

// Effect timers: full handle XOR throw-origin sig. Slot reuse + lingering dead
// HE shells caused every-other-round expired timers for warn + world ESP.
struct WorldFxSlot {
	uint32_t key = 0;
	int kind = -1;
	float start = 0.f;
	bool seen = false;
	bool expired = false;
	uint32_t throwSig = 0;
};
static WorldFxSlot s_worldFx[64];
static int s_worldFxN = 0;

static uint32_t WorldHandleKey(CEntityInstance* ent, int fallbackIdx) {
	if (ent && Mem::ValidEntity(ent)) {
		CEntityIdentity* id = nullptr;
		if (Mem::ReadField(ent, 0x10, id) && id && Mem::Valid(id, 0x14)) {
			uint32_t raw = 0;
			if (Mem::ReadField(id, 0x10, raw) && (raw & 0x7FFF) != 0)
				return raw;
		}
		const CBaseHandle h = ent->handle();
		if (h.valid())
			return static_cast<uint32_t>(h.index())
				| (static_cast<uint32_t>(h.serial_number()) << 15);
	}
	return static_cast<uint32_t>(fallbackIdx);
}

static uint32_t WorldThrowSig(const Vector_t& p) {
	const int x = static_cast<int>(p.x * 0.5f);
	const int y = static_cast<int>(p.y * 0.5f);
	const int z = static_cast<int>(p.z * 0.5f);
	uint32_t h = 2166136261u;
	h = (h ^ static_cast<uint32_t>(x)) * 16777619u;
	h = (h ^ static_cast<uint32_t>(y)) * 16777619u;
	h = (h ^ static_cast<uint32_t>(z)) * 16777619u;
	return h ? h : 1u;
}

// Handle + round epoch + kind + throw origin. Live origin was mixed in before →
// key churn while airborne. Kind isolates smoke/molly/HE on recycled handles.
static uint32_t s_worldRoundEpoch = 1;
static uint32_t WorldFxKey(CEntityInstance* ent, int fallbackIdx, int nadeKind, uint32_t throwSig = 0) {
	return WorldHandleKey(ent, fallbackIdx)
		^ (s_worldRoundEpoch * 0x85EBCA6Bu)
		^ (static_cast<uint32_t>(nadeKind + 1) * 0xC2B2AE3Du)
		^ (throwSig * 0x9E3779B9u);
}

static void WorldFxBeginFrame() {
	for (int j = 0; j < s_worldFxN; ++j)
		s_worldFx[j].seen = false;
}

static void WorldFxEndFrame() {
	for (int j = 0; j < s_worldFxN; ) {
		if (!s_worldFx[j].seen) {
			s_worldFx[j] = s_worldFx[s_worldFxN - 1];
			--s_worldFxN;
		} else {
			++j;
		}
	}
}

// Wall-clock fallback only — live clear uses explode/fire flags when available.
static float WorldFxLimit(int nadeKind, bool effectActive) {
	if (nadeKind == WORLD_SMOKE) return 18.f;
	// Fire: 7s hard cap; live clear uses lit flags / postFx. Projectile: air only.
	if (nadeKind == WORLD_MOLOTOV) return effectActive ? 7.f : 2.5f;
	if (nadeKind == WORLD_DECOY) return 15.f;
	if (nadeKind == WORLD_HE || nadeKind == WORLD_FLASH) return 1.6f; // match nade_pred kDurHeFlashFuse
	return 0.f;
}

// Remaining effect time; starts track on first see. <0 = no timer.
// kindTag encodes effectActive so fire vs projectile don't share clocks.
static float WorldEffectRemaining(uint32_t handleKey, int nadeKind, bool effectActive = false,
	uint32_t throwSig = 0) {
	const float limit = WorldFxLimit(nadeKind, effectActive);
	if (limit <= 0.f || handleKey == 0)
		return -1.f;

	// Separate slots for fire vs projectile of same handle
	const int kindTag = nadeKind + (effectActive ? 100 : 0);
	const float now = static_cast<float>(ImGui::GetTime());
	for (int j = 0; j < s_worldFxN; ++j) {
		if (s_worldFx[j].key != handleKey)
			continue;

		const bool newThrow = (throwSig != 0 && s_worldFx[j].throwSig != 0
			&& s_worldFx[j].throwSig != throwSig);
		if (s_worldFx[j].kind != kindTag || newThrow) {
			s_worldFx[j].kind = kindTag;
			s_worldFx[j].start = now;
			s_worldFx[j].expired = false;
			s_worldFx[j].throwSig = throwSig;
			s_worldFx[j].seen = true;
			return limit;
		}

		if (s_worldFx[j].expired) {
			s_worldFx[j].seen = true;
			return 0.f;
		}

		if (throwSig != 0)
			s_worldFx[j].throwSig = throwSig;

		const float age = now - s_worldFx[j].start;
		// Pause/hitch only — do NOT re-anchor old tracks (round-2 "too fast"/miss)
		if (age < -0.05f) {
			s_worldFx[j].start = now;
			s_worldFx[j].seen = true;
			return limit;
		}
		if (age >= limit) {
			s_worldFx[j].expired = true;
			s_worldFx[j].seen = true;
			return 0.f;
		}
		s_worldFx[j].seen = true;
		return limit - age;
	}
	if (s_worldFxN < 64) {
		s_worldFx[s_worldFxN] = WorldFxSlot{ handleKey, kindTag, now, true, false, throwSig };
		++s_worldFxN;
	}
	return limit;
}

static bool EffectExpired(uint32_t handleKey, int nadeKind, bool effectActive = false,
	uint32_t throwSig = 0) {
	const float left = WorldEffectRemaining(handleKey, nadeKind, effectActive, throwSig);
	return left >= 0.f && left <= 0.05f;
}
static float ReadFloatSafe(const float* p) {
	float t = 0.f;
	if (!p)
		return 0.f;
	__try { t = *p; }
	__except (EXCEPTION_EXECUTE_HANDLER) { t = 0.f; }
	return t;
}

// SEH isolated — no C++ objects with dtors
static void* SafeReadVoidPtr(uintptr_t addr) {
	void* v = nullptr;
	__try { v = *reinterpret_cast<void**>(addr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return v;
}

// CS2 GlobalVars curtime — IDA: sub that does
//   mov rax, [rip+gv]; movss xmm0, [rax+30h]; ret
// (old pattern hit a different user of same gv that reads +0x44 as int).
static float GetCurTime() {
	static uintptr_t s_gvAbs = 0;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		// Preferred: dedicated curtime getter
		uintptr_t insn = M::patternScan("client", "48 8B 05 ? ? ? ? F3 0F 10 40 30 C3");
		if (!insn)
			insn = M::patternScan("client", "48 8B 05 ? ? ? ? 0F 57 C0 8B 48");
		if (insn)
			s_gvAbs = M::getAbsoluteAddress(insn, 3, 0);
	}
	if (!s_gvAbs)
		return 0.f;
	void* gv = SafeReadVoidPtr(s_gvAbs);
	if (!gv)
		return 0.f;
	const uintptr_t base = reinterpret_cast<uintptr_t>(gv);
	// IDA-confirmed curtime @ +0x30; keep fallbacks if build shifts
	const int offs[5] = { 0x30, 0x2C, 0x34, 0x38, 0x24 };
	for (int i = 0; i < 5; ++i) {
		const float t = ReadFloatSafe(reinterpret_cast<float*>(base + offs[i]));
		if (t > 1.f && t < 1.0e7f && std::isfinite(t))
			return t;
	}
	return 0.f;
}

// Wall-clock bomb fuse — immune to bad curtime after round transitions
static uint32_t s_bombTrackKey = 0;
static float s_bombEndWall = 0.f;

static bool ClassLooksLikeWeapon(const char* n) {
	if (!n || !n[0])
		return false;
	if (strstr(n, "Projectile") || strstr(n, "Planted") || strstr(n, "Inferno")
		|| strstr(n, "Player") || strstr(n, "Controller") || strstr(n, "Viewmodel")
		|| strstr(n, "HudModel") || strstr(n, "Wearable") || strstr(n, "Item")
		|| strstr(n, "Ragdoll") || strstr(n, "Chicken"))
		return false;
	// designer: weapon_ak47 / class: C_WeaponAK47 / C_AK47 / C_DEagle / C_Knife
	if (strncmp(n, "weapon_", 7) == 0)
		return true;
	if (strstr(n, "Weapon") || strstr(n, "DEagle") || strstr(n, "AK47") || strstr(n, "Knife")
		|| strstr(n, "M4A") || strstr(n, "SSG") || strstr(n, "AWP") || strstr(n, "Glock")
		|| strstr(n, "USP") || strstr(n, "P250") || strstr(n, "FiveSeven") || strstr(n, "Tec9")
		|| strstr(n, "Elite") || strstr(n, "Revolver") || strstr(n, "Negev") || strstr(n, "M249")
		|| strstr(n, "Nova") || strstr(n, "XM1014") || strstr(n, "MAG7") || strstr(n, "Sawed")
		|| strstr(n, "MAC10") || strstr(n, "MP5") || strstr(n, "MP7") || strstr(n, "MP9")
		|| strstr(n, "P90") || strstr(n, "Bizon") || strstr(n, "UMP") || strstr(n, "Galil")
		|| strstr(n, "Famas") || strstr(n, "SG556") || strstr(n, "AUG") || strstr(n, "SCAR")
		|| strstr(n, "G3SG") || strstr(n, "Taser") || strstr(n, "C4") || strstr(n, "Molotov")
		|| strstr(n, "Flashbang") || strstr(n, "HEGrenade") || strstr(n, "SmokeGrenade")
		|| strstr(n, "Decoy") || strstr(n, "Incendiary") || strstr(n, "BaseCSGrenade")
		|| strstr(n, "BasePlayerWeapon") || strstr(n, "CSWeaponBase"))
		return true;
	return false;
}

// owner handle: schema preferred, dump offset 0x520 fallback
static bool ReadOwnerHandle(C_BaseEntity* ent, CBaseHandle* out) {
	if (!ent || !out)
		return false;
	__try {
		const uint32_t sch = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_hOwnerEntity"));
		const uint32_t off = sch ? sch : 0x520u;
		*out = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uintptr_t>(ent) + off);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static bool OwnerIsLivingPlayer(const CBaseHandle& owner) {
	if (!owner.valid() || owner.index() == 0)
		return false;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return false;
	auto* ent = I::GameEntity->Instance->Get(owner);
	if (!ent)
		return false;
	SchemaClassInfoData_t* cls = nullptr;
	ent->dump_class_info(&cls);
	if (!cls || !cls->szName)
		return false;
	const char* n = cls->szName;
	if (!strstr(n, "PlayerPawn") && !strstr(n, "ObserverPawn"))
		return false;
	__try {
		auto* base = reinterpret_cast<C_BaseEntity*>(ent);
		return base->m_iHealth() > 0 && base->m_lifeState() == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return true; // assume held if unreadable
	}
}

// Dropped if no owner, or owner is not an alive pawn (still on ground)
static bool IsDroppedWeapon(C_BaseEntity* ent) {
	CBaseHandle owner{};
	if (!ReadOwnerHandle(ent, &owner))
		return false;
	if (!owner.valid() || owner.index() == 0)
		return true;
	return !OwnerIsLivingPlayer(owner);
}

// CEntityInstance::m_pEntity @+0x10, designerName @+0x20 (dump-stable)
static const char* GetDesignerName(CEntityInstance* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	__try {
		CEntityIdentity* id = nullptr;
		if (!Mem::ReadField(ent, 0x10, id) || !id || !Mem::Valid(id, 0x28))
			id = ent->m_pEntityIdentity();
		if (!id || !Mem::Valid(id, 0x28))
			return nullptr;
		const char* p = nullptr;
		if (!Mem::ReadField(id, 0x20, p) || !p)
			p = id->m_designerName();
		if (!p || !Mem::IsReadable(p, 2) || !p[0])
			return nullptr;
		return p;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Flying nade / inferno detection (class + designer)
static int ClassifyWorldNade(const char* cls, const char* designer) {
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";
	if (std::strstr(a, "SmokeGrenadeProjectile") || std::strcmp(a, "C_SmokeGrenadeProjectile") == 0
		|| (std::strstr(b, "smokegrenade") && std::strstr(b, "projectile")))
		return WORLD_SMOKE;
	if (std::strstr(a, "Inferno") || std::strstr(b, "inferno"))
		return WORLD_MOLOTOV; // fire pool — label set by caller
	if (std::strstr(a, "MolotovProjectile") || std::strcmp(a, "C_MolotovProjectile") == 0
		|| std::strstr(a, "IncendiaryGrenadeProjectile")
		|| ((std::strstr(b, "molotov") || std::strstr(b, "incendiary") || std::strstr(b, "incgrenade"))
			&& std::strstr(b, "projectile")))
		return WORLD_MOLOTOV;
	if (std::strstr(a, "HEGrenadeProjectile") || std::strcmp(a, "C_HEGrenadeProjectile") == 0
		|| (std::strstr(b, "hegrenade") && std::strstr(b, "projectile")))
		return WORLD_HE;
	if (std::strstr(a, "FlashbangProjectile") || std::strcmp(a, "C_FlashbangProjectile") == 0
		|| (std::strstr(b, "flashbang") && std::strstr(b, "projectile")))
		return WORLD_FLASH;
	if (std::strstr(a, "DecoyProjectile") || std::strcmp(a, "C_DecoyProjectile") == 0
		|| (std::strstr(b, "decoy") && std::strstr(b, "projectile")))
		return WORLD_DECOY;
	return -1;
}

struct PlantedBombState {
	bool ok = false;
	bool ticking = false;
	bool exploded = false;
	bool defused = false;
	bool defusing = false;
	int site = -1;
	float blow = 0.f;
	float defuseEnd = 0.f;
	float timerLength = 40.f; // IDA: C_PlantedC4->m_flTimerLength @ 0x11D8
};

static PlantedBombState ReadPlantedBomb(C_PlantedC4* bomb) {
	PlantedBombState s{};
	if (!bomb)
		return s;
	__try {
		s.ticking = bomb->m_bBombTicking();
		s.exploded = bomb->m_bHasExploded();
		s.defused = bomb->m_bBombDefused();
		s.defusing = bomb->m_bBeingDefused();
		s.site = bomb->m_nBombSite();
		s.blow = bomb->m_flC4Blow();
		s.defuseEnd = bomb->m_flDefuseCountDown();
		s.timerLength = bomb->m_flTimerLength();
		if (!(s.timerLength >= 10.f && s.timerLength <= 60.f))
			s.timerLength = 40.f;
		s.ok = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		s.ok = false;
	}
	return s;
}

static void DrawTextOutlined(ImDrawList* dl, float x, float y, ImU32 col, const char* text) {
	if (!dl || !text || !text[0])
		return;
	const ImU32 shadow = IM_COL32(0, 0, 0, 200);
	// Soft 8-dir outline + slight drop for readability
	for (int ox = -1; ox <= 1; ++ox) {
		for (int oy = -1; oy <= 1; ++oy) {
			if (ox == 0 && oy == 0) continue;
			dl->AddText(ImVec2(x + (float)ox, y + (float)oy), shadow, text);
		}
	}
	dl->AddText(ImVec2(x + 1.f, y + 1.f), IM_COL32(0, 0, 0, 120), text);
	dl->AddText(ImVec2(x, y), col, text);
}

static void DrawCornerLines(ImDrawList* dl, float x, float y, float w, float h, ImU32 col, float t) {
	const float cl = (std::min)(w, h) * 0.25f;
	// TL
	dl->AddLine(ImVec2(x, y), ImVec2(x + cl, y), col, t);
	dl->AddLine(ImVec2(x, y), ImVec2(x, y + cl), col, t);
	// TR
	dl->AddLine(ImVec2(x + w, y), ImVec2(x + w - cl, y), col, t);
	dl->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + cl), col, t);
	// BL
	dl->AddLine(ImVec2(x, y + h), ImVec2(x + cl, y + h), col, t);
	dl->AddLine(ImVec2(x, y + h), ImVec2(x, y + h - cl), col, t);
	// BR
	dl->AddLine(ImVec2(x + w, y + h), ImVec2(x + w - cl, y + h), col, t);
	dl->AddLine(ImVec2(x + w, y + h), ImVec2(x + w, y + h - cl), col, t);
}

// Clean box: outer black → color → thin inner black (crisp, not muddy)
static void DrawBoxOutlined(ImDrawList* dl, float x, float y, float w, float h, ImU32 col, float thickness, int style) {
	const float t = std::clamp(thickness, 1.f, 4.f);
	const ImU32 black = IM_COL32(0, 0, 0, 210);
	if (style == Config::ESP_BOX_CORNER) {
		DrawCornerLines(dl, x - 1.f, y - 1.f, w + 2.f, h + 2.f, black, t + 1.2f);
		DrawCornerLines(dl, x, y, w, h, col, t);
		return;
	}
	dl->AddRect(ImVec2(x - 1.f, y - 1.f), ImVec2(x + w + 1.f, y + h + 1.f), black, 0.f, 0, t + 1.2f);
	dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col, 0.f, 0, t);
	dl->AddRect(ImVec2(x + 1.f, y + 1.f), ImVec2(x + w - 1.f, y + h - 1.f), IM_COL32(0, 0, 0, 160), 0.f, 0, 1.f);
}

// Slim modern side bar (HP/armor)
static void DrawSideBar(ImDrawList* dl, float x, float y, float w, float h, float ratio, ImU32 fill) {
	ratio = std::clamp(ratio, 0.f, 1.f);
	const float fillH = h * ratio;
	const float fillY = y + (h - fillH);
	const ImU32 bg = IM_COL32(12, 12, 14, 210);
	const ImU32 border = IM_COL32(0, 0, 0, 230);
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, 1.f);
	if (fillH > 0.5f) {
		// Slight top highlight on fill for depth
		dl->AddRectFilled(ImVec2(x, fillY), ImVec2(x + w, y + h), fill, 1.f);
		if (fillH > 3.f)
			dl->AddRectFilled(ImVec2(x, fillY), ImVec2(x + w, fillY + 1.5f), IM_COL32(255, 255, 255, 45), 0.f);
	}
	dl->AddRect(ImVec2(x - 1.f, y - 1.f), ImVec2(x + w + 1.f, y + h + 1.f), border, 1.f, 0, 1.f);
}

static void DrawCenteredText(ImDrawList* dl, float cx, float y, ImU32 col, const char* text) {
	if (!text || !text[0])
		return;
	const ImVec2 ts = ImGui::CalcTextSize(text);
	DrawTextOutlined(dl, floorf(cx - ts.x * 0.5f), floorf(y), col, text);
}

static void FillPlayerFlags(C_CSPlayerPawn* pawn, PlayerCache& entry) {
	entry.flashed = false;
	entry.bomb = false;
	entry.scoped = false;
	entry.reloading = false;
	entry.defusing = false;
	if (!pawn)
		return;

	__try {
		entry.scoped = pawn->m_bIsScoped();
		entry.defusing = pawn->m_bIsDefusing();

		const float flashDur = pawn->m_flFlashDuration();
		const float flashAlpha = pawn->m_flFlashOverlayAlpha();
		entry.flashed = (flashDur > 0.05f) || (flashAlpha > 20.f);

		C_CSWeaponBase* wep = pawn->GetActiveWeapon();
		if (wep) {
			entry.reloading = wep->m_bInReload();

			// Bomb: weapon class C_C4 / name c4, or actively planting
			bool isC4 = false;
			auto* inst = reinterpret_cast<CEntityInstance*>(wep);
			SchemaClassInfoData_t* cls = nullptr;
			if (inst)
				inst->dump_class_info(&cls);
			if (cls && cls->szName) {
				const char* n = cls->szName;
				if (strstr(n, "C4") || strstr(n, "c4"))
					isC4 = true;
			}
			if (!isC4 && entry.weapon_name[0]) {
				if (strstr(entry.weapon_name, "c4") || strstr(entry.weapon_name, "C4"))
					isC4 = true;
			}
			if (isC4) {
				entry.bomb = true;
				// Planting still counts as bomb flag
				if (wep->m_bStartedArming())
					entry.bomb = true;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

bool Visuals::ensureViewMatrix() {
	if (viewMatrix.viewMatrix)
		return true;
	const uintptr_t abs = M::getAbsoluteAddress(
		M::patternScan("client", "48 8D 0D ? ? ? ? 48 C1 E0 06"), 3, 0);
	if (!abs)
		return false;
	viewMatrix.viewMatrix = reinterpret_cast<viewmatrix_t*>(abs);
	return true;
}

void Visuals::init() {
	viewMatrix.viewMatrix = nullptr;
	ensureViewMatrix();
}

void Esp::cache() {
	// Gate on entity system only — EngineClient::valid() (connected&&in_game vfuncs)
	// was false every other round and wiped world/nade cache entirely.
	if (!I::GameEntity || !I::GameEntity->Instance || !Mem::Valid(I::GameEntity->Instance, 0x2100)) {
		cached_players.clear();
		cached_world.clear();
		cached_local.reset();
		g_plantedBomb = {};
		return; // NadePred::Update runs on Present
	}

	cached_players.clear();
	cached_players.reserve(64);
	cached_world.clear();
	cached_world.reserve(64);
	g_plantedBomb = {};
	// Reset per-frame local state while preserving lastTeam. If the local
	// controller disappears during respawn/round teardown, stale active/team/
	// position data otherwise leaks into the next round's ESP filtering and
	// distance labels.
	cached_local.reset();

	const bool wantWorld = AnyWorldEspEnabled();
	const float curtime = wantWorld ? GetCurTime() : 0.f;
	if (wantWorld)
		WorldFxBeginFrame();

	int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0) {
		if (wantWorld)
			WorldFxEndFrame();
		return; // NadePred::Update runs on Present
	}
	if (nMax < 8192 - 512)
		nMax += 512; // planted C4 / projectiles can sit above highest

	int playerCount = 0;
	bool sawBomb = false;
	for (int i = 1; i <= nMax; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity))
			continue;

		// Class dump can fail after round restart — never hard-require it for nades
		SchemaClassInfoData_t* _class = nullptr;
		__try {
			Entity->dump_class_info(&_class);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			_class = nullptr;
		}
		const char* clsName = "";
		if (_class && Mem::Valid(_class, sizeof(void*)) && _class->szName
			&& Mem::IsReadable(_class->szName, 1))
			clsName = _class->szName;

		// ── Players via controller ──────────────────────────
		if (clsName[0] && HASH(clsName) == HASH("CCSPlayerController")) {
			// Controllers need a valid handle for pawn resolve
			if (!Entity->handle().valid())
				continue;
			if (playerCount >= Mem::kMaxPlayers)
				continue;

			auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
			const CBaseHandle hPawn = Controller->m_hPawn();
			if (!hPawn.valid())
				continue;

			C_CSPlayerPawn* Player = nullptr;
			if (!ResolvePawn(hPawn, &Player) || !Mem::ValidEntity(Player))
				continue;

			const int health = Mem::ClampHealth(Player->m_iHealth());
			const uint8_t life = Player->m_lifeState();
			const int teamNum = static_cast<int>(Player->m_iTeamNum());
			if (!Mem::ValidTeam(teamNum))
				continue;

			if (Controller->IsLocalPlayer()) {
				cached_local.active = true;
				cached_local.alive = (health > 0 && life == 0);
				if (cached_local.alive) {
					cached_local.position = GetAbsOrigin(Player);
					if (!Bones::IsValidPos(cached_local.position))
						cached_local.position = Vector_t{ 0.f, 0.f, 0.f };
					cached_local.health = health;
					cached_local.armor = Mem::ClampArmor(Player->m_ArmorValue());
					cached_local.handle = Player->handle().index();
					cached_local.team = teamNum;
					cached_local.lastTeam = teamNum;
				}
				else {
					cached_local.health = 0;
					cached_local.armor = 0;
					cached_local.handle = 0;
					cached_local.alive = false;
					cached_local.team = cached_local.lastTeam;
				}
				continue;
			}

			if (health <= 0 || life != 0)
				continue;
			if (IsDormant(Player))
				continue;

			PlayerCache entry{};
			entry.handle = Player->handle();
			entry.health = health;
			entry.maxHealth = Mem::ClampHealth(Player->m_iMaxHealth());
			if (entry.maxHealth <= 0)
				entry.maxHealth = 100;
			entry.armor = Mem::ClampArmor(Player->m_ArmorValue());
			entry.team_num = teamNum;
			entry.position = GetAbsOrigin(Player);
			if (!Bones::IsValidPos(entry.position))
				continue;
			entry.viewOffset = Player->m_vecViewOffset();
			if (!Mem::Finite(entry.viewOffset.x) || !Mem::Finite(entry.viewOffset.y)
				|| !Mem::Finite(entry.viewOffset.z))
				entry.viewOffset = Vector_t{ 0.f, 0.f, 0.f };

			const int filterTeam = cached_local.team != 0 ? cached_local.team : cached_local.lastTeam;
			if (filterTeam != 0 && teamNum == filterTeam)
				entry.type = team;
			else
				entry.type = enemy;

			if (!Controller->ReadSanitizedName(entry.name, sizeof(entry.name)))
				entry.name[0] = '\0';
			entry.steamId = 0;
			__try { entry.steamId = Controller->m_steamID(); }
			__except (EXCEPTION_EXECUTE_HANDLER) { entry.steamId = 0; }
			ReadWeaponName(Player, entry.weapon_name, sizeof(entry.weapon_name),
				entry.weapon_key, sizeof(entry.weapon_key));
			FillPlayerFlags(Player, entry);
			entry.visible = true; // filled in post-pass

			cached_players.push_back(entry);
			++playerCount;
			continue;
		}

		if (!wantWorld)
			continue;

		auto* base = reinterpret_cast<C_BaseEntity*>(Entity);
		const char* designer = GetDesignerName(Entity);
		// Match on class OR designer — designer alone is enough for nades
		const char* tag = clsName;
		const char* tag2 = designer ? designer : "";
		if (!tag[0] && !tag2[0])
			continue;

		const Vector_t origin = GetEntityAbsOrigin(base);
		// reject only truly invalid origins (origin can be near zero on some maps)
		if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
			continue;
		if (std::fabs(origin.x) < 0.01f && std::fabs(origin.y) < 0.01f && std::fabs(origin.z) < 0.01f)
			continue;

		// Planted bomb only (weapon C_C4 / weapon_c4 is NOT planted)
		const bool isBomb = (Config::world_esp_bomb || Config::glow_world_bomb || Config::widget_bomb) &&
			(strstr(tag, "PlantedC4") || strstr(tag, "planted_c4")
				|| strstr(tag2, "planted_c4") || strstr(tag2, "PlantedC4")
				|| strstr(tag2, "planted c4"));
		const bool nadeGlow = Config::glow_world_grenades;
		const bool forceWarn = Config::nade_warn; // always cache nades for warn every round
		const int nadeKind = ClassifyWorldNade(tag, tag2);
		const bool isSmoke = nadeKind == WORLD_SMOKE && (Config::world_esp_smoke || nadeGlow || forceWarn);
		const bool isMolly = nadeKind == WORLD_MOLOTOV && (Config::world_esp_molotov || nadeGlow || forceWarn);
		const bool isHE = nadeKind == WORLD_HE && (Config::world_esp_he || nadeGlow || forceWarn);
		const bool isFlash = nadeKind == WORLD_FLASH && (Config::world_esp_flash || nadeGlow || forceWarn);
		const bool isDecoy = nadeKind == WORLD_DECOY && (Config::world_esp_decoy || nadeGlow || forceWarn);

		// ── Planted C4 ──────────────────────────────────────
		if (isBomb) {
			const PlantedBombState bs = ReadPlantedBomb(reinterpret_cast<C_PlantedC4*>(Entity));
			// Hide after real explode/defuse. Recycled ents can keep stale flags —
			// if still ticking / blow in future, treat as live plant.
			const bool liveBlow = bs.ok && curtime > 0.f && bs.blow > curtime && (bs.blow - curtime) <= 45.f;
			const bool dead = bs.ok && (bs.exploded || bs.defused) && !bs.ticking && !liveBlow;
			if (dead) {
				s_bombTrackKey = 0;
				s_bombEndWall = 0.f;
				continue;
			}

			sawBomb = true;
			WorldCache w{};
			w.kind = WORLD_BOMB;
			w.position = origin;
			w.bomb_site = bs.ok ? bs.site : -1;
			w.defusing = bs.ok && bs.defusing;
			w.timer = -1.f;

			float gameLeft = -1.f;
			if (liveBlow)
				gameLeft = bs.blow - curtime;

			// Epoch isolates recycled C4 handles across rounds
			const uint32_t bKey = WorldHandleKey(Entity, i)
				^ (s_worldRoundEpoch * 0x9E3779B9u)
				^ WorldThrowSig(origin);
			const float wallNow = static_cast<float>(ImGui::GetTime());
			float wallLeft = (s_bombEndWall > 0.f && bKey == s_bombTrackKey)
				? (s_bombEndWall - wallNow) : -1.f;
			const float seedLen = (bs.ok && bs.timerLength >= 10.f && bs.timerLength <= 60.f)
				? bs.timerLength : 40.f;
			const bool needSeed = (bKey != 0) && (
				bKey != s_bombTrackKey
				|| wallLeft < 0.f
				|| (gameLeft >= 0.f && std::fabs(gameLeft - wallLeft) > 2.0f));
			if (needSeed) {
				s_bombTrackKey = bKey;
				const float seed = (gameLeft >= 0.f && gameLeft <= 45.f) ? gameLeft : seedLen;
				s_bombEndWall = wallNow + seed;
				wallLeft = seed;
			}

			// Prefer game time when it agrees with wall (±1.5s); else wall (stable across rounds)
			if (gameLeft >= 0.f && wallLeft >= 0.f && std::fabs(gameLeft - wallLeft) <= 1.5f)
				w.timer = gameLeft;
			else if (gameLeft >= 0.f)
				w.timer = gameLeft; // engine blow time is authoritative
			else if (wallLeft >= 0.f && wallLeft <= 45.f)
				w.timer = wallLeft;
			else if (bs.ok && bs.ticking)
				w.timer = seedLen;

			if (bs.ok && bs.defusing) {
				const float defLeft = (curtime > 0.f && bs.defuseEnd > curtime) ? (bs.defuseEnd - curtime) : -1.f;
				const float blowLeft = w.timer;
				if (defLeft >= 0.f && blowLeft >= 0.f)
					snprintf(w.label, sizeof(w.label), "DEFUSE %.1fs | %.1fs", defLeft, blowLeft);
				else
					snprintf(w.label, sizeof(w.label), "DEFUSING");
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = true;
				g_plantedBomb.blowLeft = blowLeft;
				g_plantedBomb.defuseLeft = defLeft;
				if (defLeft >= 0.f)
					w.timer = defLeft;
			}
			else if (w.timer >= 0.f) {
				const char siteCh = (w.bomb_site == 0) ? 'A' : (w.bomb_site == 1) ? 'B' : '?';
				snprintf(w.label, sizeof(w.label), "BOMB %c  %.1fs", siteCh, w.timer);
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = false;
				g_plantedBomb.blowLeft = w.timer;
				g_plantedBomb.defuseLeft = -1.f;
			}
			else if (w.bomb_site == 0)
				snprintf(w.label, sizeof(w.label), "BOMB A");
			else if (w.bomb_site == 1)
				snprintf(w.label, sizeof(w.label), "BOMB B");
			else
				snprintf(w.label, sizeof(w.label), "BOMB");
			snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");

			if (!g_plantedBomb.active && bs.ok) {
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = w.defusing;
				g_plantedBomb.blowLeft = w.timer;
				g_plantedBomb.defuseLeft = -1.f;
			}

			if (Config::glow_world_bomb)
				Glow::ApplyWorld(Entity, Config::world_esp_bomb_color, true);

			if (Config::world_esp_bomb)
				cached_world.push_back(w);
			continue;
		}

		// ── Projectiles / inferno ───────────────────────────
		// Never drop for dormant — nades often flagged dormant while airborne
		if (isSmoke || isMolly || isHE || isFlash || isDecoy) {
			WorldCache w{};
			ImVec4 gcol = Config::world_esp_smoke_color;
			if (isSmoke) {
				w.kind = WORLD_SMOKE;
				snprintf(w.weapon_key, sizeof(w.weapon_key), "smokegrenade");
				gcol = Config::world_esp_smoke_color;
				w.use_badge = true;
				bool didSmoke = false;
				const uint32_t offSmoke = SchemaFinder::Get(
					hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_bDidSmokeEffect"));
				const uint32_t smokeOff = offSmoke ? offSmoke : 0x127C;
				{
					auto* pb = reinterpret_cast<uint8_t*>(base) + smokeOff;
					if (Mem::IsReadable(pb, 1))
						didSmoke = (*pb != 0);
				}
				w.effect_active = didSmoke;
				// client.dll: cl_smoke_torus_ring_radius(61)+subradius(88)
				w.radius = 149.f;
				snprintf(w.label, sizeof(w.label), "SMOKE");
			}
			else if (isMolly) {
				w.kind = WORLD_MOLOTOV;
				const bool fire = strstr(tag, "Inferno") || strstr(tag2, "inferno");
				gcol = Config::world_esp_molotov_color;
				w.use_badge = true;
				if (fire) {
					snprintf(w.label, sizeof(w.label), "FIRE");
					snprintf(w.weapon_key, sizeof(w.weapon_key), "molotov");
					w.effect_active = true;
					w.radius = 60.f;
					const uint32_t offFc = SchemaFinder::Get(
						hash_32_fnv1a_const("C_Inferno->m_fireCount"));
					const int fcOff = offFc ? (int)offFc : 0x1960;
					int fc = 0;
					{
						auto* pi = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + fcOff);
						if (Mem::IsReadable(pi, sizeof(int)))
							fc = *pi;
					}
					if (fc <= 0)
						continue;
					// Post-effect = fire dying — clear with game
					bool postFx = false;
					{
						auto* pb = reinterpret_cast<uint8_t*>(base) + 0x196C;
						if (Mem::IsReadable(pb, 1))
							postFx = (*pb != 0);
					}
					if (postFx)
						continue;
					float halfW = 0.f;
					{
						auto* pf = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x858C);
						if (Mem::IsReadable(pf, sizeof(float)))
							halfW = *pf;
					}
					float maxR = 0.f;
					int lit = 0;
					const auto* fbase = reinterpret_cast<const uint8_t*>(base) + 0x1020;
					const auto* burn = reinterpret_cast<const uint8_t*>(base) + 0x1620;
					const int n = (fc > 64) ? 64 : fc;
					if (Mem::IsReadable(fbase, n * 12) && Mem::IsReadable(burn, n)) {
						for (int fi = 0; fi < n; ++fi) {
							if (!burn[fi]) continue;
							++lit;
							const float* xyz = reinterpret_cast<const float*>(fbase + fi * 12);
							const float dx = xyz[0] - origin.x;
							const float dy = xyz[1] - origin.y;
							const float rr = std::sqrt(dx * dx + dy * dy) + 50.f;
							if (rr > maxR) maxR = rr;
						}
					}
					// No lit fires after warm-up → fire out (halfW alone was too late)
					if (lit <= 0) {
						const float leftWarm = WorldEffectRemaining(
							WorldFxKey(Entity, i, WORLD_MOLOTOV, WorldThrowSig(origin)),
							WORLD_MOLOTOV, true, WorldThrowSig(origin));
						const float age = 7.f - leftWarm;
						if (age >= 0.35f || leftWarm <= 0.05f)
							continue;
						w.radius = 60.f; // warm-up only
					} else {
						w.radius = maxR;
						if (halfW > 15.f && halfW < 200.f && halfW > w.radius)
							w.radius = halfW;
						if (w.radius < 50.f) w.radius = 50.f;
						if (w.radius > 250.f) w.radius = 250.f;
					}
				} else {
					snprintf(w.label, sizeof(w.label), "MOLLY");
					snprintf(w.weapon_key, sizeof(w.weapon_key), "molotov");
					w.effect_active = false;
					w.radius = 130.f;
					// Landed shell is dead — fire entity owns warn
					float vx = 0.f, vy = 0.f, vz = 0.f;
					{
						auto* pv = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x3F8);
						if (Mem::IsReadable(pv, 12)) {
							vx = pv[0]; vy = pv[1]; vz = pv[2];
						}
					}
					if ((vx * vx + vy * vy + vz * vz) < 25.f)
						continue;
				}
			}
			else if (isHE) {
				w.kind = WORLD_HE;
				snprintf(w.label, sizeof(w.label), "HE");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "hegrenade");
				gcol = Config::world_esp_he_color;
				w.use_badge = true;
				// Explode flags only when stopped (mid-air false positives wiped warn)
				float vx = 0.f, vy = 0.f, vz = 0.f;
				{
					auto* pv = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x3F8);
					if (Mem::IsReadable(pv, 12)) { vx = pv[0]; vy = pv[1]; vz = pv[2]; }
				}
				const bool flying = (vx * vx + vy * vy + vz * vz) >= 25.f;
				if (!flying) {
					int explodeTick = 0;
					bool explodeBegan = false;
					auto* pi = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + 0x11F0);
					if (Mem::IsReadable(pi, 4)) explodeTick = *pi;
					auto* pb = reinterpret_cast<uint8_t*>(base) + 0x1214;
					if (Mem::IsReadable(pb, 1)) explodeBegan = (*pb != 0);
					if (explodeTick > 0 || explodeBegan)
						continue;
				}
			}
			else if (isFlash) {
				w.kind = WORLD_FLASH;
				snprintf(w.label, sizeof(w.label), "FLASH");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "flashbang");
				gcol = Config::world_esp_flash_color;
				w.use_badge = true;
				float vx = 0.f, vy = 0.f, vz = 0.f;
				{
					auto* pv = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x3F8);
					if (Mem::IsReadable(pv, 12)) { vx = pv[0]; vy = pv[1]; vz = pv[2]; }
				}
				const bool flying = (vx * vx + vy * vy + vz * vz) >= 25.f;
				if (!flying) {
					int explodeTick = 0;
					bool explodeBegan = false;
					auto* pi = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + 0x11F0);
					if (Mem::IsReadable(pi, 4)) explodeTick = *pi;
					auto* pb = reinterpret_cast<uint8_t*>(base) + 0x1214;
					if (Mem::IsReadable(pb, 1)) explodeBegan = (*pb != 0);
					if (explodeTick > 0 || explodeBegan)
						continue;
				}
			}
			else {
				w.kind = WORLD_DECOY;
				snprintf(w.label, sizeof(w.label), "DECOY");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "decoy");
				gcol = Config::world_esp_decoy_color;
				w.use_badge = true;
			}
			w.position = origin;

			// Expiry + timer: handle+epoch+kind+throwSig so recycled slots don't bleed
			const uint32_t throwSig = WorldThrowSig(origin);
			const uint32_t fxKey = WorldFxKey(Entity, i, nadeKind, throwSig);
			float left = WorldEffectRemaining(fxKey, nadeKind, w.effect_active, throwSig);

			// HE/Flash: engine m_flDetonateTime beats late wall discovery ("too fast")
			if ((isHE || isFlash) && left > 0.05f) {
				constexpr uint32_t kOffDet = 0x1188; // C_BaseGrenade->m_flDetonateTime (IDA)
				float det = 0.f;
				auto* pf = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + kOffDet);
				if (Mem::IsReadable(pf, sizeof(float)))
					det = *pf;
				if (det > 1.f && det < 1.0e7f && curtime > 1.f && det > curtime && (det - curtime) <= 3.f) {
					const float eng = det - curtime;
					if (eng < left - 0.15f)
						left = eng;
				}
			}

			w.timer = left;
			// Drop the frame effect/fuse ends (instant clear on explode / smoke end)
			if (left >= 0.f && left <= 0.05f)
				continue;

			if (Config::glow_world_grenades)
				Glow::ApplyWorld(Entity, gcol, true);
			const bool wantNadeEsp =
				forceWarn
				|| (isSmoke && Config::world_esp_smoke) || (isMolly && Config::world_esp_molotov)
				|| (isHE && Config::world_esp_he) || (isFlash && Config::world_esp_flash)
				|| (isDecoy && Config::world_esp_decoy);
			if (wantNadeEsp)
				cached_world.push_back(w);
			continue;
		}

		// ── Dropped weapons ─────────────────────────────────
		const bool wantWeaponEsp = Config::world_esp_weapons;
		const bool wantWeaponGlow = Config::glow_world_weapons;
		if (!wantWeaponEsp && !wantWeaponGlow)
			continue;
		if (!ClassLooksLikeWeapon(tag) && !ClassLooksLikeWeapon(tag2))
			continue;
		if (IsEntityDormant(base))
			continue;
		if (!IsDroppedWeapon(base))
			continue;

		if (wantWeaponGlow)
			Glow::ApplyWorld(Entity, Config::world_esp_weapon_color, true);

		if (!wantWeaponEsp)
			continue;

		WorldCache w{};
		w.kind = WORLD_WEAPON;
		w.position = origin;
		w.timer = -1.f;
		ReadWeaponEntityName(reinterpret_cast<C_CSWeaponBase*>(Entity),
			w.label, sizeof(w.label), w.weapon_key, sizeof(w.weapon_key));
		if (!w.label[0] && designer && designer[0]) {
			CopyCleanWeaponName(designer, w.label, sizeof(w.label));
			if (!w.weapon_key[0])
				MakeWeaponIconKey(designer, w.weapon_key, sizeof(w.weapon_key));
		}
		if (!w.label[0]) {
			CopyCleanWeaponName(clsName, w.label, sizeof(w.label));
			if (!w.weapon_key[0])
				MakeWeaponIconKey(clsName, w.weapon_key, sizeof(w.weapon_key));
		}
		if (!w.label[0])
			continue;
		cached_world.push_back(w);
	}

	// Fallback: CUtlAutoList<C_PlantedC4> when index scan misses (round recycle)
	if (wantWorld && !sawBomb && (Config::world_esp_bomb || Config::glow_world_bomb || Config::widget_bomb)) {
		constexpr uintptr_t kRvaPlantedHead = 0x236D678; // IDA: client.dll + RVA (head qword valid)
		constexpr uintptr_t kOffPlantedNext = 0x1198;
		static HMODULE s_client = nullptr;
		if (!s_client)
			s_client = GetModuleHandleW(L"client.dll");
		if (s_client) {
			void** pHead = reinterpret_cast<void**>(
				reinterpret_cast<uintptr_t>(s_client) + kRvaPlantedHead);
			void* e = nullptr;
			if (Mem::IsReadable(pHead, sizeof(void*)) && Mem::Read(pHead, e) && e) {
				for (int guard = 0; e && guard < 8; ++guard) {
					if (!Mem::ValidEntity(e))
						break;
					auto* ent = reinterpret_cast<CEntityInstance*>(e);
					auto* base = reinterpret_cast<C_BaseEntity*>(e);
					const Vector_t origin = GetEntityAbsOrigin(base);
					if (!std::isfinite(origin.x) || (std::fabs(origin.x) < 0.01f
						&& std::fabs(origin.y) < 0.01f && std::fabs(origin.z) < 0.01f)) {
						void* next = nullptr;
						if (!Mem::ReadField(e, kOffPlantedNext, next) || next == e)
							break;
						e = next;
						continue;
					}
					const PlantedBombState bs = ReadPlantedBomb(reinterpret_cast<C_PlantedC4*>(e));
					const bool liveBlow = bs.ok && curtime > 0.f && bs.blow > curtime
						&& (bs.blow - curtime) <= 45.f;
					const bool dead = bs.ok && (bs.exploded || bs.defused) && !bs.ticking && !liveBlow;
					if (!dead) {
						sawBomb = true;
						WorldCache w{};
						w.kind = WORLD_BOMB;
						w.position = origin;
						w.bomb_site = bs.ok ? bs.site : -1;
						w.defusing = bs.ok && bs.defusing;
						float gameLeft = liveBlow ? (bs.blow - curtime) : -1.f;
						const uint32_t bKey = WorldHandleKey(ent, 0)
							^ (s_worldRoundEpoch * 0x9E3779B9u)
							^ WorldThrowSig(origin);
						const float wallNow = static_cast<float>(ImGui::GetTime());
						float wallLeft = (s_bombEndWall > 0.f && bKey == s_bombTrackKey)
							? (s_bombEndWall - wallNow) : -1.f;
						const float seedLen = (bs.ok && bs.timerLength >= 10.f && bs.timerLength <= 60.f)
							? bs.timerLength : 40.f;
						if (bKey != s_bombTrackKey || wallLeft < 0.f
							|| (gameLeft >= 0.f && std::fabs(gameLeft - wallLeft) > 2.f)) {
							s_bombTrackKey = bKey;
							const float seed = (gameLeft >= 0.f) ? gameLeft : seedLen;
							s_bombEndWall = wallNow + seed;
							wallLeft = seed;
						}
						if (gameLeft >= 0.f)
							w.timer = gameLeft;
						else if (wallLeft >= 0.f && wallLeft <= 45.f)
							w.timer = wallLeft;
						else
							w.timer = seedLen;
						if (bs.ok && bs.defusing) {
							snprintf(w.label, sizeof(w.label), "DEFUSING");
						} else if (w.timer >= 0.f) {
							const char siteCh = (w.bomb_site == 0) ? 'A' : (w.bomb_site == 1) ? 'B' : '?';
							snprintf(w.label, sizeof(w.label), "BOMB %c  %.1fs", siteCh, w.timer);
						} else {
							snprintf(w.label, sizeof(w.label), "BOMB");
						}
						snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
						g_plantedBomb.active = true;
						g_plantedBomb.site = w.bomb_site;
						g_plantedBomb.position = origin;
						g_plantedBomb.defusing = w.defusing;
						g_plantedBomb.blowLeft = w.timer;
						g_plantedBomb.defuseLeft = -1.f;
						if (bs.ok && bs.defusing && curtime > 0.f && bs.defuseEnd > curtime) {
							g_plantedBomb.defuseLeft = bs.defuseEnd - curtime;
							g_plantedBomb.blowLeft = (gameLeft >= 0.f) ? gameLeft : w.timer;
						}
						if (Config::glow_world_bomb)
							Glow::ApplyWorld(ent, Config::world_esp_bomb_color, true);
						if (Config::world_esp_bomb)
							cached_world.push_back(w);
						break; // one planted bomb
					}
					void* next = nullptr;
					if (!Mem::ReadField(e, kOffPlantedNext, next) || next == e)
						break;
					e = next;
				}
			}
		}
	}

	// Drop timers for entities gone this frame (new serial = new nade next round)
	if (wantWorld) {
		WorldFxEndFrame();
		// Bomb entity recycled / gone between rounds — don't keep a dead wall clock
		static int s_noBombFrames = 0;
		if (sawBomb)
			s_noBombFrames = 0;
		else if (++s_noBombFrames >= 45) {
			s_bombTrackKey = 0;
			s_bombEndWall = 0.f;
			s_noBombFrames = 0;
		}
	}

	// NadePred::Update + path→world mirror run on Present (Visuals::esp).

	// Andromeda-style vis: any of head/pelvis/arms/ankles hits target pawn
	if (Config::esp_vis_check
		&& Trace::Ready() && H::oGetLocalPlayer && !cached_players.empty()) {
		C_CSPlayerPawn* localPawn = H::oGetLocalPlayer(0);
		if (localPawn && localPawn->m_iHealth() > 0) {
			const Vector_t eye = Bones::GetEyePos(localPawn);
			if (Bones::IsValidPos(eye)) {
				const int pc = (int)cached_players.size();
				for (int i = 0; i < pc; ++i) {
					auto& entry = cached_players[i];
					C_CSPlayerPawn* tgt = nullptr;
					if (!ResolvePawn(entry.handle, &tgt) || !tgt) {
						entry.visible = true;
						continue;
					}

					// Andromeda g_AllTraceVisibleCheckBones
					Vector_t samples[6]{};
					int n = 0;
					uintptr_t ba = 0;
					Vector_t origin{};
					float height = 0.f;
					Bones::Map map{};
					if (Bones::GetBoneArray(tgt, ba, origin, height)
						&& Bones::ResolveMapCached(tgt, ba, origin, height, map)) {
						const int slots[] = {
							Bones::S_HEAD, Bones::S_PELVIS,
							Bones::S_ARM_L_R, Bones::S_ARM_L_L,
							Bones::S_ANKLE_R, Bones::S_ANKLE_L
						};
						for (int s : slots) {
							Vector_t p{};
							if (n < 6 && Bones::GetSlotPos(ba, map, s, p) && Bones::IsValidPos(p))
								samples[n++] = p;
						}
					}
					if (n < 1) {
						const Vector_t head = entry.position + entry.viewOffset;
						samples[n++] = head;
						samples[n++] = entry.position;
					}

					entry.visible = Trace::IsBodyVisible(eye, samples, n, localPawn, tgt);
				}
			}
		}
	}
}

static void DrawSkeleton(ImDrawList* drawList, C_CSPlayerPawn* pawn, const ViewMatrix& vm, ImU32 col) {
	if (!drawList || !pawn)
		return;

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	if (!Bones::GetBoneArray(pawn, boneArray, origin, height))
		return;

	Bones::Map map{};
	if (!Bones::ResolveMapCached(pawn, boneArray, origin, height, map))
		return;

	const float th = std::clamp(Config::esp_skeleton_thickness, 1.f, 4.f);
	const ImU32 outline = IM_COL32(0, 0, 0, 200);

	for (int i = 0; i < Bones::kSkeletonCount; ++i) {
		Vector_t a{}, b{};
		if (!Bones::GetSlotPos(boneArray, map, Bones::kSkeleton[i].from, a))
			continue;
		if (!Bones::GetSlotPos(boneArray, map, Bones::kSkeleton[i].to, b))
			continue;
		const float len = (a - b).Length();
		if (len < 1.f || len > 70.f)
			continue;

		Vector_t sa{}, sb{};
		if (!vm.WorldToScreen(a, sa) || !vm.WorldToScreen(b, sb))
			continue;

		drawList->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), outline, th + 1.5f);
		drawList->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), col, th);
	}
}

// CS2 ESP box (Andromeda / UC standard):
//   1) Primary: collision AABB (mins+origin, maxs+origin) → project 8 corners
//      Perspective-correct at all ranges (fixes close-up head-only + fat mid-range).
//   2) Fallback: feet origin + viewOffset head, width = height * scale.
static bool ComputeEspBox(C_CSPlayerPawn* pawn, const PlayerCache& player,
	const ViewMatrix& vm, float& outX, float& outY, float& outW, float& outH)
{
	const float widthScale = std::clamp(Config::esp_box_width, 0.28f, 0.70f);

	// --- Primary: collision hull 8-corner projection ---
	if (pawn && Mem::ValidEntity(pawn)) {
		CCollisionProperty* col = pawn->m_pCollision();
		if (col && Mem::Valid(col, 0x50)) {
			const Vector_t origin = GetAbsOrigin(pawn);
			Vector_t mins = col->m_vecMins();
			Vector_t maxs = col->m_vecMaxs();

			const bool minsOk = Mem::Finite(mins.x) && Mem::Finite(mins.y) && Mem::Finite(mins.z);
			const bool maxsOk = Mem::Finite(maxs.x) && Mem::Finite(maxs.y) && Mem::Finite(maxs.z);
			const bool originOk = Bones::IsValidPos(origin);
			// Reject degenerate / zero hulls
			const float hx = maxs.x - mins.x;
			const float hy = maxs.y - mins.y;
			const float hz = maxs.z - mins.z;

			if (minsOk && maxsOk && originOk && hx > 1.f && hy > 1.f && hz > 8.f) {
				mins.x += origin.x; mins.y += origin.y; mins.z += origin.z;
				maxs.x += origin.x; maxs.y += origin.y; maxs.z += origin.z;

				const Vector_t corners[8] = {
					{ mins.x, mins.y, mins.z }, { mins.x, maxs.y, mins.z },
					{ maxs.x, maxs.y, mins.z }, { maxs.x, mins.y, mins.z },
					{ maxs.x, maxs.y, maxs.z }, { mins.x, maxs.y, maxs.z },
					{ mins.x, mins.y, maxs.z }, { maxs.x, mins.y, maxs.z },
				};

				float minSX = 1e9f, maxSX = -1e9f, minSY = 1e9f, maxSY = -1e9f;
				int n = 0;
				for (const Vector_t& c : corners) {
					Vector_t s{};
					if (!vm.WorldToScreen(c, s))
						continue; // skip behind-camera corner, keep rest (Andromeda fix)
					minSX = (std::min)(minSX, s.x);
					maxSX = (std::max)(maxSX, s.x);
					minSY = (std::min)(minSY, s.y);
					maxSY = (std::max)(maxSY, s.y);
					++n;
				}

				const float boxW0 = maxSX - minSX;
				const float boxH0 = maxSY - minSY;
				if (n >= 2 && boxW0 >= 2.f && boxH0 >= 4.f
					&& std::isfinite(boxW0) && std::isfinite(boxH0))
				{
					// Small pad so box is not skin-tight on collision hull
					const float padY = (std::max)(1.5f, boxH0 * 0.025f);
					const float padX = (std::max)(1.0f, boxW0 * 0.04f);
					// Width scale nudges collision width (1.0 = hull as-is; slider ~0.42 default is narrow)
					// Map slider so 0.42 ≈ slight tighten, 0.55 ≈ hull, 0.70 ≈ loose
					const float wMul = 0.70f + (widthScale - 0.28f) * (0.55f / 0.42f); // ~0.70..1.25
					const float cx = (minSX + maxSX) * 0.5f;
					float boxW = boxW0 * std::clamp(wMul, 0.65f, 1.30f) + padX * 2.f;
					float boxH = boxH0 + padY * 1.5f;

					outX = cx - boxW * 0.5f;
					outY = minSY - padY;
					outW = boxW;
					outH = boxH;
					return true;
				}
			}
		}
	}

	// --- Fallback: feet + eye height (no max height clamp) ---
	const float eyeZ = player.viewOffset.z;
	const float headPad = (eyeZ > 1.f) ? eyeZ * 0.18f : 12.f;
	const float footPad = (eyeZ > 1.f) ? eyeZ * 0.08f : 5.f;

	Vector_t feetWorld = player.position;
	feetWorld.z -= footPad;
	Vector_t headWorld = player.position + player.viewOffset;
	headWorld.z += headPad;

	Vector_t feetScreen{}, headScreen{};
	if (!vm.WorldToScreen(feetWorld, feetScreen) || !vm.WorldToScreen(headWorld, headScreen))
		return false;

	float top = headScreen.y;
	float bottom = feetScreen.y;
	if (bottom < top)
		std::swap(top, bottom);

	float boxH = bottom - top;
	if (boxH < 4.f)
		return false;

	const float padY = (std::max)(2.f, boxH * 0.03f);
	top -= padY;
	bottom += padY;
	boxH = bottom - top;

	float boxW = boxH * widthScale;
	const float padX = (std::max)(1.5f, boxW * 0.04f);
	boxW += padX * 2.f;

	const float centerX = (feetScreen.x + headScreen.x) * 0.5f;
	outX = centerX - boxW * 0.5f;
	outY = top;
	outW = boxW;
	outH = boxH;
	return true;
}

void Visuals::drawPlayers() {
	const bool anyFlag = Config::flag_flashed || Config::flag_bomb || Config::flag_scoped
		|| Config::flag_reloading || Config::flag_defusing;
	if (!Config::esp && !Config::showHealth && !Config::espFill && !Config::showNameTags
		&& !Config::showArmor && !Config::showDistance && !Config::showWeapon
		&& !Config::showWeaponIcon
		&& !Config::esp_skeleton && !anyFlag)
		return;

	if (cached_players.empty())
		return;

	const int filterTeam = cached_local.team != 0 ? cached_local.team : cached_local.lastTeam;

	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	if (!drawList)
		return;

	for (const auto& Player : cached_players) {
		if (!Player.handle.valid() || Player.health <= 0)
			continue;

		if (GameMode::WantTeamCheck(Config::teamCheck) && filterTeam != 0 && Player.team_num == filterTeam)
			continue;

		const bool useOccluded = Config::esp_vis_check && !Player.visible;
		const ImVec4 boxColF = useOccluded ? Config::espColorInvisible : Config::espColor;
		const ImVec4 skelColF = useOccluded ? Config::esp_skeleton_color_invisible : Config::esp_skeleton_color;

		C_CSPlayerPawn* drawPawn = nullptr;
		ResolvePawn(Player.handle, &drawPawn);

		if (Config::esp_skeleton && drawPawn)
			DrawSkeleton(drawList, drawPawn, viewMatrix, ImGui::ColorConvertFloat4ToU32(skelColF));

		float boxX = 0.f, boxY = 0.f, boxW = 0.f, boxH = 0.f;
		if (!ComputeEspBox(drawPawn, Player, viewMatrix, boxX, boxY, boxW, boxH))
			continue;
		if (boxW < 2.f || boxH < 4.f)
			continue;

		const ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(boxColF);
		ImVec4 fillCol = boxColF;
		fillCol.w = Config::espFillOpacity;
		const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(fillCol);

		if (Config::espFill && Config::esp) {
			drawList->AddRectFilled(
				ImVec2(boxX, boxY),
				ImVec2(boxX + boxW, boxY + boxH),
				fillColor);
		}

		if (Config::esp)
			DrawBoxOutlined(drawList, boxX, boxY, boxW, boxH, boxColor, Config::espThickness, Config::esp_box_style);

		const float barW = std::clamp(Config::esp_bar_width, 2.f, 6.f);
		const float barGap = 2.f;
		float barX = boxX - barGap - barW;

		if (Config::showHealth) {
			const float maxHp = static_cast<float>((std::max)(Player.maxHealth, 1));
			const float ratio = std::clamp(Player.health / maxHp, 0.f, 1.f);
			ImU32 hpCol;
			if (Config::esp_health_auto) {
				// Smooth green → yellow → red
				const float r = std::clamp(2.f * (1.f - ratio), 0.f, 1.f);
				const float g = std::clamp(2.f * ratio, 0.f, 1.f);
				hpCol = IM_COL32((int)(r * 255.f), (int)(g * 220.f + 20.f), 45, 255);
			} else {
				hpCol = ImGui::ColorConvertFloat4ToU32(Config::esp_health_color);
			}
			DrawSideBar(drawList, barX, boxY, barW, boxH, ratio, hpCol);
			// HP number when damaged / small boxes skip clutter
			if (Player.health < Player.maxHealth && boxH >= 28.f) {
				char hpTxt[8];
				snprintf(hpTxt, sizeof(hpTxt), "%d", Player.health);
				const ImVec2 hs = ImGui::CalcTextSize(hpTxt);
				const float hx = floorf(barX + barW * 0.5f - hs.x * 0.5f);
				const float hy = floorf(boxY + boxH - hs.y - 1.f);
				DrawTextOutlined(drawList, hx, hy, IM_COL32(255, 255, 255, 230), hpTxt);
			}
			barX -= (barW + barGap);
		}

		if (Config::showArmor && Player.armor > 0) {
			const float ratio = std::clamp(Player.armor / 100.f, 0.f, 1.f);
			DrawSideBar(drawList, barX, boxY, barW, boxH, ratio,
				ImGui::ColorConvertFloat4ToU32(Config::esp_armor_color));
		}

		const float cx = boxX + boxW * 0.5f;
		// Single shared line height for name / icon / weapon / distance
		const float lineH = ImGui::GetFontSize();

		if (Config::showNameTags && Player.name[0] != '\0') {
			const ImVec2 ns = ImGui::CalcTextSize(Player.name);
			const ImU32 nameCol = ImGui::ColorConvertFloat4ToU32(Config::esp_name_color);
			const float avatarSz = floorf(lineH + 1.f);
			const float avatarGap = 3.f;
			const bool drawAvatar = Config::esp_name_avatar;

			float totalW = ns.x;
			if (drawAvatar)
				totalW += avatarSz + avatarGap;

			float x = floorf(cx - totalW * 0.5f);
			const float ny = floorf(boxY - ns.y - 3.f);

			if (drawAvatar) {
				const ImVec2 avMin(x, floorf(ny + (ns.y - avatarSz) * 0.5f));
				const ImVec2 avMax(avMin.x + avatarSz, avMin.y + avatarSz);
				const float round = avatarSz * 0.5f;
				const ImTextureID tex = SteamAvatar::Get(Player.steamId, pDevice);
				if (tex != ImTextureID_Invalid) {
					drawList->AddImageRounded(ImTextureRef(tex), avMin, avMax,
						ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 230), round);
				} else {
					drawList->AddCircleFilled(
						ImVec2(avMin.x + round, avMin.y + round), round,
						IM_COL32(36, 38, 46, 200), 16);
					char letter[2] = { '?', '\0' };
					if (Player.name[0]) {
						char c = Player.name[0];
						if (c >= 'a' && c <= 'z')
							c = static_cast<char>(c - 'a' + 'A');
						letter[0] = c;
					}
					const ImVec2 ls = ImGui::CalcTextSize(letter);
					drawList->AddText(
						ImVec2(avMin.x + round - ls.x * 0.5f, avMin.y + round - ls.y * 0.5f),
						IM_COL32(200, 205, 215, 220), letter);
				}
				x = floorf(x + avatarSz + avatarGap);
			}

			DrawTextOutlined(drawList, x, ny, nameCol, Player.name);
		}

		// Under-box stack: icon → weapon text → distance (tight, aligned)
		float textY = floorf(boxY + boxH + 2.f);
		const ImU32 wepCol = ImGui::ColorConvertFloat4ToU32(Config::esp_weapon_color);

		if (Config::showWeaponIcon) {
			const float ih = DrawWeaponIconCentered(drawList, cx, textY, wepCol, Player.weapon_key);
			if (ih > 0.f)
				textY += ih;
		}

		if (Config::showWeapon && Player.weapon_name[0] != '\0') {
			DrawCenteredText(drawList, cx, textY, wepCol, Player.weapon_name);
			textY += lineH;
		}

		if (Config::showDistance && cached_local.active) {
			const float dx = Player.position.x - cached_local.position.x;
			const float dy = Player.position.y - cached_local.position.y;
			const float dz = Player.position.z - cached_local.position.z;
			const float units = std::sqrt(dx * dx + dy * dy + dz * dz);
			const int meters = static_cast<int>(units * 0.0254f + 0.5f);
			char distText[24];
			snprintf(distText, sizeof(distText), "%dm", meters);
			DrawCenteredText(drawList, cx, textY,
				ImGui::ColorConvertFloat4ToU32(Config::esp_distance_color), distText);
		}

		if (anyFlag) {
			float flagY = floorf(boxY);
			const float flagX = floorf(boxX + boxW + 3.f);
			const float flagLine = lineH;
			struct FlagItem { bool on; const char* text; ImU32 col; };
			const FlagItem flags[] = {
				{ Config::flag_flashed   && Player.flashed,   "FLASH",  IM_COL32(255, 235, 90, 255)  },
				{ Config::flag_bomb      && Player.bomb,      "C4",     IM_COL32(255, 95, 95, 255)   },
				{ Config::flag_scoped    && Player.scoped,    "ZOOM",   IM_COL32(130, 205, 255, 255) },
				{ Config::flag_reloading && Player.reloading, "R",      IM_COL32(255, 185, 70, 255)  },
				{ Config::flag_defusing  && Player.defusing,  "DEF",    IM_COL32(90, 220, 140, 255)  },
			};
			for (const auto& f : flags) {
				if (!f.on)
					continue;
				DrawTextOutlined(drawList, flagX, flagY, f.col, f.text);
				flagY += flagLine;
			}
		}
	}
}

static void DrawWorldRadiusRing(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
                                float radius, ImU32 col, int segs = 40) {
	if (!dl || radius <= 1.f || !vm.viewMatrix)
		return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x <= 1.f || ds.y <= 1.f)
		return;
	const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;

	// Near-plane distance: below this a vertex is at/behind the camera.
	// Segments crossing it are clipped to the plane instead of dropped whole,
	// so the ring stays continuous when the local player stands on the effect.
	constexpr float kClipNear = 0.1f;

	struct RV { float numX = 0.f, numY = 0.f, w = 0.f; bool ok = false; } prev{};
	auto toScreen = [cx, cy](const RV& v) {
		const float invW = 1.f / v.w;
		return ImVec2(cx + v.numX * invW * cx, cy - v.numY * invW * cy);
	};

	for (int i = 0; i <= segs; ++i) {
		const float a = (static_cast<float>(i) / static_cast<float>(segs)) * 6.2831853f;
		const Vector_t w{
			center.x + std::cos(a) * radius,
			center.y + std::sin(a) * radius,
			center.z + 2.f
		};
		RV cur{};
		cur.ok = vm.WorldToClip(w, cur.numX, cur.numY, cur.w);

		if (i > 0 && prev.ok && cur.ok) {
			const bool aFront = prev.w >= kClipNear;
			const bool bFront = cur.w >= kClipNear;
			if (aFront || bFront) {
				RV ca = prev, cb = cur;
				if (aFront != bFront) {
					const float t = (kClipNear - prev.w) / (cur.w - prev.w);
					RV mid;
					mid.numX = prev.numX + (cur.numX - prev.numX) * t;
					mid.numY = prev.numY + (cur.numY - prev.numY) * t;
					mid.w = kClipNear;
					mid.ok = true;
					if (aFront) cb = mid; else ca = mid;
				}
				dl->AddLine(toScreen(ca), toScreen(cb), col, 1.6f);
			}
		}
		prev = cur;
	}
}

// Badge: NAME above icon, TIMER under, fuse progress arc. Uses nade_warn_icon_size.
static void DrawWorldNadeBadge(ImDrawList* dl, float cx, float cy, const ImVec4& col4,
                               const char* weaponKey, const char* label, bool active,
                               float timeLeft, NadePred::NadeType typeHint = NadePred::NadeType::Unknown) {
	if (!dl)
		return;
	const float t = static_cast<float>(ImGui::GetTime());
	const float pulse = 0.5f + 0.5f * std::sin(t * (active ? 5.5f : 3.0f));
	const float iconBase = std::clamp(Config::nade_warn_icon_size, 16.f, 56.f);
	const float r = iconBase * 0.70f;
	const ImU32 accent = ImGui::ColorConvertFloat4ToU32(col4);

	if (label && label[0]) {
		const ImVec2 lsz = ImGui::CalcTextSize(label);
		const float lx = floorf(cx - lsz.x * 0.5f);
		const float ly = floorf(cy - r - lsz.y - 6.f);
		dl->AddRectFilled(ImVec2(lx - 5.f, ly - 1.f), ImVec2(lx + lsz.x + 5.f, ly + lsz.y + 1.f),
			IM_COL32(6, 8, 12, 210), 4.f);
		dl->AddText(ImVec2(lx + 1.f, ly + 1.f), IM_COL32(0, 0, 0, 180), label);
		dl->AddText(ImVec2(lx, ly), IM_COL32(236, 238, 242, 255), label);
	}

	const int glowA = static_cast<int>((active ? 50.f : 28.f) + pulse * 16.f);
	dl->AddCircleFilled(ImVec2(cx, cy), r + 6.f,
		IM_COL32((int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255), glowA), 28);
	dl->AddCircleFilled(ImVec2(cx, cy), r + 2.5f, IM_COL32(0, 0, 0, 185), 32);
	dl->AddCircleFilled(ImVec2(cx, cy), r,
		IM_COL32((int)(col4.x * 28), (int)(col4.y * 28), (int)(col4.z * 28), 230), 32);

	dl->AddCircle(ImVec2(cx, cy), r + 1.5f, IM_COL32(0, 0, 0, 210), 32, 2.2f);
	dl->AddCircle(ImVec2(cx, cy), r + 1.5f,
		IM_COL32((int)(col4.x * 180), (int)(col4.y * 180), (int)(col4.z * 180), 150), 32, 1.3f);

	if (timeLeft >= 0.05f) {
		float full = 18.f;
		if (typeHint == NadePred::NadeType::Molly) full = 7.f;
		else if (typeHint == NadePred::NadeType::Decoy) full = 15.f;
		else if (typeHint == NadePred::NadeType::HE || typeHint == NadePred::NadeType::Flash) full = 1.625f;
		else if (weaponKey) {
			if (strstr(weaponKey, "molotov") || strstr(weaponKey, "incendiary")) full = 7.f;
			else if (strstr(weaponKey, "decoy")) full = 15.f;
			else if (strstr(weaponKey, "hegrenade") || strstr(weaponKey, "flash")) full = 1.625f;
		}
		const float frac = std::clamp(timeLeft / full, 0.f, 1.f);
		const float a0 = -3.14159265f * 0.5f;
		dl->PathArcTo(ImVec2(cx, cy), r + 1.5f, a0, a0 + frac * 3.14159265f * 2.f, 28);
		dl->PathStroke(accent, 0, active ? 2.5f : 2.0f);
	}

	const char* glyph = nullptr;
	if (weaponKey && weaponKey[0]) {
		auto it = weapon_icons::icon_table.find(weaponKey);
		if (it != weapon_icons::icon_table.end() && !it->second.empty())
			glyph = it->second.c_str();
	}
	if (glyph && g_WeaponIconFont) {
		const float iconSz = iconBase * 0.92f;
		const ImVec2 isz = g_WeaponIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, glyph);
		const float ix = floorf(cx - isz.x * 0.5f);
		const float iy = floorf(cy - isz.y * 0.55f);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(ix + 1.f, iy + 1.f), IM_COL32(0, 0, 0, 210), glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(ix, iy), IM_COL32(255, 255, 255, 255), glyph);
	}

	if (timeLeft >= 0.05f) {
		char line[16];
		std::snprintf(line, sizeof(line), "%.1fs", timeLeft);
		const ImVec2 tsz = ImGui::CalcTextSize(line);
		const float tx = floorf(cx - tsz.x * 0.5f);
		const float ty = floorf(cy + r + 6.f);
		dl->AddRectFilled(ImVec2(tx - 4.f, ty - 1.f), ImVec2(tx + tsz.x + 4.f, ty + tsz.y + 1.f),
			IM_COL32(6, 8, 12, 210), 4.f);
		dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 180), line);
		dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 255), line);
	}
}

static bool NadeWarnInRange(const Vector_t& pos) {
	if (!Config::nade_warn_only_near)
		return true;
	if (!cached_local.active || !cached_local.alive)
		return true;
	const float rangeM = std::clamp(Config::nade_warn_range, 1.f, 200.f);
	const float rangeU = rangeM / 0.0254f;
	const float dx = pos.x - cached_local.position.x;
	const float dy = pos.y - cached_local.position.y;
	const float dz = pos.z - cached_local.position.z;
	return (dx * dx + dy * dy + dz * dz) <= rangeU * rangeU;
}

void Visuals::drawWorld() {
	// nade_warn alone still draws nade badges from cache (no world_esp_* required)
	if (cached_world.empty())
		return;
	if (!AnyWorldEspEnabled())
		return;

	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	if (!drawList)
		return;

	const float lineH = ImGui::GetFontSize();
	const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.f);

	for (const auto& item : cached_world) {
		if (!item.label[0])
			continue;

		const bool isNadeKind =
			item.kind == WORLD_HE || item.kind == WORLD_FLASH || item.kind == WORLD_DECOY
			|| item.kind == WORLD_SMOKE || item.kind == WORLD_MOLOTOV;

		// When only nade_warn (no world esp for this type), still draw nades
		const bool worldWants =
			(item.kind == WORLD_BOMB && Config::world_esp_bomb)
			|| (item.kind == WORLD_WEAPON && Config::world_esp_weapons)
			|| (item.kind == WORLD_SMOKE && Config::world_esp_smoke)
			|| (item.kind == WORLD_MOLOTOV && Config::world_esp_molotov)
			|| (item.kind == WORLD_HE && Config::world_esp_he)
			|| (item.kind == WORLD_FLASH && Config::world_esp_flash)
			|| (item.kind == WORLD_DECOY && Config::world_esp_decoy);
		if (!worldWants && !(Config::nade_warn && isNadeKind))
			continue;

		Vector_t screen{};
		// Nade warn: project even when off-screen (edge badge). Other world ESP stays strict W2S.
		if (isNadeKind && Config::nade_warn) {
			if (!viewMatrix.viewMatrix)
				continue;
			const auto* m = viewMatrix.viewMatrix->matrix;
			float w = m[3][0] * item.position.x + m[3][1] * item.position.y
				+ m[3][2] * item.position.z + m[3][3];
			float x = m[0][0] * item.position.x + m[0][1] * item.position.y
				+ m[0][2] * item.position.z + m[0][3];
			float y = m[1][0] * item.position.x + m[1][1] * item.position.y
				+ m[1][2] * item.position.z + m[1][3];
			const ImVec2 ds = ImGui::GetIO().DisplaySize;
			if (ds.x <= 1.f || ds.y <= 1.f)
				continue;
			const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;
			const bool behind = w < 0.01f;
			if (behind) { x = -x; y = -y; w = 0.01f; }
			const float invW = 1.f / w;
			screen.x = cx + x * invW * cx;
			screen.y = cy - y * invW * cy;
			const float margin = 36.f;
			const float minX = margin, maxX = ds.x - margin;
			const float minY = margin, maxY = ds.y - margin;
			if (behind || screen.x < minX || screen.x > maxX || screen.y < minY || screen.y > maxY) {
				float dx = screen.x - cx, dy = screen.y - cy;
				if (std::fabs(dx) < 0.01f && std::fabs(dy) < 0.01f) { dx = 0.f; dy = -1.f; }
				const float sx = (dx > 0.f) ? (maxX - cx) / dx : ((dx < 0.f) ? (minX - cx) / dx : 1e9f);
				const float sy = (dy > 0.f) ? (maxY - cy) / dy : ((dy < 0.f) ? (minY - cy) / dy : 1e9f);
				float t = (sy < sx) ? sy : sx;
				if (t < 0.f) t = 0.f;
				if (t > 1.f) t = 1.f;
				screen.x = cx + dx * t;
				screen.y = cy + dy * t;
				if (screen.x < minX) screen.x = minX;
				if (screen.x > maxX) screen.x = maxX;
				if (screen.y < minY) screen.y = minY;
				if (screen.y > maxY) screen.y = maxY;
			}
		} else if (!viewMatrix.WorldToScreen(item.position, screen)) {
			continue;
		}

		ImVec4 col4 = Config::world_esp_weapon_color;
		switch (item.kind) {
		case WORLD_BOMB:    col4 = Config::world_esp_bomb_color; break;
		case WORLD_SMOKE:   col4 = Config::world_esp_smoke_color; break;
		case WORLD_MOLOTOV: col4 = Config::world_esp_molotov_color; break;
		case WORLD_HE:      col4 = Config::world_esp_he_color; break;
		case WORLD_FLASH:   col4 = Config::world_esp_flash_color; break;
		case WORLD_DECOY:   col4 = Config::world_esp_decoy_color; break;
		default: break;
		}
		const ImU32 col = ImGui::ColorConvertFloat4ToU32(col4);

		// Nade projectiles / effects — single badge (name top / timer bottom)
		if (item.use_badge || isNadeKind) {
			// Instant hide when timer ran out (explode / effect end)
			if (item.timer >= 0.f && item.timer <= 0.05f)
				continue;
			// only_near applies to warn-only badges, not dedicated world ESP
			if (Config::nade_warn && isNadeKind && !worldWants && !NadeWarnInRange(item.position))
				continue;
			NadePred::NadeType nt = NadePred::NadeType::Unknown;
			switch (item.kind) {
			case WORLD_SMOKE: nt = NadePred::NadeType::Smoke; break;
			case WORLD_MOLOTOV: nt = NadePred::NadeType::Molly; break;
			case WORLD_HE: nt = NadePred::NadeType::HE; break;
			case WORLD_FLASH: nt = NadePred::NadeType::Flash; break;
			case WORLD_DECOY: nt = NadePred::NadeType::Decoy; break;
			default: break;
			}
			DrawWorldNadeBadge(drawList, screen.x, screen.y - 18.f, col4,
				item.weapon_key, item.label, item.effect_active, item.timer, nt);
			if (item.effect_active && item.radius > 1.f) {
				const ImU32 ring = IM_COL32(
					(int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255),
					static_cast<int>(70 + pulse * 50));
				// Smoke torus (IDA): outer ring+sub (149), centerline 61
				if (item.kind == WORLD_SMOKE) {
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 149.f, ring, 56);
					const ImU32 ringIn = IM_COL32(
						(int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255),
						static_cast<int>(45 + pulse * 35));
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 61.f, ringIn, 40);
				} else {
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, item.radius, ring, 48);
				}
			}
			if (Config::world_esp_distance && cached_local.active) {
				const float dx = item.position.x - cached_local.position.x;
				const float dy = item.position.y - cached_local.position.y;
				const float dz = item.position.z - cached_local.position.z;
				const float units = std::sqrt(dx * dx + dy * dy + dz * dz);
				const int meters = static_cast<int>(units * 0.0254f + 0.5f);
				char distText[24];
				snprintf(distText, sizeof(distText), "%dm", meters);
				DrawCenteredText(drawList, screen.x, screen.y + 28.f,
					ImGui::ColorConvertFloat4ToU32(Config::esp_distance_color), distText);
			}
			continue;
		}

		const float r = 2.8f;
		drawList->AddCircleFilled(ImVec2(screen.x, screen.y), r + 1.f, IM_COL32(0, 0, 0, 200), 12);
		drawList->AddCircleFilled(ImVec2(screen.x, screen.y), r, col, 12);

		float textY = floorf(screen.y + r + 3.f);
		bool drewIcon = false;
		if (item.weapon_key[0]) {
			const float ih = DrawWeaponIconCentered(drawList, screen.x, textY, col, item.weapon_key);
			if (ih > 0.f) {
				textY += ih;
				drewIcon = true;
			}
		}
		if (!drewIcon || item.kind != WORLD_WEAPON) {
			DrawCenteredText(drawList, screen.x, textY, col, item.label);
			textY += lineH;
		}

		if (Config::world_esp_distance && cached_local.active) {
			const float dx = item.position.x - cached_local.position.x;
			const float dy = item.position.y - cached_local.position.y;
			const float dz = item.position.z - cached_local.position.z;
			const float units = std::sqrt(dx * dx + dy * dy + dz * dz);
			const int meters = static_cast<int>(units * 0.0254f + 0.5f);
			char distText[24];
			snprintf(distText, sizeof(distText), "%dm", meters);
			DrawCenteredText(drawList, screen.x, textY,
				ImGui::ColorConvertFloat4ToU32(Config::esp_distance_color), distText);
		}
	}
}

void Esp::ResetWorldFxTimers() {
	s_worldFxN = 0;
	++s_worldRoundEpoch;
	s_bombTrackKey = 0;
	s_bombEndWall = 0.f;
}

// Mirror NadePred paths into world cache so drawWorld always has nade badges
// even when entity-class dump misses a frame (first-round / restart).
static void MirrorNadePathsToWorld() {
	if (!Config::nade_warn && !Config::world_esp_smoke && !Config::world_esp_molotov
		&& !Config::world_esp_he && !Config::world_esp_flash && !Config::world_esp_decoy)
		return;

	const auto& paths = NadePred::Paths();
	for (const auto& p : paths) {
		if (p.local_preview || p.type == NadePred::NadeType::Unknown)
			continue;
		// Instant clear when fuse/effect ends (no lingering badge)
		if (p.time_left >= 0.f && p.time_left <= 0.05f)
			continue;
		if (!std::isfinite(p.origin.x) || (std::fabs(p.origin.x) < 0.01f
			&& std::fabs(p.origin.y) < 0.01f && std::fabs(p.origin.z) < 0.01f))
			continue;

		WorldEspKind kind = WORLD_HE;
		const char* key = "hegrenade";
		const char* lab = "HE";
		float rad = p.radius > 1.f ? p.radius : 0.f;
		bool want = Config::nade_warn;
		switch (p.type) {
		case NadePred::NadeType::Smoke:
			kind = WORLD_SMOKE; key = "smokegrenade"; lab = "SMOKE";
			want = want || Config::world_esp_smoke;
			if (rad < 1.f) rad = 149.f; // torus outer (61+88)
			break;
		case NadePred::NadeType::Molly:
			kind = WORLD_MOLOTOV; key = "molotov"; lab = p.effect_active ? "FIRE" : "MOLLY";
			want = want || Config::world_esp_molotov;
			if (rad < 1.f) rad = 130.f;
			break;
		case NadePred::NadeType::HE:
			kind = WORLD_HE; key = "hegrenade"; lab = "HE";
			want = want || Config::world_esp_he;
			break;
		case NadePred::NadeType::Flash:
			kind = WORLD_FLASH; key = "flashbang"; lab = "FLASH";
			want = want || Config::world_esp_flash;
			break;
		case NadePred::NadeType::Decoy:
			kind = WORLD_DECOY; key = "decoy"; lab = "DECOY";
			want = want || Config::world_esp_decoy;
			break;
		default: break;
		}
		if (!want)
			continue;
		// only_near: skip warn mirrors that aren't also enabled as world ESP
		const bool worldEspOn =
			(kind == WORLD_SMOKE && Config::world_esp_smoke)
			|| (kind == WORLD_MOLOTOV && Config::world_esp_molotov)
			|| (kind == WORLD_HE && Config::world_esp_he)
			|| (kind == WORLD_FLASH && Config::world_esp_flash)
			|| (kind == WORLD_DECOY && Config::world_esp_decoy);
		if (Config::nade_warn && !worldEspOn && !NadeWarnInRange(p.origin))
			continue;

		bool dup = false;
		for (auto& e : cached_world) {
			if (e.kind != kind)
				continue;
			const float dx = e.position.x - p.origin.x;
			const float dy = e.position.y - p.origin.y;
			const float dz = e.position.z - p.origin.z;
			if (dx * dx + dy * dy + dz * dz < 400.f) {
				// Prefer shorter remaining fuse (engine path) over late wall clocks
				if (p.time_left >= 0.05f && (e.timer < 0.05f || p.time_left < e.timer))
					e.timer = p.time_left;
				e.position = p.origin;
				e.effect_active = e.effect_active || p.effect_active;
				if (p.radius > e.radius)
					e.radius = p.radius;
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		WorldCache w{};
		w.kind = kind;
		w.position = p.origin;
		w.radius = rad;
		w.use_badge = true;
		w.effect_active = p.effect_active;
		w.timer = p.time_left;
		snprintf(w.label, sizeof(w.label), "%s", lab);
		snprintf(w.weapon_key, sizeof(w.weapon_key), "%s", key);
		cached_world.push_back(w);
	}
}

void Visuals::esp() {
	// VAC soft-pause: no overlay / world ESP work
	if (VacDetect::IsSoftPaused())
		return;

	if (!ensureViewMatrix())
		return;

	// Collect on Present (FRAME_RENDER_END can SEH-abort before cache Update)
	if (Config::nade_pred || Config::nade_warn)
		NadePred::Update();

	// Dual path: entity cache + NadePred paths → world badges every throw/round
	MirrorNadePathsToWorld();

	if (H::oGetLocalPlayer && H::oGetLocalPlayer(0))
		drawPlayers();
	drawWorld();
	NadePred::Draw(viewMatrix);
}
