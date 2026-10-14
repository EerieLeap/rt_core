#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

#include "subsys/cdmp/models/cdmp_state_data.h"

namespace eerie_leap::subsys::cdmp::models {

// State Change Notification (Base + 4)
// ====================================
// Byte 0:    Source Device ID
// Byte 1:    Record Version (one counter per source)
// Byte 2:    State Type (0x00 invalid)
// Byte 3-7:  State Data, 0-5 bytes; none withdraws the record
struct CdmpStateChangeMessage {
    static constexpr size_t HEADER_SIZE = 3;
    static constexpr size_t MAX_FRAME_SIZE = HEADER_SIZE + CdmpStateData::MAX_SIZE;

    using Frame = std::array<uint8_t, MAX_FRAME_SIZE>;

    uint8_t source_device_id;
    uint8_t version;
    uint8_t state_type;
    CdmpStateData data; ///< Empty withdraws the record.

    /** @return The message, or nothing for a frame shorter than 3 or longer than 8 bytes; never throws. */
    static std::optional<CdmpStateChangeMessage> TryFromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data.size() < HEADER_SIZE || frame_data.size() > MAX_FRAME_SIZE)
            return std::nullopt;

        return CdmpStateChangeMessage{
            .source_device_id = frame_data[0],
            .version = frame_data[1],
            .state_type = frame_data[2],
            .data = CdmpStateData(frame_data.subspan(HEADER_SIZE))
        };
    }

    /** @throws std::invalid_argument for a frame shorter than 3 or longer than 8 bytes. */
    static CdmpStateChangeMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        auto message = TryFromCanFrame(frame_data);
        if(!message.has_value())
            throw std::invalid_argument("State change frame is not 3 to 8 bytes long");

        return message.value();
    }

    /** @brief Writes the frame without allocating. @return Its length, 3 to 8 bytes. */
    size_t ToCanFrame(Frame& frame) const {
        frame[0] = source_device_id;
        frame[1] = version;
        frame[2] = state_type;
        std::ranges::copy(data.GetData(), frame.begin() + HEADER_SIZE);

        return HEADER_SIZE + data.GetSize();
    }
};

} // namespace eerie_leap::subsys::cdmp::models
