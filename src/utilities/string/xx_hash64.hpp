#pragma once

#include <cstdint>
#include <string_view>
#include <bit>

class XxHash64 {
public:
    XxHash64() = delete;

    static constexpr uint32_t GetHash(std::string_view str) noexcept {
        return static_cast<uint32_t>(XxHash64::XXH64(str));
    }

    static constexpr uint64_t GetHash64(std::string_view str) noexcept {
        return XxHash64::XXH64(str);
    }

private:
    static constexpr uint64_t XXH_PRIME64_1 = 0x9E3779B185EBCA87ULL;
    static constexpr uint64_t XXH_PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
    static constexpr uint64_t XXH_PRIME64_3 = 0x165667B19E3779F9ULL;
    static constexpr uint64_t XXH_PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
    static constexpr uint64_t XXH_PRIME64_5 = 0x27D4EB2F165667C5ULL;

    static constexpr uint64_t ReadLE64(const unsigned char* p) noexcept {
        return  static_cast<uint64_t>(p[0])
              | (static_cast<uint64_t>(p[1]) << 8)
              | (static_cast<uint64_t>(p[2]) << 16)
              | (static_cast<uint64_t>(p[3]) << 24)
              | (static_cast<uint64_t>(p[4]) << 32)
              | (static_cast<uint64_t>(p[5]) << 40)
              | (static_cast<uint64_t>(p[6]) << 48)
              | (static_cast<uint64_t>(p[7]) << 56);
    }

    static constexpr uint32_t ReadLE32(const unsigned char* p) noexcept {
        return  static_cast<uint32_t>(p[0])
              | (static_cast<uint32_t>(p[1]) << 8)
              | (static_cast<uint32_t>(p[2]) << 16)
              | (static_cast<uint32_t>(p[3]) << 24);
    }

    static constexpr uint64_t Round(uint64_t acc, uint64_t input) noexcept {
        acc += input * XXH_PRIME64_2;
        acc = std::rotl(acc, 31);
        acc *= XXH_PRIME64_1;
        return acc;
    }

    static constexpr uint64_t MergeRound(uint64_t acc, uint64_t val) noexcept {
        val = Round(0, val);
        acc ^= val;
        acc = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
        return acc;
    }

    static constexpr uint64_t Avalanche(uint64_t h) noexcept {
        h ^= h >> 33;
        h *= XXH_PRIME64_2;
        h ^= h >> 29;
        h *= XXH_PRIME64_3;
        h ^= h >> 32;
        return h;
    }

    static constexpr uint64_t XXH64(std::string_view str, uint64_t seed = 0) noexcept {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(str.data());
        const unsigned char* const end = p + str.size();
        const size_t len = str.size();
        uint64_t h64;

        if (len >= 32) {
            const unsigned char* const limit = end - 32;
            uint64_t v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
            uint64_t v2 = seed + XXH_PRIME64_2;
            uint64_t v3 = seed + 0;
            uint64_t v4 = seed - XXH_PRIME64_1;

            do {
                v1 = Round(v1, ReadLE64(p)); p += 8;
                v2 = Round(v2, ReadLE64(p)); p += 8;
                v3 = Round(v3, ReadLE64(p)); p += 8;
                v4 = Round(v4, ReadLE64(p)); p += 8;
            } while (p <= limit);

            h64 = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
            h64 = MergeRound(h64, v1);
            h64 = MergeRound(h64, v2);
            h64 = MergeRound(h64, v3);
            h64 = MergeRound(h64, v4);
        } else {
            h64 = seed + XXH_PRIME64_5;
        }

        h64 += static_cast<uint64_t>(len);

        while (p + 8 <= end) {
            h64 ^= Round(0, ReadLE64(p));
            h64 = std::rotl(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
            p += 8;
        }

        if (p + 4 <= end) {
            h64 ^= static_cast<uint64_t>(ReadLE32(p)) * XXH_PRIME64_1;
            h64 = std::rotl(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
            p += 4;
        }

        while (p < end) {
            h64 ^= static_cast<uint64_t>(*p) * XXH_PRIME64_5;
            h64 = std::rotl(h64, 11) * XXH_PRIME64_1;
            ++p;
        }

        return Avalanche(h64);
    }
};
