#include "visuals.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>
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
#include "../nade_lineup/nade_lineup.h"
#include "../hitmarker/hitmarker.h"
#include "../hitlog/hitlog.h"
#include "../tracers/tracers.h"
#include "../backtrack/backtrack.h"
#include "../w2s/w2s.h"
#include "../bomb/bomb.h"
#include "../widgets/steam_avatar.h"
#include "../../offsets/offsets.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/security/vacdetect.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
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
	// Use font size as target height — avoid CalcTextSize("A") every icon
	const float targetH = textSz * 0.90f;

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

	// 4-dir outline (was 8-neigh) — half the icon AddText work
	const ImU32 shadow = IM_COL32(0, 0, 0, 200);
	dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x - 1.f, drawY), shadow, glyph);
	dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x + 1.f, drawY), shadow, glyph);
	dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x, drawY - 1.f), shadow, glyph);
	dl->AddText(g_WeaponIconFont, iconSz, ImVec2(x, drawY + 1.f), shadow, glyph);
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
	// aim/trigger/autofire smoke checks reuse cached_world smokes (no CreateMove scan)
	return Config::nade_warn
		|| Config::widget_bomb
		|| Config::world_esp_weapons || Config::world_esp_bomb
		|| Config::world_esp_smoke || Config::world_esp_molotov
		|| Config::world_esp_he || Config::world_esp_flash
		|| Config::world_esp_decoy
		|| Config::glow_world_weapons || Config::glow_world_bomb
		|| Config::glow_world_grenades
		|| Config::aim_smoke_check || Config::autofire_smoke_check
		|| Config::trigger_smoke_check;
}

// Player controller pass — skip when nothing consumes cached_players / local
static bool AnyPlayerCacheNeeded() {
	// Glow needs player cache for vis + ApplyPlayer; radar/knifebot too.
	return Config::esp || Config::espFill || Config::showHealth || Config::showArmor
		|| Config::showNameTags || Config::esp_skeleton || Config::showWeapon
		|| Config::showWeaponIcon || Config::showDistance
		|| Config::flag_flashed || Config::flag_scoped
		|| Config::flag_defusing || Config::flag_bomb || Config::flag_reloading
		|| Config::flag_money || Config::flag_kit || Config::flag_helmet || Config::flag_nades
		|| Config::esp_rank || Config::esp_3d_box || Config::esp_oof
		|| Config::glow || Config::widget_radar || Config::knifebot
		|| Config::nade_pred || Config::nade_warn;
}

// CS2 competitive ranks (0 = unranked). Display short labels for ESP.
static const char* CompetitiveRankName(int rank) {
	static const char* kNames[] = {
		"Unranked",
		"S1", "S2", "S3", "S4", "SE", "SEM",
		"GN1", "GN2", "GN3", "GNM",
		"MG1", "MG2", "MGE", "DMG",
		"LE", "LEM", "SMFC", "GE"
	};
	if (rank < 0 || rank >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0])))
		return nullptr;
	return kNames[rank];
}

// Nade def indices (CS2 item defs)
static bool IsNadeDef(std::uint16_t def, bool& he, bool& flash, bool& smoke, bool& molly, bool& decoy) {
	// 43 HE, 44 flash, 45 smoke, 46 molly, 47 decoy, 48 incendiary
	switch (def) {
	case 43: he = true; return true;
	case 44: flash = true; return true;
	case 45: smoke = true; return true;
	case 46: molly = true; return true;
	case 47: decoy = true; return true;
	case 48: molly = true; return true;
	default: return false;
	}
}

static void FillEquipFromController(CCSPlayerController* ctrl, PlayerCache& entry) {
	entry.money = -1;
	entry.rank = 0;
	if (!ctrl)
		return;
	__try {
		entry.rank = ctrl->m_iCompetitiveRanking();
		void* moneySvc = ctrl->m_pInGameMoneyServices();
		if (moneySvc && Mem::IsUserPtr(moneySvc)) {
			auto* ms = reinterpret_cast<CCSPlayerController_InGameMoneyServices*>(moneySvc);
			const int acc = ms->m_iAccount();
			if (acc >= 0 && acc <= 16000)
				entry.money = acc;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		entry.money = -1;
		entry.rank = 0;
	}
}

// Walk m_hMyWeapons for nade inventory (same layout as skinchanger).
static void FillNadeInventory(C_CSPlayerPawn* pawn, PlayerCache& entry) {
	entry.nade_he = entry.nade_flash = entry.nade_smoke = entry.nade_molly = entry.nade_decoy = false;
	if (!pawn || !I::GameEntity || !I::GameEntity->Instance)
		return;
	CCSPlayer_WeaponServices* ws = pawn->GetWeaponServices();
	if (!ws || !Mem::IsUserPtr(ws))
		return;

	static std::uint32_t s_myWeapons = 0;
	if (!s_myWeapons)
		s_myWeapons = SchemaFinder::Get(hash_32_fnv1a_const("CPlayer_WeaponServices->m_hMyWeapons"));
	if (!s_myWeapons)
		return;

	__try {
		auto* base = reinterpret_cast<std::uint8_t*>(ws) + s_myWeapons;
		struct Try { CBaseHandle* elems; int sz; };
		const Try tries[2] = {
			{ *reinterpret_cast<CBaseHandle**>(base + 8), *reinterpret_cast<int*>(base + 0) },
			{ *reinterpret_cast<CBaseHandle**>(base + 0), *reinterpret_cast<int*>(base + 8) },
		};
		for (const auto& t : tries) {
			if (t.sz <= 0 || t.sz > 64 || !t.elems || !Mem::IsUserPtr(t.elems))
				continue;
			for (int i = 0; i < t.sz; ++i) {
				if (!t.elems[i].valid())
					continue;
				auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(t.elems[i]);
				if (!w)
					continue;
				const std::uint16_t def = w->m_iItemDefinitionIndex();
				IsNadeDef(def, entry.nade_he, entry.nade_flash, entry.nade_smoke,
					entry.nade_molly, entry.nade_decoy);
			}
			break;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static Vector_t GetEntityAbsOrigin(C_BaseEntity* ent) {
	if (!ent || !Mem::ValidEntity(ent))
		return {};
	// Schema-first (Offset::) with dump FB — no fixed 0x330/0xC8
	const uint32_t kSceneNode = Offset::m_pGameSceneNode();
	const uint32_t kAbsOrigin = Offset::m_vecAbsOrigin();
	CGameSceneNode* node = nullptr;
	if (!kSceneNode || !kAbsOrigin
		|| !Mem::ReadField(ent, kSceneNode, node)
		|| !node || !Mem::Valid(node, kAbsOrigin + 12)) {
		node = ent->m_pGameSceneNode();
		if (!node || !Mem::Valid(node, (kAbsOrigin ? kAbsOrigin : 0xC8) + 12))
			return {};
	}
	Vector_t o{};
	const uint32_t absOff = kAbsOrigin ? kAbsOrigin : 0xC8;
	if (Mem::ReadField(node, absOff, o)
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

// Wall clock for FX timers — must NOT use ImGui::GetTime (freezes when Present skips ImGui)
static float WallTimeSec() {
	return static_cast<float>(GetTickCount64()) * 0.001f;
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
	const float now = WallTimeSec();
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

// IDA: C_C4 (0x181A771B4) vs C_PlantedC4 (0x181B34E68) — dropped bomb is C_C4 / weapon_c4
static bool IsWorldDroppedC4(const char* cls, const char* designer) {
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";
	if (std::strstr(a, "Planted") || std::strstr(b, "planted"))
		return false;
	if (std::strcmp(a, "C_C4") == 0 || (std::strstr(a, "C_C4") && !std::strstr(a, "Planted")))
		return true;
	if (std::strstr(b, "weapon_c4") || std::strcmp(b, "c4") == 0)
		return true;
	// class "C4" without Planted (ClassLooksLikeWeapon path)
	if (std::strstr(a, "C4") && !std::strstr(a, "Projectile"))
		return true;
	return false;
}

// owner handle: schema preferred; IDA C_BaseEntity schema = 1312 (0x520)
static bool ReadOwnerHandle(C_BaseEntity* ent, CBaseHandle* out) {
	if (!ent || !out)
		return false;
	static uint32_t s_ownerOff = 0;
	if (!s_ownerOff) {
		const uint32_t sch = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_hOwnerEntity"));
		s_ownerOff = sch ? sch : 0x520u; // IDA: 1312
	}
	__try {
		*out = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uintptr_t>(ent) + s_ownerOff);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Resolve owner handle with serial check (Get(index) alone can hit recycled slots)
static C_BaseEntity* ResolveOwnerEntity(const CBaseHandle& owner) {
	if (!owner.valid() || owner.index() == 0)
		return nullptr;
	if (!I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	auto* ent = I::GameEntity->Instance->Get(owner);
	if (!ent || !Mem::ValidEntity(ent))
		return nullptr;
	const CBaseHandle actual = ent->handle();
	if (!actual.valid()
		|| actual.index() != owner.index()
		|| actual.serial_number() != owner.serial_number())
		return nullptr;
	return reinterpret_cast<C_BaseEntity*>(ent);
}

static bool OwnerIsLivingPlayer(const CBaseHandle& owner) {
	auto* base = ResolveOwnerEntity(owner);
	if (!base)
		return false;
	// Prefer IsBasePlayer — skips dump_class_info on the hot weapon path
	__try {
		if (!base->IsBasePlayer())
			return false;
		return base->m_iHealth() > 0 && base->m_lifeState() == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// Unreadable owner → treat as NOT held so dropped ESP still shows
		return false;
	}
}

static Vector_t OwnerAbsOrigin(const CBaseHandle& owner) {
	auto* ent = ResolveOwnerEntity(owner);
	if (!ent)
		return {};
	return GetEntityAbsOrigin(ent);
}

// True if owner's active weapon is this entity (still held)
static bool OwnerHoldsThisWeapon(const CBaseHandle& owner, C_BaseEntity* wep) {
	if (!wep)
		return false;
	auto* ent = ResolveOwnerEntity(owner);
	if (!ent)
		return false;
	__try {
		auto* pawn = reinterpret_cast<C_CSPlayerPawn*>(ent);
		C_CSWeaponBase* active = pawn->GetActiveWeapon();
		return active && reinterpret_cast<void*>(active) == reinterpret_cast<void*>(wep);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Dropped if no owner, or owner is not an alive pawn.
// C4 and some guns keep living m_hOwnerEntity after drop — use hold + distance.
static bool IsDroppedWeapon(C_BaseEntity* ent, const Vector_t& origin, bool /*isC4*/) {
	CBaseHandle owner{};
	if (!ReadOwnerHandle(ent, &owner))
		return true; // unreadable owner → show
	if (!owner.valid() || owner.index() == 0)
		return true;
	if (!OwnerIsLivingPlayer(owner))
		return true;
	// Actively holding this weapon
	if (OwnerHoldsThisWeapon(owner, ent))
		return false;
	// Inventory (knife out / holstered): entity stays near pawn
	const Vector_t op = OwnerAbsOrigin(owner);
	if (std::isfinite(op.x) && std::isfinite(op.y) && std::isfinite(op.z)) {
		const float dx = origin.x - op.x;
		const float dy = origin.y - op.y;
		const float dz = origin.z - op.z;
		// ~72u ≈ still on person; farther = on ground
		if (dx * dx + dy * dy + dz * dz > 72.f * 72.f)
			return true;
		return false; // near living owner, not active → still inventory
	}
	// Living owner but origin unreadable — show (fail-open for world ESP)
	return true;
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

// Flying nade / inferno — aligned with NadePred::ClassifyProjectile (IDA class names)
static int ClassifyWorldNade(const char* cls, const char* designer) {
	const char* a = cls ? cls : "";
	const char* b = designer ? designer : "";

	auto has = [](const char* s, const char* k) -> bool {
		return s && k && k[0] && std::strstr(s, k) != nullptr;
	};

	// Designer-first (cheap, no class dump required for most projectiles)
	if (b[0]) {
		if (std::strcmp(b, "inferno") == 0 || has(b, "inferno"))
			return WORLD_MOLOTOV;
		if (has(b, "smokegrenade") && !has(b, "weapon_"))
			return WORLD_SMOKE;
		if ((has(b, "molotov") || has(b, "incgrenade") || has(b, "incendiary")) && !has(b, "weapon_"))
			return WORLD_MOLOTOV;
		if (has(b, "hegrenade") && !has(b, "weapon_"))
			return WORLD_HE;
		if (has(b, "flashbang") && !has(b, "weapon_"))
			return WORLD_FLASH;
		if (has(b, "decoy") && !has(b, "weapon_"))
			return WORLD_DECOY;
		if (has(b, "projectile")) {
			if (has(b, "smoke")) return WORLD_SMOKE;
			if (has(b, "molotov") || has(b, "incendiary") || has(b, "incgrenade")) return WORLD_MOLOTOV;
			if (has(b, "hegrenade") || has(b, "he_grenade")) return WORLD_HE;
			if (has(b, "flash")) return WORLD_FLASH;
			if (has(b, "decoy")) return WORLD_DECOY;
		}
	}

	if (has(a, "Inferno") || has(a, "FireCrackerBlast"))
		return WORLD_MOLOTOV;
	if (has(a, "SmokeGrenadeProjectile") || std::strcmp(a, "C_SmokeGrenadeProjectile") == 0)
		return WORLD_SMOKE;
	if (has(a, "MolotovProjectile") || has(a, "IncendiaryGrenadeProjectile")
		|| has(a, "IncGrenadeProjectile") || std::strcmp(a, "C_MolotovProjectile") == 0)
		return WORLD_MOLOTOV;
	if (has(a, "HEGrenadeProjectile") || has(a, "FragGrenadeProjectile")
		|| std::strcmp(a, "C_HEGrenadeProjectile") == 0)
		return WORLD_HE;
	if (has(a, "FlashbangProjectile") || std::strcmp(a, "C_FlashbangProjectile") == 0)
		return WORLD_FLASH;
	if (has(a, "DecoyProjectile") || std::strcmp(a, "C_DecoyProjectile") == 0)
		return WORLD_DECOY;

	if (has(a, "Projectile") || has(a, "GrenadeProjectile")) {
		if (has(a, "Smoke")) return WORLD_SMOKE;
		if (has(a, "Molotov") || has(a, "Incendiary")) return WORLD_MOLOTOV;
		if (has(a, "HEGrenade") || has(a, "Frag")) return WORLD_HE;
		if (has(a, "Flash")) return WORLD_FLASH;
		if (has(a, "Decoy")) return WORLD_DECOY;
		if (has(a, "BaseCSGrenadeProjectile") || has(a, "GrenadeProjectile"))
			return WORLD_HE;
	}

	return -1;
}

// Cheap gate: skip dump_class_info + origin for the vast majority of entity slots.
static bool DesignerMayMatter(const char* d, bool wantPlayers, bool wantWeapons,
	bool wantNades, bool wantBomb)
{
	if (!d || !d[0])
		return true; // unknown — may need class dump
	// Controllers
	if (wantPlayers) {
		if (d[0] == 'c' && (std::strcmp(d, "cs_player_controller") == 0
			|| std::strstr(d, "controller")))
			return true;
	}
	// Bomb
	if (wantBomb) {
		if (std::strstr(d, "c4") || std::strstr(d, "planted"))
			return true;
	}
	// Projectiles / fire
	if (wantNades) {
		if (std::strstr(d, "projectile") || std::strstr(d, "inferno")
			|| std::strstr(d, "grenade") || std::strstr(d, "molotov")
			|| std::strstr(d, "flash") || std::strstr(d, "decoy")
			|| std::strstr(d, "smoke") || std::strstr(d, "incendiary")
			|| std::strstr(d, "incgrenade"))
			return true;
	}
	// Dropped guns
	if (wantWeapons && std::strncmp(d, "weapon_", 7) == 0)
		return true;
	return false;
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
	// 4-dir outline (was 8) — half the AddText calls, still readable
	dl->AddText(ImVec2(x - 1.f, y), shadow, text);
	dl->AddText(ImVec2(x + 1.f, y), shadow, text);
	dl->AddText(ImVec2(x, y - 1.f), shadow, text);
	dl->AddText(ImVec2(x, y + 1.f), shadow, text);
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
	entry.has_helmet = false;
	entry.has_defuser = false;
	if (!pawn)
		return;

	__try {
		entry.scoped = pawn->m_bIsScoped();
		entry.defusing = pawn->m_bIsDefusing();

		const float flashDur = pawn->m_flFlashDuration();
		const float flashAlpha = pawn->m_flFlashOverlayAlpha();
		entry.flashed = (flashDur > 0.05f) || (flashAlpha > 20.f);

		// ItemServices: dump CCSPlayer_ItemServices m_bHasDefuser@+0x48 m_bHasHelmet@+0x49
		void* itemSvc = pawn->m_pItemServices();
		if (itemSvc && Mem::IsUserPtr(itemSvc)) {
			const auto* p = reinterpret_cast<const std::uint8_t*>(itemSvc);
			entry.has_defuser = p[0x48] != 0;
			entry.has_helmet = p[0x49] != 0;
		}

		C_CSWeaponBase* wep = pawn->GetActiveWeapon();
		if (wep) {
			entry.reloading = wep->m_bInReload();

			// Bomb: prefer already-resolved weapon name/key (avoid dump_class_info)
			bool isC4 = false;
			if (entry.weapon_key[0] && (strstr(entry.weapon_key, "c4") || strstr(entry.weapon_key, "C4")))
				isC4 = true;
			else if (entry.weapon_name[0]
				&& (strstr(entry.weapon_name, "c4") || strstr(entry.weapon_name, "C4")))
				isC4 = true;
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
	W2S::Init();
	if (const viewmatrix_t* live = W2S::Matrix()) {
		viewMatrix.viewMatrix = const_cast<viewmatrix_t*>(live);
		return true;
	}
	if (viewMatrix.viewMatrix)
		return true;

	// One-shot pattern resolve — never re-scan every Present
	static bool s_triedFallback = false;
	static viewmatrix_t* s_fallback = nullptr;
	if (!s_triedFallback) {
		s_triedFallback = true;
		uintptr_t site = M::patternScan("client", "48 8D 0D ? ? ? ? 48 C1 E0 06");
		if (!site)
			site = M::patternScan("client.dll", "48 8D 0D ? ? ? ? 48 C1 E0 06");
		const uintptr_t abs = site ? M::getAbsoluteAddress(site, 3, 0) : 0;
		if (abs)
			s_fallback = reinterpret_cast<viewmatrix_t*>(abs);
	}
	if (!s_fallback)
		return false;
	viewMatrix.viewMatrix = s_fallback;
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

	// Keep capacity — clear+push without reserve reallocs every RENDER_END
	if (cached_players.capacity() < 64)
		cached_players.reserve(64);
	if (cached_world.capacity() < 128)
		cached_world.reserve(128);
	cached_players.clear();
	cached_world.clear();
	g_plantedBomb = {};
	// Reset per-frame local state while preserving lastTeam. If the local
	// controller disappears during respawn/round teardown, stale active/team/
	// position data otherwise leaks into the next round's ESP filtering and
	// distance labels.
	cached_local.reset();

	const bool wantWorld = AnyWorldEspEnabled();
	const bool wantPlayers = AnyPlayerCacheNeeded();
	if (!wantWorld && !wantPlayers)
		return;

	const float curtime = wantWorld ? GetCurTime() : 0.f;
	if (wantWorld)
		WorldFxBeginFrame();

	int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
	if (nMax <= 0) {
		if (wantWorld)
			WorldFxEndFrame();
		return; // NadePred::Update runs on Present
	}
	// Projectiles / dropped C4: pad only when needed — full +512 every frame was heavy
	if (wantWorld) {
		// Weapons/nades/bomb rarely need more than highest+128; pad 256 max
		const int pad = (nMax < 2048) ? 256 : 128;
		if (nMax < 8192 - pad)
			nMax += pad;
	} else if (wantPlayers && nMax > 128) {
		nMax = 128; // controllers only — no need to walk high entity slots
	}

	int playerCount = 0;
	bool sawBomb = false;

	const bool wantWeapons = Config::world_esp_weapons || Config::glow_world_weapons;
	const bool wantBombScan = Config::world_esp_bomb || Config::glow_world_bomb || Config::widget_bomb;
	const bool wantNades = Config::nade_warn || Config::world_esp_smoke || Config::world_esp_molotov
		|| Config::world_esp_he || Config::world_esp_flash || Config::world_esp_decoy
		|| Config::glow_world_grenades
		|| Config::aim_smoke_check || Config::autofire_smoke_check || Config::trigger_smoke_check;

	// Sticky vis from last frame (handle → last sample) — avoid re-trace every RENDER_END
	struct VisSticky {
		std::uint32_t h = 0;
		bool vis = true;
		float x = 0.f, y = 0.f, z = 0.f;
		bool ok = false;
	};
	static VisSticky s_visSticky[64]{};
	static int s_visStickyN = 0;
	const int stickyInN = s_visStickyN;
	VisSticky stickyIn[64]{};
	const int stickyCopy = (stickyInN < 64) ? stickyInN : 64;
	for (int si = 0; si < stickyCopy; ++si)
		stickyIn[si] = s_visSticky[si];

	// Per-frame scene offsets (SchCached is cheap after first resolve)
	const uint32_t kSceneNodeOff = Offset::m_pGameSceneNode();
	const uint32_t kAbsOriginOff = Offset::m_vecAbsOrigin();
	(void)kSceneNodeOff;
	(void)kAbsOriginOff;

	for (int i = 1; i <= nMax; ++i) {
		auto* Entity = I::GameEntity->Instance->Get(i);
		if (!Mem::ValidEntity(Entity))
			continue;

		// Designer first — most slots are props; skip dump_class_info entirely.
		const char* designerEarly = GetDesignerName(Entity);
		const bool designerIsCtrl = designerEarly && designerEarly[0]
			&& (std::strcmp(designerEarly, "cs_player_controller") == 0
				|| std::strstr(designerEarly, "player_controller") != nullptr);

		// Hard-skip only for world-only slots. When wantPlayers, always allow low
		// controller slots (i<=128) so a weird designer never hides players.
		if (designerEarly && designerEarly[0] && !designerIsCtrl
			&& !DesignerMayMatter(designerEarly, wantPlayers, wantWeapons, wantNades, wantBombScan)) {
			if (!(wantPlayers && i <= 128))
				continue;
		}

		// dump_class_info only when designer is missing or world class is required
		SchemaClassInfoData_t* _class = nullptr;
		const char* clsName = "";
		bool needClass = !designerEarly || !designerEarly[0];
		if (!needClass && wantWorld && !designerIsCtrl) {
			const bool knownWeapon = wantWeapons && std::strncmp(designerEarly, "weapon_", 7) == 0;
			const bool knownNade = ClassifyWorldNade(nullptr, designerEarly) >= 0;
			const bool knownBomb = IsWorldDroppedC4("", designerEarly)
				|| std::strstr(designerEarly, "planted") != nullptr;
			// Interesting designer but no keyword match → need class name
			if (DesignerMayMatter(designerEarly, false, wantWeapons, wantNades, wantBombScan)
				&& !knownWeapon && !knownNade && !knownBomb)
				needClass = true;
		}
		// Controllers without designer: still dump in low slots only
		if (!needClass && wantPlayers && i <= 128 && (!designerEarly || !designerEarly[0]))
			needClass = true;

		if (needClass) {
			__try {
				Entity->dump_class_info(&_class);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				_class = nullptr;
			}
			if (_class && Mem::Valid(_class, sizeof(void*)) && _class->szName
				&& Mem::IsReadable(_class->szName, 1))
				clsName = _class->szName;
		}

		// ── Players via controller ──────────────────────────
		const bool isController =
			designerIsCtrl
			|| (clsName[0] && HASH(clsName) == HASH("CCSPlayerController"));
		if (wantPlayers && isController) {
			// Controllers need a valid handle for pawn resolve
			if (!Entity->handle().valid())
				continue;
			if (playerCount >= Mem::kMaxPlayers)
				continue;

			auto* Controller = reinterpret_cast<CCSPlayerController*>(Entity);
			// Prefer m_hPlayerPawn (alive CS pawn); m_hPawn can be observer
			CBaseHandle hPawn = Controller->m_hPlayerPawn();
			if (!hPawn.valid())
				hPawn = Controller->m_hPawn();
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
			FillEquipFromController(Controller, entry);
			if (Config::flag_nades)
				FillNadeInventory(Player, entry);
			entry.visible = true; // filled in post-pass
			// Seed sticky vis if handle+pos match last frame (~same spot)
			{
				const std::uint32_t hk = static_cast<std::uint32_t>(entry.handle.index());
				for (int si = 0; si < stickyCopy; ++si) {
					if (!stickyIn[si].ok || stickyIn[si].h != hk)
						continue;
					const float dx = entry.position.x - stickyIn[si].x;
					const float dy = entry.position.y - stickyIn[si].y;
					const float dz = entry.position.z - stickyIn[si].z;
					if (dx * dx + dy * dy + dz * dz < 36.f) { // ~6u still
						entry.visible = stickyIn[si].vis;
						entry.visCached = true;
						entry.visSampleX = stickyIn[si].x;
						entry.visSampleY = stickyIn[si].y;
						entry.visSampleZ = stickyIn[si].z;
					}
					break;
				}
			}

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
		const bool forceSmokeAim = Config::aim_smoke_check || Config::autofire_smoke_check
			|| Config::trigger_smoke_check;
		const int nadeKind = ClassifyWorldNade(tag, tag2);
		const bool isSmoke = nadeKind == WORLD_SMOKE
			&& (Config::world_esp_smoke || nadeGlow || forceWarn || forceSmokeAim);
		const bool isMolly = nadeKind == WORLD_MOLOTOV && (Config::world_esp_molotov || nadeGlow || forceWarn);
		const bool isHE = nadeKind == WORLD_HE && (Config::world_esp_he || nadeGlow || forceWarn);
		const bool isFlash = nadeKind == WORLD_FLASH && (Config::world_esp_flash || nadeGlow || forceWarn);
		const bool isDecoy = nadeKind == WORLD_DECOY && (Config::world_esp_decoy || nadeGlow || forceWarn);

		// ── Planted C4 ──────────────────────────────────────
		if (isBomb) {
			const PlantedBombState bs = ReadPlantedBomb(reinterpret_cast<C_PlantedC4*>(Entity));
			// Hide after real explode. Defused stays on the bomb widget with timer.
			const bool liveBlow = bs.ok && curtime > 0.f && bs.blow > curtime && (bs.blow - curtime) <= 45.f;
			const bool explodedDead = bs.ok && bs.exploded && !bs.ticking && !liveBlow;
			if (explodedDead) {
				s_bombTrackKey = 0;
				s_bombEndWall = 0.f;
				continue;
			}

			sawBomb = true;
			WorldCache w{};
			w.kind = WORLD_BOMB;
			w.position = origin;
			w.bomb_site = bs.ok ? bs.site : -1;
			if (w.bomb_site < 0 || w.bomb_site > 1) {
				const int classified = Bomb::ClassifySite(origin);
				if (classified >= 0)
					w.bomb_site = classified;
			}
			w.defusing = bs.ok && bs.defusing && !bs.defused;
			w.timer = -1.f;

			float gameLeft = -1.f;
			if (liveBlow)
				gameLeft = bs.blow - curtime;

			// Epoch isolates recycled C4 handles across rounds
			const uint32_t bKey = WorldHandleKey(Entity, i)
				^ (s_worldRoundEpoch * 0x9E3779B9u)
				^ WorldThrowSig(origin);
			const float wallNow = WallTimeSec();
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
			else if (bs.ok && (bs.ticking || bs.defused))
				w.timer = seedLen;

			const bool isDefused = bs.ok && bs.defused;

			if (isDefused) {
				const char siteCh = (w.bomb_site == 0) ? 'A' : (w.bomb_site == 1) ? 'B' : '?';
				if (w.timer >= 0.f)
					snprintf(w.label, sizeof(w.label), "DEFUSED %c  %.1fs", siteCh, w.timer);
				else
					snprintf(w.label, sizeof(w.label), "DEFUSED %c", siteCh);
				g_plantedBomb.active = true;
				g_plantedBomb.site = w.bomb_site;
				g_plantedBomb.position = origin;
				g_plantedBomb.defusing = false;
				g_plantedBomb.defused = true;
				g_plantedBomb.blowLeft = w.timer;
				g_plantedBomb.defuseLeft = -1.f;
				snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
				// Widget keeps showing; world ESP can still draw DEFUSED chip
				if (Config::glow_world_bomb)
					Glow::ApplyWorld(Entity, Config::world_esp_bomb_color, true);
				if (Config::world_esp_bomb)
					cached_world.push_back(w);
				continue;
			}

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
				g_plantedBomb.defused = false;
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
				g_plantedBomb.defused = false;
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
				g_plantedBomb.defused = false;
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
			w.position = origin;
			ImVec4 gcol = Config::world_esp_smoke_color;
			if (isSmoke) {
				w.kind = WORLD_SMOKE;
				snprintf(w.weapon_key, sizeof(w.weapon_key), "smokegrenade");
				gcol = Config::world_esp_smoke_color;
				w.use_badge = true;
				bool didSmoke = false;
				static uint32_t s_smokeDidOff = 0;
				static uint32_t s_smokeDetOff = 0;
				if (!s_smokeDidOff) {
					s_smokeDidOff = SchemaFinder::Get(
						hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_bDidSmokeEffect"));
					if (!s_smokeDidOff) s_smokeDidOff = 0x127C;
				}
				if (!s_smokeDetOff) {
					s_smokeDetOff = SchemaFinder::Get(
						hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_vSmokeDetonationPos"));
					if (s_smokeDetOff < 0x100) s_smokeDetOff = 0x1290;
				}
				const uint32_t smokeOff = s_smokeDidOff;
				{
					auto* pb = reinterpret_cast<uint8_t*>(base) + smokeOff;
					if (Mem::IsReadable(pb, 1))
						didSmoke = (*pb != 0);
				}
				w.effect_active = didSmoke;
				// IDA SmokeVolume: outer = cl_smoke_torus_ring_radius(61)+subradius(88)
				w.radius = 149.f;
				if (didSmoke) {
					const uint32_t detOff = s_smokeDetOff;
					auto* pd = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + detOff);
					if (Mem::IsReadable(pd, 12) && std::isfinite(pd[0]) && std::isfinite(pd[1]) && std::isfinite(pd[2])) {
						const float dx = pd[0] - origin.x;
						const float dy = pd[1] - origin.y;
						const float dz = pd[2] - origin.z;
						if (dx * dx + dy * dy + dz * dz > 1.f)
							w.position = Vector_t{ pd[0], pd[1], pd[2] };
					}
				}
				snprintf(w.label, sizeof(w.label), "SMOKE");
			}
			else if (isMolly) {
				w.kind = WORLD_MOLOTOV;
				const bool fire = strstr(tag, "Inferno") || strstr(tag2, "inferno");
				gcol = Config::world_esp_molotov_color;
				w.use_badge = true;
				if (fire) {
					// C_Inferno: lit fire positions → convex hull with live half-width
					snprintf(w.label, sizeof(w.label), "FIRE");
					snprintf(w.weapon_key, sizeof(w.weapon_key), "molotov");
					w.effect_active = true;
					w.radius = 50.f;
					w.fire_count = 0;
					w.fire_half_width = 0.f;

					// Cache once — SchemaFinder map lookup was per-inferno per frame
					static int s_fcOff = 0, s_posOff = 0, s_burnOff = 0, s_halfOff = 0;
					if (!s_fcOff) {
						const uint32_t offFc = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_fireCount"));
						const uint32_t offPos = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_firePositions"));
						const uint32_t offBurn = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_bFireIsBurning"));
						const uint32_t offHalf = SchemaFinder::Get(
							hash_32_fnv1a_const("C_Inferno->m_maxFireHalfWidth"));
						s_fcOff = offFc ? (int)offFc : 0x1960;
						s_posOff = offPos ? (int)offPos : 0x1020;
						s_burnOff = offBurn ? (int)offBurn : 0x1620;
						// IDA schema: HalfWidth @ 0x858C (0x8588 = m_nlosperiod — wrong)
						s_halfOff = (offHalf >= 0x100) ? (int)offHalf : 0x858C;
					}
					const int fcOff = s_fcOff;
					const int posOff = s_posOff;
					const int burnOff = s_burnOff;
					const int halfOff = s_halfOff;

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

					// IDA drawable min 60. Store raw halfW (draw path clamps) — no 1.35 pad
					// here (old code double-padded and over-drew).
					float flameR = 60.f;
					{
						auto tryHalf = [&](int off) -> float {
							auto* pf = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + off);
							if (!Mem::IsReadable(pf, sizeof(float)))
								return 0.f;
							const float hw = *pf;
							return (hw > 12.f && hw < 120.f) ? hw : 0.f;
						};
						float hw = tryHalf(halfOff);
						if (hw < 1.f && halfOff != 0x858C)
							hw = tryHalf(0x858C);
						if (hw > 1.f)
							flameR = hw;
						if (flameR < 50.f)
							flameR = 50.f;
						if (flameR > 90.f)
							flameR = 90.f;
					}
					w.fire_half_width = flameR;

					float sumX = 0.f, sumY = 0.f, sumZ = 0.f;
					float maxR = 0.f;
					int lit = 0;
					const auto* fbase = reinterpret_cast<const uint8_t*>(base) + posOff;
					const auto* burn = reinterpret_cast<const uint8_t*>(base) + burnOff;
					const int n = (fc > 64) ? 64 : fc;
					const bool burnOk = Mem::IsReadable(burn, n);
					if (Mem::IsReadable(fbase, n * 12)) {
						// Include all non-zero fire slots in range — burning flags lag vs visuals
						for (int fi = 0; fi < n; ++fi) {
							const float* xyz = reinterpret_cast<const float*>(fbase + fi * 12);
							if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
								continue;
							if (std::fabs(xyz[0]) < 0.01f && std::fabs(xyz[1]) < 0.01f
								&& std::fabs(xyz[2]) < 0.01f)
								continue;
							const float dx = xyz[0] - origin.x;
							const float dy = xyz[1] - origin.y;
							const float dist = std::sqrt(dx * dx + dy * dy);
							if (dist > 190.f)
								continue;
							// Prefer burning; still accept non-burning non-zero (outer linger)
							if (burnOk && !burn[fi] && dist > 5.f && lit >= 1) {
								// keep outer flames that still have positions
							}
							if (w.fire_count < 64) {
								w.fire_pos[w.fire_count] = Vector_t{ xyz[0], xyz[1], xyz[2] };
								++w.fire_count;
							}
							sumX += xyz[0];
							sumY += xyz[1];
							sumZ += xyz[2];
							++lit;
							const float rr = dist + flameR;
							if (rr > maxR)
								maxR = rr;
						}
					}

					// No lit fires after warm-up → fire out
					if (lit <= 0) {
						const float leftWarm = WorldEffectRemaining(
							WorldFxKey(Entity, i, WORLD_MOLOTOV, WorldThrowSig(origin)),
							WORLD_MOLOTOV, true, WorldThrowSig(origin));
						const float age = 7.f - leftWarm;
						if (age >= 0.35f || leftWarm <= 0.05f)
							continue;
						w.radius = flameR;
						w.fire_count = 0;
					} else {
						// Badge at fire cluster center
						const float inv = 1.f / static_cast<float>(lit);
						w.position = Vector_t{ sumX * inv, sumY * inv, sumZ * inv };
						// inferno_max_range 150 + padded flame radius
						w.radius = (maxR > flameR) ? maxR : flameR;
						if (w.radius > 230.f)
							w.radius = 230.f;
					}
				} else {
					snprintf(w.label, sizeof(w.label), "MOLLY");
					snprintf(w.weapon_key, sizeof(w.weapon_key), "molotov");
					w.effect_active = false;
					w.radius = 150.f; // inferno_max_range — predicted full spread
					// Keep landed shell until fuse expires — Inferno handoff can lag
				}
			}
			else if (isHE) {
				w.kind = WORLD_HE;
				snprintf(w.label, sizeof(w.label), "HE");
				snprintf(w.weapon_key, sizeof(w.weapon_key), "hegrenade");
				gcol = Config::world_esp_he_color;
				w.use_badge = true;
				// Only hide once explode began AND body is stopped (avoid mid-air wipe)
				float vx = 0.f, vy = 0.f, vz = 0.f;
				{
					auto* pv = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + 0x3F8);
					if (Mem::IsReadable(pv, 12)) { vx = pv[0]; vy = pv[1]; vz = pv[2]; }
				}
				const bool flying = (vx * vx + vy * vy + vz * vz) >= 4.f;
				if (!flying) {
					bool explodeBegan = false;
					auto* pb = reinterpret_cast<uint8_t*>(base) + 0x1214;
					if (Mem::IsReadable(pb, 1)) explodeBegan = (*pb != 0);
					if (explodeBegan)
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
				const bool flying = (vx * vx + vy * vy + vz * vz) >= 4.f;
				if (!flying) {
					bool explodeBegan = false;
					auto* pb = reinterpret_cast<uint8_t*>(base) + 0x1214;
					if (Mem::IsReadable(pb, 1)) explodeBegan = (*pb != 0);
					if (explodeBegan)
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
				|| (isSmoke && (Config::world_esp_smoke || forceSmokeAim))
				|| (isMolly && Config::world_esp_molotov)
				|| (isHE && Config::world_esp_he) || (isFlash && Config::world_esp_flash)
				|| (isDecoy && Config::world_esp_decoy);
			if (wantNadeEsp)
				cached_world.push_back(w);
			continue;
		}

		// ── Dropped C4 (C_C4 / weapon_c4) — bomb ESP, not weapons ──
		// IDA: C_C4 vs C_PlantedC4; dropped bomb often keeps living m_hOwnerEntity
		const bool droppedC4 = IsWorldDroppedC4(tag, tag2);
		if (droppedC4) {
			const bool wantBombEsp = Config::world_esp_bomb;
			const bool wantBombGlow = Config::glow_world_bomb;
			if (!wantBombEsp && !wantBombGlow)
				continue;
			// Never skip dormant for ground C4 (same as nades)
			if (!IsDroppedWeapon(base, origin, true))
				continue;
			if (wantBombGlow)
				Glow::ApplyWorld(Entity, Config::world_esp_bomb_color, true);
			if (!wantBombEsp)
				continue;
			WorldCache w{};
			w.kind = WORLD_BOMB;
			w.position = origin;
			w.timer = -1.f;
			w.bomb_site = -1;
			w.use_badge = false;
			snprintf(w.label, sizeof(w.label), "C4");
			snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
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
		if (!IsDroppedWeapon(base, origin, false))
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

	// Fallback: pPlantedC4s pattern list, then CUtlAutoList RVA
	if (wantWorld && !sawBomb && (Config::world_esp_bomb || Config::glow_world_bomb || Config::widget_bomb)) {
		if (void* planted = Bomb::PlantedC4Entity()) {
			if (Mem::ValidEntity(planted)) {
				auto* ent = reinterpret_cast<CEntityInstance*>(planted);
				auto* base = reinterpret_cast<C_BaseEntity*>(planted);
				const Vector_t origin = GetEntityAbsOrigin(base);
				if (std::isfinite(origin.x) && !(std::fabs(origin.x) < 0.01f
					&& std::fabs(origin.y) < 0.01f && std::fabs(origin.z) < 0.01f)) {
					const PlantedBombState bs = ReadPlantedBomb(reinterpret_cast<C_PlantedC4*>(planted));
					const bool liveBlow = bs.ok && curtime > 0.f && bs.blow > curtime
						&& (bs.blow - curtime) <= 45.f;
					const bool explodedDead = bs.ok && bs.exploded && !bs.ticking && !liveBlow;
					if (!explodedDead) {
						sawBomb = true;
						WorldCache w{};
						w.kind = WORLD_BOMB;
						w.position = origin;
						w.bomb_site = bs.ok ? bs.site : -1;
						if (w.bomb_site < 0 || w.bomb_site > 1) {
							const int classified = Bomb::ClassifySite(origin);
							if (classified >= 0)
								w.bomb_site = classified;
						}
						const bool isDefused = bs.ok && bs.defused;
						w.defusing = bs.ok && bs.defusing && !isDefused;
						float gameLeft = liveBlow ? (bs.blow - curtime) : -1.f;
						w.timer = (gameLeft >= 0.f) ? gameLeft : ((bs.ok && bs.timerLength >= 10.f) ? bs.timerLength : 40.f);
						const char siteCh = (w.bomb_site == 0) ? 'A' : (w.bomb_site == 1) ? 'B' : '?';
						if (isDefused) {
							if (w.timer >= 0.f)
								snprintf(w.label, sizeof(w.label), "DEFUSED %c  %.1fs", siteCh, w.timer);
							else
								snprintf(w.label, sizeof(w.label), "DEFUSED %c", siteCh);
						} else if (bs.ok && bs.defusing)
							snprintf(w.label, sizeof(w.label), "DEFUSING");
						else
							snprintf(w.label, sizeof(w.label), "BOMB %c  %.1fs", siteCh, w.timer);
						snprintf(w.weapon_key, sizeof(w.weapon_key), "c4");
						g_plantedBomb.active = true;
						g_plantedBomb.site = w.bomb_site;
						g_plantedBomb.position = origin;
						g_plantedBomb.defusing = w.defusing;
						g_plantedBomb.defused = isDefused;
						g_plantedBomb.blowLeft = w.timer;
						g_plantedBomb.defuseLeft = -1.f;
						if (!isDefused && bs.ok && bs.defusing && curtime > 0.f && bs.defuseEnd > curtime)
							g_plantedBomb.defuseLeft = bs.defuseEnd - curtime;
						if (Config::glow_world_bomb)
							Glow::ApplyWorld(ent, Config::world_esp_bomb_color, true);
						if (Config::world_esp_bomb)
							cached_world.push_back(w);
					}
				}
			}
		}
	}
	// Stale Autolist RVA fallback removed — was 0x236D678 (wrong; IDA is 0x236E678)
	// and double-deref'd the slot. Bomb::PlantedC4Entity() above is pattern-resolved.

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

	// Vis for ESP color + glow (vis/invis palette + glow_only_visible).
	// Must run when glow is on even if esp_vis_check is off — otherwise visible stays true.
	// Sticky: if pawn moved <~6u and we traced last frame, reuse (big FPS with full glow+vis).
	const bool needVis = (Config::esp_vis_check || Config::glow)
		&& Trace::Ready() && H::oGetLocalPlayer && !cached_players.empty();
	if (needVis) {
		C_CSPlayerPawn* localPawn = H::SafeLocalPlayer();
		if (localPawn && localPawn->m_iHealth() > 0) {
			const Vector_t eye = Bones::GetEyePos(localPawn);
			if (Bones::IsValidPos(eye)) {
				const int pc = (int)cached_players.size();
				const Vector_t localPos = cached_local.active ? cached_local.position : eye;
				// Alternating recheck: even frames recheck half, odd the other — always fresh-ish
				static int s_visFrame = 0;
				++s_visFrame;
				for (int i = 0; i < pc; ++i) {
					auto& entry = cached_players[i];
					// Reuse sticky if position barely moved
					if (entry.visCached) {
						const float mdx = entry.position.x - entry.visSampleX;
						const float mdy = entry.position.y - entry.visSampleY;
						const float mdz = entry.position.z - entry.visSampleZ;
						const float moved2 = mdx * mdx + mdy * mdy + mdz * mdz;
						// Stagger full re-trace: only every other frame per slot when still
						if (moved2 < 36.f && ((i + s_visFrame) & 1) == 0)
							continue; // keep entry.visible from sticky seed
					}

					C_CSPlayerPawn* tgt = nullptr;
					if (!ResolvePawn(entry.handle, &tgt) || !tgt) {
						entry.visible = true;
						entry.visCached = false;
						continue;
					}

					const float dx = entry.position.x - localPos.x;
					const float dy = entry.position.y - localPos.y;
					const float dz = entry.position.z - localPos.z;
					const float dist2 = dx * dx + dy * dy + dz * dz;
					// ~80m — far LOD still samples head+spine (head-only caused false occluded)
					const bool farLod = dist2 > (3150.f * 3150.f);

					Vector_t samples[3]{};
					int n = 0;
					uintptr_t ba = 0;
					Vector_t origin{};
					float height = 0.f;
					Bones::Map map{};
					if (Bones::GetBoneArrayReadonly(tgt, ba, origin, height)
						&& Bones::ResolveMapCached(tgt, ba, origin, height, map)) {
						Vector_t head{};
						if (Bones::GetSlotPos(ba, map, Bones::S_HEAD, head) && Bones::IsValidPos(head))
							samples[n++] = head;
						Vector_t p{};
						if (n < 3 && Bones::GetSlotPos(ba, map, Bones::S_SPINE2, p) && Bones::IsValidPos(p))
							samples[n++] = p;
						if (!farLod && n < 3
							&& Bones::GetSlotPos(ba, map, Bones::S_PELVIS, p) && Bones::IsValidPos(p))
							samples[n++] = p;
					}
					if (n < 1) {
						samples[n++] = entry.position + entry.viewOffset;
						if (!farLod)
							samples[n++] = entry.position;
					}

					// Head-first: stop on first hit (IsBodyVisible already early-outs)
					entry.visible = Trace::IsBodyVisible(eye, samples, n, localPawn, tgt);
					entry.visCached = true;
					entry.visSampleX = entry.position.x;
					entry.visSampleY = entry.position.y;
					entry.visSampleZ = entry.position.z;
				}
			}
		}
	}

	// Rebuild sticky table for next frame
	s_visStickyN = 0;
	for (const auto& e : cached_players) {
		if (s_visStickyN >= 64)
			break;
		if (!e.handle.valid())
			continue;
		VisSticky& s = s_visSticky[s_visStickyN++];
		s.h = static_cast<std::uint32_t>(e.handle.index());
		s.vis = e.visible;
		s.x = e.position.x;
		s.y = e.position.y;
		s.z = e.position.z;
		s.ok = true;
	}

	// Keep engine glow colours sticky and independent of chams (DrawArray mesh tint).
	// Uses cache visibility so glow vis/invis palettes stay correct without re-trace.
	if (Config::glow && !VacDetect::IsSoftPaused() && !cached_players.empty()) {
		for (const auto& entry : cached_players) {
			C_CSPlayerPawn* tgt = nullptr;
			if (!ResolvePawn(entry.handle, &tgt) || !tgt)
				continue;
			Glow::ApplyPlayer(tgt, entry.visible);
		}
	}
}

static void DrawSkeleton(ImDrawList* drawList, C_CSPlayerPawn* pawn, const ViewMatrix& vm, ImU32 col) {
	if (!drawList || !pawn)
		return;

	uintptr_t boneArray = 0;
	Vector_t origin{};
	float height = 0.f;
	if (!Bones::GetBoneArrayReadonly(pawn, boneArray, origin, height))
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

// 3D collision AABB wireframe (oriented via abs origin + hull mins/maxs).
static void Draw3DBox(ImDrawList* dl, C_CSPlayerPawn* pawn, const ViewMatrix& vm, ImU32 col, float thickness) {
	if (!dl || !pawn || !Mem::ValidEntity(pawn))
		return;
	CCollisionProperty* colp = pawn->m_pCollision();
	if (!colp || !Mem::Valid(colp, 0x50))
		return;
	const Vector_t origin = GetAbsOrigin(pawn);
	Vector_t mins = colp->m_vecMins();
	Vector_t maxs = colp->m_vecMaxs();
	if (!Bones::IsValidPos(origin))
		return;
	const float hx = maxs.x - mins.x, hy = maxs.y - mins.y, hz = maxs.z - mins.z;
	if (hx < 1.f || hy < 1.f || hz < 8.f)
		return;
	mins.x += origin.x; mins.y += origin.y; mins.z += origin.z;
	maxs.x += origin.x; maxs.y += origin.y; maxs.z += origin.z;

	const Vector_t corners[8] = {
		{ mins.x, mins.y, mins.z }, { maxs.x, mins.y, mins.z },
		{ maxs.x, maxs.y, mins.z }, { mins.x, maxs.y, mins.z },
		{ mins.x, mins.y, maxs.z }, { maxs.x, mins.y, maxs.z },
		{ maxs.x, maxs.y, maxs.z }, { mins.x, maxs.y, maxs.z },
	};
	Vector_t scr[8]{};
	bool ok[8]{};
	int nOk = 0;
	for (int i = 0; i < 8; ++i) {
		ok[i] = vm.WorldToScreen(corners[i], scr[i]);
		if (ok[i]) ++nOk;
	}
	if (nOk < 2)
		return;

	static const int edges[12][2] = {
		{0,1},{1,2},{2,3},{3,0},
		{4,5},{5,6},{6,7},{7,4},
		{0,4},{1,5},{2,6},{3,7}
	};
	const float t = std::clamp(thickness, 1.f, 4.f);
	const ImU32 outline = IM_COL32(0, 0, 0, 180);
	for (const auto& e : edges) {
		if (!ok[e[0]] || !ok[e[1]])
			continue;
		const ImVec2 a(scr[e[0]].x, scr[e[0]].y);
		const ImVec2 b(scr[e[1]].x, scr[e[1]].y);
		dl->AddLine(a, b, outline, t + 1.2f);
		dl->AddLine(a, b, col, t);
	}
}

// OOF: W2S::ProjectOrEdge (front = NDC edge, behind = yaw/pitch compass).
// Old path used WorldToScreen + world-XY atan2 → wrong side when behind, and
// radius floated mid-screen without clamping to the visible edge rect.
static bool GetLocalEyeAndView(Vector_t& eye, QAngle_t& ang) {
	eye.x = eye.y = eye.z = 0.f;
	ang.x = ang.y = ang.z = 0.f;
	if (cached_local.active && Bones::IsValidPos(cached_local.position)) {
		eye = cached_local.position;
		// Approximate eye height if we have no live pawn this frame
		eye.z += 64.f;
	}
	C_CSPlayerPawn* local = H::SafeLocalPlayer();
	if (local) {
		const Vector_t e = Bones::GetEyePos(local);
		if (Bones::IsValidPos(e))
			eye = e;
	}
	if (!Bones::IsValidPos(eye))
		return false;

	if (Input::GetViewAngles && Input::viewAngleContext) {
		const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
		if (viewPtr && Mem::IsReadable(reinterpret_cast<void*>(viewPtr), sizeof(Vector_t))) {
			Vector_t v{};
			v.x = v.y = v.z = 0.f;
			__try { v = *reinterpret_cast<const Vector_t*>(viewPtr); }
			__except (EXCEPTION_EXECUTE_HANDLER) { v.x = v.y = v.z = 0.f; }
			if (std::isfinite(v.x) && std::isfinite(v.y)) {
				ang.x = v.x;
				ang.y = v.y;
				ang.z = 0.f;
			}
		}
	}
	return true;
}

static ImU32 OofMulAlpha(ImU32 col, float a) {
	a = std::clamp(a, 0.f, 1.f);
	const int aa = static_cast<int>(((col >> 24) & 0xFFu) * a);
	return (col & 0x00FFFFFFu) | (static_cast<ImU32>(aa) << 24);
}

static void DrawOofArrow(ImDrawList* dl, const PlayerCache& player,
	const Vector_t& eye, const QAngle_t& viewAng,
	ImU32 baseCol, float radius, float size, bool occluded)
{
	if (!dl)
		return;
	const ImGuiIO& io = ImGui::GetIO();
	const float sw = io.DisplaySize.x;
	const float sh = io.DisplaySize.y;
	if (sw < 32.f || sh < 32.f)
		return;
	const float cx = sw * 0.5f;
	const float cy = sh * 0.5f;

	// Chest aim point (stable vs head bob)
	Vector_t world = player.position;
	const float eyeLift = (player.viewOffset.z > 1.f) ? player.viewOffset.z * 0.55f : 36.f;
	world.z += eyeLift;
	if (!Bones::IsValidPos(world))
		return;

	// Edge margin keeps tip + label inside HUD chrome
	const float edgeMargin = 22.f + std::clamp(size, 8.f, 28.f) * 0.35f;
	float edgeX = 0.f, edgeY = 0.f;
	bool onScreen = false;
	if (!W2S::ProjectOrEdge(world, edgeX, edgeY, onScreen, edgeMargin, eye, viewAng))
		return;
	if (onScreen)
		return; // fully in view — box ESP owns them

	float dx = edgeX - cx;
	float dy = edgeY - cy;
	float edgeLen = std::sqrt(dx * dx + dy * dy);
	if (edgeLen < 1e-3f) {
		dx = 0.f; dy = -1.f; edgeLen = 1.f;
	} else {
		dx /= edgeLen; dy /= edgeLen;
	}

	// Radius = max px from center; always clamp so tip stays on-screen edge.
	const float maxR = edgeLen - 2.f;
	if (maxR < 24.f)
		return;
	const float r = std::clamp(radius, 48.f, maxR);
	const ImVec2 tip(cx + dx * r, cy + dy * r);

	// Distance (hammer units → meters, same scale as player ESP)
	float meters = 0.f;
	if (cached_local.active) {
		const float wx = player.position.x - cached_local.position.x;
		const float wy = player.position.y - cached_local.position.y;
		const float wz = player.position.z - cached_local.position.z;
		meters = std::sqrt(wx * wx + wy * wy + wz * wz) * 0.0254f;
	}

	// Size: base + slight boost when close (awareness)
	float s = std::clamp(size, 8.f, 32.f);
	if (meters > 0.f && meters < 25.f)
		s *= 1.f + (25.f - meters) * 0.012f;

	// Alpha: wall targets quieter; far targets fade a bit
	float alpha = occluded ? 0.55f : 1.f;
	if (meters > 40.f)
		alpha *= std::clamp(1.f - (meters - 40.f) / 80.f, 0.45f, 1.f);

	// Subtle pulse so OOF stays readable without flash spam
	const float pulse = 0.88f + 0.12f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.5f);
	alpha *= pulse;

	const ImU32 col = OofMulAlpha(baseCol, alpha);
	const ImU32 ink = OofMulAlpha(IM_COL32(0, 0, 0, 230), alpha);
	const ImU32 glow = OofMulAlpha(baseCol, alpha * 0.28f);

	// Soft glow disc under arrow
	dl->AddCircleFilled(tip, s * 0.95f, glow, 16);

	// Chevron: longer tip, wider base, slight shaft
	const float px = -dy, py = dx;
	const float tipLen = s * 1.05f;
	const float baseW = s * 0.62f;
	const float shaft = s * 0.28f;
	const ImVec2 t0(tip.x + dx * tipLen * 0.15f, tip.y + dy * tipLen * 0.15f);
	const ImVec2 mid(tip.x - dx * tipLen * 0.55f, tip.y - dy * tipLen * 0.55f);
	const ImVec2 t1(mid.x + px * baseW, mid.y + py * baseW);
	const ImVec2 t2(mid.x - px * baseW, mid.y - py * baseW);
	const ImVec2 tail(tip.x - dx * (tipLen + shaft), tip.y - dy * (tipLen + shaft));

	// Outline pass
	dl->AddTriangle(t0, t1, t2, ink, 2.4f);
	dl->AddLine(mid, tail, ink, 3.2f);
	// Fill
	dl->AddTriangleFilled(t0, t1, t2, col);
	dl->AddLine(mid, tail, col, 1.8f);
	// Inner highlight (top edge of chevron)
	const ImU32 hi = OofMulAlpha(IM_COL32(255, 255, 255, 90), alpha);
	dl->AddLine(t0, t1, hi, 1.1f);

	// Distance label — inward from tip so it never clips the bezel
	if (meters > 0.5f) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%dm", static_cast<int>(meters + 0.5f));
		const ImVec2 ts = ImGui::CalcTextSize(buf);
		const float lx = tip.x - dx * (s + 10.f) - ts.x * 0.5f;
		const float ly = tip.y - dy * (s + 10.f) - ts.y * 0.5f;
		DrawTextOutlined(dl, floorf(lx), floorf(ly), col, buf);
	}

	// Low HP accent pip
	if (player.health > 0 && player.health <= 30) {
		const ImU32 hpPip = OofMulAlpha(IM_COL32(255, 70, 70, 255), alpha);
		dl->AddCircleFilled(
			ImVec2(tip.x + px * (s * 0.55f), tip.y + py * (s * 0.55f)),
			2.6f, hpPip, 8);
	}
}

void Visuals::drawPlayers() {
	const bool anyFlag = Config::flag_flashed || Config::flag_bomb || Config::flag_scoped
		|| Config::flag_reloading || Config::flag_defusing
		|| Config::flag_money || Config::flag_kit || Config::flag_helmet || Config::flag_nades;
	if (!Config::esp && !Config::showHealth && !Config::espFill && !Config::showNameTags
		&& !Config::showArmor && !Config::showDistance && !Config::showWeapon
		&& !Config::showWeaponIcon
		&& !Config::esp_skeleton && !anyFlag
		&& !Config::esp_rank && !Config::esp_3d_box && !Config::esp_oof)
		return;

	if (cached_players.empty())
		return;

	const int filterTeam = cached_local.team != 0 ? cached_local.team : cached_local.lastTeam;

	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	if (!drawList)
		return;

	// Precompute colors once per frame (not per-player)
	const ImU32 colBoxVis = ImGui::ColorConvertFloat4ToU32(Config::espColor);
	const ImU32 colBoxOcc = ImGui::ColorConvertFloat4ToU32(Config::espColorInvisible);
	const ImU32 colSkelVis = ImGui::ColorConvertFloat4ToU32(Config::esp_skeleton_color);
	const ImU32 colSkelOcc = ImGui::ColorConvertFloat4ToU32(Config::esp_skeleton_color_invisible);
	const ImU32 colName = ImGui::ColorConvertFloat4ToU32(Config::esp_name_color);
	const ImU32 colWep = ImGui::ColorConvertFloat4ToU32(Config::esp_weapon_color);
	const ImU32 colDist = ImGui::ColorConvertFloat4ToU32(Config::esp_distance_color);
	const ImU32 colArmor = ImGui::ColorConvertFloat4ToU32(Config::esp_armor_color);
	const ImU32 colHpFixed = ImGui::ColorConvertFloat4ToU32(Config::esp_health_color);
	const ImU32 colRank = ImGui::ColorConvertFloat4ToU32(Config::esp_rank_color);
	const ImU32 col3d = ImGui::ColorConvertFloat4ToU32(Config::esp_3d_box_color);
	const ImU32 colOof = ImGui::ColorConvertFloat4ToU32(Config::esp_oof_color);
	ImVec4 fillVis = Config::espColor;
	fillVis.w = Config::espFillOpacity;
	ImVec4 fillOcc = Config::espColorInvisible;
	fillOcc.w = Config::espFillOpacity;
	const ImU32 colFillVis = ImGui::ColorConvertFloat4ToU32(fillVis);
	const ImU32 colFillOcc = ImGui::ColorConvertFloat4ToU32(fillOcc);
	const float lineH = ImGui::GetFontSize();
	const bool doTeamCheck = GameMode::WantTeamCheck(Config::teamCheck);
	const bool wantSkel = Config::esp_skeleton;
	const bool wantBox = Config::esp;
	const bool wantFill = Config::espFill && Config::esp;
	const bool want3d = Config::esp_3d_box;
	const bool wantOof = Config::esp_oof;

	// Resolve eye/view once per frame for OOF (ProjectOrEdge behind-cam path)
	Vector_t oofEye;
	QAngle_t oofAng;
	oofEye.x = oofEye.y = oofEye.z = 0.f;
	oofAng.x = oofAng.y = oofAng.z = 0.f;
	const bool oofReady = wantOof && GetLocalEyeAndView(oofEye, oofAng);

	for (const auto& Player : cached_players) {
		if (!Player.handle.valid() || Player.health <= 0)
			continue;

		if (doTeamCheck && filterTeam != 0 && Player.team_num == filterTeam)
			continue;

		const bool useOccluded = Config::esp_vis_check && !Player.visible;
		const ImU32 boxColor = useOccluded ? colBoxOcc : colBoxVis;
		const ImU32 skelColor = useOccluded ? colSkelOcc : colSkelVis;
		const ImU32 fillColor = useOccluded ? colFillOcc : colFillVis;

		C_CSPlayerPawn* drawPawn = nullptr;
		// Skeleton + collision box need live pawn; text-only path can skip resolve
		if (wantSkel || wantBox || want3d || Config::espFill || Config::showHealth || Config::showArmor
			|| Config::showNameTags || Config::showDistance || Config::showWeapon
			|| Config::showWeaponIcon || anyFlag || Config::esp_rank)
			ResolvePawn(Player.handle, &drawPawn);

		if (oofReady)
			DrawOofArrow(drawList, Player, oofEye, oofAng, colOof,
				Config::esp_oof_radius, Config::esp_oof_size, useOccluded);

		if (wantSkel && drawPawn)
			DrawSkeleton(drawList, drawPawn, viewMatrix, skelColor);

		if (want3d && drawPawn)
			Draw3DBox(drawList, drawPawn, viewMatrix, col3d, Config::espThickness);

		float boxX = 0.f, boxY = 0.f, boxW = 0.f, boxH = 0.f;
		if (!ComputeEspBox(drawPawn, Player, viewMatrix, boxX, boxY, boxW, boxH))
			continue;
		if (boxW < 2.f || boxH < 4.f)
			continue;

		if (wantFill) {
			drawList->AddRectFilled(
				ImVec2(boxX, boxY),
				ImVec2(boxX + boxW, boxY + boxH),
				fillColor);
		}

		if (wantBox)
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
				hpCol = colHpFixed;
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
			DrawSideBar(drawList, barX, boxY, barW, boxH, ratio, colArmor);
		}

		const float cx = boxX + boxW * 0.5f;

		float nameTopY = boxY;
		if (Config::showNameTags && Player.name[0] != '\0') {
			const ImVec2 ns = ImGui::CalcTextSize(Player.name);
			const float avatarSz = floorf(lineH + 1.f);
			const float avatarGap = 3.f;
			const bool drawAvatar = Config::esp_name_avatar;

			float totalW = ns.x;
			if (drawAvatar)
				totalW += avatarSz + avatarGap;

			float x = floorf(cx - totalW * 0.5f);
			const float ny = floorf(boxY - ns.y - 3.f);
			nameTopY = ny;

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
						IM_COL32(36, 38, 46, 200), 12);
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

			DrawTextOutlined(drawList, x, ny, colName, Player.name);
		}

		if (Config::esp_rank) {
			const char* rn = CompetitiveRankName(Player.rank);
			if (rn && rn[0]) {
				const ImVec2 rs = ImGui::CalcTextSize(rn);
				const float ry = floorf(nameTopY - rs.y - 1.f);
				DrawTextOutlined(drawList, floorf(cx - rs.x * 0.5f), ry, colRank, rn);
			}
		}

		// Under-box stack: icon → weapon text → distance (tight, aligned)
		float textY = floorf(boxY + boxH + 2.f);

		if (Config::showWeaponIcon) {
			const float ih = DrawWeaponIconCentered(drawList, cx, textY, colWep, Player.weapon_key);
			if (ih > 0.f)
				textY += ih;
		}

		if (Config::showWeapon && Player.weapon_name[0] != '\0') {
			DrawCenteredText(drawList, cx, textY, colWep, Player.weapon_name);
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
			DrawCenteredText(drawList, cx, textY, colDist, distText);
		}

		if (anyFlag) {
			float flagY = floorf(boxY);
			const float flagX = floorf(boxX + boxW + 3.f);
			const float flagLine = lineH;

			if (Config::flag_money && Player.money >= 0) {
				char mon[16];
				snprintf(mon, sizeof(mon), "$%d", Player.money);
				DrawTextOutlined(drawList, flagX, flagY, IM_COL32(120, 220, 130, 255), mon);
				flagY += flagLine;
			}

			struct FlagItem { bool on; const char* text; ImU32 col; };
			const FlagItem flags[] = {
				{ Config::flag_flashed   && Player.flashed,   "FLASH",  IM_COL32(255, 235, 90, 255)  },
				{ Config::flag_bomb      && Player.bomb,      "C4",     IM_COL32(255, 95, 95, 255)   },
				{ Config::flag_scoped    && Player.scoped,    "ZOOM",   IM_COL32(130, 205, 255, 255) },
				{ Config::flag_reloading && Player.reloading, "R",      IM_COL32(255, 185, 70, 255)  },
				{ Config::flag_defusing  && Player.defusing,  "DEF",    IM_COL32(90, 220, 140, 255)  },
				{ Config::flag_kit       && Player.has_defuser, "KIT",  IM_COL32(90, 200, 255, 255)  },
				{ Config::flag_helmet    && Player.has_helmet,  "HK",   IM_COL32(180, 200, 255, 255) },
			};
			for (const auto& f : flags) {
				if (!f.on)
					continue;
				DrawTextOutlined(drawList, flagX, flagY, f.col, f.text);
				flagY += flagLine;
			}

			if (Config::flag_nades) {
				char nbuf[12]{};
				int ni = 0;
				if (Player.nade_he && ni < 10) nbuf[ni++] = 'H';
				if (Player.nade_flash && ni < 10) nbuf[ni++] = 'F';
				if (Player.nade_smoke && ni < 10) nbuf[ni++] = 'S';
				if (Player.nade_molly && ni < 10) nbuf[ni++] = 'M';
				if (Player.nade_decoy && ni < 10) nbuf[ni++] = 'D';
				nbuf[ni] = '\0';
				if (ni > 0) {
					DrawTextOutlined(drawList, flagX, flagY, IM_COL32(255, 170, 80, 255), nbuf);
				}
			}
		}
	}
}

// Forward — used by DrawInfernoRadius (defined below)
static void DrawWorldRadiusRing(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
	float radius, ImU32 col, int segs);

// 2D monotone chain on XY (world). Drops non-finite / duplicates.
static bool InfernoCompareXY(const Vector_t& a, const Vector_t& b)
{
	return (a.x < b.x) || (a.x == b.x && a.y < b.y);
}

static std::vector<Vector_t> InfernoConvexHull(std::vector<Vector_t> points)
{
	// Filter garbage so sort never hits NaN
	std::vector<Vector_t> clean;
	clean.reserve(points.size());
	for (const auto& p : points) {
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
			continue;
		clean.push_back(p);
	}
	if (clean.size() < 3)
		return clean;

	std::sort(clean.begin(), clean.end(), InfernoCompareXY);
	// Dedup nearly identical XY (keeps hull stable)
	{
		std::vector<Vector_t> uniq;
		uniq.reserve(clean.size());
		for (const auto& p : clean) {
			if (!uniq.empty()) {
				const float dx = p.x - uniq.back().x;
				const float dy = p.y - uniq.back().y;
				if (dx * dx + dy * dy < 0.25f)
					continue;
			}
			uniq.push_back(p);
		}
		clean.swap(uniq);
	}
	if (clean.size() < 3)
		return clean;

	auto cross = [](const Vector_t& O, const Vector_t& A, const Vector_t& B) {
		return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
	};

	std::vector<Vector_t> hull;
	hull.reserve(clean.size() + 2);
	for (const auto& p : clean) {
		while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0.f)
			hull.pop_back();
		hull.push_back(p);
	}

	const size_t lower = hull.size();
	for (int i = static_cast<int>(clean.size()) - 2; i >= 0; --i) {
		while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), clean[static_cast<size_t>(i)]) <= 0.f)
			hull.pop_back();
		hull.push_back(clean[static_cast<size_t>(i)]);
	}

	if (!hull.empty())
		hull.pop_back();
	return hull;
}

// Clip-aware closed polyline (same near-plane rule as DrawWorldRadiusRing).
// Never drops half a hull into a self-intersecting screen poly.
static void DrawClippedWorldPoly(ImDrawList* dl, const ViewMatrix& vm,
	const std::vector<Vector_t>& worldPts, ImU32 col, float thick)
{
	if (!dl || !vm.viewMatrix || worldPts.size() < 2)
		return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x <= 1.f || ds.y <= 1.f)
		return;
	const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;
	constexpr float kClipNear = 0.12f;

	struct CV { float numX = 0.f, numY = 0.f, w = 0.f; bool ok = false; };
	auto project = [&](const Vector_t& wp, CV& o) {
		o.ok = vm.WorldToClip(wp, o.numX, o.numY, o.w);
	};
	auto toScr = [cx, cy](const CV& v) {
		const float inv = 1.f / v.w;
		return ImVec2(cx + v.numX * inv * cx, cy - v.numY * inv * cy);
	};
	auto clipSeg = [&](const CV& a, const CV& b) {
		if (!a.ok || !b.ok)
			return;
		const bool af = a.w >= kClipNear;
		const bool bf = b.w >= kClipNear;
		if (!af && !bf)
			return;
		CV ca = a, cb = b;
		if (af != bf) {
			const float den = b.w - a.w;
			if (std::fabs(den) < 1e-6f)
				return;
			const float t = (kClipNear - a.w) / den;
			if (t < 0.f || t > 1.f)
				return;
			CV mid;
			mid.numX = a.numX + (b.numX - a.numX) * t;
			mid.numY = a.numY + (b.numY - a.numY) * t;
			mid.w = kClipNear;
			mid.ok = true;
			if (af) cb = mid; else ca = mid;
		}
		dl->AddLine(toScr(ca), toScr(cb), col, thick);
	};

	const int n = static_cast<int>(worldPts.size());
	CV prev{};
	project(worldPts[0], prev);
	for (int i = 1; i < n; ++i) {
		CV cur{};
		project(worldPts[static_cast<size_t>(i)], cur);
		clipSeg(prev, cur);
		prev = cur;
	}
	// Close loop
	CV first{};
	project(worldPts[0], first);
	clipSeg(prev, first);
}

// Soft fill only when every hull vertex projects (avoids broken fan from partial W2S)
static void InfernoFillIfFullyVisible(ImDrawList* dl, const ViewMatrix& vm,
	const std::vector<Vector_t>& hull, const ImVec4& col, float aMul)
{
	if (!dl || hull.size() < 3)
		return;
	std::vector<ImVec2> scr;
	scr.reserve(hull.size());
	for (const auto& wp : hull) {
		Vector_t s{};
		if (!vm.WorldToScreen(wp, s))
			return; // any miss → skip fill (outline still drawn via clip)
		if (!std::isfinite(s.x) || !std::isfinite(s.y))
			return;
		scr.emplace_back(s.x, s.y);
	}
	if (scr.size() < 3)
		return;

	ImVec2 c(0.f, 0.f);
	for (const auto& p : scr) { c.x += p.x; c.y += p.y; }
	const float inv = 1.f / static_cast<float>(scr.size());
	c.x *= inv; c.y *= inv;

	const int cr = (int)(col.x * 255.f), cg = (int)(col.y * 255.f), cb = (int)(col.z * 255.f);
	const int centerA = static_cast<int>(std::clamp(38.f * aMul, 0.f, 255.f));
	const ImU32 fill = IM_COL32(
		(std::min)(255, cr + 28), (std::min)(255, cg + 12), cb, centerA);

	const int n = static_cast<int>(scr.size());
	for (int i = 0; i < n; ++i) {
		const ImVec2& a = scr[static_cast<size_t>(i)];
		const ImVec2& b = scr[static_cast<size_t>((i + 1) % n)];
		dl->AddTriangleFilled(c, a, b, fill);
	}
}

// Live inferno: clip-safe hull outline + soft fill when fully on-screen + per-flame rings.
// Old path dropped partial W2S verts → self-intersecting polylines / broken fill.
static void DrawInfernoRadius(ImDrawList* dl, const ViewMatrix& vm, const WorldCache& item,
	const ImVec4& col4, float alphaScale)
{
	if (!dl || !vm.viewMatrix || item.fire_count <= 0 || alphaScale < 0.01f)
		return;

	const ImVec4 col = Config::fire_color ? Config::fire_color_value : col4;
	const float aMul = std::clamp(alphaScale, 0.f, 1.f);
	const int cr = (int)(col.x * 255.f), cg = (int)(col.y * 255.f), cb = (int)(col.z * 255.f);

	// half-width already includes visual pad from cache; clamp to sane flame size
	float fireR = item.fire_half_width;
	if (!(fireR > 10.f && fireR < 100.f))
		fireR = 60.f; // IDA min drawable — no extra 1.35 here (was double-padded)
	// Slight pad only for outline readability
	fireR = std::clamp(fireR, 40.f, 90.f);

	// Collect valid fires (skip zeros / NaN)
	Vector_t fires[64];
	int nFire = 0;
	const int nIn = (item.fire_count > 64) ? 64 : item.fire_count;
	float cx = 0.f, cy = 0.f, cz = 0.f;
	for (int i = 0; i < nIn; ++i) {
		const Vector_t& p = item.fire_pos[i];
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
			continue;
		if (std::fabs(p.x) < 0.01f && std::fabs(p.y) < 0.01f && std::fabs(p.z) < 0.01f)
			continue;
		fires[nFire++] = p;
		cx += p.x; cy += p.y; cz += p.z;
	}
	if (nFire <= 0)
		return;
	const float invN = 1.f / static_cast<float>(nFire);
	cx *= invN; cy *= invN; cz *= invN;

	// Cap fires used for hull construction
	const int nHullFire = (nFire > 20) ? 20 : nFire;
	constexpr int kSegs = 10; // enough for envelope; less than 14×24 spam
	constexpr float kAngleStep = 6.28318530718f / static_cast<float>(kSegs);

	std::vector<Vector_t> ringPts;
	ringPts.reserve(static_cast<size_t>(nHullFire) * static_cast<size_t>(kSegs));
	for (int fi = 0; fi < nHullFire; ++fi) {
		const Vector_t& pos = fires[fi];
		for (int j = 0; j < kSegs; ++j) {
			const float a = static_cast<float>(j) * kAngleStep;
			ringPts.push_back(Vector_t{
				pos.x + std::cos(a) * fireR,
				pos.y + std::sin(a) * fireR,
				pos.z + 1.5f
			});
		}
	}

	const std::vector<Vector_t> hull = InfernoConvexHull(std::move(ringPts));

	// Soft fill first (under outline), only if fully visible
	if (hull.size() >= 3)
		InfernoFillIfFullyVisible(dl, vm, hull, col, aMul);

	// Clip-safe outline (works when standing inside fire / camera clips hull)
	if (hull.size() >= 3) {
		const int outlineA = static_cast<int>(std::clamp(185.f * aMul, 0.f, 255.f));
		const int darkA = static_cast<int>(std::clamp(120.f * aMul, 0.f, 255.f));
		DrawClippedWorldPoly(dl, vm, hull, IM_COL32(0, 0, 0, darkA), 2.4f);
		DrawClippedWorldPoly(dl, vm, hull, IM_COL32(cr, cg, cb, outlineA), 1.35f);
	}

	// Per-flame soft ground rings (clean footprint; primary when hull fails)
	const int nRings = (nFire > 12) ? 12 : nFire;
	const ImU32 ringCol = IM_COL32(cr, cg, cb, static_cast<int>(std::clamp(70.f * aMul, 0.f, 255.f)));
	const ImU32 ringSoft = IM_COL32(cr, cg, cb, static_cast<int>(std::clamp(28.f * aMul, 0.f, 255.f)));
	for (int fi = 0; fi < nRings; ++fi) {
		// Only outer-ish fires when many (skip dense cluster core spam)
		if (nFire > 8) {
			const float dx = fires[fi].x - cx;
			const float dy = fires[fi].y - cy;
			if (dx * dx + dy * dy < (fireR * 0.35f) * (fireR * 0.35f) && fi > 2)
				continue;
		}
		DrawWorldRadiusRing(dl, vm, fires[fi], fireR * 0.55f, ringSoft, 16);
		DrawWorldRadiusRing(dl, vm, fires[fi], fireR * 0.55f, ringCol, 16);
	}
}

static void DrawWorldRadiusRing(ImDrawList* dl, const ViewMatrix& vm, const Vector_t& center,
                                float radius, ImU32 col, int segs = 40) {
	if (!dl || radius <= 1.f || !vm.viewMatrix)
		return;
	if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)
		|| !std::isfinite(radius) || radius > 500.f)
		return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	if (ds.x <= 1.f || ds.y <= 1.f)
		return;
	const float cx = ds.x * 0.5f, cy = ds.y * 0.5f;
	if (segs < 8) segs = 8;
	if (segs > 64) segs = 64;

	// Near-plane distance: below this a vertex is at/behind the camera.
	// Segments crossing it are clipped to the plane instead of dropped whole,
	// so the ring stays continuous when the local player stands on the effect.
	constexpr float kClipNear = 0.12f;

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
					const float den = cur.w - prev.w;
					if (std::fabs(den) < 1e-6f) {
						prev = cur;
						continue;
					}
					const float t = (kClipNear - prev.w) / den;
					if (t < 0.f || t > 1.f) {
						prev = cur;
						continue;
					}
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

// Compact warn badge: soft disc + track arc + icon + timer chip. Uses nade_warn_icon_size.
static void DrawWorldNadeBadge(ImDrawList* dl, float cx, float cy, const ImVec4& col4,
                               const char* weaponKey, const char* label, bool active,
                               float timeLeft, NadePred::NadeType typeHint = NadePred::NadeType::Unknown) {
	if (!dl)
		return;
	// Guard bad screen coords (edge project / NaN) — ImGui path can AV on huge verts
	if (!std::isfinite(cx) || !std::isfinite(cy))
		return;
	if (cx < -4000.f || cx > 8000.f || cy < -4000.f || cy > 8000.f)
		return;
	const float t = static_cast<float>(ImGui::GetTime());
	const float pulse = 0.5f + 0.5f * std::sin(t * (active ? 4.8f : 2.4f));
	const float iconBase = std::clamp(Config::nade_warn_icon_size, 16.f, 56.f);
	const float r = iconBase * 0.62f;
	const int cr = std::clamp((int)(col4.x * 255.f), 0, 255);
	const int cg = std::clamp((int)(col4.y * 255.f), 0, 255);
	const int cb = std::clamp((int)(col4.z * 255.f), 0, 255);
	const ImU32 accent = IM_COL32(cr, cg, cb, 255);

	// Full durations — match NadePred kDur* (smoke 18, molly 7, decoy 15, HE/flash ~1.625)
	float full = 18.f;
	if (typeHint == NadePred::NadeType::Molly) full = 7.f;
	else if (typeHint == NadePred::NadeType::Decoy) full = 15.f;
	else if (typeHint == NadePred::NadeType::Smoke) full = 18.f;
	else if (typeHint == NadePred::NadeType::HE || typeHint == NadePred::NadeType::Flash) full = 1.625f;
	else if (weaponKey) {
		if (strstr(weaponKey, "molotov") || strstr(weaponKey, "incendiary")) full = 7.f;
		else if (strstr(weaponKey, "decoy")) full = 15.f;
		else if (strstr(weaponKey, "smoke")) full = 18.f;
		else if (strstr(weaponKey, "hegrenade") || strstr(weaponKey, "flash")) full = 1.625f;
	}
	// Reject garbage timers (bad entity / epoch bleed)
	const bool hasTimer = timeLeft >= 0.05f && timeLeft < 60.f && std::isfinite(timeLeft);
	const float frac = hasTimer ? std::clamp(timeLeft / full, 0.f, 1.f) : 0.f;

	// Name chip (slim, above)
	if (label && label[0]) {
		const ImVec2 lsz = ImGui::CalcTextSize(label);
		const float padX = 6.f, padY = 2.f;
		const float lx = floorf(cx - lsz.x * 0.5f);
		const float ly = floorf(cy - r - lsz.y - 10.f);
		const ImVec2 mn(lx - padX, ly - padY);
		const ImVec2 mx(lx + lsz.x + padX, ly + lsz.y + padY);
		dl->AddRectFilled(mn, mx, IM_COL32(8, 10, 14, 220), 5.f);
		dl->AddRect(mn, mx, IM_COL32(cr, cg, cb, 90), 5.f, 0, 1.0f);
		// Accent bar left
		dl->AddRectFilled(ImVec2(mn.x, mn.y + 2.f), ImVec2(mn.x + 2.5f, mx.y - 2.f), accent, 2.f);
		dl->AddText(ImVec2(lx + 1.f, ly + 1.f), IM_COL32(0, 0, 0, 160), label);
		dl->AddText(ImVec2(lx, ly), IM_COL32(240, 242, 246, 255), label);
	}

	// Soft outer glow
	const int glowA = static_cast<int>((active ? 42.f : 22.f) + pulse * 14.f);
	dl->AddCircleFilled(ImVec2(cx, cy), r + 7.f, IM_COL32(cr, cg, cb, glowA), 36);
	// Disc body
	dl->AddCircleFilled(ImVec2(cx, cy), r + 2.0f, IM_COL32(0, 0, 0, 200), 36);
	dl->AddCircleFilled(ImVec2(cx, cy), r,
		IM_COL32((int)(col4.x * 18), (int)(col4.y * 18), (int)(col4.z * 18), 235), 36);

	// Track ring (dim) + remaining arc
	const float ringR = r + 1.8f;
	dl->AddCircle(ImVec2(cx, cy), ringR, IM_COL32(255, 255, 255, 22), 40, 2.4f);
	dl->AddCircle(ImVec2(cx, cy), ringR, IM_COL32(0, 0, 0, 180), 40, 1.4f);
	if (hasTimer && frac > 0.01f) {
		const float a0 = -3.14159265f * 0.5f;
		const float a1 = a0 + frac * 3.14159265f * 2.f;
		// Dark understroke for contrast
		dl->PathArcTo(ImVec2(cx, cy), ringR, a0, a1, 36);
		dl->PathStroke(IM_COL32(0, 0, 0, 200), 0, 3.4f);
		dl->PathArcTo(ImVec2(cx, cy), ringR, a0, a1, 36);
		dl->PathStroke(accent, 0, active ? 2.35f : 1.9f);
	}

	// Weapon icon
	const char* glyph = nullptr;
	if (weaponKey && weaponKey[0]) {
		auto it = weapon_icons::icon_table.find(weaponKey);
		if (it != weapon_icons::icon_table.end() && !it->second.empty())
			glyph = it->second.c_str();
	}
	if (glyph && g_WeaponIconFont) {
		const float iconSz = iconBase * 0.86f;
		const ImVec2 isz = g_WeaponIconFont->CalcTextSizeA(iconSz, FLT_MAX, 0.f, glyph);
		const float ix = floorf(cx - isz.x * 0.5f);
		const float iy = floorf(cy - isz.y * 0.55f);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(ix + 1.f, iy + 1.f), IM_COL32(0, 0, 0, 200), glyph);
		dl->AddText(g_WeaponIconFont, iconSz, ImVec2(ix, iy), IM_COL32(250, 250, 252, 255), glyph);
	}

	// Timer chip — overlaps bottom of disc (less vertical stack)
	if (hasTimer) {
		char line[16];
		std::snprintf(line, sizeof(line), "%.1fs", timeLeft);
		const ImVec2 tsz = ImGui::CalcTextSize(line);
		const float padX = 5.f, padY = 1.5f;
		const float tx = floorf(cx - tsz.x * 0.5f);
		const float ty = floorf(cy + r - 2.f);
		const ImVec2 mn(tx - padX, ty - padY);
		const ImVec2 mx(tx + tsz.x + padX, ty + tsz.y + padY);
		dl->AddRectFilled(mn, mx, IM_COL32(8, 10, 14, 230), 4.f);
		dl->AddRect(mn, mx, IM_COL32(cr, cg, cb, static_cast<int>(70 + pulse * 40)), 4.f, 0, 1.0f);
		// Urgent tint when fuse low
		const ImU32 tcol = (timeLeft < 1.0f && typeHint != NadePred::NadeType::Smoke
			&& typeHint != NadePred::NadeType::Decoy)
			? IM_COL32(255, 90, 90, 255)
			: IM_COL32(245, 246, 250, 255);
		dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 160), line);
		dl->AddText(ImVec2(tx, ty), tcol, line);
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

	Vector_t warnEye{};
	QAngle_t warnAng{};
	bool warnHaveView = false;
	if (Config::nade_warn) {
		C_CSPlayerPawn* localPawn = H::SafeLocalPlayer();
		if (localPawn) {
			warnEye = Bones::GetEyePos(localPawn);
			if (!Bones::IsValidPos(warnEye) && cached_local.active)
				warnEye = cached_local.position;
		}
		if (Input::GetViewAngles && Input::viewAngleContext) {
			const uintptr_t viewPtr = Input::GetViewAngles(Input::viewAngleContext, 0);
			if (viewPtr) {
				__try {
					const Vector_t va = *reinterpret_cast<const Vector_t*>(viewPtr);
					warnAng = { va.x, va.y, 0.f };
					warnHaveView = Bones::IsValidPos(warnEye);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					warnHaveView = false;
				}
			}
		}
	}

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
		// Nade warn: always ProjectOrEdge (on-screen + OOF edge). Was warn-only —
		// with world ESP on, behind-camera nades vanished (strict W2S fail).
		const bool useWarnEdge = Config::nade_warn && isNadeKind && warnHaveView;
		if (useWarnEdge) {
			bool onScr = false;
			float ox = 0.f, oy = 0.f;
			if (!W2S::ProjectOrEdge(item.position, ox, oy, onScr, 36.f, warnEye, warnAng))
				continue;
			screen.x = ox;
			screen.y = oy;
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
		// Warn-only badges use the dedicated Warn Accent (menu color was unused before)
		if (Config::nade_warn && isNadeKind && !worldWants)
			col4 = Config::nade_warn_color;
		const ImU32 col = ImGui::ColorConvertFloat4ToU32(col4);

		// Nade projectiles / effects — single badge (name top / timer bottom)
		if (item.use_badge || isNadeKind) {
			// Instant hide when timer ran out (explode / effect end)
			if (item.timer >= 0.f && item.timer <= 0.05f)
				continue;
			if (!std::isfinite(item.position.x) || !std::isfinite(item.position.y)
				|| !std::isfinite(item.position.z))
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
				// Fade near end of lifetime (yougey flRemainingTime / fadeOut)
				float alphaScale = 0.85f + pulse * 0.15f;
				if (item.timer >= 0.f && item.timer < 0.3f)
					alphaScale *= item.timer / 0.3f;

				if (item.kind == WORLD_SMOKE) {
					const int cr = (int)(col4.x * 255), cg = (int)(col4.y * 255), cb = (int)(col4.z * 255);
					// Outer soft halo + main torus edge + centerline
					const ImU32 ringSoft = IM_COL32(cr, cg, cb,
						static_cast<int>((26 + pulse * 16) * alphaScale));
					const ImU32 ring = IM_COL32(cr, cg, cb,
						static_cast<int>((88 + pulse * 32) * alphaScale));
					const ImU32 ringIn = IM_COL32(cr, cg, cb,
						static_cast<int>((48 + pulse * 26) * alphaScale));
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 154.f, ringSoft, 24);
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 149.f, ring, 28);
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, 61.f, ringIn, 20);
				} else if (item.kind == WORLD_MOLOTOV && item.fire_count > 0) {
					DrawInfernoRadius(drawList, viewMatrix, item, col4, alphaScale);
				} else {
					const ImU32 ring = IM_COL32(
						(int)(col4.x * 255), (int)(col4.y * 255), (int)(col4.z * 255),
						static_cast<int>((70 + pulse * 50) * alphaScale));
					DrawWorldRadiusRing(drawList, viewMatrix, item.position, item.radius, ring, 24);
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
				const float iconBase = std::clamp(Config::nade_warn_icon_size, 16.f, 56.f);
				DrawCenteredText(drawList, screen.x, screen.y + iconBase * 0.55f + 14.f,
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

void Esp::InvalidateCaches() {
	cached_players.clear();
	cached_world.clear();
	cached_local.reset();
	g_plantedBomb = {};
	ResetWorldFxTimers();
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
		// Ghost bridge only for warn — never invent world-ESP-only badges from a guess
		const bool isGhost = p.ent_index <= 0;
		if (isGhost && !Config::nade_warn)
			continue;
		if (isGhost && !p.show_warn)
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
			if (rad < 1.f) rad = 150.f; // inferno_max_range
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

		// Dedup against entity-scan badges. Ghost uses a wider radius so the
		// bridge never stacks a second SMOKE/HE chip on top of the real ent.
		const float dedupR = isGhost ? 320.f : 28.f;
		const float dedupR2 = dedupR * dedupR;
		bool dup = false;
		for (auto& e : cached_world) {
			if (e.kind != kind)
				continue;
			const float dx = e.position.x - p.origin.x;
			const float dy = e.position.y - p.origin.y;
			const float dz = e.position.z - p.origin.z;
			if (dx * dx + dy * dy + dz * dz < dedupR2) {
				if (isGhost) {
					// Entity owns the badge — only refresh timer if ghost is tighter
					if (p.time_left >= 0.05f && (e.timer < 0.05f || p.time_left < e.timer))
						e.timer = p.time_left;
				} else {
					if (p.time_left >= 0.05f && (e.timer < 0.05f || p.time_left < e.timer))
						e.timer = p.time_left;
					e.position = p.origin;
					e.effect_active = e.effect_active || p.effect_active;
					if (p.radius > e.radius)
						e.radius = p.radius;
					// Prefer live fire samples for hull (entity scan may lag)
					if (p.fire_count > 0 && p.fire_count >= e.fire_count) {
						e.fire_count = p.fire_count > 64 ? 64 : p.fire_count;
						for (int fi = 0; fi < e.fire_count; ++fi)
							e.fire_pos[fi] = p.fire_pos[fi];
						if (p.fire_half_width > 1.f) {
							float hw = p.fire_half_width;
							if (hw > 95.f && hw < 130.f) hw /= 1.35f;
							e.fire_half_width = std::clamp(hw, 40.f, 90.f);
						}
					}
				}
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		// Ghost: also skip if another path already has a real entity of same type
		if (isGhost) {
			bool realEnt = false;
			for (const auto& o : paths) {
				if (o.local_preview || o.ent_index <= 0)
					continue;
				if (o.type == p.type) {
					realEnt = true;
					break;
				}
			}
			if (realEnt)
				continue;
		}

		WorldCache w{};
		w.kind = kind;
		w.position = p.origin;
		w.radius = rad;
		w.use_badge = true;
		w.effect_active = p.effect_active && !isGhost; // ghost never draws live rings
		w.timer = p.time_left;
		// Inferno hull samples from NadePred (accurate spread outline)
		if (kind == WORLD_MOLOTOV && p.fire_count > 0) {
			w.fire_count = p.fire_count > 64 ? 64 : p.fire_count;
			for (int fi = 0; fi < w.fire_count; ++fi)
				w.fire_pos[fi] = p.fire_pos[fi];
			// Prefer raw halfW; strip legacy 1.35 pad if NadePred still multiplies
			float hw = p.fire_half_width > 1.f ? p.fire_half_width : 60.f;
			if (hw > 95.f && hw < 130.f)
				hw /= 1.35f; // undo old visual pad
			w.fire_half_width = std::clamp(hw, 40.f, 90.f);
		}
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

	// Collect on Present (FRAME_RENDER_END can SEH-abort before cache Update).
	// World ESP nades also need NadePred paths for Mirror fill-in when class dump misses.
	// Include glow_world_grenades so nade glow still gets Mirror fill when text ESP off.
	const bool wantWorldNades = Config::world_esp_smoke || Config::world_esp_molotov
		|| Config::world_esp_he || Config::world_esp_flash || Config::world_esp_decoy
		|| Config::glow_world_grenades;
	if (Config::nade_pred || Config::nade_warn || wantWorldNades)
		NadePred::Update();
	if (Config::nade_lineup)
		NadeLineup::Update();

	// Dual path: entity cache + NadePred paths → world badges every throw/round
	if (Config::nade_pred || Config::nade_warn || wantWorldNades)
		MirrorNadePathsToWorld();

	// SEH each block — Present crash from one feature must not kill overlay forever
	if (H::SafeLocalPlayer()) {
		__try { drawPlayers(); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	__try { drawWorld(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { NadePred::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { NadeLineup::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Hitmarker::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Tracers::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { Backtrack::Draw(viewMatrix); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try { HitLog::Draw(); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
