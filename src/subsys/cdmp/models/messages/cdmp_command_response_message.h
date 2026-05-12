#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <utility>

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpResultCode;
using eerie_leap::subsys::cdmp::utilities::CdmpServiceCommandCode;
using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpHealthStatus;

// (Base + 3)
struct CdmpCommandResponseMessage {
    uint8_t source_device_id;
    uint8_t command_code;
    uint8_t transaction_id;
    CdmpResultCode result_code;
    std::vector<uint8_t> data;

    static CdmpCommandResponseMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        CdmpCommandResponseMessage message = {};
        message.source_device_id = frame_data[0];
        message.command_code = frame_data[1];
        message.transaction_id = frame_data[2];
        message.result_code = static_cast<CdmpResultCode>(frame_data[3]);
        message.data = std::vector<uint8_t>(frame_data.begin() + 4, frame_data.end());

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            source_device_id,
            command_code,
            transaction_id,
            std::to_underlying(result_code)};
        frame_data.insert(frame_data.end(), data.begin(), data.end());

        return frame_data;
    }
};

// Status Request Response
// =======================
// Byte 0:    Source Device ID
// Byte 1:    STATUS_REQUEST
// Byte 2:    Transaction ID (echo from request)
// Byte 3:    Current Status
// Byte 4:    Protocol Version
// Byte 5:    Health Status
// Byte 6-7:  Reserved
struct CdmpCommandResponseStatusMessage : public CdmpCommandResponseMessage {
    CdmpCommandResponseStatusMessage(
        uint8_t source_device_id,
        uint8_t transaction_id,
        CdmpResultCode result_code,
        CdmpDeviceStatus status,
        uint8_t protocol_version,
        CdmpHealthStatus health_status) : CdmpCommandResponseMessage() {

        this->source_device_id = source_device_id;
        this->command_code = std::to_underlying(CdmpServiceCommandCode::STATUS_REQUEST);
        this->transaction_id = transaction_id;
        this->result_code = result_code;
        this->data = {
            std::to_underlying(status),
            protocol_version,
            std::to_underlying(health_status)};
    }
};

} // namespace eerie_leap::subsys::cdmp::models
