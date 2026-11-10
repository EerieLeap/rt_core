#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "can_monitor_test_utils.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_monitor_record_codec, NULL, NULL, NULL, NULL, NULL);

using RecordBuffer = std::array<uint8_t, CAN_MAX_RECORD_SIZE>;

static void ExpectBytes(const RecordBuffer& buffer, size_t size, std::initializer_list<uint8_t> expected) {
    zassert_equal(size, expected.size());

    size_t i = 0;
    for(const uint8_t byte : expected) {
        zassert_equal(buffer[i], byte, "byte %zu is 0x%02x, expected 0x%02x", i, buffer[i], byte);
        ++i;
    }
}

static std::span<const uint8_t> Record(const RecordBuffer& buffer, size_t size) {
    return std::span<const uint8_t>(buffer).first(size);
}

static bool SameBytes(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    return std::ranges::equal(a, b);
}

// Frames.

ZTEST(can_monitor_record_codec, test_ClassicFrameBytes) {
    const std::array<uint8_t, 3> payload = { 1, 2, 3 };
    const CanRecordFrame frame{ .id = 0x123, .dlc = 3, .payload = payload };

    RecordBuffer buffer{};
    const size_t size = EncodeFrame(buffer, 5, frame);

    // head (DLC 3), dt, ID (LE), payload: no ext byte.
    ExpectBytes(buffer, size, { 0x03, 0x05, 0x23, 0x01, 1, 2, 3 });
    zassert_equal(FrameRecordSize(5, frame), size);
    zassert_equal(RecordSize(Record(buffer, size)), size);

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::FRAME);
    zassert_equal(record->dt_us, 5U);
    zassert_equal(record->id, 0x123U);
    zassert_equal(record->dlc, 3U);
    zassert_false(
        record->ide || record->fdf || record->brs || record->rtr || record->esi || record->tx || record->tx_failed
    );
    zassert_true(SameBytes(record->payload, payload));
    zassert_equal(record->size, size);
}

ZTEST(can_monitor_record_codec, test_ExtendedIdFrameBytes) {
    const auto payload = MakePayload(8);
    const CanRecordFrame frame{ .id = 0x1ABCDEF0, .dlc = 8, .ide = true, .payload = payload };

    RecordBuffer buffer{};
    const size_t size = EncodeFrame(buffer, 300, frame);

    // IDE is bit 4 of the head; 300 is 0xAC 0x02; the ID takes four bytes.
    zassert_equal(size, 1U + 2U + 4U + 8U);
    zassert_equal(buffer[0], 0x18U);
    zassert_equal(buffer[1], 0xACU);
    zassert_equal(buffer[2], 0x02U);
    zassert_equal(sys_get_le32(&buffer[3]), 0x1ABCDEF0U);

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->ide);
    zassert_equal(record->id, 0x1ABCDEF0U);
    zassert_equal(record->dt_us, 300U);
    zassert_true(SameBytes(record->payload, payload));
}

ZTEST(can_monitor_record_codec, test_EveryDlcClassicAndFd) {
    for(const bool fdf : { false, true }) {
        for(const bool ide : { false, true }) {
            for(uint8_t dlc = 0; dlc <= 15; ++dlc) {
                const size_t length = fdf ? FD_LENGTHS[dlc] : std::min<size_t>(FD_LENGTHS[dlc], 8);
                const auto payload = MakePayload(length, dlc);
                const CanRecordFrame frame{
                    .id = ide ? 0x15555555U : 0x555U,
                    .dlc = dlc,
                    .ide = ide,
                    .fdf = fdf,
                    .brs = fdf,
                    .payload = payload,
                };

                zassert_equal(PayloadLength(dlc, fdf, false), length);

                RecordBuffer buffer{};
                const size_t size = EncodeFrame(buffer, 1, frame);
                zassert_equal(size, 1U + 1U + (ide ? 4U : 2U) + length, "dlc %u fdf %d ide %d", dlc, fdf, ide);

                const auto record = ParseRecord(Record(buffer, size));
                zassert_true(record.has_value());
                zassert_equal(record->dlc, dlc);
                zassert_equal(record->fdf, fdf);
                zassert_equal(record->brs, fdf);
                zassert_equal(record->ide, ide);
                zassert_equal(record->id, frame.id);
                zassert_true(SameBytes(record->payload, payload));

                // A payload that isn't the DLC's length is no frame.
                const auto longer = MakePayload(length + 1);
                CanRecordFrame wrong = frame;
                wrong.payload = longer;
                zassert_equal(EncodeFrame(buffer, 1, wrong), 0U);
                zassert_equal(FrameRecordSize(1, wrong), 0U);

                if(length > 0) {
                    wrong.payload = std::span<const uint8_t>(payload).first(length - 1);
                    zassert_equal(EncodeFrame(buffer, 1, wrong), 0U);
                }
            }
        }
    }
}

ZTEST(can_monitor_record_codec, test_ClassicDlcAboveEightCarriesEightBytes) {
    for(uint8_t dlc = 9; dlc <= 15; ++dlc)
        zassert_equal(PayloadLength(dlc, false, false), 8U);

    zassert_equal(PayloadLength(9, true, false), 12U);
    zassert_equal(PayloadLength(15, true, false), 64U);
}

ZTEST(can_monitor_record_codec, test_FdFrameHeadBits) {
    const auto payload = MakePayload(64);
    const CanRecordFrame frame{ .id = 0x7FF, .dlc = 15, .fdf = true, .brs = true, .payload = payload };

    RecordBuffer buffer{};
    const size_t size = EncodeFrame(buffer, 0, frame);

    zassert_equal(size, 1U + 1U + 2U + 64U);
    zassert_equal(buffer[0], 0x6FU);
}

ZTEST(can_monitor_record_codec, test_RemoteFrameBytes) {
    const CanRecordFrame frame{ .id = 0x0A5, .dlc = 4, .rtr = true };

    RecordBuffer buffer{};
    const size_t size = EncodeFrame(buffer, 9, frame);

    // The ext byte (RTR) follows the head; no payload.
    ExpectBytes(buffer, size, { 0x84, 0x01, 0x09, 0xA5, 0x00 });

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->rtr);
    zassert_equal(record->dlc, 4U);
    zassert_true(record->payload.empty());

    // A remote frame has no payload whatever its DLC.
    const auto payload = MakePayload(4);
    CanRecordFrame with_payload = frame;
    with_payload.payload = payload;
    zassert_equal(EncodeFrame(buffer, 9, with_payload), 0U);
}

ZTEST(can_monitor_record_codec, test_FrameFlagsEveryCombination) {
    for(unsigned flags = 0; flags < 16; ++flags) {
        const bool rtr = (flags & 1U) != 0;
        const auto payload = MakePayload(rtr ? 0 : 2);
        const CanRecordFrame frame{
            .id = 0x10,
            .dlc = 2,
            .fdf = true,
            .rtr = rtr,
            .esi = (flags & 2U) != 0,
            .tx = (flags & 4U) != 0,
            .tx_failed = (flags & 8U) != 0,
            .payload = payload,
        };

        RecordBuffer buffer{};
        const size_t size = EncodeFrame(buffer, 3, frame);
        zassert_true(size > 0);

        if(flags == 0) {
            zassert_equal(buffer[0] & CAN_HEAD_EXT, 0);
        } else {
            zassert_equal(buffer[0] & CAN_HEAD_EXT, CAN_HEAD_EXT);
            // Kind 0 in the upper nibble, the flags in the lower.
            zassert_equal(buffer[1], flags);
        }

        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_true(record->kind == CanRecordKind::FRAME);
        zassert_equal(record->rtr, frame.rtr);
        zassert_equal(record->esi, frame.esi);
        zassert_equal(record->tx, frame.tx);
        zassert_equal(record->tx_failed, frame.tx_failed);
        zassert_equal(record->size, size);
    }
}

ZTEST(can_monitor_record_codec, test_MalformedFramesAreNotEncoded) {
    RecordBuffer buffer{};
    const auto payload = MakePayload(8);

    zassert_equal(EncodeFrame(buffer, 0, { .id = 1, .dlc = 16, .payload = payload }), 0U);
    zassert_equal(EncodeFrame(buffer, 0, { .id = 0x800, .dlc = 8, .payload = payload }), 0U);
    zassert_equal(EncodeFrame(buffer, 0, { .id = 0x20000000, .dlc = 8, .ide = true, .payload = payload }), 0U);
    zassert_true(EncodeFrame(buffer, 0, { .id = 0x1FFFFFFF, .dlc = 8, .ide = true, .payload = payload }) > 0);
}

ZTEST(can_monitor_record_codec, test_EncodersFailWhenTheOutIsTooSmall) {
    const auto payload = MakePayload(12);
    const CanRecordFrame frame{ .id = 0x77, .dlc = 9, .fdf = true, .esi = true, .payload = payload };

    RecordBuffer buffer{};
    const size_t size = EncodeFrame(buffer, 1000, frame);
    zassert_true(size > 0);

    for(size_t room = 0; room < size; ++room)
        zassert_equal(EncodeFrame(std::span<uint8_t>(buffer).first(room), 1000, frame), 0U);
    zassert_equal(EncodeFrame(std::span<uint8_t>(buffer).first(size), 1000, frame), size);

    CanStatistics statistics;
    statistics.busy_us = UINT64_MAX;
    const size_t statistics_size = EncodeStatistics(buffer, 1000, statistics);
    zassert_true(statistics_size > 0);
    for(size_t room = 0; room < statistics_size; ++room)
        zassert_equal(EncodeStatistics(std::span<uint8_t>(buffer).first(room), 1000, statistics), 0U);

    for(size_t room = 0; room < 4; ++room) {
        zassert_equal(EncodeGap(std::span<uint8_t>(buffer).first(room), 0, 1, CanGapCause::RING), 0U);
        zassert_equal(EncodeEnd(std::span<uint8_t>(buffer).first(room), 0, CanEndReason::CLOSED), 0U);
        zassert_equal(EncodeTap(std::span<uint8_t>(buffer).first(room), 0, CanTapMark::MAY_HAVE_MISSED), 0U);
    }
}

ZTEST(can_monitor_record_codec, test_DeltaTimesAtTheirLimits) {
    const auto payload = MakePayload(1);

    for(const uint64_t dt : std::initializer_list<uint64_t>{ 0, 127, 128, uint64_t{ 1 } << 63, UINT64_MAX }) {
        RecordBuffer buffer{};
        const size_t size = EncodeFrame(buffer, dt, { .id = 1, .dlc = 1, .payload = payload });
        zassert_equal(size, 1U + Leb128Size(dt) + 2U + 1U);

        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(record->dt_us, dt);
    }

    RecordBuffer buffer{};
    zassert_equal(EncodeFrame(buffer, UINT64_MAX, { .id = 1, .dlc = 1, .payload = payload }), 14U);
}

// Changed bytes.

ZTEST(can_monitor_record_codec, test_ChangedBytesBytes) {
    const auto previous = MakePayload(12);
    auto current = previous;
    current[1] = 0xEE;
    current[9] = 0xDD;

    const CanRecordFrame frame{ .id = 0x100, .dlc = 9, .fdf = true, .brs = true, .payload = current };

    RecordBuffer buffer{};
    const size_t size = EncodeChangedBytes(buffer, 1, frame, previous);

    // head with EXT, ext (kind 1), dt, ID, two mask bytes (payload bytes 1 and 9), the two bytes.
    ExpectBytes(buffer, size, { 0xE9, 0x10, 0x01, 0x00, 0x01, 0x02, 0x02, 0xEE, 0xDD });

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::CHANGED_BYTES);
    zassert_equal(record->id, 0x100U);
    zassert_equal(record->dlc, 9U);
    zassert_true(record->fdf && record->brs);
    zassert_equal(record->masks.size(), 2U);
    zassert_equal(record->payload.size(), 2U);
    zassert_equal(record->payload[0], 0xEE);
    zassert_equal(record->payload[1], 0xDD);
}

ZTEST(can_monitor_record_codec, test_ChangedBytesEveryLength) {
    for(const size_t length : FD_LENGTHS) {
        if(length == 0)
            continue;

        const auto previous = MakePayload(length, 3);
        auto current = previous;
        // The first and the last byte change.
        current.front() ^= 0xFF;
        current.back() ^= 0x0F;

        uint8_t dlc = 0;
        while(FD_LENGTHS[dlc] != length)
            ++dlc;

        RecordBuffer buffer{};
        const size_t size = EncodeChangedBytes(
            buffer, 2, { .id = 0x3, .dlc = dlc, .fdf = true, .payload = current }, previous
        );
        const size_t masks = (length + 7) / 8;
        const size_t changed = length == 1 ? 1 : 2;
        zassert_equal(size, 2U + 1U + 2U + masks + changed, "length %zu", length);

        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(record->masks.size(), masks);
        zassert_equal(record->payload.size(), changed);
        zassert_equal(record->masks[0] & 1U, 1U);
        const size_t last = length - 1;
        zassert_equal((record->masks[last / 8] >> (last % 8)) & 1U, 1U);
    }
}

ZTEST(can_monitor_record_codec, test_ChangedBytesAllAndNone) {
    const auto previous = MakePayload(64);
    std::vector<uint8_t> all(64);
    for(size_t i = 0; i < 64; ++i)
        all[i] = static_cast<uint8_t>(previous[i] + 1);

    RecordBuffer buffer{};
    size_t size = EncodeChangedBytes(buffer, 0, { .id = 1, .dlc = 15, .fdf = true, .payload = all }, previous);
    zassert_equal(size, 2U + 1U + 2U + 8U + 64U);

    auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    for(const uint8_t mask : record->masks)
        zassert_equal(mask, 0xFFU);

    size = EncodeChangedBytes(buffer, 0, { .id = 1, .dlc = 15, .fdf = true, .payload = previous }, previous);
    zassert_equal(size, 2U + 1U + 2U + 8U);
    record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->payload.empty());
}

ZTEST(can_monitor_record_codec, test_ChangedBytesKeepTheFrameFlags) {
    const auto previous = MakePayload(8);
    auto current = previous;
    current[7] = 0x42;

    RecordBuffer buffer{};
    const size_t size = EncodeChangedBytes(
        buffer, 4,
        { .id = 0x1FFFFFFF, .dlc = 8, .ide = true, .esi = true, .tx = true, .tx_failed = true, .payload = current },
        previous
    );
    zassert_true(size > 0);
    zassert_equal(buffer[0], 0x98U);
    zassert_equal(buffer[1], 0x1EU);

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->esi && record->tx && record->tx_failed);
    zassert_false(record->rtr);
    zassert_true(record->ide);
    zassert_equal(record->id, 0x1FFFFFFFU);
}

ZTEST(can_monitor_record_codec, test_ChangedBytesRefusesWhatItCannotDiff) {
    const auto previous = MakePayload(8);
    const auto shorter = MakePayload(4);
    RecordBuffer buffer{};

    zassert_equal(EncodeChangedBytes(buffer, 0, { .id = 1, .dlc = 4, .payload = shorter }, previous), 0U);
    zassert_equal(EncodeChangedBytes(buffer, 0, { .id = 1, .dlc = 8, .rtr = true }, {}), 0U);
}

// The other kinds.

ZTEST(can_monitor_record_codec, test_StateBytes) {
    RecordBuffer buffer{};
    size_t size = EncodeState(buffer, 0x10, CanBusState::ERROR_PASSIVE, 130, 7, 500000);

    ExpectBytes(buffer, size, { 0x80, 0x20, 0x10, 2, 130, 7, 0xA0, 0xC2, 0x1E });

    auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::STATE);
    zassert_true(record->state == CanBusState::ERROR_PASSIVE);
    zassert_equal(record->tec, 130U);
    zassert_equal(record->rec, 7U);
    zassert_equal(record->detected_bitrate, 500000U);
    zassert_equal(record->dt_us, 0x10U);

    size = EncodeState(buffer, 0, CanBusState::BUS_OFF, 255, 0, 0);
    ExpectBytes(buffer, size, { 0x80, 0x20, 0x00, 3, 255, 0, 0 });

    for(uint8_t state = 0; state <= 4; ++state) {
        size = EncodeState(buffer, 1, static_cast<CanBusState>(state), 1, 2, 1000000);
        record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(static_cast<uint8_t>(record->state), state);
        zassert_equal(record->detected_bitrate, 1000000U);
    }
}

ZTEST(can_monitor_record_codec, test_ZephyrStatesMapToTheWireCodes) {
    zassert_true(ToCanBusState(CAN_STATE_ERROR_ACTIVE) == CanBusState::ERROR_ACTIVE);
    zassert_true(ToCanBusState(CAN_STATE_ERROR_WARNING) == CanBusState::ERROR_WARNING);
    zassert_true(ToCanBusState(CAN_STATE_ERROR_PASSIVE) == CanBusState::ERROR_PASSIVE);
    zassert_true(ToCanBusState(CAN_STATE_BUS_OFF) == CanBusState::BUS_OFF);
    zassert_true(ToCanBusState(CAN_STATE_STOPPED) == CanBusState::STOPPED);
    zassert_equal(static_cast<uint8_t>(CanBusState::ERROR_ACTIVE), 0U);
    zassert_equal(static_cast<uint8_t>(CanBusState::STOPPED), 4U);
}

ZTEST(can_monitor_record_codec, test_StatisticsKeepTheirOrder) {
    CanStatistics statistics;
    statistics.frames_received = 1;
    statistics.frames_sent = 2;
    statistics.busy_us = 3;
    statistics.errors = { 4, 5, 6, 7, 8 };
    statistics.lost = { 9, 10, 11, 12 };
    statistics.filtered = 13;

    RecordBuffer buffer{};
    const size_t size = EncodeStatistics(buffer, 0, statistics);
    ExpectBytes(buffer, size, { 0x80, 0x30, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 });

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::STATISTICS);
    zassert_true(record->statistics == statistics);
    zassert_equal(record->statistics.errors[CanStatistics::ERROR_ACK], 8U);
    zassert_equal(record->statistics.lost[CanStatistics::LOST_ECHOES], 12U);
}

ZTEST(can_monitor_record_codec, test_LargestRecordIsTheMaxRecordSize) {
    CanStatistics statistics;
    statistics.frames_received = UINT64_MAX;
    statistics.frames_sent = UINT64_MAX;
    statistics.busy_us = UINT64_MAX;
    statistics.errors.fill(UINT64_MAX);
    statistics.lost.fill(UINT64_MAX);
    statistics.filtered = UINT64_MAX;

    RecordBuffer buffer{};
    const size_t size = EncodeStatistics(buffer, UINT64_MAX, statistics);
    zassert_equal(size, CAN_MAX_RECORD_SIZE);

    const auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->statistics == statistics);
    zassert_equal(record->dt_us, UINT64_MAX);

    // The biggest frame is smaller.
    const auto previous = MakePayload(64);
    std::vector<uint8_t> current(64, 0xAA);
    const size_t frame_size = EncodeChangedBytes(
        buffer, UINT64_MAX, { .id = 0x1FFFFFFF, .dlc = 15, .ide = true, .fdf = true, .esi = true, .payload = current },
        previous
    );
    zassert_true(frame_size > 0 && frame_size < CAN_MAX_RECORD_SIZE);
}

ZTEST(can_monitor_record_codec, test_GapEndTxEventAndTapBytes) {
    RecordBuffer buffer{};
    size_t size = EncodeGap(buffer, 2, 300, CanGapCause::CONTROLLER);
    ExpectBytes(buffer, size, { 0x80, 0x40, 0x02, 0xAC, 0x02, 3 });

    auto record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::GAP);
    zassert_equal(record->frames_lost, 300U);
    zassert_true(record->gap_cause == CanGapCause::CONTROLLER);

    size = EncodeEnd(buffer, 0, CanEndReason::CONFIGURATION_STORED);
    ExpectBytes(buffer, size, { 0x80, 0x50, 0x00, 4 });
    record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::END);
    zassert_true(record->end_reason == CanEndReason::CONFIGURATION_STORED);

    size = EncodeTxEvent(buffer, 0, CanTxEvent::REPLAY_UNDERRUN, 12);
    ExpectBytes(buffer, size, { 0x80, 0x60, 0x00, 5, 12 });
    record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::TX_EVENT);
    zassert_true(record->tx_event == CanTxEvent::REPLAY_UNDERRUN);
    zassert_equal(record->tx_count, 12U);

    size = EncodeTap(buffer, 1, CanTapMark::MAY_HAVE_MISSED);
    ExpectBytes(buffer, size, { 0x80, 0x70, 0x01, 2 });
    record = ParseRecord(Record(buffer, size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::TAP);
    zassert_true(record->tap_mark == CanTapMark::MAY_HAVE_MISSED);
}

ZTEST(can_monitor_record_codec, test_EveryEnumValueRoundTrips) {
    RecordBuffer buffer{};

    for(uint8_t cause = 1; cause <= 4; ++cause) {
        const size_t size = EncodeGap(buffer, 0, UINT64_MAX, static_cast<CanGapCause>(cause));
        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(static_cast<uint8_t>(record->gap_cause), cause);
        zassert_equal(record->frames_lost, UINT64_MAX);
    }

    for(uint8_t reason = 1; reason <= 5; ++reason) {
        const size_t size = EncodeEnd(buffer, 0, static_cast<CanEndReason>(reason));
        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(static_cast<uint8_t>(record->end_reason), reason);
    }

    for(uint8_t event = 1; event <= 5; ++event) {
        const size_t size = EncodeTxEvent(buffer, 0, static_cast<CanTxEvent>(event), UINT64_MAX);
        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(static_cast<uint8_t>(record->tx_event), event);
        zassert_equal(record->tx_count, UINT64_MAX);
    }

    for(uint8_t mark = 1; mark <= 2; ++mark) {
        const size_t size = EncodeTap(buffer, 0, static_cast<CanTapMark>(mark));
        const auto record = ParseRecord(Record(buffer, size));
        zassert_true(record.has_value());
        zassert_equal(static_cast<uint8_t>(record->tap_mark), mark);
    }
}

ZTEST(can_monitor_record_codec, test_NotificationHeaderBytes) {
    std::array<uint8_t, CAN_NOTIFICATION_HEADER_SIZE> header{};

    const size_t size = EncodeNotificationHeader(header, CAN_SYNTHETIC_PORT, 0x0102, 0x1122334455667788ULL, 17);
    zassert_equal(size, 13U);

    const std::array<uint8_t, 13> expected = {
        1,
        255,
        0x02,
        0x01,
        0x88,
        0x77,
        0x66,
        0x55,
        0x44,
        0x33,
        0x22,
        0x11,
        17,
    };
    zassert_true(SameBytes(header, expected));

    zassert_equal(EncodeNotificationHeader(std::span<uint8_t>(header).first(12), 0, 0, 0, 0), 0U);
}

// The reader.

static std::vector<std::vector<uint8_t>> SampleRecords() {
    std::vector<std::vector<uint8_t>> samples;
    RecordBuffer buffer{};

    const auto classic = MakePayload(8);
    const auto fd = MakePayload(64);
    const auto previous = MakePayload(64, 9);

    auto add = [&](size_t size) {
        zassert_true(size > 0);
        samples.emplace_back(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(size));
    };

    add(EncodeFrame(buffer, 1000, { .id = 0x123, .dlc = 8, .payload = classic }));
    add(EncodeFrame(
        buffer, 1,
        { .id = 0x1FFFFFFF, .dlc = 15, .ide = true, .fdf = true, .brs = true, .esi = true, .tx = true, .payload = fd }
    ));
    add(EncodeFrame(buffer, UINT64_MAX, { .id = 0x7FF, .dlc = 3, .rtr = true }));
    add(EncodeChangedBytes(buffer, 300, { .id = 0x55, .dlc = 15, .fdf = true, .payload = fd }, previous));
    add(EncodeState(buffer, 4, CanBusState::BUS_OFF, 1, 2, 500000));

    CanStatistics statistics;
    statistics.frames_received = UINT64_MAX;
    statistics.errors = { 1, 200, 30000, 4000000, 500000000000ULL };
    statistics.filtered = 1ULL << 40;
    add(EncodeStatistics(buffer, 0, statistics));

    add(EncodeGap(buffer, 0, 1000, CanGapCause::RING));
    add(EncodeEnd(buffer, 0, CanEndReason::LEASE));
    add(EncodeTxEvent(buffer, 0, CanTxEvent::FRAMES_SKIPPED, 70000));
    add(EncodeTap(buffer, 0, CanTapMark::MAY_HAVE_REPEATED));

    return samples;
}

ZTEST(can_monitor_record_codec, test_ARecordThatEndsEarlyIsIncomplete) {
    for(const auto& sample : SampleRecords()) {
        const std::span<const uint8_t> bytes(sample);

        const auto whole = InspectRecord(bytes);
        zassert_true(whole.status == CanRecordStatus::OK);
        zassert_equal(whole.size, sample.size());

        for(size_t length = 0; length < sample.size(); ++length) {
            const auto cut = InspectRecord(bytes.first(length));
            zassert_true(cut.status == CanRecordStatus::INCOMPLETE, "%zu of %zu bytes", length, sample.size());
            zassert_equal(RecordSize(bytes.first(length)), 0U);
            zassert_false(ParseRecord(bytes.first(length)).has_value());
        }
    }
}

ZTEST(can_monitor_record_codec, test_RecordSizeStopsAtTheFirstRecord) {
    const auto samples = SampleRecords();

    std::vector<uint8_t> stream;
    for(const auto& sample : samples)
        stream.insert(stream.end(), sample.begin(), sample.end());

    size_t pos = 0;
    for(const auto& sample : samples) {
        const auto rest = std::span<const uint8_t>(stream).subspan(pos);
        zassert_equal(RecordSize(rest), sample.size());
        pos += sample.size();
    }
    zassert_equal(pos, stream.size());
    zassert_equal(RecordSize(std::span<const uint8_t>()), 0U);
}

ZTEST(can_monitor_record_codec, test_InspectionGivesKindAndDt) {
    const auto samples = SampleRecords();

    const auto first = InspectRecord(samples[0]);
    zassert_true(first.kind == CanRecordKind::FRAME);
    zassert_equal(first.dt_us, 1000U);

    const auto changed = InspectRecord(samples[3]);
    zassert_true(changed.kind == CanRecordKind::CHANGED_BYTES);
    zassert_equal(changed.dt_us, 300U);

    const auto remote = InspectRecord(samples[2]);
    zassert_equal(remote.dt_us, UINT64_MAX);
}

static void ExpectMalformed(std::initializer_list<uint8_t> bytes, const char* what) {
    const std::vector<uint8_t> record(bytes);

    zassert_true(InspectRecord(record).status == CanRecordStatus::MALFORMED, "%s", what);
    zassert_equal(RecordSize(record), 0U, "%s", what);
    zassert_false(ParseRecord(record).has_value(), "%s", what);
}

ZTEST(can_monitor_record_codec, test_MalformedRecordsAreRefused) {
    ExpectMalformed({ 0x80, 0x80, 0x00 }, "kind 8");
    ExpectMalformed({ 0x80, 0xF0, 0x00 }, "kind 15");
    ExpectMalformed({ 0x81, 0x40, 0x00, 0x01, 0x01 }, "a gap with a DLC in its head");
    ExpectMalformed({ 0x90, 0x40, 0x00, 0x01, 0x01 }, "a gap with IDE in its head");
    ExpectMalformed({ 0x80, 0x71, 0x00, 0x01 }, "a tap with RTR");
    ExpectMalformed({ 0x80, 0x24, 0x00, 0, 0, 0, 0 }, "a state with TX");
    ExpectMalformed({ 0x80, 0x11, 0x00, 0x01, 0x00 }, "a remote changed-bytes frame");
    ExpectMalformed({ 0x01, 0x00, 0x00, 0x08, 0xAA }, "a standard ID above 11 bits");
    ExpectMalformed({ 0x10, 0x00, 0xFF, 0xFF, 0xFF, 0x20 }, "an extended ID above 29 bits");
    ExpectMalformed({ 0x81, 0x10, 0x00, 0x01, 0x00, 0x02, 0xAA }, "a mask that names byte 1 of 1");
    ExpectMalformed({ 0xA9, 0x10, 0x00, 0x01, 0x00, 0x00, 0x10, 0xAA }, "a mask that names byte 12 of 12");
    ExpectMalformed(
        { 0x03, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 0, 0, 1, 2, 3 }, "an 11-byte dt"
    );
}

ZTEST(can_monitor_record_codec, test_EveryKindOfSampleParses) {
    const auto samples = SampleRecords();

    const auto state = ParseRecord(samples[4]);
    zassert_true(state.has_value());
    zassert_true(state->state == CanBusState::BUS_OFF);
    zassert_equal(state->detected_bitrate, 500000U);

    const auto statistics = ParseRecord(samples[5]);
    zassert_true(statistics.has_value());
    zassert_equal(statistics->statistics.frames_received, UINT64_MAX);
    zassert_equal(statistics->statistics.errors[CanStatistics::ERROR_ACK], 500000000000ULL);
    zassert_equal(statistics->statistics.errors[CanStatistics::ERROR_CRC], 30000U);
    zassert_equal(statistics->statistics.filtered, 1ULL << 40);

    const auto fd = ParseRecord(samples[1]);
    zassert_true(fd.has_value());
    zassert_true(fd->ide && fd->fdf && fd->brs && fd->esi && fd->tx);
    zassert_false(fd->tx_failed);
    zassert_equal(fd->payload.size(), 64U);

    const auto tx_event = ParseRecord(samples[8]);
    zassert_true(tx_event.has_value());
    zassert_equal(tx_event->tx_count, 70000U);
}
