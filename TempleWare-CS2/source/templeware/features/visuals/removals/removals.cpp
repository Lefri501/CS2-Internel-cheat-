#include "../../../hooks/hooks.h"
#include "../../../config/config.h"
#include "../../../../debug/debug.h"

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

// Misnamed hook: pattern hits SetParticleControl TRANSFORM (sub_1809C50F0), not CreateEffect.
// Old signature used `int mgr` which truncated the 64-bit this-pointer → crash on muzzle/impact FX.
std::int64_t __fastcall H::hkCreateParticleEffect(void* mgr, int effectIndex, unsigned int cp, void* transform, float time) {
	// Hyp A: shoot crash was truncated mgr — log first few calls in debug
#ifdef _DEBUG
	static int s_n = 0;
	if (s_n < 8) {
		ADLogf("A", "removals.cpp:hkCreateParticleEffect", "setcp_transform",
			"{\"n\":%d,\"mgr\":%llu,\"idx\":%d,\"cp\":%u,\"xform\":%llu}",
			s_n, (unsigned long long)(uintptr_t)mgr, effectIndex, cp,
			(unsigned long long)(uintptr_t)transform);
		++s_n;
	}
#endif

	// remove_particles: only block if config on — pass-through otherwise (weather + gun FX need this)
	if (Config::remove_particles)
		return 0;

	auto original = CreateParticleEffect.GetOriginal();
	if (!original)
		return 0;

	__try {
		return original(mgr, effectIndex, cp, transform, time);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
#ifdef _DEBUG
		ADLogf("A", "removals.cpp:hkCreateParticleEffect", "seh",
			"{\"code\":%u,\"mgr\":%llu,\"idx\":%d}",
			GetExceptionCode(), (unsigned long long)(uintptr_t)mgr, effectIndex);
#endif
		return 0;
	}
}
