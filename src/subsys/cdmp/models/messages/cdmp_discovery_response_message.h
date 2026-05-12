#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>
#include <utility>

#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpManagementMessageType;
using eerie_leap::subsys::cdmp::models::CdmpDeviceType;

// Base message structures (Base + 0)
struct CdmpDiscoveryResponseMessage {
    static constexpr CdmpManagementMessageType message_type = CdmpManagementMessageType::DISCOVERY_RESPONSE;
    uint8_t device_id;
    uint32_t uid;
    CdmpDeviceType device_type;

    static CdmpDiscoveryResponseMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data[0] != static_cast<uint8_t>(CdmpManagementMessageType::DISCOVERY_RESPONSE))
            throw std::invalid_argument("Incorrect message type");

        CdmpDiscoveryResponseMessage message = {};
        message.device_id = frame_data[1];
        message.uid =
            (static_cast<uint32_t>(frame_data[5]) << 24)
            | (static_cast<uint32_t>(frame_data[4]) << 16)
            | (static_cast<uint32_t>(frame_data[3]) << 8)
            | static_cast<uint32_t>(frame_data[2]);
        message.device_type = static_cast<CdmpDeviceType>(frame_data[6]);

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            std::to_underlying(message_type),
            device_id,
            static_cast<uint8_t>(uid),
            static_cast<uint8_t>(uid >> 8),
            static_cast<uint8_t>(uid >> 16),
            static_cast<uint8_t>(uid >> 24),
            std::to_underlying(device_type)};

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
