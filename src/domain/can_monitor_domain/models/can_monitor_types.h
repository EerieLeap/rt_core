#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "subsys/canbus/canbus_type.h"

namespace eerie_leap::domain::can_monitor_domain::models {

using eerie_leap::subsys::canbus::CanbusType;

/**
 * @brief The errors of the `canmon` group, in the group's SMP v2 `err` map. The values are the wire's.
 *
 * The service answers with them and the group sends them as they are.
 */
enum class CanMonitorError : uint16_t {
    OK = 0,
    UNKNOWN = 1,
    /// No session holds this token.
    NO_SESSION = 2,
    /// The token is 0, which no session has.
    BAD_TOKEN = 3,
    /// The port has a session, every session slot is taken, or the service is stopped.
    BUSY = 4,
    /// No such port.
    NOT_FOUND = 5,
    /// A mode the port can't take.
    BAD_MODE = 6,
    BAD_BITRATE = 7,
    /// `bus` notifications are off.
    NOT_LISTENING = 8,
    /// The ring can't be had.
    NO_MEMORY = 9,
    NOT_ARMED = 10,
    RESERVED = 11,
    RATE = 12,
    BUS_OFF = 13,
    NO_ROOM = 14,
    TX_UNSUPPORTED = 15,
    BAD_SEQ = 16,
};

/** @brief OPEN's `mode`; the values are the wire's. */
enum class CanMonitorMode : uint8_t {
    /// Sees the frames of a configured channel as it runs.
    TAP = 0,
    /// A bus of its own on a port the configuration doesn't use, receiving only.
    LISTEN_ONLY = 1,
    /// A bus of its own that may transmit.
    ACTIVE = 2,
};

inline constexpr uint32_t CAN_MONITOR_MODE_COUNT = 3;

/** @brief The port a synthetic session states (the same value the records' header carries). */
inline constexpr uint8_t CAN_MONITOR_SYNTHETIC_PORT = 255;

/** @brief A unit filter: a frame passes when `(frame.id & mask) == (id & mask)` and its IDE is `ext`. */
struct CanUnitFilter {
    uint32_t id = 0;
    uint32_t mask = 0;
    bool ext = false;

    bool operator==(const CanUnitFilter&) const = default;

    [[nodiscard]] bool Matches(uint32_t frame_id, bool frame_ext) const {
        return frame_ext == ext && (frame_id & mask) == (id & mask);
    }
};

/** @brief The generated traffic of a synthetic session. */
struct CanSyntheticParameters {
    /// Frames a second.
    uint32_t fps = 0;
    /// Distinct identifiers used in turn.
    uint32_t ids = 0;
    /// The DLC of every frame (0 to 8, or 15 with `fd`).
    uint8_t dlc = 0;
    /// 29-bit identifiers.
    bool ext = false;
    /// CAN FD frames with a bit rate switch.
    bool fd = false;

    bool operator==(const CanSyntheticParameters&) const = default;

    static constexpr uint32_t MAX_FPS = 200000;
    /// Standard identifiers stay distinct up to 2048.
    static constexpr uint32_t MAX_IDS = 2048;

    [[nodiscard]] constexpr bool IsValid() const {
        return fps >= 1 && fps <= MAX_FPS && ids >= 1 && ids <= MAX_IDS && dlc <= (fd ? 15 : 8);
    }
};

/** @brief OPEN's request. */
struct CanMonitorOpenRequest {
    uint8_t ch = 0;
    CanMonitorMode mode = CanMonitorMode::TAP;
    /// The type and bitrates of a session that builds a bus of its own (LISTEN_ONLY, ACTIVE): classical CAN
    /// by default, bitrate 0 or absent detects it, an FD data bitrate absent or 0 is the nominal one. A TAP
    /// session runs at the channel's and ignores them.
    std::optional<CanbusType> type;
    std::optional<uint32_t> bitrate;
    std::optional<uint32_t> data_bitrate;
    /// The unit filters; an empty list passes every frame.
    std::span<const CanUnitFilter> filters;
    /// Changed-bytes records; ignored in a build without EERIE_LEAP_CAN_MONITOR_DELTA.
    bool delta = false;
    /// A session with no bus; `ch` and `mode` are then ignored.
    std::optional<CanSyntheticParameters> synthetic;
};

/** @brief OPEN's answer. */
struct CanMonitorOpenAnswer {
    uint32_t tok = 0;
    uint8_t ch = 0;
    CanMonitorMode mode = CanMonitorMode::TAP;
    CanbusType type = CanbusType::NONE;
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    uint32_t lease_ms = 0;
};

/** @brief The version of the records and of STATUS's answer. */
inline constexpr uint8_t CAN_MONITOR_VERSION = 1;

/** @brief One CAN port of the devicetree, as STATUS lists it. */
struct CanMonitorPortStatus {
    uint8_t ch = 0;
    /// The controller can run CAN FD besides classical CAN.
    bool is_fd_capable = false;
    /// The stored configuration runs the port.
    bool is_configured = false;
    /// The controller's state as a state record's code (0 error active ... 4 stopped).
    uint8_t state = 0;
    /// The configured channel's, or those of the session's own bus on the port; 0 until detected.
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    /// A session holds the port.
    bool has_session = false;
};

/** @brief STATUS's answer: the ports and what the build and the link allow. */
struct CanMonitorStatus {
    std::vector<CanMonitorPortStatus> ports;
    uint32_t filters_max = 0;
    uint32_t ring_bytes = 0;
    /// The largest notification the link takes now; 0 while `bus` notifications are off.
    uint32_t notify_max = 0;
    bool has_tx = false;
    bool has_synthetic = false;
    bool has_delta = false;
};

/** @brief STATS's answer: totals since OPEN, and the controller's state now. */
struct CanMonitorStats {
    uint8_t state = 0;
    uint8_t tec = 0;
    uint8_t rec = 0;
    uint64_t frames_received = 0;
    uint64_t frames_sent = 0;
    uint64_t busy_us = 0;
    /// Bit, stuff, CRC, form, acknowledgement.
    std::array<uint64_t, 5> errors{};
    /// Controller, receive queue, ring, echoes.
    std::array<uint64_t, 4> lost{};
    uint64_t filtered = 0;
};

} // namespace eerie_leap::domain::can_monitor_domain::models
