#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>
#include <utility>

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpManagementMessageType;

// Base message structures (Base + 0)
struct CdmpDiscoveryRequestMessage {
    static constexpr CdmpManagementMessageType message_type = CdmpManagementMessageType::DISCOVERY_REQUEST;
    uint32_t uid;

    static CdmpDiscoveryRequestMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data[0] != static_cast<uint8_t>(CdmpManagementMessageType::DISCOVERY_REQUEST))
            throw std::invalid_argument("Incorrect message type");

        CdmpDiscoveryRequestMessage message = {};
        message.uid =
            (static_cast<uint32_t>(frame_data[4]) << 24)
            | (static_cast<uint32_t>(frame_data[3]) << 16)
            | (static_cast<uint32_t>(frame_data[2]) << 8)
            | static_cast<uint32_t>(frame_data[1]);

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            std::to_underlying(message_type),
            static_cast<uint8_t>(uid),
            static_cast<uint8_t>(uid >> 8),
            static_cast<uint8_t>(uid >> 16),
            static_cast<uint8_t>(uid >> 24)
        };

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
