#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../debug/debug.h"
#include "../../../utils/memory/memsafe/memsafe.h"
#include "../../../utils/memory/patternscan/patternscan.h"
#include "../../../utils/memory/gaa/gaa.h"
#include "../../w2s/w2s.h"
#include "../../../../cs2/datatypes/viewmatrix/viewmatrix.h"
#include "../../../interfaces/interfaces.h"
#include "../../../utils/schema/schema.h"
#include "../../../utils/fnv1a/fnv1a.h"
#include "../../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../utils/console/console.h"

#include <Windows.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>

// DRAWLEGS — Firstperson Legs render passes
void __fastcall H::hkDrawLegs(void* a1, void* a2, void* a3, void* a4, void* a5) {
	if (Config::remove_legs)
		return;
	auto original = DrawLegs.GetOriginal();
	if (original) original(a1, a2, a3, a4, a5);
}

// DRAWSMOKEVERTEX — smoke volume draw dispatcher
std::int64_t __fastcall H::hkDrawSmokeVertex(void* a1, void* a2, int a3, int a4, void* a5, void* a6) {
	if (Config::remove_smoke)
		return 0;
	auto original = DrawSmokeVertex.GetOriginal();
	if (original) return original(a1, a2, a3, a4, a5, a6);
	return 0;
}

// RENDERDECALS — bullet/blood/explosion decal passes
void* __fastcall H::hkRenderDecals(void* a1, void* a2, char a3, char a4) {
	if (Config::remove_decals)
		return nullptr;
	auto original = RenderDecals.GetOriginal();
	if (original) return original(a1, a2, a3, a4);
	return nullptr;
}

// Keep weather / map precip when blanket particle remove is on.
static bool ParticleNameIsWeather(const char* name) {
	if (!name || !Mem::IsUserPtr(const_cast<char*>(name)))
		return false;
	__try {
		if (!name[0])
			return false;
		char buf[160]{};
		for (int i = 0; i < 159; ++i) {
			const char c = name[i];
			if (!c) { buf[i] = 0; break; }
			buf[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + 32) : c);
		}
		buf[159] = 0;
		return std::strstr(buf, "rain") || std::strstr(buf, "snow")
			|| std::strstr(buf, "ash") || std::strstr(buf, "weather")
			|| std::strstr(buf, "precip") || std::strstr(buf, "fog")
			|| std::strstr(buf, "dust") || std::strstr(buf, "env_fx");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// IDA CacheParticleEffect @ 0x18078EE10 — real effect spawn.
void* __fastcall H::hkCacheParticleEffect(void* mgr, unsigned int* outIndex, const char* name,
	int attach, void* entity, void* a6, void* a7, int a8)
{
	if (Config::remove_particles && !ParticleNameIsWeather(name)) {
		if (outIndex && Mem::IsUserPtr(outIndex)) {
			__try { *outIndex = 0xFFFFFFFFu; }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		return outIndex;
	}
	auto original = CacheParticleEffect.GetOriginal();
	if (!original)
		return outIndex;
	return original(mgr, outIndex, name, attach, entity, a6, a7, a8);
}

// ── Smoke color ─────────────────────────────────────────────────────────────
// Entity m_vSmokeColor is NOT re-read by SmokeRenderer each frame.
// DrawSmokeArray (IDA 0x180CB4190) pulls RGB from smoke VOLUME objects:
//   volume+0xD0 = primary RGB (v24[0..2])
//   volume+0x100 = secondary RGB (v24[12..14])
// Active volumes live in a linked list:
//   head index: word at dword_1823FDFC0
//   entries:    base[index*16] = volume*, next word @ +10
// Resolved from gather pattern (sub_180CB7EF0).

namespace {
	// IDA SmokeConstantBuffer source floats
	constexpr uintptr_t kVolColor0 = 0xD0;  // primary RGB
	constexpr uintptr_t kVolColor1 = 0x100; // secondary RGB
	constexpr uintptr_t kVolStride = 16;    // list entry size
	constexpr int kMaxSmokeVols = 16;       // engine cap ("Unable to render more than %d smokes")

	// dump schema fallback for entity field
	constexpr uint32_t kSmokeColorOffDump = 0x1284;
	constexpr uint32_t kSmokeDidOffDump = 0x127C;

	std::uint16_t* g_pSmokeHead = nullptr; // word head index (0xFFFF = empty)
	void* g_pSmokeListBase = nullptr;      // array base (not double ptr)
	bool g_smokeListResolved = false;
	bool g_smokeListFailed = false;

	void ResolveSmokeVolumeList() {
		if (g_smokeListResolved || g_smokeListFailed)
			return;

		// sub_180CB7EF0: movzx eax, word ptr [rip+head]; xor esi,esi
		const uintptr_t headHit = M::patternScan("client",
			"0F B7 05 ? ? ? ? 33 F6 4D 89 6B 10");
		// mov rcx, [rip+list]; movzx edx, word ptr [rip+flags]
		const uintptr_t listHit = M::patternScan("client",
			"48 8B 0D ? ? ? ? 0F B7 15 ? ? ? ? 4D 89 63 08");

		if (!headHit || !listHit) {
			g_smokeListFailed = true;
			Con::OffsetMiss("SmokeVolumeList (head/list)");
			return;
		}

		g_pSmokeHead = reinterpret_cast<std::uint16_t*>(
			M::getAbsoluteAddress(headHit, 3));
		// getAbsoluteAddress on mov rcx,[rip+disp] → address of the qword (list base storage)
		// code loads the qword into rcx and uses it as array base
		void** ppList = reinterpret_cast<void**>(
			M::getAbsoluteAddress(listHit, 3));
		if (!g_pSmokeHead || !ppList) {
			g_smokeListFailed = true;
			Con::OffsetMiss("SmokeVolumeList GAA");
			return;
		}
		// Store the address of the global qword; read *ppList each tick
		// (list base can be null until first smoke)
		g_pSmokeListBase = ppList; // actually points at the global holding the base
		g_smokeListResolved = true;
		Con::Ok("SmokeVolumeList head@%p listGlob@%p",
			(void*)g_pSmokeHead, g_pSmokeListBase);
	}

	void TintOneVolume(void* vol, float r, float g, float b) {
		if (!vol || !Mem::IsUserPtr(vol))
			return;
		__try {
			if (!Mem::IsReadable(vol, kVolColor1 + 12))
				return;
			float* c0 = reinterpret_cast<float*>(
				reinterpret_cast<char*>(vol) + kVolColor0);
			float* c1 = reinterpret_cast<float*>(
				reinterpret_cast<char*>(vol) + kVolColor1);
			// Only write if values look like colors (not garbage / huge)
			auto sane = [](float v) {
				return std::isfinite(v) && v > -4.f && v < 8.f;
			};
			if (sane(c0[0]) && sane(c0[1]) && sane(c0[2])) {
				c0[0] = r; c0[1] = g; c0[2] = b;
			}
			if (sane(c1[0]) && sane(c1[1]) && sane(c1[2])) {
				c1[0] = r; c1[1] = g; c1[2] = b;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// Walk active smoke volume list; return how many tinted.
	int TintSmokeVolumes(float r, float g, float b) {
		ResolveSmokeVolumeList();
		if (!g_smokeListResolved || !g_pSmokeHead || !g_pSmokeListBase)
			return 0;

		int n = 0;
		__try {
			const std::uint16_t head = *g_pSmokeHead;
			if (head == 0xFFFFu)
				return 0;

			// g_pSmokeListBase is address of global qword → actual array base
			void* base = *reinterpret_cast<void**>(g_pSmokeListBase);
			if (!base || !Mem::IsUserPtr(base))
				return 0;

			std::uint16_t idx = head;
			for (int guard = 0; guard < kMaxSmokeVols && idx != 0xFFFFu; ++guard) {
				// entry @ base + idx*16: [0]=volume*, [10]=next index word
				auto* entry = reinterpret_cast<std::uint8_t*>(base) + (static_cast<std::size_t>(idx) * kVolStride);
				if (!Mem::IsReadable(entry, kVolStride))
					break;
				void* vol = *reinterpret_cast<void**>(entry);
				const std::uint16_t next = *reinterpret_cast<std::uint16_t*>(entry + 10);
				if (vol)
					TintOneVolume(vol, r, g, b);
				++n;
				if (next == idx)
					break;
				idx = next;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return n;
	}

	// Backup: still poke entity field (may help killfeed / other paths)
	void TintSmokeEntities(float r, float g, float b) {
		if (!I::GameEntity || !I::GameEntity->Instance)
			return;

		uint32_t colOff = SchemaFinder::Get(
			hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_vSmokeColor"));
		if (!colOff)
			colOff = kSmokeColorOffDump;
		uint32_t didOff = SchemaFinder::Get(
			hash_32_fnv1a_const("C_SmokeGrenadeProjectile->m_bDidSmokeEffect"));
		if (!didOff)
			didOff = kSmokeDidOffDump;

		const int nMax = I::GameEntity->Instance->GetHighestEntityIndex();
		if (nMax <= 0 || nMax > 8192)
			return;

		int checked = 0;
		for (int i = 1; i <= nMax && checked < 64; ++i) {
			auto* ent = I::GameEntity->Instance->Get(i);
			if (!Mem::ValidEntity(ent))
				continue;

			SchemaClassInfoData_t* cls = nullptr;
			__try { ent->dump_class_info(&cls); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!cls || !cls->szName || !Mem::IsReadable(cls->szName, 1))
				continue;
			if (HASH(cls->szName) != HASH("C_SmokeGrenadeProjectile"))
				continue;
			++checked;

			auto* base = reinterpret_cast<std::uint8_t*>(ent);
			bool did = false;
			__try {
				if (Mem::IsReadable(base + didOff, 1))
					did = base[didOff] != 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!did)
				continue;

			__try {
				float* col = reinterpret_cast<float*>(base + colOff);
				if (!Mem::IsReadable(col, 12))
					continue;
				col[0] = r;
				col[1] = g;
				col[2] = b;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
	}

	void ApplySmokeTintNow() {
		if (!Config::smoke_color || Config::remove_smoke)
			return;
		if (Config::loading.load(std::memory_order_acquire))
			return;

		const float r = std::clamp(Config::smoke_color_value.x, 0.f, 1.f);
		const float g = std::clamp(Config::smoke_color_value.y, 0.f, 1.f);
		const float b = std::clamp(Config::smoke_color_value.z, 0.f, 1.f);

		// Primary path: volume list used by SmokeRenderer
		const int vols = TintSmokeVolumes(r, g, b);
		// Entity field as backup (schema / other consumers)
		TintSmokeEntities(r, g, b);
		(void)vols;
	}
} // namespace

// DRAWSMOKEARRAY — IDA 0x180CB4190 SmokeConstantBuffer batch
// Tint volumes BEFORE original so CB picks up new RGB at +0xD0.
std::int64_t __fastcall H::hkDrawSmokeArray(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
	if (Config::remove_smoke)
		return 0;
	if (Config::smoke_color)
		ApplySmokeTintNow();
	auto original = DrawSmokeArray.GetOriginal();
	if (original) return original(a1, a2, a3, a4, a5, a6);
	return 0;
}

// Called from World::Update (~frame) when smoke_color on.
// DrawSmokeArray already tints right before draw — this is backup only.
// Throttle: full entity walk every frame was free FPS tax with smoke_color on.
void ApplySmokeColorTick() {
	if (!Config::smoke_color || Config::remove_smoke)
		return;
	if (Config::loading.load(std::memory_order_acquire))
		return;

	static std::uint64_t s_lastMs = 0;
	const std::uint64_t now = GetTickCount64();
	if (s_lastMs != 0 && now < s_lastMs + 80ull) // ~12.5 Hz backup
		return;
	s_lastMs = now;
	ApplySmokeTintNow();
}

// ── Particle color (fire / explosion) ───────────────────────────────────────
// UC Particle Modulation (particles.dll ParticleDrawArray):
//   pattern: 48 89 5C 24 ? 4C 89 4C 24 ? 4C 89 44 24 ? 55  @ particles.dll
//   IDA sub_1802826D0
//   ParticleContext_t: data* @ +0x48, float RGB @ +0x50/+0x54/+0x58
//   Name: a2+0x48 → +0x18 nameptr → +0x8 char** → *psz
namespace ParticleColorFx {
	struct ParticleData_t {
		char pad0[0x18];
		void* m_pNamePtr; // +0x18
	};
	struct ParticleName_t {
		char pad0[0x8];
		const char** m_pszName; // +0x8
	};
	struct ParticleContext_t {
		char pad0[0x48];
		ParticleData_t* m_pData; // +0x48
		float m_flRed;           // +0x50
		float m_flGreen;         // +0x54
		float m_flBlue;          // +0x58
	};

	// Lowercase name into buf; return false on bad ptr / empty.
	bool LowerName(const char* name, char* buf, int cap) {
		if (!name || !buf || cap < 4 || !Mem::IsReadable(name, 4))
			return false;
		__try {
			for (int i = 0; i < cap - 1; ++i) {
				const char c = name[i];
				if (!c) { buf[i] = 0; break; }
				buf[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + 32) : c);
			}
			buf[cap - 1] = 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return buf[0] != 0;
	}

	bool NameIsFire(const char* lower) {
		if (!lower) return false;
		if (std::strstr(lower, "inferno_fx")) return true;
		if (std::strstr(lower, "groundfire")) return true;
		if (std::strstr(lower, "molotov_fire")) return true;
		if (std::strstr(lower, "incendiary")) return true;
		if (std::strstr(lower, "explosion_molotov")) return true;
		return false;
	}

	// HE / C4 / generic explosion FX — exclude molotov (handled as fire).
	bool NameIsExplosion(const char* lower) {
		if (!lower) return false;
		if (std::strstr(lower, "explosion_molotov")) return false;
		if (std::strstr(lower, "explosions_fx")) return true;
		if (std::strstr(lower, "explosion_he")) return true;
		if (std::strstr(lower, "hegrenade")) return true;
		if (std::strstr(lower, "he_grenade")) return true;
		if (std::strstr(lower, "c4_explosion")) return true;
		if (std::strstr(lower, "bomb_explosion")) return true;
		// generic explosion_* but not molly
		if (std::strstr(lower, "explosion") && !std::strstr(lower, "molotov")
			&& !std::strstr(lower, "inferno") && !std::strstr(lower, "incendiary"))
			return true;
		return false;
	}

	const char* ContextName(ParticleContext_t* ctx) {
		if (!ctx || !Mem::IsReadable(ctx, sizeof(ParticleContext_t)))
			return nullptr;
		__try {
			ParticleData_t* data = ctx->m_pData;
			if (!data || !Mem::IsReadable(data, 0x20))
				return nullptr;
			auto* nameObj = reinterpret_cast<ParticleName_t*>(data->m_pNamePtr);
			if (!nameObj || !Mem::IsReadable(nameObj, 0x10))
				return nullptr;
			if (!nameObj->m_pszName || !Mem::IsReadable(nameObj->m_pszName, 8))
				return nullptr;
			const char* n = *nameObj->m_pszName;
			if (!n || !Mem::IsReadable(n, 1))
				return nullptr;
			return n;
		} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	void ApplyTint(ParticleContext_t* ctx, float r, float g, float b) {
		if (!ctx)
			return;
		r = std::clamp(r, 0.f, 1.f);
		g = std::clamp(g, 0.f, 1.f);
		b = std::clamp(b, 0.f, 1.f);
		__try {
			ctx->m_flRed = r;
			ctx->m_flGreen = g;
			ctx->m_flBlue = b;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
} // namespace ParticleColorFx

// particles.dll ParticleDrawArray — live RGB modulation (UC 2026 method)
// Class cache by ParticleData* — name lower+strstr every draw was hot with fire on.
void* __fastcall H::hkParticleDrawArray(void* a1, void* a2, void* a3, void* a4, void* a5) {
	const bool wantFire = Config::fire_color;
	const bool wantExpl = Config::explosion_color;
	if ((wantFire || wantExpl) && a2 && !Config::remove_particles) {
		auto* ctx = reinterpret_cast<ParticleColorFx::ParticleContext_t*>(a2);
		// 0=unknown/other, 1=fire, 2=explosion
		enum : std::uint8_t { kOther = 0, kFire = 1, kExpl = 2 };
		struct ClassSlot {
			void* data = nullptr;
			std::uint8_t cls = kOther;
		};
		static ClassSlot s_cls[128]{};
		std::uint8_t kind = kOther;
		void* dataKey = nullptr;
		bool haveClass = false;
		if (Mem::IsReadable(ctx, 0x50)) {
			dataKey = ctx->m_pData;
			if (dataKey) {
				const auto u = reinterpret_cast<std::uintptr_t>(dataKey);
				const int home = static_cast<int>(((u >> 4) ^ (u >> 9)) & 127u);
				for (int p = 0; p < 3; ++p) {
					const ClassSlot& s = s_cls[(home + p) & 127];
					if (s.data == dataKey) {
						kind = s.cls;
						haveClass = true;
						break;
					}
				}
				if (!haveClass) {
					const char* name = ParticleColorFx::ContextName(ctx);
					char lower[192]{};
					if (name && ParticleColorFx::LowerName(name, lower, sizeof(lower))) {
						if (ParticleColorFx::NameIsFire(lower))
							kind = kFire;
						else if (ParticleColorFx::NameIsExplosion(lower))
							kind = kExpl;
						else
							kind = kOther;
					}
					// store
					int write = home;
					for (int p = 0; p < 3; ++p) {
						const int i = (home + p) & 127;
						if (!s_cls[i].data || s_cls[i].data == dataKey) {
							write = i;
							break;
						}
					}
					s_cls[write].data = dataKey;
					s_cls[write].cls = kind;
					haveClass = true;
				}
			}
		}
		if (haveClass) {
			if (wantFire && kind == kFire) {
				ParticleColorFx::ApplyTint(ctx,
					Config::fire_color_value.x,
					Config::fire_color_value.y,
					Config::fire_color_value.z);
			} else if (wantExpl && kind == kExpl) {
				ParticleColorFx::ApplyTint(ctx,
					Config::explosion_color_value.x,
					Config::explosion_color_value.y,
					Config::explosion_color_value.z);
			}
		}
	}
	auto original = ParticleDrawArray.GetOriginal();
	if (original)
		return original(a1, a2, a3, a4, a5);
	return nullptr;
}

// Tick path retired — tint is draw-hook only. Keep symbol for World::Update.
void ApplyFireColorTick() {
}

void* __fastcall H::hkGetMatrixForView(void* a1, void* a2, void* a3, void* a4, void* worldToProjection, void* a6) {
	(void)a1; (void)a2; (void)a3; (void)a4; (void)worldToProjection; (void)a6;
	auto original = GetMatrixForView.GetOriginal();
	if (original)
		return original(a1, a2, a3, a4, worldToProjection, a6);
	return nullptr;
}

namespace {
	// cl_disable_postprocessing ConVar (IDA register @ 0x1800E8260 → unk_1823B7328)
	// Layout: ConVar* +0x08 → data; value bool @ data+0x58 (same as gamemode cvar poke).
	constexpr uintptr_t kCvarDataOffset = 0x08;
	constexpr uintptr_t kCvarValueOffset = 0x58;
	constexpr char kDisablePostName[] = "cl_disable_postprocessing";

	uintptr_t g_cvarDisablePost = 0;
	bool g_cvarDisablePostTried = false;
	bool g_lastPostWant = false;

	uintptr_t RipRel3(uintptr_t insn) {
		const int32_t disp = *reinterpret_cast<const int32_t*>(insn + 3);
		return insn + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp));
	}

	uintptr_t FindCString(const char* sz, uintptr_t start, uintptr_t end) {
		const size_t len = std::strlen(sz) + 1;
		if (end <= start || end - start < len)
			return 0;
		for (uintptr_t p = start; p + len <= end; ++p) {
			if (std::memcmp(reinterpret_cast<const void*>(p), sz, len) == 0)
				return p;
		}
		return 0;
	}

	void ResolveDisablePostCvar() {
		g_cvarDisablePostTried = true;
		g_cvarDisablePost = 0;

		HMODULE hClient = GetModuleHandleA("client.dll");
		if (!hClient)
			hClient = GetModuleHandleA("client");
		if (!hClient)
			return;

		auto* pDos = reinterpret_cast<PIMAGE_DOS_HEADER>(hClient);
		if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
			return;
		auto* pNt = reinterpret_cast<PIMAGE_NT_HEADERS64>(
			reinterpret_cast<uintptr_t>(pDos) + pDos->e_lfanew);
		if (pNt->Signature != IMAGE_NT_SIGNATURE)
			return;

		const uintptr_t imageBase = reinterpret_cast<uintptr_t>(hClient);
		const uintptr_t imageEnd = imageBase + pNt->OptionalHeader.SizeOfImage;
		const uintptr_t codeStart = imageBase + pNt->OptionalHeader.BaseOfCode;
		const uintptr_t codeEnd = codeStart + pNt->OptionalHeader.SizeOfCode;

		const uintptr_t strAddr = FindCString(kDisablePostName, imageBase, imageEnd);
		if (!strAddr)
			return;

		// Register path: lea rcx/rdx name, then lea to ConVar object nearby
		for (uintptr_t p = codeStart; p + 14 <= codeEnd; ++p) {
			if (*reinterpret_cast<const uint8_t*>(p) != 0x48) continue;
			if (*reinterpret_cast<const uint8_t*>(p + 1) != 0x8D) continue;
			// lea r??, [rip+disp] — any of rcx/rdx/r8.. via ModRM
			const uint8_t modrm = *reinterpret_cast<const uint8_t*>(p + 2);
			if ((modrm & 0xC7) != 0x05) continue; // [rip+disp32]
			if (RipRel3(p) != strAddr)
				continue;

			// Scan nearby for another lea [rip] → ConVar object (within ±0x40)
			for (int off = -0x30; off <= 0x40; off += 1) {
				if (off == 0)
					continue;
				const uintptr_t q = p + static_cast<uintptr_t>(off);
				if (q < codeStart || q + 7 > codeEnd)
					continue;
				if (*reinterpret_cast<const uint8_t*>(q) != 0x48) continue;
				if (*reinterpret_cast<const uint8_t*>(q + 1) != 0x8D) continue;
				if ((*reinterpret_cast<const uint8_t*>(q + 2) & 0xC7) != 0x05) continue;
				const uintptr_t obj = RipRel3(q);
				if (obj == strAddr)
					continue;
				if (obj < imageBase || obj >= imageEnd)
					continue;
				g_cvarDisablePost = obj;
				Con::Ok("cl_disable_postprocessing ConVar @ %p", (void*)obj);
				return;
			}
		}
		Con::OffsetMiss("cl_disable_postprocessing ConVar");
	}

	void WriteDisablePost(bool want) {
		if (!g_cvarDisablePostTried)
			ResolveDisablePostCvar();
		if (!g_cvarDisablePost)
			return;
		__try {
			const uintptr_t pData = *reinterpret_cast<uintptr_t*>(
				g_cvarDisablePost + kCvarDataOffset);
			if (pData < 0x10000)
				return;
			*reinterpret_cast<uint8_t*>(pData + kCvarValueOffset) = want ? 1 : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	bool LocalIsScoped() {
		C_CSPlayerPawn* lp = H::SafeLocalPlayer();
		if (!lp)
			return false;
		bool scoped = false;
		__try { scoped = lp->m_bIsScoped(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return scoped;
	}
} // namespace

// Call each frame from World::Update — dump "UpdatePostProcessing" was wrong
// (tournament dem file path). Real disable = cl_disable_postprocessing.
void ApplyPostProcessRemovalTick() {
	const bool want = Config::remove_postprocess;
	if (want == g_lastPostWant && g_cvarDisablePostTried)
		return;
	g_lastPostWant = want;
	WriteDisablePost(want);
}

bool __fastcall H::hkDrawCrosshair(void* a1) {
	if (Config::remove_crosshair)
		return false;
	// Force crosshair: show when unscoped; vanish when scoped (real sniper feel)
	if (Config::force_crosshair) {
		if (LocalIsScoped())
			return false;
		return true;
	}
	auto original = DrawCrosshair.GetOriginal();
	if (original) return original(a1);
	return true;
}

bool __fastcall H::hkWeaponHidesCrosshair(void* zoomData) {
	if (Config::force_crosshair) {
		// true = weapon hides crosshair. When scoped, hide; else force show.
		if (LocalIsScoped())
			return true;
		return false;
	}
	auto original = WeaponHidesCrosshair.GetOriginal();
	if (original) return original(zoomData);
	return false;
}

// IDA 0x180F5BBF0 — bool() no args; returns cl_drawhud && !cl_drawhud_force_disabled-ish
bool __fastcall H::hkShouldShowHudElements(void* a1) {
	(void)a1;
	if (Config::remove_hud)
		return false;
	auto original = ShouldShowHudElements.GetOriginal();
	if (original) return original(a1);
	return true;
}

// Legacy symbol kept so vcxproj/hooks link; dump pattern pointed at dem-file code.
// Real work is ApplyPostProcessRemovalTick (ConVar poke).
void __fastcall H::hkUpdatePostProcessing(void* a1, void* a2) {
	(void)a1;
	(void)a2;
	// No-op if ever installed — do not call dem path.
}
