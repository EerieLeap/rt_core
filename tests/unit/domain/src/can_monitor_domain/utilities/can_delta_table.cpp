#include <zephyr/ztest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

#include "can_monitor_test_utils.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA

    #include "domain/can_monitor_domain/utilities/can_delta_table.h"

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_monitor_delta_table, NULL, NULL, NULL, NULL, NULL);

namespace {

/** @brief An encoder table, a decoder table over its own storage, and the app's view of every frame sent. */
class Pair {
private:
    std::unique_ptr<CanDeltaTable> encoder_table_;
    std::unique_ptr<CanDeltaTable> decoder_table_;
    CanDeltaDecoder decoder_;

    static std::unique_ptr<CanDeltaTable> Make(size_t capacity) {
        auto table = CanDeltaTable::Create(capacity, std::pmr::get_default_resource());
        zassert_not_null(table.get());

        return table;
    }

public:
    explicit Pair(size_t capacity) :
        encoder_table_(Make(capacity)),
        decoder_table_(Make(capacity)),
        decoder_(*decoder_table_) {}

    CanDeltaTable& Encoder() {
        return *encoder_table_;
    }
    CanDeltaDecoder& Decoder() {
        return decoder_;
    }

    /** @brief Encodes @p frame, parses and decodes the record, checks the frame came back whole; returns the kind. */
    CanRecordKind Send(const CanRecordFrame& frame, uint64_t dt = 7) {
        std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
        const size_t size = encoder_table_->Encode(buffer, dt, frame);
        zassert_true(size > 0);

        const auto record = ParseRecord(std::span<const uint8_t>(buffer).first(size));
        zassert_true(record.has_value());
        zassert_equal(record->size, size);
        zassert_equal(record->dt_us, dt);

        std::array<uint8_t, CAN_MAX_PAYLOAD_SIZE> payload{};
        const auto length = decoder_.Apply(*record, payload);
        zassert_true(length.has_value());
        zassert_equal(*length, frame.payload.size());
        zassert_true(std::equal(frame.payload.begin(), frame.payload.end(), payload.begin()));

        zassert_equal(record->id, frame.id);
        zassert_equal(record->ide, frame.ide);
        zassert_equal(record->dlc, frame.dlc);
        zassert_equal(record->fdf, frame.fdf);
        zassert_equal(record->brs, frame.brs);
        zassert_equal(record->rtr, frame.rtr);
        zassert_equal(record->esi, frame.esi);
        zassert_equal(record->tx, frame.tx);
        zassert_equal(record->tx_failed, frame.tx_failed);

        return record->kind;
    }
};

uint8_t DlcOf(size_t length) {
    uint8_t dlc = 0;
    while(FD_LENGTHS[dlc] != length)
        ++dlc;

    return dlc;
}

CanRecordFrame ClassicFrame(uint32_t id, std::span<const uint8_t> payload, bool ide = false) {
    return { .id = id, .dlc = static_cast<uint8_t>(payload.size()), .ide = ide, .payload = payload };
}

} // namespace

ZTEST(can_monitor_delta_table, test_CreateChecksItsCapacityAndFreesItsMemory) {
    auto* memory = std::pmr::get_default_resource();

    zassert_is_null(CanDeltaTable::Create(0, memory).get());
    zassert_is_null(CanDeltaTable::Create(CanDeltaTable::MAX_CAPACITY + 1, memory).get());
    zassert_is_null(CanDeltaTable::Create(16, nullptr).get());

    auto table = CanDeltaTable::Create(1, memory);
    zassert_not_null(table.get());
    zassert_equal(table->Capacity(), 1U);
    zassert_equal(table->Size(), 0U);
}

ZTEST(can_monitor_delta_table, test_AFirstFrameGoesWholeThenOnlyWhatChanged) {
    Pair pair(8);
    auto payload = MakePayload(8);

    zassert_true(pair.Send(ClassicFrame(0x100, payload)) == CanRecordKind::FRAME);
    zassert_equal(pair.Encoder().Size(), 1U);

    // The same payload: a mask and nothing else.
    zassert_true(pair.Send(ClassicFrame(0x100, payload)) == CanRecordKind::CHANGED_BYTES);

    payload[3] ^= 0x55;
    payload[7] ^= 0x01;
    zassert_true(pair.Send(ClassicFrame(0x100, payload)) == CanRecordKind::CHANGED_BYTES);

    // The entry follows the latest payload: the same again is nothing changed.
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    const size_t size = pair.Encoder().Encode(buffer, 1, ClassicFrame(0x100, payload));
    const auto record = ParseRecord(std::span<const uint8_t>(buffer).first(size));
    zassert_true(record.has_value());
    zassert_true(record->kind == CanRecordKind::CHANGED_BYTES);
    zassert_true(record->payload.empty());
    zassert_equal(record->masks.size(), 1U);
    zassert_equal(record->masks[0], 0U);
}

ZTEST(can_monitor_delta_table, test_AChangedBytesRecordIsSmallerThanTheWholeOne) {
    Pair pair(8);
    const auto payload = MakePayload(8);
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> whole{};
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> delta{};

    const size_t whole_size = pair.Encoder().Encode(whole, 1, ClassicFrame(0x100, payload));
    const size_t delta_size = pair.Encoder().Encode(delta, 1, ClassicFrame(0x100, payload));
    zassert_equal(whole_size, 1U + 1U + 2U + 8U);
    zassert_equal(delta_size, 2U + 1U + 2U + 1U);
}

ZTEST(can_monitor_delta_table, test_ATableOfEntriesKeepsEachIdApart) {
    Pair pair(64);

    for(uint32_t round = 0; round < 3; ++round) {
        for(uint32_t id = 0; id < 40; ++id) {
            auto payload = MakePayload(8, static_cast<uint8_t>(id));
            payload[0] ^= static_cast<uint8_t>(round);
            const auto kind = pair.Send(ClassicFrame(id * 0x20, payload));
            zassert_true(
                kind == (round == 0 ? CanRecordKind::FRAME : CanRecordKind::CHANGED_BYTES), "id %u round %u", id, round
            );
        }
    }

    zassert_equal(pair.Encoder().Size(), 40U);
}

ZTEST(can_monitor_delta_table, test_IdeSeparatesTheEntries) {
    Pair pair(8);
    const auto standard = MakePayload(4, 1);
    const auto extended = MakePayload(4, 50);

    zassert_true(pair.Send(ClassicFrame(0x123, standard)) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x123, extended, true)) == CanRecordKind::FRAME);
    zassert_equal(pair.Encoder().Size(), 2U);

    zassert_true(pair.Send(ClassicFrame(0x123, standard)) == CanRecordKind::CHANGED_BYTES);
    zassert_true(pair.Send(ClassicFrame(0x123, extended, true)) == CanRecordKind::CHANGED_BYTES);
}

ZTEST(can_monitor_delta_table, test_ALengthChangeGoesWhole) {
    Pair pair(8);

    const auto eight = MakePayload(8);
    const auto four = MakePayload(4);
    auto four_changed = MakePayload(4);
    four_changed[0] ^= 1;

    zassert_true(pair.Send(ClassicFrame(0x10, eight)) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x10, four)) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x10, four_changed)) == CanRecordKind::CHANGED_BYTES);
    zassert_true(pair.Send(ClassicFrame(0x10, eight)) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x10, eight)) == CanRecordKind::CHANGED_BYTES);
    zassert_equal(pair.Encoder().Size(), 1U);
}

ZTEST(can_monitor_delta_table, test_EveryFdLengthRoundTrips) {
    Pair pair(8);

    for(const size_t length : FD_LENGTHS) {
        if(length == 0)
            continue;

        auto payload = MakePayload(length, static_cast<uint8_t>(length));
        const CanRecordFrame first{ .id = 0x44, .dlc = DlcOf(length), .fdf = true, .brs = true, .payload = payload };
        zassert_true(pair.Send(first) == CanRecordKind::FRAME);

        // One byte in the middle changes. The changed-bytes record is 6 + masks bytes, the whole one
        // 4 + length: smaller from 4 bytes on.
        payload[length / 2] ^= 0x80;
        const CanRecordFrame second{ .id = 0x44, .dlc = DlcOf(length), .fdf = true, .brs = true, .payload = payload };
        const auto kind = pair.Send(second);
        zassert_true(kind == (length >= 4 ? CanRecordKind::CHANGED_BYTES : CanRecordKind::FRAME), "length %zu", length);
    }
}

ZTEST(can_monitor_delta_table, test_ADeltaThatIsNotSmallerGoesWhole) {
    Pair pair(8);

    // One data byte: whole is head, dt, ID, byte (5); changed is head, ext, dt, ID, mask, byte (7).
    auto one = MakePayload(1);
    zassert_true(pair.Send(ClassicFrame(0x1, one)) == CanRecordKind::FRAME);
    one[0] ^= 0xFF;
    zassert_true(pair.Send(ClassicFrame(0x1, one)) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x1, one)) == CanRecordKind::FRAME);

    // 64 changed bytes: the masks cost 8 more than the whole record saves.
    const auto before = MakePayload(64, 1);
    std::vector<uint8_t> after(64);
    for(size_t i = 0; i < 64; ++i)
        after[i] = static_cast<uint8_t>(before[i] + 1);
    const CanRecordFrame fd_before{ .id = 0x2, .dlc = 15, .fdf = true, .payload = before };
    const CanRecordFrame fd_after{ .id = 0x2, .dlc = 15, .fdf = true, .payload = after };
    zassert_true(pair.Send(fd_before) == CanRecordKind::FRAME);
    zassert_true(pair.Send(fd_after) == CanRecordKind::FRAME);
    zassert_true(pair.Send(fd_after) == CanRecordKind::CHANGED_BYTES);
}

ZTEST(can_monitor_delta_table, test_RemoteFramesNeverUseTheTable) {
    Pair pair(8);
    const auto payload = MakePayload(8);

    const CanRecordFrame remote{ .id = 0x20, .dlc = 8, .rtr = true };
    zassert_true(pair.Send(remote) == CanRecordKind::FRAME);
    zassert_true(pair.Send(remote) == CanRecordKind::FRAME);
    zassert_equal(pair.Encoder().Size(), 0U);

    // A data frame of the same ID is a first frame, and a remote one in between changes nothing.
    zassert_true(pair.Send(ClassicFrame(0x20, payload)) == CanRecordKind::FRAME);
    zassert_true(pair.Send(remote) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x20, payload)) == CanRecordKind::CHANGED_BYTES);
    zassert_equal(pair.Encoder().Size(), 1U);
}

ZTEST(can_monitor_delta_table, test_EmptyPayloadsGoWholeAndStayOut) {
    Pair pair(8);

    zassert_true(pair.Send(ClassicFrame(0x30, std::span<const uint8_t>())) == CanRecordKind::FRAME);
    zassert_true(pair.Send(ClassicFrame(0x30, std::span<const uint8_t>())) == CanRecordKind::FRAME);
    zassert_equal(pair.Encoder().Size(), 0U);
}

ZTEST(can_monitor_delta_table, test_FlagsTravelWithTheRecord) {
    Pair pair(8);
    const auto payload = MakePayload(8);

    CanRecordFrame frame = ClassicFrame(0x40, payload);
    frame.esi = true;
    frame.tx = true;
    frame.tx_failed = true;

    zassert_true(pair.Send(frame) == CanRecordKind::FRAME);
    zassert_true(pair.Send(frame) == CanRecordKind::CHANGED_BYTES);

    frame.tx = false;
    zassert_true(pair.Send(frame) == CanRecordKind::CHANGED_BYTES);
}

ZTEST(can_monitor_delta_table, test_AFullTableSendsTheRestWhole) {
    Pair pair(2);
    const auto payload = MakePayload(8);

    for(uint32_t id = 1; id <= 3; ++id)
        zassert_true(pair.Send(ClassicFrame(id, payload)) == CanRecordKind::FRAME);
    zassert_equal(pair.Encoder().Size(), 2U);

    // Entries are never evicted: 1 and 2 keep their place, 3 goes whole each time.
    for(uint32_t round = 0; round < 3; ++round) {
        zassert_true(pair.Send(ClassicFrame(1, payload)) == CanRecordKind::CHANGED_BYTES);
        zassert_true(pair.Send(ClassicFrame(2, payload)) == CanRecordKind::CHANGED_BYTES);
        zassert_true(pair.Send(ClassicFrame(3, payload)) == CanRecordKind::FRAME);
    }
    zassert_equal(pair.Encoder().Size(), 2U);
}

ZTEST(can_monitor_delta_table, test_ResetSendsEveryIdWholeAgain) {
    Pair pair(8);
    const auto payload = MakePayload(8);

    for(uint32_t id = 1; id <= 3; ++id) {
        zassert_true(pair.Send(ClassicFrame(id, payload)) == CanRecordKind::FRAME);
        zassert_true(pair.Send(ClassicFrame(id, payload)) == CanRecordKind::CHANGED_BYTES);
    }

    pair.Encoder().Reset();
    zassert_equal(pair.Encoder().Size(), 0U);
    zassert_equal(pair.Encoder().Lookup(1, false).size(), 0U);

    // The decoder empties its table at the gap or tap record that goes with it.
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    const size_t size = EncodeTap(buffer, 0, CanTapMark::MAY_HAVE_REPEATED);
    const auto tap = ParseRecord(std::span<const uint8_t>(buffer).first(size));
    zassert_true(tap.has_value());
    std::array<uint8_t, CAN_MAX_PAYLOAD_SIZE> scratch{};
    zassert_equal(pair.Decoder().Apply(*tap, scratch).value(), 0U);

    for(uint32_t id = 1; id <= 3; ++id) {
        zassert_true(pair.Send(ClassicFrame(id, payload)) == CanRecordKind::FRAME);
        zassert_true(pair.Send(ClassicFrame(id, payload)) == CanRecordKind::CHANGED_BYTES);
    }
}

ZTEST(can_monitor_delta_table, test_ACapacityFullOfCollidingLookingIdsStaysCorrect) {
    constexpr size_t IDS = 256;
    Pair pair(IDS);

    // Identifiers that share low bits, as a vehicle's do, in both ID widths.
    for(uint32_t round = 0; round < 3; ++round) {
        for(uint32_t i = 0; i < IDS; ++i) {
            const bool ide = (i % 2) == 1;
            const uint32_t id = ide ? (0x18FF0000U + ((i / 2) << 8)) : (0x100U + ((i / 2) << 3));
            const size_t length = 1 + ((i * 3) % 8);
            auto payload = MakePayload(length, static_cast<uint8_t>(i));
            payload.back() = static_cast<uint8_t>(round * 17);

            const auto kind = pair.Send(ClassicFrame(id, payload, ide));
            if(round == 0)
                zassert_true(kind == CanRecordKind::FRAME);
        }
    }

    zassert_equal(pair.Encoder().Size(), IDS);
}

ZTEST(can_monitor_delta_table, test_ARandomStreamRoundTrips) {
    Pair pair(64);

    uint32_t state = 12345;
    auto next = [&state] {
        state = (state * 1664525U) + 1013904223U;
        return state >> 8;
    };

    std::array<std::vector<uint8_t>, 48> latest;
    size_t changed = 0;
    size_t whole = 0;

    for(size_t step = 0; step < 3000; ++step) {
        const size_t slot = next() % latest.size();
        auto& payload = latest[slot];

        const uint32_t roll = next() % 100;
        if(payload.empty() || roll < 5) {
            // A new payload, sometimes of another length (FD lengths too).
            payload = MakePayload(FD_LENGTHS[1 + (next() % 15)], static_cast<uint8_t>(next()));
        } else {
            for(uint32_t k = next() % 3; k > 0; --k)
                payload[next() % payload.size()] = static_cast<uint8_t>(next());
        }

        const bool ide = (slot % 2) == 0;
        const CanRecordFrame frame{
            .id = ide ? 0x1000U + static_cast<uint32_t>(slot) : static_cast<uint32_t>(slot),
            .dlc = DlcOf(payload.size()),
            .ide = ide,
            .fdf = true,
            .payload = payload,
        };

        if(pair.Send(frame) == CanRecordKind::CHANGED_BYTES)
            ++changed;
        else
            ++whole;
    }

    zassert_true(changed > whole, "%zu changed, %zu whole", changed, whole);
}

ZTEST(can_monitor_delta_table, test_TheDecoderRefusesWhatItCannotApply) {
    Pair pair(8);
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> other_buffer{};
    std::array<uint8_t, CAN_MAX_PAYLOAD_SIZE> out{};

    const auto previous = MakePayload(8, 1);
    auto current = previous;
    current[2] = 0x99;

    // A changed-bytes record for an ID the decoder never saw.
    size_t size = EncodeChangedBytes(buffer, 0, ClassicFrame(0x7, current), previous);
    auto record = ParseRecord(std::span<const uint8_t>(buffer).first(size));
    zassert_true(record.has_value());
    zassert_false(pair.Decoder().Apply(*record, out).has_value());

    // An entry of another length.
    const auto four = MakePayload(4);
    size = EncodeFrame(other_buffer, 0, ClassicFrame(0x7, four));
    const auto whole = ParseRecord(std::span<const uint8_t>(other_buffer).first(size));
    zassert_true(whole.has_value());
    zassert_true(pair.Decoder().Apply(*whole, out).has_value());
    zassert_false(pair.Decoder().Apply(*record, out).has_value());

    // An out that is too small.
    zassert_false(pair.Decoder().Apply(*whole, std::span<uint8_t>(out).first(3)).has_value());

    // Records that are not frames are no business of the table's.
    size = EncodeEnd(other_buffer, 0, CanEndReason::CLOSED);
    record = ParseRecord(std::span<const uint8_t>(other_buffer).first(size));
    zassert_true(record.has_value());
    zassert_equal(pair.Decoder().Apply(*record, out).value(), 0U);
}

ZTEST(can_monitor_delta_table, test_EncodeTouchesNothingWhenItFails) {
    Pair pair(8);
    const auto payload = MakePayload(8);
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};

    zassert_equal(pair.Encoder().Encode(std::span<uint8_t>(buffer).first(3), 0, ClassicFrame(0x1, payload)), 0U);
    zassert_equal(pair.Encoder().Size(), 0U);

    const auto bad = MakePayload(5);
    zassert_equal(pair.Encoder().Encode(buffer, 0, { .id = 1, .dlc = 8, .payload = bad }), 0U);
    zassert_equal(pair.Encoder().Size(), 0U);
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
