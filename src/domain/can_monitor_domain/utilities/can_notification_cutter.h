#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eerie_leap::domain::can_monitor_domain::utilities {

/** @brief What CanNotificationCutter::Cut() found at the start of a span of records. */
struct CanNotificationCut {
    /** @brief The bytes of the whole records that fit. */
    size_t bytes = 0;
    /** @brief How many records those are (255 at most). */
    size_t records = 0;
    /** @brief The sum of their dts: what the next notification's `base_us` adds to this one's. */
    uint64_t dt_sum_us = 0;
    /**
     * @brief The first record is whole but larger than the room, so nothing was cut. The sender skips
     * it at a notification boundary: `first_record_size` bytes, and its dt still counts for
     * `base_us` (`first_record_dt_us`).
     */
    bool first_too_large = false;
    size_t first_record_size = 0;
    uint64_t first_record_dt_us = 0;
    /** @brief The span ends in a record that isn't all there (more bytes are due). */
    bool incomplete_tail = false;
    /** @brief The span holds bytes that are no record; the cut stops before them. */
    bool malformed = false;
};

/** @brief Cuts a notification's records out of the bytes the sender copied from the ring. */
class CanNotificationCutter {
public:
    /**
     * @brief Finds the longest prefix of whole records of @p records that fits @p room bytes.
     *
     * @param records Bytes from the ring, starting at a record.
     * @param room The notification's bytes left after its header.
     * @param max_records The cap on the records in a notification (its `n` is one byte).
     */
    static CanNotificationCut Cut(std::span<const uint8_t> records, size_t room, size_t max_records = 255);
};

} // namespace eerie_leap::domain::can_monitor_domain::utilities
