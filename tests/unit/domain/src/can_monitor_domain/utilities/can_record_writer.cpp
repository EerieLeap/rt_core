#include <zephyr/ztest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "can_monitor_test_utils.h"
#include "domain/can_monitor_domain/utilities/byte_ring.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    #include "domain/can_monitor_domain/utilities/can_delta_table.h"
#endif

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_monitor_record_writer, NULL, NULL, NULL, NULL, NULL);

namespace {

// A classical frame with 8 data bytes and a one-byte dt takes 1 + 1 + 2 + 8 bytes.
constexpr size_t FRAME_SIZE = 12;
// A gap with a one-byte count and a dt of 0.
constexpr size_t GAP_SIZE = 5;

const std::array<uint8_t, 8> PAYLOAD = { 1, 2, 3, 4, 5, 6, 7, 8 };

CanRecordFrame Frame(uint32_t id) {
    return { .id = id, .dlc = 8, .payload = PAYLOAD };
}

} // namespace

ZTEST(can_monitor_record_writer, test_DtChainsFromTheLastRecordWritten) {
    auto ring = MakeRing(1024);
    CanRecordWriter writer(*ring, 1000);

    zassert_equal(writer.LastTimeUs(), 1000U);
    zassert_true(writer.WriteFrame(1100, Frame(1)));
    zassert_true(writer.WriteFrame(1100, Frame(2)));
    zassert_true(writer.WriteFrame(1350, Frame(3)));
    zassert_equal(writer.LastTimeUs(), 1350U);
    zassert_equal(writer.FramesWritten(), 3U);

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 3U);
    zassert_equal(drained.records[0].dt_us, 100U);
    zassert_equal(drained.records[1].dt_us, 0U);
    zassert_equal(drained.records[2].dt_us, 250U);
    zassert_equal(drained.records[2].id, 3U);
}

ZTEST(can_monitor_record_writer, test_ATimeThatGoesBackwardsGivesDtZero) {
    auto ring = MakeRing(1024);
    CanRecordWriter writer(*ring, 1000);

    zassert_true(writer.WriteFrame(1500, Frame(1)));
    zassert_true(writer.WriteFrame(1400, Frame(2)));
    zassert_equal(writer.LastTimeUs(), 1500U);
    zassert_true(writer.WriteFrame(1520, Frame(3)));
    zassert_true(writer.WriteFrame(100, Frame(4)));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 4U);
    zassert_equal(drained.records[1].dt_us, 0U);
    zassert_equal(drained.records[2].dt_us, 20U);
    zassert_equal(drained.records[3].dt_us, 0U);
    zassert_equal(writer.LastTimeUs(), 1520U);
}

ZTEST(can_monitor_record_writer, test_ARecordsTimeIsTheSumOfItsDts) {
    auto ring = MakeRing(1024);
    constexpr uint64_t START = 5000;
    CanRecordWriter writer(*ring, START);

    const std::array<uint64_t, 6> times = { 5000, 5001, 5130, 70000, 70000, 5000000000ULL };
    for(size_t i = 0; i < times.size(); ++i)
        zassert_true(writer.WriteFrame(times[i], Frame(static_cast<uint32_t>(i))));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), times.size());

    uint64_t time = START;
    for(size_t i = 0; i < times.size(); ++i) {
        time += drained.records[i].dt_us;
        zassert_equal(time, times[i], "record %zu", i);
    }
}

ZTEST(can_monitor_record_writer, test_StateAndTapRecordsChainToo) {
    auto ring = MakeRing(1024);
    CanRecordWriter writer(*ring, 1000);

    zassert_true(writer.WriteFrame(1010, Frame(1)));
    zassert_true(writer.WriteState(1020, CanBusState::ERROR_WARNING, 100, 3, 500000));
    zassert_true(writer.WriteTap(1015, CanTapMark::MAY_HAVE_MISSED));
    zassert_true(writer.WriteFrame(1030, Frame(2)));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 4U);

    zassert_true(drained.records[1].kind == CanRecordKind::STATE);
    zassert_equal(drained.records[1].dt_us, 10U);
    zassert_true(drained.records[1].state == CanBusState::ERROR_WARNING);
    zassert_equal(drained.records[1].tec, 100U);
    zassert_equal(drained.records[1].rec, 3U);
    zassert_equal(drained.records[1].detected_bitrate, 500000U);

    zassert_true(drained.records[2].kind == CanRecordKind::TAP);
    zassert_equal(drained.records[2].dt_us, 0U);
    zassert_true(drained.records[2].tap_mark == CanTapMark::MAY_HAVE_MISSED);

    zassert_equal(drained.records[3].dt_us, 10U);
    zassert_equal(writer.LastTimeUs(), 1030U);
    zassert_equal(writer.FramesWritten(), 2U);
}

ZTEST(can_monitor_record_writer, test_AFrameThatDoesNotFitIsLostAndCounted) {
    auto ring = MakeRing(64);
    CanRecordWriter writer(*ring, 0);

    for(uint32_t i = 1; i <= 5; ++i)
        zassert_true(writer.WriteFrame(i, Frame(i)));
    zassert_equal(ring->Free(), 4U);

    zassert_false(writer.WriteFrame(6, Frame(6)));
    zassert_equal(writer.LostCount(CanGapCause::RING), 1U);
    zassert_equal(writer.Pending(CanGapCause::RING), 1U);

    // The time stays where the last record put it.
    zassert_equal(writer.LastTimeUs(), 5U);
    zassert_equal(writer.FramesWritten(), 5U);
    zassert_equal(ring->Size(), 5 * FRAME_SIZE);
}

ZTEST(can_monitor_record_writer, test_AGapFollowsOnceRoomReturns) {
    auto ring = MakeRing(64);
    CanRecordWriter writer(*ring, 0);

    for(uint32_t i = 1; i <= 5; ++i)
        zassert_true(writer.WriteFrame(i, Frame(i)));
    zassert_false(writer.WriteFrame(6, Frame(6)));
    zassert_false(writer.WriteFrame(7, Frame(7)));
    zassert_equal(writer.LostCount(CanGapCause::RING), 2U);

    // Two frames' room back.
    zassert_true(ring->Consume(2 * FRAME_SIZE));
    zassert_true(writer.WriteFrame(8, Frame(8)));
    zassert_equal(writer.Pending(CanGapCause::RING), 0U);
    zassert_equal(writer.LostCount(CanGapCause::RING), 2U);
    zassert_equal(writer.FramesWritten(), 6U);

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 5U);
    zassert_equal(drained.records[0].id, 3U);
    zassert_equal(drained.records[2].id, 5U);

    // The gap says how many, right after the last record that made it, with no time of its own.
    zassert_true(drained.records[3].kind == CanRecordKind::GAP);
    zassert_equal(drained.records[3].frames_lost, 2U);
    zassert_true(drained.records[3].gap_cause == CanGapCause::RING);
    zassert_equal(drained.records[3].dt_us, 0U);

    // The frame behind it has the whole time since frame 5.
    zassert_true(drained.records[4].kind == CanRecordKind::FRAME);
    zassert_equal(drained.records[4].id, 8U);
    zassert_equal(drained.records[4].dt_us, 3U);
}

ZTEST(can_monitor_record_writer, test_AGapThatDoesNotFitStaysPendingAndTheFrameBehindItIsLost) {
    auto ring = MakeRing(64);
    CanRecordWriter writer(*ring, 0);

    for(uint32_t i = 1; i <= 5; ++i)
        zassert_true(writer.WriteFrame(i, Frame(i)));
    zassert_false(writer.WriteFrame(6, Frame(6)));

    // Room for the gap (5) but not for the gap and the frame (17): 4 + 12 = 16.
    zassert_true(ring->Consume(FRAME_SIZE));
    zassert_equal(ring->Free(), 16U);
    zassert_false(writer.WriteFrame(7, Frame(7)));
    zassert_equal(writer.LostCount(CanGapCause::RING), 2U);
    zassert_equal(writer.Pending(CanGapCause::RING), 1U);
    zassert_equal(ring->Free(), 16U - GAP_SIZE);

    // Room for neither: the gap waits, and this frame is lost with it.
    zassert_true(ring->Consume(2 * FRAME_SIZE));
    zassert_true(writer.WriteFrame(8, Frame(8)));
    zassert_equal(writer.Pending(CanGapCause::RING), 0U);
    zassert_equal(writer.LostCount(CanGapCause::RING), 2U);

    const auto drained = Drain(*ring);
    // Frames 4 and 5 are left, then the gap (frame 6), the gap (frame 7) and frame 8.
    zassert_equal(drained.records.size(), 5U);
    zassert_equal(drained.records[0].id, 4U);
    zassert_equal(drained.records[1].id, 5U);
    zassert_true(drained.records[2].kind == CanRecordKind::GAP);
    zassert_equal(drained.records[2].frames_lost, 1U);
    zassert_true(drained.records[3].kind == CanRecordKind::GAP);
    zassert_equal(drained.records[3].frames_lost, 1U);
    zassert_equal(drained.records[4].id, 8U);
}

ZTEST(can_monitor_record_writer, test_AGapThatCannotBeWrittenAtAllKeepsEverythingPending) {
    auto ring = MakeRing(64);
    CanRecordWriter writer(*ring, 0);

    for(uint32_t i = 1; i <= 5; ++i)
        zassert_true(writer.WriteFrame(i, Frame(i)));
    zassert_equal(ring->Free(), 4U);

    writer.AddLoss(CanGapCause::RECEIVE_QUEUE, 3);
    writer.AddLoss(CanGapCause::CONTROLLER, 0);
    zassert_false(writer.WriteFrame(6, Frame(6)));

    zassert_equal(writer.Pending(CanGapCause::RING), 1U);
    zassert_equal(writer.Pending(CanGapCause::RECEIVE_QUEUE), 3U);
    zassert_equal(writer.Pending(CanGapCause::CONTROLLER), 0U);
    zassert_equal(writer.LostCount(CanGapCause::RECEIVE_QUEUE), 3U);

    // Another failed attempt adds to the ring's count only.
    zassert_false(writer.WriteFrame(7, Frame(7)));
    zassert_equal(writer.Pending(CanGapCause::RING), 2U);
    zassert_equal(writer.Pending(CanGapCause::RECEIVE_QUEUE), 3U);

    zassert_true(ring->Consume(ring->Size()));
    zassert_true(writer.WriteFrame(8, Frame(8)));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 3U);
    zassert_true(drained.records[0].gap_cause == CanGapCause::RING);
    zassert_equal(drained.records[0].frames_lost, 2U);
    zassert_true(drained.records[1].gap_cause == CanGapCause::RECEIVE_QUEUE);
    zassert_equal(drained.records[1].frames_lost, 3U);
    zassert_true(drained.records[2].kind == CanRecordKind::FRAME);
}

ZTEST(can_monitor_record_writer, test_LossesSeenElsewhereBecomeGapsBeforeTheNextRecord) {
    auto ring = MakeRing(1024);
    CanRecordWriter writer(*ring, 100);

    zassert_true(writer.WriteFrame(110, Frame(1)));
    writer.AddLoss(CanGapCause::CONTROLLER, 4);
    writer.AddLoss(CanGapCause::ECHO_POOL, 2);
    writer.AddLoss(CanGapCause::CONTROLLER, 1);

    // Nothing is written until a record is.
    zassert_equal(ring->Size(), FRAME_SIZE);
    zassert_true(writer.WriteState(120, CanBusState::BUS_OFF, 255, 0, 0));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 4U);
    zassert_true(drained.records[1].kind == CanRecordKind::GAP);
    zassert_true(drained.records[1].gap_cause == CanGapCause::CONTROLLER);
    zassert_equal(drained.records[1].frames_lost, 5U);
    zassert_true(drained.records[2].gap_cause == CanGapCause::ECHO_POOL);
    zassert_equal(drained.records[2].frames_lost, 2U);
    zassert_true(drained.records[3].kind == CanRecordKind::STATE);
    zassert_equal(drained.records[3].dt_us, 10U);

    zassert_equal(writer.LostCount(CanGapCause::CONTROLLER), 5U);
    zassert_equal(writer.LostCount(CanGapCause::ECHO_POOL), 2U);
    zassert_equal(writer.LostCount(CanGapCause::RING), 0U);
}

ZTEST(can_monitor_record_writer, test_AStateOrTapThatDoesNotFitIsCountedLost) {
    auto ring = MakeRing(16);
    CanRecordWriter writer(*ring, 0);

    zassert_true(writer.WriteFrame(1, Frame(1)));
    zassert_equal(ring->Free(), 4U);

    zassert_false(writer.WriteState(2, CanBusState::STOPPED, 0, 0, 0));
    zassert_false(writer.WriteTap(3, CanTapMark::MAY_HAVE_REPEATED));
    zassert_equal(writer.LostCount(CanGapCause::RING), 2U);
    zassert_equal(writer.LastTimeUs(), 1U);

    // A gap (5 bytes) and a frame without data (4) fit the 16.
    zassert_true(ring->Consume(ring->Size()));
    zassert_true(writer.WriteFrame(4, { .id = 4, .dlc = 0 }));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 2U);
    zassert_true(drained.records[0].kind == CanRecordKind::GAP);
    zassert_equal(drained.records[0].frames_lost, 2U);
    zassert_equal(drained.records[1].dt_us, 3U);
}

ZTEST(can_monitor_record_writer, test_AMalformedFrameIsRejectedWithoutAGap) {
    auto ring = MakeRing(1024);
    CanRecordWriter writer(*ring, 0);

    const std::array<uint8_t, 3> short_payload = { 1, 2, 3 };
    zassert_false(writer.WriteFrame(1, { .id = 1, .dlc = 8, .payload = short_payload }));
    zassert_false(writer.WriteFrame(2, { .id = 0x800, .dlc = 3, .payload = short_payload }));

    zassert_equal(writer.FramesRejected(), 2U);
    zassert_equal(writer.FramesWritten(), 0U);
    zassert_equal(writer.Pending(CanGapCause::RING), 0U);
    zassert_equal(writer.LostCount(CanGapCause::RING), 0U);
    zassert_equal(ring->Size(), 0U);
    zassert_equal(writer.LastTimeUs(), 0U);
}

ZTEST(can_monitor_record_writer, test_TheCountersWidenAcrossAWrap) {
    CanCounterWidener widener;

    zassert_equal(widener.Update(10), 10U);
    zassert_equal(widener.Update(10), 10U);
    zassert_equal(widener.Update(0xFFFFFFF0U), 0xFFFFFFF0ULL);
    zassert_equal(widener.Update(5), 0x100000005ULL);
}

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA

namespace {

struct Sent {
    uint32_t id;
    std::array<uint8_t, 8> payload;
};

/** @brief Bytes 0 to 3 follow the id; bytes 4 to 7 follow the round. */
std::array<uint8_t, 8> PayloadAt(uint32_t id, uint8_t round) {
    std::array<uint8_t, 8> payload{};
    for(size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(i < 4 ? id + i : round + i);

    return payload;
}

/** @brief Plays a ring's records through the decoder, as the app does, against the frames that were written. */
void ExpectDecodes(const Drained& drained, CanDeltaDecoder& decoder, const std::vector<Sent>& sent) {
    std::array<uint8_t, CAN_MAX_PAYLOAD_SIZE> payload{};
    size_t next = 0;

    for(const CanRecord& record : drained.records) {
        const auto length = decoder.Apply(record, payload);
        zassert_true(
            length.has_value(), "a record of kind %u has nothing to apply to", static_cast<unsigned>(record.kind)
        );

        if(record.kind != CanRecordKind::FRAME && record.kind != CanRecordKind::CHANGED_BYTES)
            continue;

        zassert_true(next < sent.size());
        zassert_equal(record.id, sent[next].id);
        zassert_equal(*length, sent[next].payload.size());
        zassert_true(std::equal(sent[next].payload.begin(), sent[next].payload.end(), payload.begin()));
        ++next;
    }

    zassert_equal(next, sent.size());
}

std::vector<CanRecordKind> KindsOf(const Drained& drained) {
    std::vector<CanRecordKind> kinds;
    kinds.reserve(drained.records.size());
    for(const CanRecord& record : drained.records)
        kinds.push_back(record.kind);

    return kinds;
}

} // namespace

ZTEST(can_monitor_record_writer, test_ChangedBytesGoThroughTheTable) {
    auto ring = MakeRing(1024);
    auto table = CanDeltaTable::Create(16, std::pmr::get_default_resource());
    zassert_not_null(table.get());
    CanRecordWriter writer(*ring, 0, table.get());

    const auto first = PayloadAt(5, 0);
    const auto second = PayloadAt(5, 1);
    zassert_true(writer.WriteFrame(1, { .id = 5, .dlc = 8, .payload = first }));
    zassert_true(writer.WriteFrame(2, { .id = 5, .dlc = 8, .payload = first }));
    zassert_true(writer.WriteFrame(3, { .id = 5, .dlc = 8, .payload = second }));

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 3U);
    zassert_true(drained.records[0].kind == CanRecordKind::FRAME);
    zassert_true(drained.records[1].kind == CanRecordKind::CHANGED_BYTES);
    zassert_true(drained.records[2].kind == CanRecordKind::CHANGED_BYTES);
    zassert_equal(drained.records[2].dt_us, 1U);

    // Without a table every frame goes whole.
    auto plain_ring = MakeRing(1024);
    CanRecordWriter plain(*plain_ring, 0, nullptr);
    zassert_true(plain.WriteFrame(1, { .id = 5, .dlc = 8, .payload = first }));
    zassert_true(plain.WriteFrame(2, { .id = 5, .dlc = 8, .payload = first }));
    for(const CanRecord& record : Drain(*plain_ring).records)
        zassert_true(record.kind == CanRecordKind::FRAME);
}

ZTEST(can_monitor_record_writer, test_ATapOrAGapSendsEveryIdWholeAgain) {
    auto ring = MakeRing(1024);
    auto table = CanDeltaTable::Create(16, std::pmr::get_default_resource());
    auto decoder_table = CanDeltaTable::Create(16, std::pmr::get_default_resource());
    zassert_not_null(table.get());
    zassert_not_null(decoder_table.get());
    CanRecordWriter writer(*ring, 0, table.get());
    CanDeltaDecoder decoder(*decoder_table);

    std::vector<Sent> sent;
    auto write = [&](uint64_t time, uint32_t id, uint8_t round) {
        const Sent frame{ id, PayloadAt(id, round) };
        zassert_true(writer.WriteFrame(time, { .id = id, .dlc = 8, .payload = frame.payload }));
        sent.push_back(frame);
    };

    write(1, 5, 0);
    write(2, 5, 1);
    zassert_true(writer.WriteTap(3, CanTapMark::MAY_HAVE_REPEATED));
    write(4, 5, 2);
    write(5, 5, 3);
    writer.AddLoss(CanGapCause::CONTROLLER, 1);
    write(6, 5, 4);
    write(7, 5, 5);

    const auto drained = Drain(*ring);
    const std::vector<CanRecordKind> expected = {
        CanRecordKind::FRAME,
        CanRecordKind::CHANGED_BYTES,
        CanRecordKind::TAP,
        CanRecordKind::FRAME,
        CanRecordKind::CHANGED_BYTES,
        CanRecordKind::GAP,
        CanRecordKind::FRAME,
        CanRecordKind::CHANGED_BYTES,
    };
    zassert_true(KindsOf(drained) == expected);

    ExpectDecodes(drained, decoder, sent);
}

ZTEST(can_monitor_record_writer, test_AFrameLostToTheRingEmptiesTheTable) {
    auto ring = MakeRing(64);
    auto table = CanDeltaTable::Create(16, std::pmr::get_default_resource());
    auto decoder_table = CanDeltaTable::Create(16, std::pmr::get_default_resource());
    zassert_not_null(table.get());
    zassert_not_null(decoder_table.get());
    CanRecordWriter writer(*ring, 0, table.get());
    CanDeltaDecoder decoder(*decoder_table);

    std::vector<Sent> sent;
    uint8_t round = 0;
    for(;; ++round) {
        const Sent frame{ 5, PayloadAt(5, round) };
        if(!writer.WriteFrame(round + 1U, { .id = 5, .dlc = 8, .payload = frame.payload }))
            break;
        sent.push_back(frame);
        zassert_true(round < 50, "the ring never filled");
    }
    zassert_true(sent.size() > 3);

    // The app has the frames that made it.
    ExpectDecodes(Drain(*ring), decoder, sent);

    // The lost frame changed the table's payload; the next one must not be a delta against it.
    sent.clear();
    ++round;
    const Sent next{ 5, PayloadAt(5, round) };
    zassert_true(writer.WriteFrame(round + 1U, { .id = 5, .dlc = 8, .payload = next.payload }));
    sent.push_back(next);

    const auto drained = Drain(*ring);
    zassert_equal(drained.records.size(), 2U);
    zassert_true(drained.records[0].kind == CanRecordKind::GAP);
    zassert_equal(drained.records[0].frames_lost, 1U);
    zassert_true(drained.records[1].kind == CanRecordKind::FRAME);
    ExpectDecodes(drained, decoder, sent);
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
