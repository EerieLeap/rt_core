#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <utility>

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpHealthStatus;

// Base message structures (Base + 1)
struct CdmpHeartbeatMessage {
    uint8_t device_id;
    CdmpHealthStatus health_status;
    uint8_t sequence_number;
    uint32_t capability_flags;

    static CdmpHeartbeatMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        CdmpHeartbeatMessage message = {};
        message.device_id = frame_data[0];
        message.health_status = static_cast<CdmpHealthStatus>(frame_data[1]);
        message.sequence_number = frame_data[2];
        // Extract 32-bit capability flags (LSB first)
        message.capability_flags =
            (static_cast<uint32_t>(frame_data[6]) << 24)
            | (static_cast<uint32_t>(frame_data[5]) << 16)
            | (static_cast<uint32_t>(frame_data[4]) << 8)
            | static_cast<uint32_t>(frame_data[3]);

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            device_id,
            std::to_underlying(health_status),
            sequence_number,
            static_cast<uint8_t>(capability_flags),
            static_cast<uint8_t>(capability_flags >> 8),
            static_cast<uint8_t>(capability_flags >> 16),
            static_cast<uint8_t>(capability_flags >> 24)};

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
