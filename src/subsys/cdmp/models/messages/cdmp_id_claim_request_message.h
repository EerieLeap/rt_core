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
struct CdmpIdClaimRequestMessage {
    static constexpr CdmpManagementMessageType message_type = CdmpManagementMessageType::ID_CLAIM;
    uint8_t claiming_device_id;
    uint32_t uid;
    CdmpDeviceType device_type;
    uint8_t protocol_version;

    static CdmpIdClaimRequestMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data[0] != static_cast<uint8_t>(CdmpManagementMessageType::ID_CLAIM))
            throw std::invalid_argument("Incorrect message type");

        CdmpIdClaimRequestMessage message = {};
        message.claiming_device_id = frame_data[1];
        message.uid =
            (static_cast<uint32_t>(frame_data[5]) << 24)
            | (static_cast<uint32_t>(frame_data[4]) << 16)
            | (static_cast<uint32_t>(frame_data[3]) << 8)
            | static_cast<uint32_t>(frame_data[2]);
        message.device_type = static_cast<CdmpDeviceType>(frame_data[6]);
        message.protocol_version = frame_data[7];

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            std::to_underlying(message_type),
            claiming_device_id,
            static_cast<uint8_t>(uid),
            static_cast<uint8_t>(uid >> 8),
            static_cast<uint8_t>(uid >> 16),
            static_cast<uint8_t>(uid >> 24),
            std::to_underlying(device_type),
            protocol_version};

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
