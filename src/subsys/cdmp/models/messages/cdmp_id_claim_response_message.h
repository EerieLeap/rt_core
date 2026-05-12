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
using eerie_leap::subsys::cdmp::utilities::CdmpIdClaimResult;

// Base message structures (Base + 0)
struct CdmpIdClaimResponseMessage {
    static constexpr CdmpManagementMessageType message_type = CdmpManagementMessageType::ID_CLAIM_RESPONSE;
    uint8_t responding_device_id;
    uint8_t claiming_device_id;
    uint32_t claiming_device_uid;
    CdmpIdClaimResult result;

    static CdmpIdClaimResponseMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        if(frame_data[0] != static_cast<uint8_t>(CdmpManagementMessageType::ID_CLAIM_RESPONSE))
            throw std::invalid_argument("Incorrect message type");

        CdmpIdClaimResponseMessage message = {};
        message.responding_device_id = frame_data[1];
        message.claiming_device_id = frame_data[2];
        message.result = static_cast<CdmpIdClaimResult>(frame_data[3]);

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            std::to_underlying(message_type),
            responding_device_id,
            claiming_device_id,
            std::to_underlying(result)
        };

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
