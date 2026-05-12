#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <utility>

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::models {

// (Base + 2)
struct CdmpCommandRequestMessage {
    uint8_t target_device_id;
    uint8_t command_code;
    uint8_t transaction_id;
    std::vector<uint8_t> data;

    static CdmpCommandRequestMessage FromCanFrame(std::span<const uint8_t> frame_data) {
        CdmpCommandRequestMessage message = {};
        message.target_device_id = frame_data[0];
        message.command_code = frame_data[1];
        message.transaction_id = frame_data[2];
        message.data = std::vector<uint8_t>(frame_data.begin() + 3, frame_data.end());

        return message;
    }

    std::vector<uint8_t> ToCanFrame() const {
        std::vector<uint8_t> frame_data = {
            // source_device_id,
            target_device_id,
            command_code,
            transaction_id};
        frame_data.insert(frame_data.end(), data.begin(), data.end());

        return frame_data;
    }
};

} // namespace eerie_leap::subsys::cdmp::models
