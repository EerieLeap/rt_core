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
    bool is_error_state_indicator = false;
    CanFramePayload data;
    /// The DLC as received. A frame built for sending may leave it 0; a remote frame has no payload but
    /// still asks for a length, which `data` cannot hold.
    uint8_t dlc = 0;
    /// Receive time in µs, on the base of NowUs() (canbus_time.h); 0 for a frame that was not received.
    uint64_t time_us = 0;
};

} // namespace eerie_leap::subsys::canbus
