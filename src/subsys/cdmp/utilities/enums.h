#pragma once

#include <cstdint>
#include <array>

namespace eerie_leap::subsys::cdmp::utilities {

enum class CdmpDeviceStatus : uint8_t {
    OFFLINE = 0x00,
    INIT = 0x01,
    CLAIMING = 0x02,
    ONLINE = 0x03,
    VERSION_MISMATCH = 0x04,
    ERROR = 0x05
};

enum class CdmpHealthStatus : uint8_t {
    OK = 0x00,
    WARNING = 0x01,
    ERROR = 0x02
};

// Result codes for Command Response (Base + 3)
enum class CdmpResultCode : uint8_t {
    SUCCESS = 0x00,
    FAILURE = 0x01,
    INVALID_PARAMETER = 0x02,
    UNSUPPORTED_COMMAND = 0x03,
    CANCELLED = 0x04,
    TIMEOUT = 0x05,
    CRC_ERROR = 0x06,
    BUFFER_OVERFLOW = 0x07,
    DEVICE_BUSY = 0x08,
    ACCESS_DENIED = 0x09,
    NOT_READY = 0x0A,
    INVALID_STATE = 0x0B,
};

// Message types for Management messages (Base + 0)
enum class CdmpManagementMessageType : uint8_t {
    ID_CLAIM = 0x01,
    ID_CLAIM_RESPONSE = 0x02,
    DISCOVERY_REQUEST = 0x03,
    DISCOVERY_RESPONSE = 0x04,
    HEARTBEAT = 0x05
};

// ID Claim Response result codes for Management messages (Base + 0)
enum class CdmpIdClaimResult : uint8_t {
    ACCEPT = 0x00,
    REJECT = 0x01,
    VERSION_INCOMPATIBLE = 0x02
};

// Command codes for Command requests (Base + 2)
enum class CdmpServiceCommandCode : uint8_t {
    STATUS_REQUEST = 0x10,
    RESET_DEVICE = 0x11,
    // Application-specific: 0x20-0xFF
};

// State types for State Change Notifications (Base + 4)
enum class CdmpStateType : uint8_t {
    OPERATING_MODE_CHANGED = 0x01,
    FAULT_CONDITION_DETECTED = 0x02,
    CALIBRATION_STATUS_CHANGED = 0x03,
    CONNECTION_STATUS_CHANGED = 0x04,
    THRESHOLD_EVENT = 0x05,
    // Application-specific: 0x10-0xFF
};

} // namespace eerie_leap::subsys::cdmp::utilities
