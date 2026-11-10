#pragma once

#include <zephyr/ztest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "domain/can_monitor_domain/utilities/byte_ring.h"
#include "domain/can_monitor_domain/utilities/can_record.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"

namespace can_monitor_test {

using namespace eerie_leap::domain::can_monitor_domain::utilities;

/** @brief The payload bytes a DLC carries, spelled out so the tests don't lean on can_dlc_to_bytes. */
inline constexpr std::array<size_t, 16> FD_LENGTHS = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

/** @brief n bytes that differ from byte to byte and from seed to seed. */
inline std::vector<uint8_t> MakePayload(size_t size, uint8_t seed = 1) {
    std::vector<uint8_t> payload(size);
    for(size_t i = 0; i < size; ++i)
        payload[i] = static_cast<uint8_t>(seed + (i * 7));

    return payload;
}

/** @brief What a ring held, parsed. The records' spans point into @c bytes. */
struct Drained {
    std::vector<uint8_t> bytes;
    std::vector<CanRecord> records;
};

/** @brief Takes everything out of @p ring and parses it as records. */
inline Drained Drain(ByteRing& ring) {
    Drained drained;
    drained.bytes.resize(ring.Size());
    zassert_true(ring.Peek(0, drained.bytes));
    zassert_true(ring.Consume(drained.bytes.size()));

    size_t pos = 0;
    while(pos < drained.bytes.size()) {
        const auto record = ParseRecord(std::span<const uint8_t>(drained.bytes).subspan(pos));
        zassert_true(record.has_value(), "ring holds a malformed record at %zu", pos);
        drained.records.push_back(*record);
        pos += record->size;
    }

    return drained;
}

inline std::unique_ptr<ByteRing> MakeRing(size_t capacity) {
    auto ring = ByteRing::Create(capacity, std::pmr::get_default_resource());
    zassert_not_null(ring.get());

    return ring;
}

} // namespace can_monitor_test
