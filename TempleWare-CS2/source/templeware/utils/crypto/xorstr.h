#pragma once
// ============================================================================
// Compile-time XOR string encryption
// Encrypts string literals at compile time, decrypts on stack at runtime.
//
// Usage: printf("%s", XS("my secret string"));
//
// Seed = FNV-1a of string content. Works in Debug/Release.
// Uses STATIC (not thread_local) buffer for manual-map compatibility —
// TLS is often broken under manual mappers.
// ============================================================================

#include <cstdint>
#include <cstddef>

namespace xor_detail {

    constexpr uint64_t fnv1a_64(const char* s, size_t n) {
        uint64_t h = 0xCBF29CE484222325ULL;
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<uint8_t>(s[i]);
            h *= 0x100000001B3ULL;
        }
        return h;
    }

    constexpr uint8_t key_byte(size_t idx, uint64_t seed) {
        uint64_t h = seed ^ (idx * 0x9E3779B97F4A7C15ULL);
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33;
        return static_cast<uint8_t>(h & 0xFF);
    }

    template <size_t N, uint64_t Seed>
    class encrypted_string {
    public:
        constexpr encrypted_string(const char(&str)[N]) : m_encrypted{} {
            for (size_t i = 0; i < N; ++i)
                m_encrypted[i] = static_cast<char>(str[i] ^ key_byte(i, Seed));
        }
        void decrypt(char* buf) const {
            for (size_t i = 0; i < N; ++i)
                buf[i] = static_cast<char>(m_encrypted[i] ^ key_byte(i, Seed));
        }
    private:
        char m_encrypted[N];
    };

} // namespace xor_detail

// Static (not thread_local) buffer — safe under manual map.
// Each call site gets its own static buffer via unique lambda type.
#define XS(str) \
    ([]() -> const char* { \
        constexpr auto _seed = xor_detail::fnv1a_64(str, sizeof(str)); \
        static constexpr auto _enc = xor_detail::encrypted_string<sizeof(str), _seed>(str); \
        static char _buf[sizeof(str)]; \
        _enc.decrypt(_buf); \
        return _buf; \
    }())
