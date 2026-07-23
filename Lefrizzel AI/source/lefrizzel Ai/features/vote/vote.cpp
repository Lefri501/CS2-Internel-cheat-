#include "vote.h"

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/crypto/xorstr.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../notify/notify.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Vote {
namespace {

using FnEvtGetName = const char*(__fastcall*)(void*);
using FnEvtGetController = void*(__fastcall*)(void*, void*);
using FnEvtGetInt64 = std::int64_t(__fastcall*)(void*, const char*, unsigned int);
// engine2 ExecuteStringCommand(this, cmd)
using FnExecStr = void(__fastcall*)(void* thisptr, const char* cmd);

FnEvtGetName g_getName = nullptr;
FnEvtGetController g_getController = nullptr;
FnEvtGetInt64 g_getInt64 = nullptr;
FnExecStr g_execStr = nullptr;
bool g_ready = false;

std::atomic<ULONGLONG> g_autoAt{ 0 };
std::atomic<int> g_autoChoice{ 0 }; // 0 yes, 1 no
std::atomic<bool> g_autoPending{ false };
std::atomic<ULONGLONG> g_lastRevealMs{ 0 };
std::atomic<int> g_lastVoterKey{ -1 };

struct EvtToken {
	std::uint32_t hash = 0;
	std::uint32_t pad = 0xFFFFFFFFu;
	const char* name = nullptr;
};

std::uint32_t Murmur2Token(const char* name) {
	if (!name || !*name) return 0;
	char buf[128]{};
	int len = 0;
	for (; name[len] && len < 127; ++len) {
		const unsigned char c = static_cast<unsigned char>(name[len]);
		buf[len] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
	}
	const std::uint32_t m = 0x5bd1e995u;
	const int r = 24;
	std::uint32_t h = 0x31415926u ^ static_cast<std::uint32_t>(len);
	const auto* d = reinterpret_cast<const unsigned char*>(buf);
	int n = len;
	while (n >= 4) {
		std::uint32_t k = 0;
		std::memcpy(&k, d, 4);
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

EvtToken MakeTok(const char* s) {
	EvtToken t{};
	t.name = s;
	t.hash = Murmur2Token(s);
	return t;
}

const char* OptionLabel(int opt) {
	// CS2: 0 = yes, 1 = no (vote option1 / option2)
	if (opt == 0) return "YES";
	if (opt == 1) return "NO";
	return "?";
}

void ReadControllerName(void* ctrl, char* out, size_t outSz) {
	if (!out || outSz < 2)
		return;
	out[0] = 0;
	if (!ctrl || !Mem::ValidEntity(ctrl)) {
		std::snprintf(out, outSz, "unknown");
		return;
	}
	auto* c = reinterpret_cast<CCSPlayerController*>(ctrl);
	if (!c->ReadSanitizedName(out, outSz) || !out[0])
		std::snprintf(out, outSz, "player");
}

void ScheduleAutoVote() {
	if (!Config::vote_auto)
		return;
	const ULONGLONG delay = static_cast<ULONGLONG>(
		std::clamp(Config::vote_auto_delay_ms, 0.f, 5000.f));
	g_autoChoice.store(Config::vote_auto_choice == 1 ? 1 : 0, std::memory_order_relaxed);
	g_autoAt.store(GetTickCount64() + delay, std::memory_order_relaxed);
	g_autoPending.store(true, std::memory_order_relaxed);
}

void SehExec(void* eng, const char* cmd) {
	if (!eng || !cmd || !g_execStr)
		return;
	__try {
		g_execStr(eng, cmd);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void FireAutoVote() {
	if (!g_execStr || !I::EngineClient)
		return;
	const int choice = g_autoChoice.load(std::memory_order_relaxed);
	// option1 = yes, option2 = no
	const char* cmd = (choice == 1) ? "vote option2" : "vote option1";
	SehExec(I::EngineClient, cmd);
	Notify::Push("Auto Vote", choice == 1 ? "Voted NO" : "Voted YES", Notify::Type::Success, 2.5f);
}

void HandleVoteCast(void* gameEvent) {
	if (!g_ready || !g_getController || !g_getInt64)
		return;

	void* voter = nullptr;
	EvtToken usrTok = MakeTok("userid");
	__try { voter = g_getController(gameEvent, &usrTok); }
	__except (EXCEPTION_EXECUTE_HANDLER) { voter = nullptr; }

	int opt = -1;
	__try { opt = static_cast<int>(g_getInt64(gameEvent, "voteoption", -1)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { opt = -1; }
	if (opt < 0) {
		__try { opt = static_cast<int>(g_getInt64(gameEvent, "vote_option", -1)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { opt = -1; }
	}

	// Local cast → cancel pending auto (already voted)
	if (voter && I::GameEntity && I::GameEntity->Instance) {
		C_CSPlayerPawn* lp = H::SafeLocalPlayer();
		if (lp) {
			void* localCtrl = nullptr;
			__try {
				CBaseHandle h = lp->m_hController();
				if (h.valid())
					localCtrl = I::GameEntity->Instance->Get(h);
			} __except (EXCEPTION_EXECUTE_HANDLER) { localCtrl = nullptr; }
			if (localCtrl && voter == localCtrl)
				g_autoPending.store(false, std::memory_order_relaxed);
		}
	}

	if (!Config::vote_reveal)
		return;

	char name[64]{};
	ReadControllerName(voter, name, sizeof(name));

	// Dedup spam: same voter within 80ms
	const int key = static_cast<int>(reinterpret_cast<std::uintptr_t>(voter) ^ static_cast<unsigned>(opt));
	const ULONGLONG now = GetTickCount64();
	if (g_lastVoterKey.load(std::memory_order_relaxed) == key
		&& now - g_lastRevealMs.load(std::memory_order_relaxed) < 80ull)
		return;
	g_lastVoterKey.store(key, std::memory_order_relaxed);
	g_lastRevealMs.store(now, std::memory_order_relaxed);

	char body[96]{};
	std::snprintf(body, sizeof(body), "%s → %s", name, OptionLabel(opt));
	Notify::Push("Vote", body, Notify::Type::Info, 3.5f);
	Notify::ChatPrintf("\x04[Vote]\x01 %s voted \x0B%s", name, OptionLabel(opt));
	// Developer console (~) — always with vote reveal
	Notify::ConsolePrintf("[Vote] %s voted %s", name, OptionLabel(opt));
}

void HandleVoteChanged(void* gameEvent) {
	// New vote activity → arm auto-vote once
	if (Config::vote_auto && !g_autoPending.load(std::memory_order_relaxed))
		ScheduleAutoVote();

	if (!Config::vote_reveal || !g_getInt64)
		return;

	int yes = -1, no = -1;
	__try {
		yes = static_cast<int>(g_getInt64(gameEvent, "vote_option1", -1));
		no = static_cast<int>(g_getInt64(gameEvent, "vote_option2", -1));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		yes = no = -1;
	}
	// Some builds use yes_count / no_count
	if (yes < 0 || no < 0) {
		__try {
			yes = static_cast<int>(g_getInt64(gameEvent, "yes_count", -1));
			no = static_cast<int>(g_getInt64(gameEvent, "no_count", -1));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	if (yes < 0 && no < 0)
		return;

	// Only toast when counts actually present
	static int s_lastYes = -1, s_lastNo = -1;
	if (yes == s_lastYes && no == s_lastNo)
		return;
	s_lastYes = yes;
	s_lastNo = no;

	char body[64]{};
	std::snprintf(body, sizeof(body), "Yes %d  ·  No %d",
		yes < 0 ? 0 : yes, no < 0 ? 0 : no);
	Notify::Push("Vote Tally", body, Notify::Type::Info, 2.2f);
	Notify::ConsolePrintf("[Vote] tally Yes %d  No %d",
		yes < 0 ? 0 : yes, no < 0 ? 0 : no);
}

} // namespace

void Install() {
	g_ready = false;
	g_getName = nullptr;
	g_getController = nullptr;
	g_getInt64 = nullptr;
	g_execStr = nullptr;
	g_autoPending.store(false, std::memory_order_relaxed);

	if (const uintptr_t pName = M::patternScan(XS("client"),
		XS("8B 41 14 0F BA E0 1E 73 05 48 8D 41 18 C3")))
		g_getName = reinterpret_cast<FnEvtGetName>(pName);
	if (const uintptr_t pCtrl = M::patternScan(XS("client"),
		XS("48 83 EC 38 8B 02 4C 8D 44 24 20")))
		g_getController = reinterpret_cast<FnEvtGetController>(pCtrl);
	if (const uintptr_t pInt = M::patternScan(XS("client"),
		XS("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 01 41 8B F0")))
		g_getInt64 = reinterpret_cast<FnEvtGetInt64>(pInt);

	// engine2 ExecuteStringCommand — (this, cmd)
	if (const uintptr_t pExec = M::patternScan(XS("engine2"),
		XS("40 53 56 48 81 EC 48 07 00 00 48 8B F1 48 8B DA"))) {
		g_execStr = reinterpret_cast<FnExecStr>(pExec);
		Con::Ok("Vote ExecuteStringCommand @ 0x%p", (void*)pExec);
	} else {
		Con::OffsetMiss("Vote ExecuteStringCommand");
	}

	g_ready = g_getName && g_getController && g_getInt64;
	if (g_ready)
		Con::Ok("Vote event helpers ready");
	else
		Con::Error("Vote event helpers incomplete");
}

void OnGameEvent(void* gameEvent) {
	if ((!Config::vote_reveal && !Config::vote_auto) || !gameEvent || !g_ready || !g_getName)
		return;

	const char* name = nullptr;
	__try { name = g_getName(gameEvent); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (!name || !name[0])
		return;

	if (std::strcmp(name, "vote_cast") == 0) {
		HandleVoteCast(gameEvent);
		// First cast of a round also arms auto if not pending
		if (Config::vote_auto && !g_autoPending.load(std::memory_order_relaxed))
			ScheduleAutoVote();
		return;
	}
	if (std::strcmp(name, "vote_changed") == 0) {
		HandleVoteChanged(gameEvent);
		return;
	}
	// Round / map wipe
	if (std::strcmp(name, "round_start") == 0 || std::strcmp(name, "game_newmap") == 0) {
		g_autoPending.store(false, std::memory_order_relaxed);
	}
}

void OnFrame() {
	if (!g_autoPending.load(std::memory_order_relaxed))
		return;
	if (!Config::vote_auto) {
		g_autoPending.store(false, std::memory_order_relaxed);
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if (now < g_autoAt.load(std::memory_order_relaxed))
		return;
	g_autoPending.store(false, std::memory_order_relaxed);
	FireAutoVote();
}

} // namespace Vote
