#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "can_monitor_test_utils.h"
#include "domain/can_monitor_domain/utilities/can_notification_cutter.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_monitor_notification_cutter, NULL, NULL, NULL, NULL, NULL);

namespace {

using Bytes = std::vector<uint8_t>;

/** @brief Appends a record of @p size bytes (the writer's output) to @p stream. */
void Append(Bytes& stream, size_t size, const std::array<uint8_t, CAN_MAX_RECORD_SIZE>& buffer) {
    zassert_true(size > 0);
    stream.insert(stream.end(), buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(size));
}

/** @brief A classical frame with @p length data bytes: 1 + dt + 2 + length bytes. */
void AppendFrame(Bytes& stream, uint64_t dt, uint32_t id, uint8_t length) {
    const auto payload = MakePayload(length, static_cast<uint8_t>(id));
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    Append(stream, EncodeFrame(buffer, dt, { .id = id, .dlc = length, .payload = payload }), buffer);
}

} // namespace

ZTEST(can_monitor_notification_cutter, test_ItCutsWholeRecordsOnly) {
    Bytes stream;
    for(uint32_t i = 0; i < 5; ++i)
        AppendFrame(stream, 10 + i, i, 8);
    zassert_equal(stream.size(), 60U);

    // Room for three whole records and a part of a fourth.
    auto cut = CanNotificationCutter::Cut(stream, 40);
    zassert_equal(cut.bytes, 36U);
    zassert_equal(cut.records, 3U);
    zassert_equal(cut.dt_sum_us, 10U + 11U + 12U);
    zassert_false(cut.first_too_large || cut.incomplete_tail || cut.malformed);

    cut = CanNotificationCutter::Cut(stream, 12);
    zassert_equal(cut.bytes, 12U);
    zassert_equal(cut.records, 1U);

    cut = CanNotificationCutter::Cut(stream, 1000);
    zassert_equal(cut.bytes, 60U);
    zassert_equal(cut.records, 5U);
    zassert_equal(cut.dt_sum_us, 10U + 11U + 12U + 13U + 14U);
    zassert_false(cut.first_too_large || cut.incomplete_tail || cut.malformed);
}

ZTEST(can_monitor_notification_cutter, test_ARecordLargerThanTheRoomIsReported) {
    Bytes stream;
    AppendFrame(stream, 77, 1, 8);
    AppendFrame(stream, 5, 2, 8);

    for(const size_t room : { size_t{ 0 }, size_t{ 1 }, size_t{ 11 } }) {
        const auto cut = CanNotificationCutter::Cut(stream, room);
        zassert_equal(cut.bytes, 0U);
        zassert_equal(cut.records, 0U);
        zassert_equal(cut.dt_sum_us, 0U);
        zassert_true(cut.first_too_large);
        zassert_equal(cut.first_record_size, 12U);
        zassert_equal(cut.first_record_dt_us, 77U);
        zassert_false(cut.incomplete_tail);
    }

    // A record that is too large behind others is the next notification's first, not a report.
    const auto cut = CanNotificationCutter::Cut(stream, 20);
    zassert_equal(cut.records, 1U);
    zassert_false(cut.first_too_large);
}

ZTEST(can_monitor_notification_cutter, test_AtMost255Records) {
    Bytes stream;
    for(uint32_t i = 0; i < 300; ++i)
        AppendFrame(stream, 1, i & 0x7FF, 0);
    zassert_equal(stream.size(), 300U * 4U);

    auto cut = CanNotificationCutter::Cut(stream, 100000);
    zassert_equal(cut.records, 255U);
    zassert_equal(cut.bytes, 255U * 4U);
    zassert_equal(cut.dt_sum_us, 255U);
    zassert_false(cut.first_too_large);

    cut = CanNotificationCutter::Cut(stream, 100000, 10);
    zassert_equal(cut.records, 10U);
    zassert_equal(cut.bytes, 40U);
}

ZTEST(can_monitor_notification_cutter, test_ATailThatIsNotAllThereIsReported) {
    Bytes stream;
    AppendFrame(stream, 3, 1, 8);
    AppendFrame(stream, 3, 2, 8);
    const size_t whole = stream.size();
    AppendFrame(stream, 3, 3, 8);

    for(const size_t extra : { size_t{ 1 }, size_t{ 2 }, size_t{ 5 }, size_t{ 11 } }) {
        const auto span = std::span<const uint8_t>(stream).first(whole + extra);
        const auto cut = CanNotificationCutter::Cut(span, 1000);
        zassert_equal(cut.bytes, whole);
        zassert_equal(cut.records, 2U);
        zassert_true(cut.incomplete_tail, "extra %zu", extra);
        zassert_false(cut.first_too_large);
        zassert_false(cut.malformed);
    }

    // Only the start of a record.
    const auto cut = CanNotificationCutter::Cut(std::span<const uint8_t>(stream).first(2), 1000);
    zassert_equal(cut.bytes, 0U);
    zassert_equal(cut.records, 0U);
    zassert_true(cut.incomplete_tail);
    zassert_false(cut.first_too_large);
}

ZTEST(can_monitor_notification_cutter, test_NothingAndMalformedBytes) {
    auto cut = CanNotificationCutter::Cut(std::span<const uint8_t>(), 100);
    zassert_equal(cut.bytes, 0U);
    zassert_equal(cut.records, 0U);
    zassert_false(cut.first_too_large || cut.incomplete_tail || cut.malformed);

    Bytes stream;
    AppendFrame(stream, 3, 1, 8);
    const size_t whole = stream.size();
    stream.insert(stream.end(), { 0x80, 0x80, 0x00 });

    cut = CanNotificationCutter::Cut(stream, 1000);
    zassert_equal(cut.bytes, whole);
    zassert_equal(cut.records, 1U);
    zassert_true(cut.malformed);
    zassert_false(cut.incomplete_tail);
}

namespace {

struct TimedStream {
    Bytes bytes;
    /** @brief The absolute time of each record, in order. */
    std::vector<uint64_t> times;
    /** @brief The size of each record. */
    std::vector<size_t> sizes;
};

/** @brief Frames of every length, dts of 1 to 3 bytes, and the other kinds now and then. */
TimedStream MakeStream(uint64_t start, size_t count) {
    TimedStream stream;
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    uint64_t time = start;

    for(size_t i = 0; i < count; ++i) {
        // 0 now and then, one byte, two bytes, three bytes.
        constexpr std::array<uint64_t, 5> DTS = { 0, 7, 300, 20000, 128 };
        const uint64_t dt = DTS[i % DTS.size()];
        time += dt;

        size_t size = 0;
        switch(i % 11) {
            case 4:
                size = EncodeState(buffer, dt, CanBusState::ERROR_PASSIVE, 130, 5, 500000);
                break;
            case 7:
                size = EncodeGap(buffer, dt, 1000 + i, CanGapCause::RECEIVE_QUEUE);
                break;
            case 9:
                size = EncodeTap(buffer, dt, CanTapMark::MAY_HAVE_MISSED);
                break;
            default: {
                const uint8_t dlc = static_cast<uint8_t>(i % 16);
                const auto payload = MakePayload(FD_LENGTHS[dlc], static_cast<uint8_t>(i));
                size = EncodeFrame(
                    buffer, dt,
                    { .id = static_cast<uint32_t>(i),
                        .dlc = dlc,
                        .ide = (i % 3) == 0,
                        .fdf = true,
                        .brs = true,
                        .payload = payload }
                );
                break;
            }
        }

        Append(stream.bytes, size, buffer);
        stream.times.push_back(time);
        stream.sizes.push_back(size);
    }

    return stream;
}

} // namespace

ZTEST(can_monitor_notification_cutter, test_BaseUsChainsBackToEveryRecordsTime) {
    constexpr uint64_t START = 123456789;
    const TimedStream stream = MakeStream(START, 160);

    for(const size_t room : { size_t{ 82 }, size_t{ 100 }, size_t{ 495 }, size_t{ 4000 } }) {
        size_t pos = 0;
        size_t index = 0;
        uint64_t base_us = START;
        uint16_t seq = 0;

        while(pos < stream.bytes.size()) {
            const auto rest = std::span<const uint8_t>(stream.bytes).subspan(pos);
            const auto cut = CanNotificationCutter::Cut(rest, room);
            zassert_true(cut.records > 0 && cut.records <= 255, "room %zu at %zu", room, pos);
            zassert_true(cut.bytes <= room);
            zassert_false(cut.incomplete_tail || cut.malformed || cut.first_too_large);

            // The notification, as the sender builds it.
            Bytes notification(CAN_NOTIFICATION_HEADER_SIZE + cut.bytes);
            zassert_equal(
                EncodeNotificationHeader(notification, 3, seq, base_us, static_cast<uint8_t>(cut.records)),
                CAN_NOTIFICATION_HEADER_SIZE
            );
            std::copy(
                rest.begin(), rest.begin() + static_cast<ptrdiff_t>(cut.bytes),
                notification.begin() + CAN_NOTIFICATION_HEADER_SIZE
            );

            // The app: base_us plus the dts, record after record.
            zassert_equal(notification[0], CAN_RECORD_VERSION);
            zassert_equal(sys_get_le16(&notification[2]), seq);
            uint64_t time = sys_get_le64(&notification[4]);
            zassert_equal(time, base_us);

            size_t offset = CAN_NOTIFICATION_HEADER_SIZE;
            for(size_t n = 0; n < notification[12]; ++n) {
                const auto record = ParseRecord(std::span<const uint8_t>(notification).subspan(offset));
                zassert_true(record.has_value());
                time += record->dt_us;
                zassert_equal(time, stream.times[index], "record %zu, room %zu", index, room);
                zassert_equal(record->size, stream.sizes[index]);
                offset += record->size;
                ++index;
            }
            zassert_equal(offset, notification.size());

            pos += cut.bytes;
            base_us += cut.dt_sum_us;
            ++seq;
        }

        zassert_equal(index, stream.times.size());
        zassert_equal(base_us, stream.times.back());
    }
}

ZTEST(can_monitor_notification_cutter, test_SkippingARecordKeepsTheTimeChain) {
    constexpr uint64_t START = 1000;
    const TimedStream stream = MakeStream(START, 120);
    constexpr size_t ROOM = 16;

    size_t pos = 0;
    size_t index = 0;
    uint64_t base_us = START;
    size_t skipped = 0;
    size_t sent = 0;

    while(pos < stream.bytes.size()) {
        const auto cut = CanNotificationCutter::Cut(std::span<const uint8_t>(stream.bytes).subspan(pos), ROOM);

        if(cut.first_too_large) {
            // The sender drops it at the boundary; its dt still moves the time.
            zassert_equal(cut.first_record_size, stream.sizes[index]);
            pos += cut.first_record_size;
            base_us += cut.first_record_dt_us;
            ++index;
            ++skipped;
            continue;
        }

        zassert_true(cut.records > 0);

        uint64_t time = base_us;
        size_t offset = pos;
        for(size_t n = 0; n < cut.records; ++n) {
            const auto record = ParseRecord(std::span<const uint8_t>(stream.bytes).subspan(offset));
            zassert_true(record.has_value());
            time += record->dt_us;
            zassert_equal(time, stream.times[index], "record %zu", index);
            offset += record->size;
            ++index;
            ++sent;
        }

        pos += cut.bytes;
        base_us += cut.dt_sum_us;
    }

    zassert_equal(index, stream.times.size());
    zassert_true(skipped > 0 && sent > 0);
    zassert_equal(base_us, stream.times.back());
}
