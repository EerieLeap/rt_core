#pragma once

#include <cstdint>

#include "can_frame_payload.h"

namespace eerie_leap::subsys::canbus {

struct CanFrame {
    uint32_t id = 0;
    bool is_extended = false;
    bool is_transmit = false;
    bool is_can_fd = false;
    bool is_bitrate_switch = false;
    bool is_remote_request = false;
    CanFramePayload data;
};

}  // namespace eerie_leap::subsys::canbus
