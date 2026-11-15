#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eerie_leap::domain::can_monitor_domain::utilities {

/** @brief The longest unsigned LEB128 of a 64-bit value: 9 x 7 bits and one more for bit 63. */
inline constexpr size_t LEB128_MAX_SIZE = 10;

/** @brief The bytes @p value takes as an unsigned LEB128 (1 to 10). */
constexpr size_t Leb128Size(uint64_t value) {
    size_t size = 1;
    while(value >= 0x80) {
        value >>= 7;
        ++size;
    }

    return size;
}

/**
 * @brief Writes @p value as an unsigned LEB128.
 * @return The bytes written (1 to 10), or 0 when @p out is too small (nothing is written then).
 */
constexpr size_t EncodeLeb128(uint64_t value, std::span<uint8_t> out) {
    const size_t size = Leb128Size(value);
    if(out.size() < size)
        return 0;

    for(size_t i = 0; i < size; ++i) {
        const uint8_t continuation = (i + 1 < size) ? 0x80 : 0x00;
        out[i] = static_cast<uint8_t>((value & 0x7F) | continuation);
        value >>= 7;
    }

    return size;
}

enum class Leb128Status : uint8_t {
    OK,
    /** @brief The span ends before the last byte: more bytes may complete it. */
    TRUNCATED,
    /** @brief More than 10 bytes, or a value past 64 bits: no more bytes can fix it. */
    INVALID,
};

struct Leb128Decoded {
    Leb128Status status = Leb128Status::INVALID;
    uint64_t value = 0;
    /** @brief The bytes the value took; 0 unless the status is OK. */
    size_t size = 0;

    [[nodiscard]] constexpr bool Ok() const {
        return status == Leb128Status::OK;
    }
};

/**
 * @brief Reads an unsigned LEB128 from the start of @p in.
 *
 * Non-canonical forms (a value with padding bytes) are accepted as long as they fit in 10 bytes
 * and 64 bits.
 */
constexpr Leb128Decoded DecodeLeb128(std::span<const uint8_t> in) {
    uint64_t value = 0;
    const size_t limit = in.size() < LEB128_MAX_SIZE ? in.size() : LEB128_MAX_SIZE;

    for(size_t i = 0; i < limit; ++i) {
        const uint8_t byte = in[i];

        // The tenth byte only has bit 63 left to carry, and ends the value.
        if(i == LEB128_MAX_SIZE - 1 && byte > 1)
            return { Leb128Status::INVALID, 0, 0 };

        value |= static_cast<uint64_t>(byte & 0x7F) << (7 * i);

        if((byte & 0x80) == 0)
            return { Leb128Status::OK, value, i + 1 };
    }

    return { Leb128Status::TRUNCATED, 0, 0 };
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities
