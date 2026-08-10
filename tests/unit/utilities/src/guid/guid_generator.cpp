#include <cstring>
#include <set>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "utilities/guid/guid.hpp"
#include "utilities/guid/guid_generator.h"

using namespace eerie_leap::utilities::guid;

ZTEST_SUITE(guid_generator, NULL, NULL, NULL, NULL, NULL);

ZTEST(guid_generator, test_first_guid_has_counter_one) {
    GuidGenerator generator;
    Guid guid = generator.Generate();

    zassert_equal(guid.counter, 1);
    zassert_not_equal(guid.AsUint64(), 0);
}

ZTEST(guid_generator, test_counter_increments_sequentially) {
    GuidGenerator generator;

    for(uint16_t expected = 1; expected <= 100; ++expected)
        zassert_equal(generator.Generate().counter, expected);
}

ZTEST(guid_generator, test_guids_are_unique) {
    GuidGenerator generator;
    Guid guid0 = generator.Generate();

    uint16_t device_hash = guid0.device_hash;
    std::set<uint16_t> counters;
    counters.insert(guid0.counter);
    std::set<uint64_t> guid_nums;
    guid_nums.insert(guid0.AsUint64());

    for(int i = 1; i < 100; ++i) {
        Guid guid = generator.Generate();

        zassert_false(counters.count(guid.counter));
        counters.insert(guid.counter);
        zassert_false(guid_nums.count(guid.AsUint64()));
        guid_nums.insert(guid.AsUint64());

        zassert_equal(guid.device_hash, device_hash);
    }
}

ZTEST(guid_generator, test_device_hash_is_stable_for_a_generator) {
    GuidGenerator generator;

    const uint16_t device_hash = generator.Generate().device_hash;
    for(int i = 0; i < 100; ++i)
        zassert_equal(generator.Generate().device_hash, device_hash);
}

ZTEST(guid_generator, test_generators_count_independently) {
    GuidGenerator first;
    GuidGenerator second;

    zassert_equal(first.Generate().counter, 1);
    zassert_equal(first.Generate().counter, 2);
    zassert_equal(second.Generate().counter, 1);
    zassert_equal(first.Generate().counter, 3);
}

ZTEST(guid_generator, test_counter_wraps_at_16_bits) {
    GuidGenerator generator;

    for(uint32_t expected = 1; expected <= 0xFFFF; ++expected)
        zassert_equal(generator.Generate().counter, static_cast<uint16_t>(expected));

    zassert_equal(generator.Generate().counter, 0);
    zassert_equal(generator.Generate().counter, 1);
}

ZTEST(guid_generator, test_timestamp_comes_from_uptime) {
    GuidGenerator generator;

    uint32_t before = k_uptime_get_32();
    Guid guid = generator.Generate();
    uint32_t after = k_uptime_get_32();

    zassert_true(guid.timestamp >= before);
    zassert_true(guid.timestamp <= after);
}

ZTEST(guid_generator, test_timestamps_are_non_decreasing) {
    GuidGenerator generator;

    uint32_t previous = generator.Generate().timestamp;
    for(int i = 0; i < 100; ++i) {
        uint32_t current = generator.Generate().timestamp;

        zassert_true(current >= previous, "timestamp went backwards at iteration %d", i);
        previous = current;
    }
}

ZTEST(guid_generator, test_Guid_is_packed) {
    static_assert(sizeof(Guid) == sizeof(uint64_t));

    zassert_equal(sizeof(Guid), 8);
    zassert_equal(alignof(Guid), 1);
}

ZTEST(guid_generator, test_AsUint64_matches_object_representation) {
    Guid guid { .device_hash = 0x1234, .counter = 0x5678, .timestamp = 0x9ABCDEF0 };

    uint64_t expected = 0;
    memcpy(&expected, &guid, sizeof(guid));

    zassert_equal(guid.AsUint64(), expected);
}

ZTEST(guid_generator, test_AsUint64_changes_with_every_field) {
    Guid base { .device_hash = 1, .counter = 2, .timestamp = 3 };
    Guid other_device { .device_hash = 9, .counter = 2, .timestamp = 3 };
    Guid other_counter { .device_hash = 1, .counter = 9, .timestamp = 3 };
    Guid other_timestamp { .device_hash = 1, .counter = 2, .timestamp = 9 };

    zassert_not_equal(base.AsUint64(), other_device.AsUint64());
    zassert_not_equal(base.AsUint64(), other_counter.AsUint64());
    zassert_not_equal(base.AsUint64(), other_timestamp.AsUint64());
}
