#pragma once

#include <cstdint>
#include <array>

namespace eerie_leap::subsys::cdmp::utilities {

struct CdmpConstants {
    static constexpr int DISCOVERY_MAX_BACKOFF_RESETS = 10;
    static constexpr int ID_CLAIM_RESPONSE_TIMEOUT_MS = 50;
    static constexpr int ID_CLAIM_MAX_ATTEMPTS = 10;
    // Claims a unit tracks until their first heartbeat; a full table replaces the oldest.
    static constexpr int PENDING_ID_CLAIMS_MAX = 8;

    static constexpr int HEARTBEAT_INTERVAL_MS = 3000;
    static constexpr int HEARTBEAT_TIMEOUT_MS = HEARTBEAT_INTERVAL_MS * 3;

    static constexpr int NETWORK_VALIDATION_INTERVAL_MS = 5000;
    static constexpr int STAGGERED_MESSAGE_TIME_OFFSET_MS = 5;

    static constexpr int USER_COMMAND_CODE_MIN = 0x20;
    static constexpr int USER_COMMAND_CODE_MAX = 0xFF;
    static constexpr int COMMAND_BROADCAST_ID = 0xFF;
};

struct CdmpTimeouts {
    static constexpr std::array<int, 3> RETRY_BACKOFFS_MS = {
        100, 200, 500};
};

} // namespace eerie_leap::subsys::cdmp::utilities
