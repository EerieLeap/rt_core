#include "domain/can_monitor_domain/utilities/can_record_writer.h"

#include <zephyr/sys/byteorder.h>

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    #include "domain/can_monitor_domain/utilities/can_delta_table.h"
#endif

namespace eerie_leap::domain::can_monitor_domain::utilities {

namespace {

/** @brief Bytes go into a span; the first one that doesn't fit makes Finish() return 0. */
class Cursor {
private:
    std::span<uint8_t> out_;
    size_t pos_ = 0;
    bool ok_ = true;

public:
    explicit Cursor(std::span<uint8_t> out) :
        out_(out) {}

    void Put(uint8_t byte) {
        if(pos_ < out_.size())
            out_[pos_++] = byte;
        else
            ok_ = false;
    }

    void Put(std::span<const uint8_t> bytes) {
        if(out_.size() - pos_ < bytes.size()) {
            ok_ = false;
            return;
        }

        for(const uint8_t byte : bytes)
            out_[pos_++] = byte;
    }

    void PutLeb128(uint64_t value) {
        const size_t size = EncodeLeb128(value, out_.subspan(pos_));
        if(size == 0)
            ok_ = false;
        else
            pos_ += size;
    }

    void PutId(uint32_t id, bool ide) {
        std::array<uint8_t, 4> bytes{};
        if(ide) {
            sys_put_le32(id, bytes.data());
            Put(std::span<const uint8_t>(bytes));
        } else {
            sys_put_le16(static_cast<uint16_t>(id), bytes.data());
            Put(std::span<const uint8_t>(bytes).first(2));
        }
    }

    [[nodiscard]] size_t Finish() const {
        return ok_ ? pos_ : 0;
    }
};

bool HasFlags(const CanRecordFrame& frame) {
    return frame.rtr || frame.esi || frame.tx || frame.tx_failed;
}

uint8_t HeadOf(const CanRecordFrame& frame, bool with_ext) {
    uint8_t head = frame.dlc & CAN_HEAD_DLC_MASK;
    if(frame.ide)
        head |= CAN_HEAD_IDE;
    if(frame.fdf)
        head |= CAN_HEAD_FDF;
    if(frame.brs)
        head |= CAN_HEAD_BRS;
    if(with_ext)
        head |= CAN_HEAD_EXT;

    return head;
}

uint8_t FlagsOf(const CanRecordFrame& frame) {
    uint8_t flags = 0;
    if(frame.rtr)
        flags |= CAN_EXT_RTR;
    if(frame.esi)
        flags |= CAN_EXT_ESI;
    if(frame.tx)
        flags |= CAN_EXT_TX;
    if(frame.tx_failed)
        flags |= CAN_EXT_TX_FAILED;

    return flags;
}

uint8_t ExtOf(CanRecordKind kind, uint8_t flags = 0) {
    return static_cast<uint8_t>((static_cast<uint8_t>(kind) << CAN_EXT_KIND_SHIFT) | flags);
}

/** @brief The head, ext and dt every record but a plain frame starts with. */
void PutExtHeader(Cursor& cursor, CanRecordKind kind, uint64_t dt_us) {
    cursor.Put(CAN_HEAD_EXT);
    cursor.Put(ExtOf(kind));
    cursor.PutLeb128(dt_us);
}

} // namespace

size_t EncodeNotificationHeader(std::span<uint8_t> out, uint8_t port, uint16_t seq, uint64_t base_us, uint8_t count) {
    if(out.size() < CAN_NOTIFICATION_HEADER_SIZE)
        return 0;

    out[0] = CAN_RECORD_VERSION;
    out[1] = port;
    sys_put_le16(seq, &out[2]);
    sys_put_le64(base_us, &out[4]);
    out[12] = count;

    return CAN_NOTIFICATION_HEADER_SIZE;
}

size_t EncodeFrame(std::span<uint8_t> out, uint64_t dt_us, const CanRecordFrame& frame) {
    if(!IsWellFormed(frame))
        return 0;

    const bool with_ext = HasFlags(frame);

    Cursor cursor(out);
    cursor.Put(HeadOf(frame, with_ext));
    if(with_ext)
        cursor.Put(ExtOf(CanRecordKind::FRAME, FlagsOf(frame)));
    cursor.PutLeb128(dt_us);
    cursor.PutId(frame.id, frame.ide);
    cursor.Put(frame.payload);

    return cursor.Finish();
}

size_t FrameRecordSize(uint64_t dt_us, const CanRecordFrame& frame) {
    if(!IsWellFormed(frame))
        return 0;

    return 1 + (HasFlags(frame) ? 1 : 0) + Leb128Size(dt_us) + (frame.ide ? 4 : 2) + frame.payload.size();
}

size_t EncodeChangedBytes(
    std::span<uint8_t> out,
    uint64_t dt_us,
    const CanRecordFrame& frame,
    std::span<const uint8_t> previous_payload
) {
    if(!IsWellFormed(frame) || frame.rtr || previous_payload.size() != frame.payload.size())
        return 0;

    // A mask byte for each 8 payload bytes: bit i of mask j is payload byte 8j + i.
    std::array<uint8_t, CAN_MAX_PAYLOAD_SIZE / 8> masks{};
    const size_t mask_count = (frame.payload.size() + 7) / 8;
    for(size_t i = 0; i < frame.payload.size(); ++i) {
        if(frame.payload[i] != previous_payload[i])
            masks[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
    }

    Cursor cursor(out);
    cursor.Put(HeadOf(frame, true));
    cursor.Put(ExtOf(CanRecordKind::CHANGED_BYTES, FlagsOf(frame)));
    cursor.PutLeb128(dt_us);
    cursor.PutId(frame.id, frame.ide);
    cursor.Put(std::span<const uint8_t>(masks).first(mask_count));
    for(size_t i = 0; i < frame.payload.size(); ++i) {
        if(frame.payload[i] != previous_payload[i])
            cursor.Put(frame.payload[i]);
    }

    return cursor.Finish();
}

size_t EncodeState(
    std::span<uint8_t> out,
    uint64_t dt_us,
    CanBusState state,
    uint8_t tec,
    uint8_t rec,
    uint64_t detected_bitrate
) {
    Cursor cursor(out);
    PutExtHeader(cursor, CanRecordKind::STATE, dt_us);
    cursor.Put(static_cast<uint8_t>(state));
    cursor.Put(tec);
    cursor.Put(rec);
    cursor.PutLeb128(detected_bitrate);

    return cursor.Finish();
}

size_t EncodeStatistics(std::span<uint8_t> out, uint64_t dt_us, const CanStatistics& statistics) {
    Cursor cursor(out);
    PutExtHeader(cursor, CanRecordKind::STATISTICS, dt_us);
    cursor.PutLeb128(statistics.frames_received);
    cursor.PutLeb128(statistics.frames_sent);
    cursor.PutLeb128(statistics.busy_us);
    for(const uint64_t value : statistics.errors)
        cursor.PutLeb128(value);
    for(const uint64_t value : statistics.lost)
        cursor.PutLeb128(value);
    cursor.PutLeb128(statistics.filtered);

    return cursor.Finish();
}

size_t EncodeGap(std::span<uint8_t> out, uint64_t dt_us, uint64_t frames_lost, CanGapCause cause) {
    Cursor cursor(out);
    PutExtHeader(cursor, CanRecordKind::GAP, dt_us);
    cursor.PutLeb128(frames_lost);
    cursor.Put(static_cast<uint8_t>(cause));

    return cursor.Finish();
}

size_t EncodeEnd(std::span<uint8_t> out, uint64_t dt_us, CanEndReason reason) {
    Cursor cursor(out);
    PutExtHeader(cursor, CanRecordKind::END, dt_us);
    cursor.Put(static_cast<uint8_t>(reason));

    return cursor.Finish();
}

size_t EncodeTxEvent(std::span<uint8_t> out, uint64_t dt_us, CanTxEvent event, uint64_t count) {
    Cursor cursor(out);
    PutExtHeader(cursor, CanRecordKind::TX_EVENT, dt_us);
    cursor.Put(static_cast<uint8_t>(event));
    cursor.PutLeb128(count);

    return cursor.Finish();
}

size_t EncodeTap(std::span<uint8_t> out, uint64_t dt_us, CanTapMark mark) {
    Cursor cursor(out);
    PutExtHeader(cursor, CanRecordKind::TAP, dt_us);
    cursor.Put(static_cast<uint8_t>(mark));

    return cursor.Finish();
}

CanRecordWriter::CanRecordWriter(ByteRing& ring, uint64_t start_time_us, CanDeltaTable* delta) :
    ring_(ring),
    delta_(delta),
    last_time_us_(start_time_us) {}

uint64_t CanRecordWriter::DeltaSince(uint64_t time_us) const {
    return time_us > last_time_us_ ? time_us - last_time_us_ : 0;
}

void CanRecordWriter::CountLost(CanGapCause cause, uint64_t count) {
    const size_t index = static_cast<size_t>(cause) - 1;

    pending_[index] += count;
    lost_[index].fetch_add(static_cast<uint32_t>(count), std::memory_order_relaxed);
}

void CanRecordWriter::ResetDelta() {
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    if(delta_ != nullptr)
        delta_->Reset();
#endif
}

bool CanRecordWriter::WriteRecord(std::span<const uint8_t> record, uint64_t dt_us) {
    if(!ring_.TryWrite(record))
        return false;

    last_time_us_ += dt_us;

    return true;
}

void CanRecordWriter::AddLoss(CanGapCause cause, uint64_t count) {
    if(count != 0)
        CountLost(cause, count);
}

void CanRecordWriter::AddPendingGap(CanGapCause cause, uint64_t count) {
    // The cause comes from a parsed record, which the reader does not check.
    const size_t index = static_cast<size_t>(cause) - 1;
    if(index < pending_.size())
        pending_[index] += count;
}

bool CanRecordWriter::FlushLosses() {
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};

    for(size_t index = 0; index < pending_.size(); ++index) {
        if(pending_[index] == 0)
            continue;

        const size_t size = EncodeGap(buffer, 0, pending_[index], static_cast<CanGapCause>(index + 1));
        if(!WriteRecord(std::span<const uint8_t>(buffer).first(size), 0))
            return false;

        pending_[index] = 0;
        ResetDelta();
    }

    return true;
}

bool CanRecordWriter::WriteFrame(uint64_t time_us, const CanRecordFrame& frame) {
    if(!IsWellFormed(frame)) {
        frames_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // The gaps go first, and with them the table is empty again when the frame is encoded.
    if(!FlushLosses()) {
        CountLost(CanGapCause::RING, 1);
        ResetDelta();
        return false;
    }

    const uint64_t dt_us = DeltaSince(time_us);

    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    size_t size = 0;
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    if(delta_ != nullptr)
        size = delta_->Encode(buffer, dt_us, frame);
    else
#endif
        size = EncodeFrame(buffer, dt_us, frame);

    if(size == 0) {
        frames_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if(!WriteRecord(std::span<const uint8_t>(buffer).first(size), dt_us)) {
        // The table took the frame the app never gets; the gap that follows empties it again, and
        // emptying it now keeps it empty until then.
        CountLost(CanGapCause::RING, 1);
        ResetDelta();
        return false;
    }

    frames_written_.fetch_add(1, std::memory_order_relaxed);

    return true;
}

bool CanRecordWriter::WriteState(
    uint64_t time_us,
    CanBusState state,
    uint8_t tec,
    uint8_t rec,
    uint64_t detected_bitrate
) {
    if(!FlushLosses()) {
        CountLost(CanGapCause::RING, 1);
        return false;
    }

    const uint64_t dt_us = DeltaSince(time_us);

    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    const size_t size = EncodeState(buffer, dt_us, state, tec, rec, detected_bitrate);
    if(!WriteRecord(std::span<const uint8_t>(buffer).first(size), dt_us)) {
        CountLost(CanGapCause::RING, 1);
        return false;
    }

    return true;
}

bool CanRecordWriter::WriteTap(uint64_t time_us, CanTapMark mark) {
    // Empty at once: whether the record fits or not, no frame may be sent against what came before.
    ResetDelta();

    if(!FlushLosses()) {
        CountLost(CanGapCause::RING, 1);
        return false;
    }

    const uint64_t dt_us = DeltaSince(time_us);

    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    const size_t size = EncodeTap(buffer, dt_us, mark);
    if(!WriteRecord(std::span<const uint8_t>(buffer).first(size), dt_us)) {
        CountLost(CanGapCause::RING, 1);
        return false;
    }

    return true;
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities
