#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>
#include <utility>

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpHealthStatus;

// Base message structures (Base + 1)
struct CdmpHeartbeatMessage {
    // Every unit sends all 8 bytes, so a shorter frame is malformed.
    static constexpr size_t FRAME_SIZE = 8;

    uint8_t device_id;
    CdmpHealthStatus health_status;
    uint8_t sequence_number;
    uint16_t features; ///< Application feature mask, bytes 3-4. Bytes 5-6 are reserved: sent 0, ignored.
    uint8_t state_digest; ///< CRC-8 over the sender's shared state records, 0 without records.

    static CdmpHeartbeatMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data.size() < FRAME_SIZE)
            throw std::invalid_argument("Heartbeat frame is shorter than 8 bytes");

        CdmpHeartbeatMessage message = {};
        message.device_id = frame_data[0];
        message.health_status = static_cast<CdmpHealthStatus>(frame_data[1]);
        message.sequence_number = frame_data[2];
        message.features = static_cast<uint16_t>(
            (static_cast<uint16_t>(frame_data[4]) << 8)
            | static_cast<uint16_t>(frame_data[3]));
        message.state_digest = frame_data[7];

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            device_id,
            std::to_underlying(health_status),
            sequence_number,
            static_cast<uint8_t>(features),
            static_cast<uint8_t>(features >> 8),
            0,
            0,
            state_digest};

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
