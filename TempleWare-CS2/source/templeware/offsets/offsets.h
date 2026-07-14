#pragma once
#include <cstddef>

// Fallbacks from client_dll.hpp dump (match IDA session). Schema preferred at runtime.
namespace Offset {
	constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x23A3238;

	namespace C_BaseEntity {
		constexpr std::ptrdiff_t m_iHealth = 0x34C;
		constexpr std::ptrdiff_t m_iTeamNum = 0x3E7; // client_dll.hpp
		constexpr std::ptrdiff_t m_pGameSceneNode = 0x330;
	}

	namespace C_BaseModelEntity {
		constexpr std::ptrdiff_t m_vecViewOffset = 0xE78;
		constexpr std::ptrdiff_t m_clrRender = 0xC98;
		constexpr std::ptrdiff_t m_nRenderMode = 0xC78;
	}

	namespace C_BasePlayerPawn {
		constexpr std::ptrdiff_t m_vOldOrigin = 0x13B8;
	}

	namespace CSkeletonInstance {
		constexpr std::ptrdiff_t m_modelState = 0x140; // embedded CModelState
	}

	// CS2 moved aim punch off pawn into services
	namespace C_CSPlayerPawn {
		constexpr std::ptrdiff_t m_pAimPunchServices = 0x14B8; // CCSPlayer_AimPunchServices*
		constexpr std::ptrdiff_t m_iShotsFired = 0x1C84;
	}

	namespace CCSPlayer_AimPunchServices {
		constexpr std::ptrdiff_t m_predictableBaseAngle = 0x50;    // QAngle
		constexpr std::ptrdiff_t m_predictableBaseAngleVel = 0x5C; // QAngle
		constexpr std::ptrdiff_t m_unpredictableBaseAngle = 0xA4;  // QAngle
	}

}
