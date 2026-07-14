#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <Windows.h>

// Lightweight pointer guards for hot paths (ESP/aim/schema).
// NO VirtualQuery on the hot path — that destroyed FPS (~10).
// Use IsReadableHeavy() only for rare/untrusted pointers.
namespace Mem {

constexpr std::uintptr_t kMinUser = 0x10000ull;
constexpr std::uintptr_t kMaxUser = 0x00007FFFFFFFFFFFull;
// Full CBaseHandle entry range (0x7FFF mask). HUD/viewmodel/hands live well above 2048.
	// Iteration caps (ESP loops) should still clamp GetHighestEntityIndex if needed.
	constexpr int kMaxEntityIndex = 0x7FFE;
constexpr int kMaxPlayers = 64;
constexpr int kMinHealth = 0;
constexpr int kMaxHealth = 200;
constexpr int kMaxArmor = 100;
constexpr int kMaxTeam = 3;
constexpr int kMaxBoneIndex = 128;

[[nodiscard]] inline bool IsUserPtr(const void* p) noexcept {
	const auto a = reinterpret_cast<std::uintptr_t>(p);
	return a >= kMinUser && a <= kMaxUser;
}

// Hot path: range only (no syscall)
[[nodiscard]] inline bool IsReadable(const void* p, std::size_t size) noexcept {
	if (!p || size == 0)
		return false;
	const auto start = reinterpret_cast<std::uintptr_t>(p);
	if (start < kMinUser || start > kMaxUser)
		return false;
	if (size > kMaxUser || start + size < start || start + size - 1 > kMaxUser)
		return false;
	return true;
}

// Rare path only — VirtualQuery (expensive)
[[nodiscard]] bool IsReadableHeavy(const void* p, std::size_t size) noexcept;

[[nodiscard]] inline bool Valid(const void* p, std::size_t minBytes = sizeof(void*)) noexcept {
	return IsReadable(p, minBytes);
}

template <typename T>
[[nodiscard]] inline bool Read(const void* p, T& out) noexcept {
	if (!IsReadable(p, sizeof(T)))
		return false;
	std::memcpy(&out, p, sizeof(T));
	return true;
}

template <typename T>
[[nodiscard]] inline T ReadOr(const void* p, T fallback = T{}) noexcept {
	T v{};
	if (!Read(p, v))
		return fallback;
	return v;
}

[[nodiscard]] inline void* ReadPtr(const void* p) noexcept {
	return ReadOr<void*>(p, nullptr);
}

template <typename T>
[[nodiscard]] inline bool ReadField(const void* base, std::size_t offset, T& out) noexcept {
	if (!IsUserPtr(base))
		return false;
	return Read(reinterpret_cast<const std::uint8_t*>(base) + offset, out);
}

template <typename T>
[[nodiscard]] inline T ReadFieldOr(const void* base, std::size_t offset, T fallback = T{}) noexcept {
	T v{};
	if (!ReadField(base, offset, v))
		return fallback;
	return v;
}

[[nodiscard]] inline int ClampHealth(int h) noexcept {
	if (h < kMinHealth) return kMinHealth;
	if (h > kMaxHealth) return kMaxHealth;
	return h;
}
[[nodiscard]] inline int ClampArmor(int a) noexcept {
	if (a < 0) return 0;
	if (a > kMaxArmor) return kMaxArmor;
	return a;
}
[[nodiscard]] inline int ClampEntityIndex(int i) noexcept {
	if (i < 0) return 0;
	if (i > kMaxEntityIndex) return kMaxEntityIndex;
	return i;
}
[[nodiscard]] inline bool ValidEntityIndex(int i) noexcept {
	return i >= 0 && i <= kMaxEntityIndex;
}
[[nodiscard]] inline bool ValidBoneIndex(int i) noexcept {
	return i >= 0 && i <= kMaxBoneIndex;
}
[[nodiscard]] inline bool ValidTeam(int t) noexcept {
	return t >= 0 && t <= kMaxTeam;
}
[[nodiscard]] inline bool ValidHealth(int h) noexcept {
	return h >= kMinHealth && h <= kMaxHealth;
}

// Entity: user-range + first qword (vtable) looks like a pointer
[[nodiscard]] inline bool ValidEntity(const void* ent) noexcept {
	if (!IsUserPtr(ent))
		return false;
	void* vt = nullptr;
	std::memcpy(&vt, ent, sizeof(vt));
	return IsUserPtr(vt);
}

[[nodiscard]] inline bool Finite(float f) noexcept {
	return f == f && f > -1.0e12f && f < 1.0e12f;
}

} // namespace Mem
