#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <stdexcept>
#include <utility>

#include "subsys/cdmp/models/messages/cdmp_command_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_command_response_message.h"
#include "subsys/cdmp/models/messages/cdmp_discovery_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_discovery_response_message.h"
#include "subsys/cdmp/models/messages/cdmp_heartbeat_message.h"
#include "subsys/cdmp/models/messages/cdmp_id_claim_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_id_claim_response_message.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpStateType;

// TODO: Rethink state change notification and response messages
// probably should be removed

// (Base + 4)
struct CdmpStateChangeNotification {
    uint8_t source_device_id;
    uint8_t state_version_number;
    CdmpStateType state_type;
    std::vector<uint8_t> data;

    static CdmpStateChangeNotification FromCanFrame(std::span<const uint8_t> frame_data) {
        CdmpStateChangeNotification message = {};
        message.source_device_id = frame_data[0];
        message.state_version_number = frame_data[1];
        message.state_type = static_cast<CdmpStateType>(frame_data[2]);
        message.data = std::vector<uint8_t>(frame_data.begin() + 3, frame_data.end());

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            source_device_id,
            state_version_number,
            std::to_underlying(state_type)};
        frame_data.insert(frame_data.end(), data.begin(), data.end());

        return frame_data;
    }
};

// (Base + 5)
struct CdmpStateChangeResponse {
    uint8_t source_device_id;
    uint8_t target_device_id;
    uint8_t state_version_number;
    CdmpResultCode response_code;
    std::vector<uint8_t> data;

    static CdmpStateChangeResponse FromCanFrame(std::span<const uint8_t> frame_data) {
        CdmpStateChangeResponse message = {};
        message.source_device_id = frame_data[0];
        message.target_device_id = frame_data[1];
        message.state_version_number = frame_data[2];
        message.response_code = static_cast<CdmpResultCode>(frame_data[3]);
        message.data = std::vector<uint8_t>(frame_data.begin() + 4, frame_data.end());

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            source_device_id,
            target_device_id,
            state_version_number,
            std::to_underlying(response_code)};
        frame_data.insert(frame_data.end(), data.begin(), data.end());

        return frame_data;
    }
};

// struct CdmpIsoTpFrame {
//     CdmpIsoTpFrameType frame_type;
//     std::vector<uint8_t> data;
// };

// struct CdmpIsoTpFlowControl {
//     CdmpIsoTpFlowStatus flow_status;
//     uint8_t block_size;
//     uint8_t separation_time_min;
//     uint8_t reserved[4];
// };

// struct CdmpIsoTpTransferHeader {
//     uint8_t source_device_id;
//     uint8_t target_device_id;
//     CdmpIsoTpTransferType transfer_type;
//     uint8_t transaction_id;
// };

// struct CdmpBulkTransferAck {
//     uint8_t target_device_id;
//     CdmpResultCode response_code; // Should be BULK_TRANSFER_ACK
//     uint8_t transaction_id;
//     CdmpResultCode result;
//     uint8_t reserved[3];
// };

} // namespace eerie_leap::subsys::cdmp::models
