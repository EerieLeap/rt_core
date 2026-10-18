#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpResultCode;

// State Change Response (Base + 5)
// ================================
// Byte 0:    Responding Device ID
// Byte 1:    Source Device ID of the acknowledged notification
// Byte 2:    Record Version (echo)
// Byte 3:    Result Code (0x00 = received)
struct CdmpStateChangeResponseMessage {
    static constexpr size_t FRAME_SIZE = 4;

    uint8_t responding_device_id;
    uint8_t source_device_id;
    uint8_t version;
    CdmpResultCode result_code = CdmpResultCode::SUCCESS;

    /** @return The message, or nothing for a frame shorter than 4 bytes; never throws. */
    static std::optional<CdmpStateChangeResponseMessage> TryFromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data.size() < FRAME_SIZE)
            return std::nullopt;

        return CdmpStateChangeResponseMessage{ .responding_device_id = frame_data[0],
            .source_device_id = frame_data[1],
            .version = frame_data[2],
            .result_code = static_cast<CdmpResultCode>(frame_data[3]) };
    }

    /** @throws std::invalid_argument for a frame shorter than 4 bytes. */
    static CdmpStateChangeResponseMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        auto message = TryFromCanFrame(frame_data);
        if(!message.has_value())
            throw std::invalid_argument("State change response is shorter than 4 bytes");

        return message.value();
    }

    std::array<uint8_t, FRAME_SIZE> ToCanFrame() const {
        return { responding_device_id, source_device_id, version, std::to_underlying(result_code) };
    }
};

} // namespace eerie_leap::subsys::cdmp::models
