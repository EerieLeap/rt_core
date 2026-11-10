#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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
    /// The port's type, bitrates: given to a session that builds a bus of its own (Phase 3); a TAP session
    /// runs at the channel's.
    std::optional<CanbusType> type;
    std::optional<uint32_t> bitrate;
    std::optional<uint32_t> data_bitrate;
    /// The unit filters; an empty list passes every frame.
    std::span<const CanUnitFilter> filters;
    /// Changed-bytes records (ignored until the build has them and Phase 3 wires them).
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

} // namespace eerie_leap::domain::can_monitor_domain::models
