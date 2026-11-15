#include <zephyr/ztest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "../utilities/can_monitor_test_utils.h"
#include "domain/can_monitor_domain/transmit/can_replay_buffer.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"

using namespace eerie_leap::domain::can_monitor_domain::transmit;
using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_replay_buffer, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint64_t LATE_US = CanReplayBuffer::LATE_US;

// A classical 11-bit frame with 1 data byte and a dt under 128 takes 1 + 1 + 2 + 1 bytes.
constexpr size_t SMALL_RECORD = 5;

std::unique_ptr<CanReplayBuffer> MakeBuffer(size_t capacity = 4096) {
    auto buffer = CanReplayBuffer::Create(std::pmr::get_default_resource(), capacity);
    zassert_not_null(buffer.get());

    return buffer;
}

// Appends a record of a frame the encoders take; the payload repeats the ID's low byte.
void Append(std::vector<uint8_t>& chunk, uint64_t dt_us, const CanRecordFrame& frame) {
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> record{};
    const size_t size = EncodeFrame(record, dt_us, frame);
    zassert_true(size > 0, "The record did not encode");

    chunk.insert(chunk.end(), record.begin(), record.begin() + size);
}

void AppendSmall(std::vector<uint8_t>& chunk, uint64_t dt_us, uint32_t id) {
    const std::array<uint8_t, 1> payload = { static_cast<uint8_t>(id) };

    Append(chunk, dt_us, { .id = id, .dlc = 1, .payload = payload });
}

// Frames with the given dts and the IDs 1, 2, 3, ...
std::vector<uint8_t> Chunk(std::initializer_list<uint64_t> dts, uint32_t first_id = 1) {
    std::vector<uint8_t> chunk;
    uint32_t id = first_id;

    for(const uint64_t dt : dts)
        AppendSmall(chunk, dt, id++);

    return chunk;
}

std::vector<uint8_t> SmallChunk(size_t frames, uint32_t first_id = 1) {
    std::vector<uint8_t> chunk;

    for(size_t i = 0; i < frames; ++i)
        AppendSmall(chunk, 10, first_id + static_cast<uint32_t>(i));

    zassert_equal(chunk.size(), frames * SMALL_RECORD);

    return chunk;
}

// Later than any time a test passes, and early enough that the timing arithmetic cannot overflow from it.
constexpr uint64_t FOREVER = 1'000'000'000'000'000'000ULL;

// Hands out the next frame at its own time.
std::optional<CanReplayFrame> PopNext(CanReplayBuffer& buffer) {
    const std::optional<uint64_t> due = buffer.NextDueUs();
    if(!due.has_value())
        return std::nullopt;

    return buffer.Pop(*due);
}

// Pops the next frame at its own time and checks its ID.
void PopOnTime(CanReplayBuffer& buffer, uint32_t id, uint64_t expected_due) {
    const std::optional<uint64_t> due = buffer.NextDueUs();
    zassert_true(due.has_value());
    zassert_equal(*due, expected_due, "Frame %u was due at %llu", id, static_cast<unsigned long long>(*due));

    zassert_false(buffer.Pop(*due - 1).has_value(), "Frame %u went a µs early", id);

    const auto popped = buffer.Pop(*due);
    zassert_true(popped.has_value());
    zassert_equal(popped->frame.id, id);
    zassert_false(popped->late);
    zassert_equal(popped->late_us, 0U);
}

} // namespace

ZTEST(can_replay_buffer, test_FramesGoAtTheirOwnPaceAt100Percent) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 1000));

    zassert_false(buffer->NextDueUs().has_value(), "Nothing waits yet");
    zassert_equal(buffer->AddChunk(0, Chunk({ 500, 1000, 250 }), 1000), CanReplayAdd::OK);

    // The first frame is due at the start plus its own dt.
    PopOnTime(*buffer, 1, 1500);
    PopOnTime(*buffer, 2, 2500);
    PopOnTime(*buffer, 3, 2750);

    zassert_false(buffer->NextDueUs().has_value());
    zassert_false(buffer->Pop(FOREVER).has_value());
    zassert_equal(buffer->FramesPlayed(), 3U);
    zassert_equal(buffer->LateFrames(), 0U);
}

ZTEST(can_replay_buffer, test_TheSpeedDividesTheDts) {
    // 200 %: half the time; 50 %: twice; 150 %: two thirds.
    const struct {
        uint32_t speed;
        std::array<uint64_t, 3> due;
    } cases[] = {
        { 200, { 500, 1500, 1750 } },
        { 50, { 2000, 6000, 7000 } },
        { 400, { 250, 750, 875 } },
        { 100, { 1000, 3000, 3500 } },
    };

    for(const auto& test_case : cases) {
        auto buffer = MakeBuffer();
        zassert_true(buffer->Start(test_case.speed, false, 0));
        zassert_equal(buffer->AddChunk(0, Chunk({ 1000, 2000, 500 }), 0), CanReplayAdd::OK);

        PopOnTime(*buffer, 1, test_case.due[0]);
        PopOnTime(*buffer, 2, test_case.due[1]);
        PopOnTime(*buffer, 3, test_case.due[2]);
    }
}

ZTEST(can_replay_buffer, test_TheRemainderOfAScaledDtIsNotLost) {
    // 1,001 µs at 200 % is 500.5: a floor would lose half a µs a frame.
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(200, false, 0));
    zassert_equal(buffer->AddChunk(0, Chunk({ 1001, 1001, 1001, 1001 }), 0), CanReplayAdd::OK);

    PopOnTime(*buffer, 1, 500);
    PopOnTime(*buffer, 2, 1001);
    PopOnTime(*buffer, 3, 1501);
    PopOnTime(*buffer, 4, 2002);

    // 1 µs at 300 % over 300 frames: 100 µs in all.
    zassert_true(buffer->Start(300, false, 0));
    for(int round = 0; round < 3; ++round) {
        zassert_equal(
            buffer->AddChunk(static_cast<uint16_t>(round), Chunk({ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }), 0), CanReplayAdd::OK
        );
    }

    uint64_t last_due = 0;
    for(int i = 0; i < 30; ++i) {
        const auto due = buffer->NextDueUs();
        zassert_true(due.has_value());
        zassert_true(buffer->Pop(*due).has_value());
        last_due = *due;
    }
    zassert_equal(last_due, 10U, "30 µs at 300 % is 10 µs, however it is cut up");
}

ZTEST(can_replay_buffer, test_TheFirstChunkRestartsTheTimingWhenItComesLate) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));

    // The first chunk took a Bluetooth round trip: the first frame is its dt after the chunk, not late.
    zassert_equal(buffer->AddChunk(0, Chunk({ 400 }), 80'000), CanReplayAdd::OK);
    PopOnTime(*buffer, 1, 80'400);
    zassert_equal(buffer->LateFrames(), 0U);
}

ZTEST(can_replay_buffer, test_AFrameBeyondTheLateLimitIsCountedAndSentAtOnce) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->AddChunk(0, Chunk({ 1000, 1000, 1000, 1000 }), 0), CanReplayAdd::OK);

    // Exactly the limit late is not late.
    auto first = buffer->Pop(1000 + LATE_US);
    zassert_true(first.has_value());
    zassert_false(first->late);
    zassert_equal(first->late_us, LATE_US);
    zassert_equal(buffer->LateFrames(), 0U);

    // Not late, so the schedule stands: the next frame is 1,000 µs after the second's time.
    zassert_equal(*buffer->NextDueUs(), 2000U);

    // The next one is handed out far too late: it goes at once, is counted, and the schedule moves with it.
    const uint64_t now = 2000 + LATE_US + 1;
    auto second = buffer->Pop(now);
    zassert_true(second.has_value());
    zassert_equal(second->frame.id, 2U);
    zassert_true(second->late);
    zassert_equal(second->late_us, LATE_US + 1);
    zassert_equal(buffer->LateFrames(), 1U);
    zassert_equal(*buffer->NextDueUs(), now + 1000, "The gap to the next frame is kept");

    // No burst to catch up: the frame behind it is not due yet.
    zassert_false(buffer->Pop(now).has_value());
    PopOnTime(*buffer, 3, now + 1000);
    PopOnTime(*buffer, 4, now + 2000);
    zassert_equal(buffer->LateFrames(), 1U);
    zassert_equal(buffer->FramesPlayed(), 4U);
}

ZTEST(can_replay_buffer, test_ChunksComeInOrderOfSeq) {
    auto buffer = MakeBuffer();

    zassert_equal(buffer->AddChunk(0, SmallChunk(1), 0), CanReplayAdd::BAD_STATE, "Not started");

    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->AddChunk(1, SmallChunk(1), 0), CanReplayAdd::BAD_SEQ, "The first chunk is 0");
    zassert_equal(buffer->AddChunk(0, SmallChunk(1), 0), CanReplayAdd::OK);
    zassert_equal(buffer->AddChunk(0, SmallChunk(1), 0), CanReplayAdd::BAD_SEQ, "A repeat");
    zassert_equal(buffer->AddChunk(2, SmallChunk(1), 0), CanReplayAdd::BAD_SEQ, "A gap");
    zassert_equal(buffer->AddChunk(1, SmallChunk(1), 0), CanReplayAdd::OK);

    // Refusals took nothing in.
    zassert_equal(buffer->Room(), 4096 - (2 * SMALL_RECORD));

    // A new start numbers from 0 again.
    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->AddChunk(2, SmallChunk(1), 0), CanReplayAdd::BAD_SEQ);
    zassert_equal(buffer->AddChunk(0, SmallChunk(1), 0), CanReplayAdd::OK);
}

ZTEST(can_replay_buffer, test_SeqWrapsAt16Bits) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));

    // Played at once, so the buffer never fills.
    const std::vector<uint8_t> chunk = SmallChunk(1);
    for(uint32_t seq = 0; seq < 65538; ++seq) {
        zassert_equal(buffer->AddChunk(static_cast<uint16_t>(seq), chunk, 0), CanReplayAdd::OK, "seq %u", seq);
        zassert_true(PopNext(*buffer).has_value());
    }
}

ZTEST(can_replay_buffer, test_RoomIsWhatIsLeftAndAChunkLargerThanItIsRefused) {
    auto buffer = MakeBuffer(10 * SMALL_RECORD);
    zassert_equal(buffer->Room(), 50U, "Before a start");

    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->Room(), 50U);

    zassert_equal(buffer->AddChunk(0, SmallChunk(6), 0), CanReplayAdd::OK);
    zassert_equal(buffer->Room(), 20U);

    // 5 records are 25 bytes: over the room, and refused whole.
    zassert_equal(buffer->AddChunk(1, SmallChunk(5, 10), 0), CanReplayAdd::NO_ROOM);
    zassert_equal(buffer->Room(), 20U);

    // The same seq goes again with what fits; the chunk may fill the room exactly.
    zassert_equal(buffer->AddChunk(1, SmallChunk(4, 10), 0), CanReplayAdd::OK);
    zassert_equal(buffer->Room(), 0U);
    zassert_equal(buffer->AddChunk(2, SmallChunk(1, 20), 0), CanReplayAdd::NO_ROOM);
    zassert_equal(buffer->AddChunk(2, {}, 0), CanReplayAdd::OK, "An empty chunk always fits");

    // A frame played gives its bytes back.
    zassert_true(PopNext(*buffer).has_value());
    zassert_equal(buffer->Room(), SMALL_RECORD);
    zassert_equal(buffer->AddChunk(3, SmallChunk(1, 20), 0), CanReplayAdd::OK);
    zassert_equal(buffer->Room(), 0U);
}

ZTEST(can_replay_buffer, test_AChunkThatIsNotWholeFrameRecordsIsRefused) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));

    const std::vector<uint8_t> good = SmallChunk(3);

    // A boundary that splits a record.
    std::vector<uint8_t> split(good.begin(), good.end() - 1);
    zassert_equal(buffer->AddChunk(0, split, 0), CanReplayAdd::MALFORMED);
    zassert_equal(buffer->AddChunk(0, std::span(good).first(SMALL_RECORD + 2), 0), CanReplayAdd::MALFORMED);
    zassert_equal(buffer->AddChunk(0, std::span(good).first(1), 0), CanReplayAdd::MALFORMED);

    // A record of another kind.
    std::vector<uint8_t> with_event = SmallChunk(1);
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> event{};
    const size_t event_size = EncodeTxEvent(event, 0, CanTxEvent::REPLAY_DONE, 1);
    zassert_true(event_size > 0);
    with_event.insert(with_event.end(), event.begin(), event.begin() + event_size);
    zassert_equal(buffer->AddChunk(0, with_event, 0), CanReplayAdd::MALFORMED);

    // A record that is no record: kind 15 in the ext byte.
    const std::array<uint8_t, 3> garbage = { 0x80, 0xF0, 0x00 };
    zassert_equal(buffer->AddChunk(0, garbage, 0), CanReplayAdd::MALFORMED);

    // Nothing was taken in, the seq is where it was, and the whole chunk goes.
    zassert_equal(buffer->Room(), 4096U);
    zassert_false(buffer->HasFrames());
    zassert_equal(buffer->AddChunk(0, good, 0), CanReplayAdd::OK);
    zassert_equal(buffer->Room(), 4096 - good.size());
}

ZTEST(can_replay_buffer, test_ARecordKeepsItsFlagsPayloadAndDlc) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));

    const auto fd_payload = MakePayload(12, 3);
    const auto classical_payload = MakePayload(8, 9);
    std::vector<uint8_t> chunk;

    Append(chunk, 10, { .id = 0x123, .dlc = 8, .payload = classical_payload });
    Append(chunk, 10, { .id = 0x1ABCDEF0, .dlc = 3, .ide = true, .rtr = true });
    Append(chunk, 10, { .id = 0x18FF0042, .dlc = 9, .ide = true, .fdf = true, .brs = true, .payload = fd_payload });
    Append(chunk, 10, { .id = 0x7FF, .dlc = 9, .fdf = true, .esi = true, .tx = true, .payload = fd_payload });
    zassert_equal(buffer->AddChunk(0, chunk, 0), CanReplayAdd::OK);

    auto classical = PopNext(*buffer);
    zassert_true(classical.has_value());
    zassert_equal(classical->frame.id, 0x123U);
    zassert_false(classical->frame.is_extended);
    zassert_false(classical->frame.is_can_fd);
    zassert_false(classical->frame.is_bitrate_switch);
    zassert_false(classical->frame.is_remote_request);
    zassert_equal(classical->frame.dlc, 8);
    zassert_equal(classical->frame.data.size(), 8U);
    zassert_mem_equal(classical->frame.data.data(), classical_payload.data(), 8);

    auto remote = PopNext(*buffer);
    zassert_true(remote.has_value());
    zassert_equal(remote->frame.id, 0x1ABCDEF0U);
    zassert_true(remote->frame.is_extended);
    zassert_true(remote->frame.is_remote_request);
    zassert_false(remote->frame.is_can_fd);
    zassert_equal(remote->frame.dlc, 3);
    zassert_true(remote->frame.data.empty());

    auto fd = PopNext(*buffer);
    zassert_true(fd.has_value());
    zassert_true(fd->frame.is_extended);
    zassert_true(fd->frame.is_can_fd);
    zassert_true(fd->frame.is_bitrate_switch);
    zassert_false(fd->frame.is_remote_request);
    zassert_equal(fd->frame.dlc, 9);
    zassert_equal(fd->frame.data.size(), 12U);
    zassert_mem_equal(fd->frame.data.data(), fd_payload.data(), 12);

    // The ESI and TX of a captured frame are not the replay's to send.
    auto last = PopNext(*buffer);
    zassert_true(last.has_value());
    zassert_true(last->frame.is_can_fd);
    zassert_false(last->frame.is_bitrate_switch);
    zassert_false(last->frame.is_error_state_indicator);
    zassert_false(last->frame.is_transmit);
}

ZTEST(can_replay_buffer, test_PeekShowsTheNextFrameWithoutTakingIt) {
    auto buffer = MakeBuffer();
    zassert_false(buffer->Peek().has_value(), "Not started");
    zassert_true(buffer->Start(100, false, 0));
    zassert_false(buffer->Peek().has_value(), "Empty");
    zassert_equal(buffer->AddChunk(0, Chunk({ 1000, 1000 }), 0), CanReplayAdd::OK);

    const auto peeked = buffer->Peek();
    zassert_true(peeked.has_value());
    zassert_equal(peeked->frame.id, 1U);
    zassert_equal(buffer->Peek()->frame.id, 1U, "Peeking again gives the same frame");
    zassert_equal(buffer->FramesPlayed(), 0U);

    zassert_false(buffer->Pop(999).has_value());
    zassert_equal(buffer->Pop(1000)->frame.id, 1U);
    zassert_equal(buffer->Peek()->frame.id, 2U);
}

ZTEST(can_replay_buffer, test_AnEmptyBufferBeforeFinishIsAnUnderrunAndPlayingGoesOnWithNewData) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));

    zassert_true(buffer->IsUnderrun(), "Empty and not finished");
    zassert_equal(buffer->Underruns(), 0U, "The empty start is not an underrun event");

    zassert_equal(buffer->AddChunk(0, Chunk({ 1000, 1000 }), 0), CanReplayAdd::OK);
    zassert_false(buffer->IsUnderrun());

    PopOnTime(*buffer, 1, 1000);
    zassert_equal(buffer->Underruns(), 0U);
    PopOnTime(*buffer, 2, 2000);

    zassert_true(buffer->IsUnderrun());
    zassert_equal(buffer->Underruns(), 1U);
    zassert_false(buffer->IsDone());
    zassert_false(buffer->NextDueUs().has_value());
    zassert_false(buffer->Pop(FOREVER).has_value());

    // Data comes 50 ms later: the timing restarts from the first new frame, which is not late.
    zassert_equal(buffer->AddChunk(1, Chunk({ 300, 700 }, 3), 52'000), CanReplayAdd::OK);
    zassert_false(buffer->IsUnderrun());
    PopOnTime(*buffer, 3, 52'300);
    PopOnTime(*buffer, 4, 53'000);
    zassert_equal(buffer->LateFrames(), 0U);

    // And it runs dry again, which is a second underrun.
    zassert_true(buffer->IsUnderrun());
    zassert_equal(buffer->Underruns(), 2U);
}

ZTEST(can_replay_buffer, test_FinishedAndEmptyIsDone) {
    auto buffer = MakeBuffer();
    zassert_false(buffer->IsDone(), "Not started");
    zassert_true(buffer->Start(100, false, 0));
    zassert_false(buffer->IsDone());

    zassert_equal(buffer->AddChunk(0, Chunk({ 100, 100 }), 0), CanReplayAdd::OK);
    buffer->Finish();

    zassert_true(buffer->IsFinished());
    zassert_false(buffer->IsDone(), "Frames still wait");
    zassert_false(buffer->IsUnderrun(), "Finished is not an underrun");
    zassert_equal(buffer->AddChunk(1, SmallChunk(1), 0), CanReplayAdd::BAD_STATE, "No chunks after Finish()");

    PopOnTime(*buffer, 1, 100);
    zassert_false(buffer->IsDone());
    PopOnTime(*buffer, 2, 200);

    zassert_true(buffer->IsDone());
    zassert_false(buffer->IsUnderrun());
    zassert_equal(buffer->Underruns(), 0U, "The end is not an underrun");
    zassert_false(buffer->NextDueUs().has_value());
    zassert_false(buffer->Pop(FOREVER).has_value());
    zassert_equal(buffer->FramesPlayed(), 2U);
    zassert_equal(buffer->Loops(), 0U);
}

ZTEST(can_replay_buffer, test_FinishOnAnEmptyBufferIsDone) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->AddChunk(0, Chunk({ 100 }), 0), CanReplayAdd::OK);
    PopOnTime(*buffer, 1, 100);
    zassert_true(buffer->IsUnderrun());

    buffer->Finish();
    buffer->Finish();

    zassert_true(buffer->IsDone());
    zassert_equal(buffer->Underruns(), 1U);

    // A replay with no frames at all.
    zassert_true(buffer->Start(100, false, 0));
    buffer->Finish();
    zassert_true(buffer->IsDone());
}

ZTEST(can_replay_buffer, test_ALoopStartsOverWhenTheWholeReplayFits) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, true, 0));
    zassert_true(buffer->CanLoop());

    zassert_equal(buffer->AddChunk(0, Chunk({ 1000, 500 }), 0), CanReplayAdd::OK);
    zassert_equal(buffer->AddChunk(1, Chunk({ 250 }, 3), 0), CanReplayAdd::OK);
    buffer->Finish();

    // First round: 1000, 1500, 1750. The last frame starts it over: the first is due 1,000 µs after it went.
    PopOnTime(*buffer, 1, 1000);
    PopOnTime(*buffer, 2, 1500);
    PopOnTime(*buffer, 3, 1750);
    zassert_equal(buffer->Loops(), 1U);
    zassert_false(buffer->IsDone());
    zassert_false(buffer->IsUnderrun());

    // Second round, the same frames in the same order, timing from the restart.
    PopOnTime(*buffer, 1, 2750);
    PopOnTime(*buffer, 2, 3250);
    PopOnTime(*buffer, 3, 3500);
    zassert_equal(buffer->Loops(), 2U);

    PopOnTime(*buffer, 1, 4500);
    zassert_equal(buffer->FramesPlayed(), 7U);
    zassert_equal(buffer->LateFrames(), 0U);
    zassert_equal(buffer->Underruns(), 0U);

    // Stopping ends it.
    buffer->Stop();
    zassert_false(buffer->NextDueUs().has_value());
    zassert_false(buffer->Pop(FOREVER).has_value());
}

ZTEST(can_replay_buffer, test_ALoopAlsoKeepsItsPayloadsAcrossRounds) {
    auto buffer = MakeBuffer(256);
    zassert_true(buffer->Start(100, true, 0));

    const auto payload = MakePayload(64, 5);
    std::vector<uint8_t> chunk;
    Append(chunk, 100, { .id = 0x10, .dlc = 15, .ide = true, .fdf = true, .brs = true, .payload = payload });
    AppendSmall(chunk, 100, 0x11);
    zassert_equal(buffer->AddChunk(0, chunk, 0), CanReplayAdd::OK);
    buffer->Finish();

    for(int round = 0; round < 5; ++round) {
        const auto fd = PopNext(*buffer);
        zassert_true(fd.has_value());
        zassert_equal(fd->frame.id, 0x10U, "Round %d", round);
        zassert_equal(fd->frame.data.size(), 64U);
        zassert_mem_equal(fd->frame.data.data(), payload.data(), 64);

        const auto small = PopNext(*buffer);
        zassert_true(small.has_value());
        zassert_equal(small->frame.id, 0x11U);
    }
    zassert_equal(buffer->Loops(), 5U);
}

ZTEST(can_replay_buffer, test_FinishAfterTheLastFrameWentStartsALoopOver) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, true, 0));
    zassert_equal(buffer->AddChunk(0, Chunk({ 1000, 1000 }), 0), CanReplayAdd::OK);

    PopOnTime(*buffer, 1, 1000);
    PopOnTime(*buffer, 2, 2000);
    zassert_true(buffer->IsUnderrun(), "The app has not said it is done");
    zassert_equal(buffer->Underruns(), 1U);

    // The app says so now: the replay is complete and fits.
    buffer->Finish();
    zassert_false(buffer->IsDone());
    zassert_equal(buffer->Loops(), 1U);
    zassert_equal(*buffer->NextDueUs(), 3000U, "Counted from the last frame's time");
}

ZTEST(can_replay_buffer, test_ALoopThatDoesNotFitLosesItsLoop) {
    // 10 records of room.
    auto buffer = MakeBuffer(10 * SMALL_RECORD);
    zassert_true(buffer->Start(100, true, 0));

    zassert_equal(buffer->AddChunk(0, SmallChunk(8), 0), CanReplayAdd::OK);
    for(uint32_t id = 1; id <= 3; ++id)
        zassert_true(PopNext(*buffer).has_value());

    // The three played frames are kept for the loop; the chunk still has to be fit in. Room() counts what is
    // not waiting to be played: 10 - 5 records.
    zassert_equal(buffer->Room(), 5 * SMALL_RECORD);
    zassert_true(buffer->CanLoop());

    // Two records fit beside the kept ones (10 - 8 = 2): nothing is discarded.
    zassert_equal(buffer->AddChunk(1, SmallChunk(2, 9), 0), CanReplayAdd::OK);
    zassert_true(buffer->CanLoop());

    // Three more do not: the played frames go, and the replay no longer fits in the buffer.
    zassert_equal(buffer->AddChunk(2, SmallChunk(3, 11), 0), CanReplayAdd::OK);
    zassert_false(buffer->CanLoop());

    buffer->Finish();
    for(uint32_t id = 4; id <= 13; ++id) {
        const auto popped = PopNext(*buffer);
        zassert_true(popped.has_value());
        zassert_equal(popped->frame.id, id);
    }

    zassert_true(buffer->IsDone(), "Loop has no effect: the app resends");
    zassert_equal(buffer->Loops(), 0U);
    zassert_false(buffer->Pop(FOREVER).has_value());
    zassert_equal(buffer->FramesPlayed(), 13U);
}

ZTEST(can_replay_buffer, test_AfterTheLoopIsLostFramesFreeTheirRoomAsWithoutALoop) {
    auto buffer = MakeBuffer(4 * SMALL_RECORD);
    zassert_true(buffer->Start(100, true, 0));

    zassert_equal(buffer->AddChunk(0, SmallChunk(4), 0), CanReplayAdd::OK);
    zassert_true(PopNext(*buffer).has_value());
    zassert_true(PopNext(*buffer).has_value());
    zassert_equal(buffer->AddChunk(1, SmallChunk(2, 5), 0), CanReplayAdd::OK);
    zassert_false(buffer->CanLoop());

    zassert_true(PopNext(*buffer).has_value());
    zassert_equal(buffer->Room(), SMALL_RECORD, "Three records wait in a buffer of four");
    zassert_equal(buffer->AddChunk(2, SmallChunk(1, 7), 0), CanReplayAdd::OK);

    // The loop never comes back.
    zassert_false(buffer->CanLoop());
    buffer->Finish();
    for(uint32_t id = 4; id <= 7; ++id) {
        const auto popped = PopNext(*buffer);
        zassert_true(popped.has_value());
        zassert_equal(popped->frame.id, id);
    }
    zassert_true(buffer->IsDone());
    zassert_equal(buffer->Loops(), 0U);
}

ZTEST(can_replay_buffer, test_RecordsThatWrapTheStorageReadBackWhole) {
    // 53 bytes: not a power of two, and the records (5 to 12 bytes) straddle the end of it.
    auto buffer = MakeBuffer(53);
    zassert_true(buffer->Start(100, false, 0));

    uint32_t next_to_add = 1;
    uint32_t next_to_play = 1;

    for(int round = 0; round < 40; ++round) {
        std::vector<uint8_t> chunk;
        const size_t length = 1 + (static_cast<size_t>(round) % 8);
        const auto payload = MakePayload(length, static_cast<uint8_t>(next_to_add));
        Append(chunk, 5, { .id = next_to_add, .dlc = static_cast<uint8_t>(length), .payload = payload });

        if(chunk.size() > buffer->Room()) {
            // Make room by playing.
            while(chunk.size() > buffer->Room()) {
                const auto popped = PopNext(*buffer);
                zassert_true(popped.has_value());
                zassert_equal(popped->frame.id, next_to_play);

                const size_t expected_length = 1 + ((next_to_play - 1) % 8);
                zassert_equal(popped->frame.data.size(), expected_length);
                zassert_equal(popped->frame.data[0], static_cast<uint8_t>(next_to_play));
                ++next_to_play;
            }
        }

        zassert_equal(
            buffer->AddChunk(static_cast<uint16_t>(round), chunk, 0), CanReplayAdd::OK, "Round %d, room %zu", round,
            buffer->Room()
        );
        ++next_to_add;
    }

    buffer->Finish();
    while(next_to_play < next_to_add) {
        const auto popped = PopNext(*buffer);
        zassert_true(popped.has_value());
        zassert_equal(popped->frame.id, next_to_play);
        ++next_to_play;
    }
    zassert_true(buffer->IsDone());
}

ZTEST(can_replay_buffer, test_StartTakesOnlyASpeedInRange) {
    auto buffer = MakeBuffer();

    zassert_false(buffer->Start(0, false, 0));
    zassert_false(buffer->Start(CanReplayBuffer::MAX_SPEED_PCT + 1, false, 0));
    zassert_false(buffer->IsActive());
    zassert_equal(buffer->AddChunk(0, SmallChunk(1), 0), CanReplayAdd::BAD_STATE);

    zassert_true(buffer->Start(CanReplayBuffer::MIN_SPEED_PCT, false, 0));
    zassert_equal(buffer->AddChunk(0, Chunk({ 10 }), 0), CanReplayAdd::OK);
    zassert_equal(*buffer->NextDueUs(), 1000U, "1 % takes a hundred times as long");

    // A refused start leaves the replay that runs alone.
    zassert_false(buffer->Start(0, true, 0));
    zassert_true(buffer->IsActive());
    zassert_equal(*buffer->NextDueUs(), 1000U);

    zassert_true(buffer->Start(CanReplayBuffer::MAX_SPEED_PCT, false, 0));
    zassert_equal(buffer->AddChunk(0, Chunk({ 10'000 }), 0), CanReplayAdd::OK);
    zassert_equal(*buffer->NextDueUs(), 100U, "100 times as fast");
}

ZTEST(can_replay_buffer, test_StopEmptiesTheBufferAndKeepsTheCounts) {
    auto buffer = MakeBuffer(10 * SMALL_RECORD);
    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->AddChunk(0, SmallChunk(5), 0), CanReplayAdd::OK);
    zassert_true(PopNext(*buffer).has_value());

    buffer->Stop();

    zassert_false(buffer->IsActive());
    zassert_equal(buffer->Room(), 50U);
    zassert_false(buffer->HasFrames());
    zassert_false(buffer->IsUnderrun());
    zassert_false(buffer->IsDone());
    zassert_false(buffer->NextDueUs().has_value());
    zassert_false(buffer->Pop(FOREVER).has_value());
    zassert_equal(buffer->AddChunk(1, SmallChunk(1), 0), CanReplayAdd::BAD_STATE);
    zassert_equal(buffer->FramesPlayed(), 1U, "The counts are the final report's");

    // A new start begins from nothing.
    zassert_true(buffer->Start(100, false, 5));
    zassert_equal(buffer->FramesPlayed(), 0U);
    zassert_equal(buffer->LateFrames(), 0U);
    zassert_equal(buffer->Underruns(), 0U);
    zassert_equal(buffer->AddChunk(0, Chunk({ 100 }), 5), CanReplayAdd::OK);
    PopOnTime(*buffer, 1, 105);
}

ZTEST(can_replay_buffer, test_StartingAgainDropsTheOldReplay) {
    auto buffer = MakeBuffer(10 * SMALL_RECORD);
    zassert_true(buffer->Start(100, true, 0));
    zassert_equal(buffer->AddChunk(0, SmallChunk(5), 0), CanReplayAdd::OK);
    zassert_true(PopNext(*buffer).has_value());
    buffer->Finish();

    zassert_true(buffer->Start(200, false, 100));
    zassert_false(buffer->IsFinished());
    zassert_false(buffer->CanLoop());
    zassert_equal(buffer->Room(), 50U);
    zassert_equal(buffer->AddChunk(0, Chunk({ 1000 }, 7), 100), CanReplayAdd::OK);
    PopOnTime(*buffer, 7, 600);
}

ZTEST(can_replay_buffer, test_AFrameCountsAsPlayedWhenHandedOut) {
    auto buffer = MakeBuffer();
    zassert_true(buffer->Start(100, false, 0));
    zassert_equal(buffer->AddChunk(0, SmallChunk(5), 0), CanReplayAdd::OK);

    for(uint32_t played = 1; played <= 5; ++played) {
        zassert_true(PopNext(*buffer).has_value());
        zassert_equal(buffer->FramesPlayed(), played);
    }
}

ZTEST(can_replay_buffer, test_ABufferNeedsMemory) {
    zassert_is_null(CanReplayBuffer::Create(nullptr, 1024).get());
    zassert_is_null(CanReplayBuffer::Create(std::pmr::get_default_resource(), 0).get());

    // A resource with nothing to give throws; Create() answers nullptr.
    zassert_is_null(CanReplayBuffer::Create(std::pmr::null_memory_resource(), 1024).get());

    auto buffer = CanReplayBuffer::Create(std::pmr::get_default_resource(), 1024);
    zassert_not_null(buffer.get());
    zassert_equal(buffer->Capacity(), 1024U);
    zassert_equal(buffer->Room(), 1024U);
}
