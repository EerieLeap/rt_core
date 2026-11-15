#include "domain/can_monitor_domain/utilities/can_record_reader.h"

#include <bit>

#include <zephyr/sys/byteorder.h>

namespace eerie_leap::domain::can_monitor_domain::utilities {

namespace {

/** @brief Reads forward through a span; the first failure sticks, as INCOMPLETE or MALFORMED. */
class Cursor {
private:
    std::span<const uint8_t> in_;
    size_t pos_ = 0;
    CanRecordStatus status_ = CanRecordStatus::OK;

public:
    explicit Cursor(std::span<const uint8_t> in) :
        in_(in) {}

    [[nodiscard]] bool Ok() const {
        return status_ == CanRecordStatus::OK;
    }
    [[nodiscard]] CanRecordStatus Status() const {
        return status_;
    }
    [[nodiscard]] size_t Pos() const {
        return pos_;
    }

    bool Fail(CanRecordStatus status) {
        if(status_ == CanRecordStatus::OK)
            status_ = status;

        return false;
    }

    bool Need(size_t count) {
        if(in_.size() - pos_ < count)
            return Fail(CanRecordStatus::INCOMPLETE);

        return true;
    }

    uint8_t Byte() {
        if(!Need(1))
            return 0;

        return in_[pos_++];
    }

    std::span<const uint8_t> Bytes(size_t count) {
        if(!Need(count))
            return {};

        const auto bytes = in_.subspan(pos_, count);
        pos_ += count;

        return bytes;
    }

    uint64_t Leb128() {
        const Leb128Decoded decoded = DecodeLeb128(in_.subspan(pos_));
        if(!decoded.Ok()) {
            Fail(decoded.status == Leb128Status::TRUNCATED ? CanRecordStatus::INCOMPLETE : CanRecordStatus::MALFORMED);
            return 0;
        }

        pos_ += decoded.size;

        return decoded.value;
    }

    uint32_t Id(bool ide) {
        if(!ide) {
            const auto bytes = Bytes(2);
            if(!Ok())
                return 0;

            const uint32_t id = sys_get_le16(bytes.data());
            if(id > CAN_STD_ID_MASK)
                Fail(CanRecordStatus::MALFORMED);

            return id;
        }

        const auto bytes = Bytes(4);
        if(!Ok())
            return 0;

        const uint32_t id = sys_get_le32(bytes.data());
        if(id > CAN_EXT_ID_MASK)
            Fail(CanRecordStatus::MALFORMED);

        return id;
    }
};

/** @brief The body of kind 0 and 1, after the dt. */
void ParseFrameBody(Cursor& cursor, CanRecord& record) {
    record.id = cursor.Id(record.ide);
    if(!cursor.Ok())
        return;

    if(record.kind == CanRecordKind::FRAME) {
        record.payload = cursor.Bytes(PayloadLength(record.dlc, record.fdf, record.rtr));
        return;
    }

    const size_t length = PayloadLength(record.dlc, record.fdf, false);
    record.masks = cursor.Bytes((length + 7) / 8);
    if(!cursor.Ok())
        return;

    size_t changed = 0;
    for(size_t j = 0; j < record.masks.size(); ++j) {
        changed += static_cast<size_t>(std::popcount(record.masks[j]));

        // A bit past the last payload byte names a byte that isn't there.
        const size_t bits_left = length - j * 8;
        if(bits_left < 8 && (record.masks[j] >> bits_left) != 0) {
            cursor.Fail(CanRecordStatus::MALFORMED);
            return;
        }
    }

    record.payload = cursor.Bytes(changed);
}

void ParseStatistics(Cursor& cursor, CanStatistics& statistics) {
    statistics.frames_received = cursor.Leb128();
    statistics.frames_sent = cursor.Leb128();
    statistics.busy_us = cursor.Leb128();
    for(uint64_t& value : statistics.errors)
        value = cursor.Leb128();
    for(uint64_t& value : statistics.lost)
        value = cursor.Leb128();
    statistics.filtered = cursor.Leb128();
}

CanRecordStatus Parse(std::span<const uint8_t> in, CanRecord& record) {
    Cursor cursor(in);

    const uint8_t head = cursor.Byte();
    const bool with_ext = (head & CAN_HEAD_EXT) != 0;
    const uint8_t ext = with_ext ? cursor.Byte() : 0;
    if(!cursor.Ok())
        return cursor.Status();

    const uint8_t kind = ext >> CAN_EXT_KIND_SHIFT;
    const uint8_t flags = ext & CAN_EXT_FLAGS_MASK;
    if(kind >= CAN_RECORD_KIND_COUNT)
        return CanRecordStatus::MALFORMED;

    record.kind = static_cast<CanRecordKind>(kind);

    // Only a frame has a head of its own, and only a frame has flags.
    if(kind >= static_cast<uint8_t>(CanRecordKind::STATE) && (head != CAN_HEAD_EXT || flags != 0))
        return CanRecordStatus::MALFORMED;

    record.dlc = head & CAN_HEAD_DLC_MASK;
    record.ide = (head & CAN_HEAD_IDE) != 0;
    record.fdf = (head & CAN_HEAD_FDF) != 0;
    record.brs = (head & CAN_HEAD_BRS) != 0;
    record.rtr = (flags & CAN_EXT_RTR) != 0;
    record.esi = (flags & CAN_EXT_ESI) != 0;
    record.tx = (flags & CAN_EXT_TX) != 0;
    record.tx_failed = (flags & CAN_EXT_TX_FAILED) != 0;

    if(record.kind == CanRecordKind::CHANGED_BYTES && record.rtr)
        return CanRecordStatus::MALFORMED;

    record.dt_us = cursor.Leb128();

    switch(record.kind) {
        case CanRecordKind::FRAME:
        case CanRecordKind::CHANGED_BYTES:
            if(cursor.Ok())
                ParseFrameBody(cursor, record);
            break;
        case CanRecordKind::STATE:
            record.state = static_cast<CanBusState>(cursor.Byte());
            record.tec = cursor.Byte();
            record.rec = cursor.Byte();
            record.detected_bitrate = cursor.Leb128();
            break;
        case CanRecordKind::STATISTICS:
            ParseStatistics(cursor, record.statistics);
            break;
        case CanRecordKind::GAP:
            record.frames_lost = cursor.Leb128();
            record.gap_cause = static_cast<CanGapCause>(cursor.Byte());
            break;
        case CanRecordKind::END:
            record.end_reason = static_cast<CanEndReason>(cursor.Byte());
            break;
        case CanRecordKind::TX_EVENT:
            record.tx_event = static_cast<CanTxEvent>(cursor.Byte());
            record.tx_count = cursor.Leb128();
            break;
        case CanRecordKind::TAP:
            record.tap_mark = static_cast<CanTapMark>(cursor.Byte());
            break;
    }

    if(!cursor.Ok())
        return cursor.Status();

    record.size = cursor.Pos();

    return CanRecordStatus::OK;
}

} // namespace

CanRecordInspection InspectRecord(std::span<const uint8_t> in) {
    CanRecord record;
    const CanRecordStatus status = Parse(in, record);

    CanRecordInspection inspection;
    inspection.status = status;
    if(status == CanRecordStatus::OK) {
        inspection.size = record.size;
        inspection.dt_us = record.dt_us;
        inspection.kind = record.kind;
    }

    return inspection;
}

size_t RecordSize(std::span<const uint8_t> in) {
    return InspectRecord(in).size;
}

std::optional<CanRecord> ParseRecord(std::span<const uint8_t> in) {
    CanRecord record;
    if(Parse(in, record) != CanRecordStatus::OK)
        return std::nullopt;

    return record;
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities
