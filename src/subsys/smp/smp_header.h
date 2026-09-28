#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace eerie_leap::subsys::smp {

/** @brief SMP operation codes (the 3-bit `op` field). */
enum class SmpOperation : uint8_t {
    READ = 0,
    READ_RESPONSE = 1,
    WRITE = 2,
    WRITE_RESPONSE = 3,
};

/** @brief The 8-byte SMP header, decoded without the bitfield layout of MCUmgr's struct smp_hdr. */
struct SmpHeader {
    static constexpr size_t SIZE = 8;

    SmpOperation operation = SmpOperation::READ;
    uint8_t version = 0;    ///< 0 for SMP v1, 1 for SMP v2.
    uint8_t flags = 0;
    uint16_t length = 0;    ///< Length of the CBOR body that follows the header.
    uint16_t group = 0;
    uint8_t sequence = 0;   ///< Echoed in the response.
    uint8_t command_id = 0;

    /** @brief Decodes the header at the start of @p data; std::nullopt if it is too short. */
    static constexpr std::optional<SmpHeader> Parse(std::span<const uint8_t> data) {
        if(data.size() < SIZE)
            return std::nullopt;

        return SmpHeader {
            .operation = static_cast<SmpOperation>(data[0] & 0x07),
            .version = static_cast<uint8_t>((data[0] >> 3) & 0x03),
            .flags = data[1],
            .length = static_cast<uint16_t>((data[2] << 8) | data[3]),
            .group = static_cast<uint16_t>((data[4] << 8) | data[5]),
            .sequence = data[6],
            .command_id = data[7],
        };
    }

    /** @brief Writes the header in network byte order. */
    constexpr void Encode(std::span<uint8_t, SIZE> data) const {
        data[0] = static_cast<uint8_t>((static_cast<uint8_t>(operation) & 0x07) | ((version & 0x03) << 3));
        data[1] = flags;
        data[2] = static_cast<uint8_t>(length >> 8);
        data[3] = static_cast<uint8_t>(length);
        data[4] = static_cast<uint8_t>(group >> 8);
        data[5] = static_cast<uint8_t>(group);
        data[6] = sequence;
        data[7] = command_id;
    }

    /** @brief Header plus body length. */
    [[nodiscard]] constexpr size_t GetPacketSize() const { return SIZE + length; }

    /** @brief True for READ and WRITE. */
    [[nodiscard]] constexpr bool IsRequest() const {
        return operation == SmpOperation::READ || operation == SmpOperation::WRITE;
    }

    /** @brief True for READ_RESPONSE and WRITE_RESPONSE. */
    [[nodiscard]] constexpr bool IsResponse() const {
        return operation == SmpOperation::READ_RESPONSE || operation == SmpOperation::WRITE_RESPONSE;
    }
};

} // namespace eerie_leap::subsys::smp
