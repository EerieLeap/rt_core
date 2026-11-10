#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <zephyr/drivers/can.h>

#include "domain/can_monitor_domain/utilities/leb128.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

/** @brief The record format version a notification's first byte states. */
inline constexpr uint8_t CAN_RECORD_VERSION = 1;

/** @brief version, port, seq (2), base_us (8), n. */
inline constexpr size_t CAN_NOTIFICATION_HEADER_SIZE = 13;

/** @brief The port a synthetic session states (no bus behind it). */
inline constexpr uint8_t CAN_SYNTHETIC_PORT = 255;

/** @brief The records a notification holds at most (its `n` is one byte). */
inline constexpr size_t CAN_NOTIFICATION_MAX_RECORDS = 255;

inline constexpr size_t CAN_MAX_PAYLOAD_SIZE = 64;

/** @brief The values of a statistics record. */
inline constexpr size_t CAN_STATISTICS_VALUES = 13;

/**
 * @brief The largest record there is: head, ext, a 10-byte dt and the 13 LEB128s of a statistics
 * record, each 10 bytes (a frame takes at most 88: 2 + 10 + 4 + 8 masks + 64).
 */
inline constexpr size_t CAN_MAX_RECORD_SIZE = 2 + LEB128_MAX_SIZE + CAN_STATISTICS_VALUES * LEB128_MAX_SIZE;

/** @brief The kind in the ext byte's bits 4 to 7. */
enum class CanRecordKind : uint8_t {
    FRAME = 0,
    CHANGED_BYTES = 1,
    STATE = 2,
    STATISTICS = 3,
    GAP = 4,
    END = 5,
    TX_EVENT = 6,
    TAP = 7,
};

inline constexpr uint8_t CAN_RECORD_KIND_COUNT = 8;

/** @brief Why frames are missing (a gap record's cause). */
enum class CanGapCause : uint8_t {
    RING = 1,
    RECEIVE_QUEUE = 2,
    CONTROLLER = 3,
    ECHO_POOL = 4,
};

inline constexpr size_t CAN_GAP_CAUSE_COUNT = 4;

enum class CanEndReason : uint8_t {
    CLOSED = 1,
    LEASE = 2,
    /** @brief Defined, never sent. */
    DISCONNECTED = 3,
    CONFIGURATION_STORED = 4,
    ERROR = 5,
};

enum class CanTxEvent : uint8_t {
    DISARMED = 1,
    BUS_OFF = 2,
    FRAMES_SKIPPED = 3,
    REPLAY_DONE = 4,
    REPLAY_UNDERRUN = 5,
};

/** @brief What the tap swap may have done to the frames around a tap record. */
enum class CanTapMark : uint8_t {
    MAY_HAVE_REPEATED = 1,
    MAY_HAVE_MISSED = 2,
};

/** @brief A state record's state; Zephyr's `can_state` in the same order. */
enum class CanBusState : uint8_t {
    ERROR_ACTIVE = 0,
    ERROR_WARNING = 1,
    ERROR_PASSIVE = 2,
    BUS_OFF = 3,
    STOPPED = 4,
};

constexpr CanBusState ToCanBusState(can_state state) {
    switch(state) {
        case CAN_STATE_ERROR_ACTIVE:
            return CanBusState::ERROR_ACTIVE;
        case CAN_STATE_ERROR_WARNING:
            return CanBusState::ERROR_WARNING;
        case CAN_STATE_ERROR_PASSIVE:
            return CanBusState::ERROR_PASSIVE;
        case CAN_STATE_BUS_OFF:
            return CanBusState::BUS_OFF;
        case CAN_STATE_STOPPED:
            return CanBusState::STOPPED;
    }

    return CanBusState::STOPPED;
}

// The head byte.
inline constexpr uint8_t CAN_HEAD_DLC_MASK = 0x0F;
inline constexpr uint8_t CAN_HEAD_IDE = 0x10;
inline constexpr uint8_t CAN_HEAD_FDF = 0x20;
inline constexpr uint8_t CAN_HEAD_BRS = 0x40;
/** @brief An ext byte follows. */
inline constexpr uint8_t CAN_HEAD_EXT = 0x80;

// The ext byte.
inline constexpr uint8_t CAN_EXT_RTR = 0x01;
inline constexpr uint8_t CAN_EXT_ESI = 0x02;
inline constexpr uint8_t CAN_EXT_TX = 0x04;
inline constexpr uint8_t CAN_EXT_TX_FAILED = 0x08;
inline constexpr uint8_t CAN_EXT_FLAGS_MASK = 0x0F;
inline constexpr uint8_t CAN_EXT_KIND_SHIFT = 4;

/** @brief What a statistics record carries; the arrays are in the wire's order. */
struct CanStatistics {
    /** @brief Indices into `errors`. */
    static constexpr size_t ERROR_BIT = 0;
    static constexpr size_t ERROR_STUFF = 1;
    static constexpr size_t ERROR_CRC = 2;
    static constexpr size_t ERROR_FORM = 3;
    static constexpr size_t ERROR_ACK = 4;
    /** @brief Indices into `lost`. */
    static constexpr size_t LOST_CONTROLLER = 0;
    static constexpr size_t LOST_RECEIVE_QUEUE = 1;
    static constexpr size_t LOST_RING = 2;
    static constexpr size_t LOST_ECHOES = 3;

    uint64_t frames_received = 0;
    uint64_t frames_sent = 0;
    uint64_t busy_us = 0;
    std::array<uint64_t, 5> errors{};
    std::array<uint64_t, 4> lost{};
    uint64_t filtered = 0;

    bool operator==(const CanStatistics&) const = default;
};

/**
 * @brief A frame as the encoders take it, independent of the CAN frame's layout.
 *
 * @c payload has the length the DLC gives (PayloadLength): none for a remote frame, 8 at most
 * without FDF.
 */
struct CanRecordFrame {
    uint32_t id = 0;
    uint8_t dlc = 0;
    bool ide = false;
    bool fdf = false;
    bool brs = false;
    bool rtr = false;
    bool esi = false;
    /** @brief The unit's own frame (an echo). */
    bool tx = false;
    bool tx_failed = false;
    std::span<const uint8_t> payload;
};

/** @brief The payload bytes of a frame with this DLC: 0 for a remote frame, 8 at most without FDF. */
inline size_t PayloadLength(uint8_t dlc, bool fdf, bool rtr) {
    if(rtr)
        return 0;

    const size_t bytes = can_dlc_to_bytes(dlc);

    return fdf ? bytes : std::min<size_t>(bytes, CAN_MAX_DLC);
}

/** @brief Whether the encoders take @p frame: a 4-bit DLC, an ID that fits, the payload the DLC gives. */
inline bool IsWellFormed(const CanRecordFrame& frame) {
    if(frame.dlc > CANFD_MAX_DLC)
        return false;

    if(frame.id > (frame.ide ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK))
        return false;

    return frame.payload.size() == PayloadLength(frame.dlc, frame.fdf, frame.rtr);
}

/**
 * @brief Widens a counter that wraps at 2^32 to 64 bits.
 *
 * The writer's counters are 32-bit atomics (64-bit ones need libatomic on some targets). The
 * reader calls Update() at least once in 2^32 counts.
 */
struct CanCounterWidener {
    uint64_t total = 0;
    uint32_t last = 0;

    uint64_t Update(uint32_t now) {
        total += static_cast<uint32_t>(now - last);
        last = now;

        return total;
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::utilities
