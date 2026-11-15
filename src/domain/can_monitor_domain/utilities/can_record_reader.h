#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "domain/can_monitor_domain/utilities/can_record.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

/** @brief A record read back; which fields hold something depends on the kind. */
struct CanRecord {
    CanRecordKind kind = CanRecordKind::FRAME;
    /** @brief µs since the previous record. */
    uint64_t dt_us = 0;
    /** @brief The bytes the record took. */
    size_t size = 0;

    // Kinds 0 and 1.
    uint32_t id = 0;
    uint8_t dlc = 0;
    bool ide = false;
    bool fdf = false;
    bool brs = false;
    bool rtr = false;
    bool esi = false;
    bool tx = false;
    bool tx_failed = false;
    /** @brief Kind 0: the payload. Kind 1: only the bytes that changed. Into the parsed span. */
    std::span<const uint8_t> payload;
    /** @brief Kind 1: the mask bytes, a bit for each payload byte that follows. Into the parsed span. */
    std::span<const uint8_t> masks;

    // Kind 2.
    CanBusState state = CanBusState::ERROR_ACTIVE;
    uint8_t tec = 0;
    uint8_t rec = 0;
    uint64_t detected_bitrate = 0;

    // Kind 3.
    CanStatistics statistics;

    // Kind 4.
    uint64_t frames_lost = 0;
    CanGapCause gap_cause = CanGapCause::RING;

    // Kind 5.
    CanEndReason end_reason = CanEndReason::CLOSED;

    // Kind 6.
    CanTxEvent tx_event = CanTxEvent::DISARMED;
    uint64_t tx_count = 0;

    // Kind 7.
    CanTapMark tap_mark = CanTapMark::MAY_HAVE_REPEATED;
};

enum class CanRecordStatus : uint8_t {
    OK,
    /** @brief The span ends inside the record. */
    INCOMPLETE,
    /** @brief Not a record of version 1: more bytes won't make it one. */
    MALFORMED,
};

/** @brief What the head of a record says. */
struct CanRecordInspection {
    CanRecordStatus status = CanRecordStatus::INCOMPLETE;
    /** @brief The record's bytes; 0 unless the status is OK. */
    size_t size = 0;
    uint64_t dt_us = 0;
    CanRecordKind kind = CanRecordKind::FRAME;
};

/**
 * @brief Reads the first record of @p in far enough to tell its size and dt, validating it as
 * ParseRecord() does.
 */
CanRecordInspection InspectRecord(std::span<const uint8_t> in);

/** @brief The size of the first record of @p in, 0 if it is incomplete or malformed. */
size_t RecordSize(std::span<const uint8_t> in);

/**
 * @brief Decodes the first record of @p in.
 *
 * A record is malformed when its kind is above 7, a kind 2 to 7 has a head other than 0x80 or
 * flags in its ext byte, a kind 1 is a remote frame, an ID doesn't fit its IDE, or a kind 1 mask
 * names a byte past the payload.
 *
 * @return nothing when the record is incomplete or malformed. The spans of the result point into @p in.
 */
std::optional<CanRecord> ParseRecord(std::span<const uint8_t> in);

} // namespace eerie_leap::domain::can_monitor_domain::utilities
