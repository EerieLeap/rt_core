#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "domain/can_monitor_domain/utilities/byte_ring.h"
#include "domain/can_monitor_domain/utilities/can_record.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

class CanDeltaTable;

// The encoders write one record into @p out and return its size, or 0 when @p out is too small or
// the record is malformed (a frame whose payload isn't the DLC's length, an ID that doesn't fit,
// a DLC above 15). @p dt_us is the time since the previous record. They hold no state.

/** @brief version, port, seq, base_us, n (CAN_NOTIFICATION_HEADER_SIZE bytes). */
size_t EncodeNotificationHeader(std::span<uint8_t> out, uint8_t port, uint16_t seq, uint64_t base_us, uint8_t count);

/** @brief Kind 0. The ext byte is there only for RTR, ESI, TX or TX failed. */
size_t EncodeFrame(std::span<uint8_t> out, uint64_t dt_us, const CanRecordFrame& frame);

/** @brief The bytes EncodeFrame() writes, or 0 for a malformed frame. */
size_t FrameRecordSize(uint64_t dt_us, const CanRecordFrame& frame);

/**
 * @brief Kind 1: the bytes of @p frame that differ from @p previous_payload.
 *
 * Fails for a remote frame and when @p previous_payload isn't as long as the payload.
 */
size_t EncodeChangedBytes(
    std::span<uint8_t> out,
    uint64_t dt_us,
    const CanRecordFrame& frame,
    std::span<const uint8_t> previous_payload
);

/** @brief Kind 2. @p detected_bitrate is 0 when unchanged. */
size_t EncodeState(
    std::span<uint8_t> out,
    uint64_t dt_us,
    CanBusState state,
    uint8_t tec,
    uint8_t rec,
    uint64_t detected_bitrate
);

/** @brief Kind 3. */
size_t EncodeStatistics(std::span<uint8_t> out, uint64_t dt_us, const CanStatistics& statistics);

/** @brief Kind 4. */
size_t EncodeGap(std::span<uint8_t> out, uint64_t dt_us, uint64_t frames_lost, CanGapCause cause);

/** @brief Kind 5. */
size_t EncodeEnd(std::span<uint8_t> out, uint64_t dt_us, CanEndReason reason);

/** @brief Kind 6. */
size_t EncodeTxEvent(std::span<uint8_t> out, uint64_t dt_us, CanTxEvent event, uint64_t count);

/** @brief Kind 7. */
size_t EncodeTap(std::span<uint8_t> out, uint64_t dt_us, CanTapMark mark);

/**
 * @brief The producer side of a session's ring: it turns what the CAN receive thread sees into records.
 *
 * Every method but the counters' getters runs on one thread, the receive thread, so none takes a
 * lock or waits. The getters are for the sender thread; the counters wrap at 2^32 (CanCounterWidener).
 *
 * - A record's dt is its time less the time of the last record written; a time before that gives 0.
 *   That time moves only when a record is written.
 * - A record that doesn't fit is counted lost with the cause ring and goes into a gap.
 * - Losses (AddLoss() and the ring's own) wait as counts, a cause each, and are written as gap
 *   records (dt 0: the frames went missing right after the last record) before the next record.
 *   When a gap doesn't fit it stays pending, and the record behind it is lost too.
 * - A gap or tap record empties the changed-bytes table (a lost frame does so at once): the next
 *   frame of every ID goes whole.
 */
class CanRecordWriter {
private:
    ByteRing& ring_;
    // Unused without CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA.
    [[maybe_unused]] CanDeltaTable* delta_;
    uint64_t last_time_us_;

    std::array<uint64_t, CAN_GAP_CAUSE_COUNT> pending_{};

    std::atomic<uint32_t> frames_written_{ 0 };
    std::atomic<uint32_t> frames_rejected_{ 0 };
    std::array<std::atomic<uint32_t>, CAN_GAP_CAUSE_COUNT> lost_{};

    uint64_t DeltaSince(uint64_t time_us) const;
    void CountLost(CanGapCause cause, uint64_t count);
    void ResetDelta();
    bool WriteRecord(std::span<const uint8_t> record, uint64_t dt_us);

public:
    /**
     * @param ring The session's ring.
     * @param start_time_us The session's start: the time of the record before the first.
     * @param delta Where changed-bytes records come from; nullptr (or a build without
     * EERIE_LEAP_CAN_MONITOR_DELTA) sends every frame whole.
     */
    CanRecordWriter(ByteRing& ring, uint64_t start_time_us, CanDeltaTable* delta = nullptr);

    ~CanRecordWriter() = default;

    CanRecordWriter(const CanRecordWriter&) = delete;
    CanRecordWriter& operator=(const CanRecordWriter&) = delete;
    CanRecordWriter(CanRecordWriter&&) = delete;
    CanRecordWriter& operator=(CanRecordWriter&&) = delete;

    /**
     * @brief Writes a frame (kind 0, or 1 through the table).
     * @return false when it was lost to a full ring (and counted), or rejected as malformed (counted
     * apart, with no gap: it is a caller's bug).
     */
    bool WriteFrame(uint64_t time_us, const CanRecordFrame& frame);

    /** @brief Writes a state record. A record that doesn't fit counts as one lost. */
    bool WriteState(uint64_t time_us, CanBusState state, uint8_t tec, uint8_t rec, uint64_t detected_bitrate);

    /** @brief Writes a tap record. A record that doesn't fit counts as one lost. */
    bool WriteTap(uint64_t time_us, CanTapMark mark);

    /** @brief Counts @p count frames lost for a cause seen elsewhere (receive queue, controller, echoes). */
    void AddLoss(CanGapCause cause, uint64_t count);

    /** @brief Writes the pending gaps; false when one doesn't fit. Every write does it first. */
    bool FlushLosses();

    /** @brief The time of the last record written (the start time before any). */
    [[nodiscard]] uint64_t LastTimeUs() const {
        return last_time_us_;
    }

    /** @brief Losses not written as gaps yet, for a cause. Producer thread. */
    [[nodiscard]] uint64_t Pending(CanGapCause cause) const {
        return pending_[static_cast<size_t>(cause) - 1];
    }

    // Sender thread.

    /** @brief Frame records written (kinds 0 and 1). */
    [[nodiscard]] uint32_t FramesWritten() const {
        return frames_written_.load(std::memory_order_relaxed);
    }

    /** @brief Frames lost for a cause, the pending ones included. */
    [[nodiscard]] uint32_t LostCount(CanGapCause cause) const {
        return lost_[static_cast<size_t>(cause) - 1].load(std::memory_order_relaxed);
    }

    /** @brief Frames the encoder refused as malformed. */
    [[nodiscard]] uint32_t FramesRejected() const {
        return frames_rejected_.load(std::memory_order_relaxed);
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::utilities
