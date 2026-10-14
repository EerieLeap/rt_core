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

    // Peers a shared state change waits for; the heartbeat digest covers any others.
    static constexpr int STATE_ACK_PEERS_MAX = 8;
    // Receivers order a record's versions within a window of 127. A record that other changes leave
    // this far behind the unit's counter is stamped and sent again, so its next change still compares
    // as newer.
    static constexpr int STATE_VERSION_REFRESH_LAG = 64;
    // One sync request per heartbeat of a source, with room for heartbeats that arrive early.
    static constexpr int STATE_SYNC_REQUEST_MIN_INTERVAL_MS = HEARTBEAT_INTERVAL_MS / 2;
};

struct CdmpTimeouts {
    static constexpr std::array<int, 3> RETRY_BACKOFFS_MS = {
        100, 200, 500};

    // Waits before each resend of an unacknowledged state change (§7.2).
    static constexpr std::array<int, 3> STATE_RETRANSMIT_BACKOFFS_MS = {
        50, 100, 200};
};

} // namespace eerie_leap::subsys::cdmp::utilities
