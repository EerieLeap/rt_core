#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>

#include <zephyr/ztest.h>

#include "subsys/random/rng.h"

using eerie_leap::subsys::random::RandomWord;
using eerie_leap::subsys::random::Rng;

namespace {

constexpr uint8_t GUARD = 0xA5;
constexpr size_t GUARD_LEN = 8;
constexpr size_t MAX_PAYLOAD = 64;

constexpr int SAMPLE_COUNT = 256;
constexpr int FILL_ROUNDS = 16;

// The payload sits between two guard regions so an overrun in either direction is caught.
struct GuardedBuffer {
    std::array<uint8_t, GUARD_LEN + MAX_PAYLOAD + GUARD_LEN> storage;

    GuardedBuffer() { storage.fill(GUARD); }

    uint8_t* payload() { return storage.data() + GUARD_LEN; }

    bool GuardsIntact(size_t payload_len) const {
        for(size_t i = 0; i < GUARD_LEN; ++i) {
            if(storage[i] != GUARD || storage[GUARD_LEN + payload_len + i] != GUARD)
                return false;
        }

        return true;
    }
};

template<RandomWord T>
struct BitCoverage {
    T ones = 0;
    T zeros = 0;

    bool IsComplete() const {
        return ones == std::numeric_limits<T>::max() && zeros == std::numeric_limits<T>::max();
    }
};

// A generator that only fills part of the word leaves the untouched bits stuck at one value.
template<RandomWord T>
BitCoverage<T> SampleBits(bool secure = false) {
    BitCoverage<T> coverage;

    for(int i = 0; i < SAMPLE_COUNT; ++i) {
        T sample = Rng::Get<T>(secure);

        coverage.ones |= sample;
        coverage.zeros |= static_cast<T>(~sample);
    }

    return coverage;
}

// Returns the first payload index never written, or MAX_PAYLOAD when all of them were.
size_t FindUnwrittenByte(bool secure) {
    GuardedBuffer buffer;
    std::array<bool, MAX_PAYLOAD> written {};

    // A byte landing on the guard value in every round is a ~2^-128 event.
    for(int round = 0; round < FILL_ROUNDS; ++round) {
        std::fill_n(buffer.payload(), MAX_PAYLOAD, GUARD);
        Rng::Get(buffer.payload(), MAX_PAYLOAD, secure);

        for(size_t i = 0; i < MAX_PAYLOAD; ++i)
            written[i] = written[i] || buffer.payload()[i] != GUARD;
    }

    auto first_unwritten = std::find(written.begin(), written.end(), false);

    return static_cast<size_t>(std::distance(written.begin(), first_unwritten));
}

} // namespace

ZTEST_SUITE(rng, NULL, NULL, NULL, NULL, NULL);

ZTEST(rng, test_RandomWord_accepts_unsigned_integers) {
    zassert_true(RandomWord<uint8_t>);
    zassert_true(RandomWord<uint16_t>);
    zassert_true(RandomWord<uint32_t>);
    zassert_true(RandomWord<uint64_t>);
    zassert_true(RandomWord<unsigned int>);
    zassert_true(RandomWord<size_t>);
}

ZTEST(rng, test_RandomWord_rejects_bool) {
    // Only 0 and 1 are valid representations of a bool, so raw random bytes cannot fill one.
    zassert_false(RandomWord<bool>);
}

ZTEST(rng, test_RandomWord_rejects_signed_integers) {
    zassert_false(RandomWord<int8_t>);
    zassert_false(RandomWord<int16_t>);
    zassert_false(RandomWord<int32_t>);
    zassert_false(RandomWord<int64_t>);
}

ZTEST(rng, test_RandomWord_rejects_non_integer_types) {
    zassert_false(RandomWord<float>);
    zassert_false(RandomWord<double>);
    zassert_false(RandomWord<void*>);
    zassert_false(RandomWord<GuardedBuffer>);
}

ZTEST(rng, test_Get_writes_every_requested_byte) {
    auto unwritten = FindUnwrittenByte(false);

    zassert_equal(unwritten, MAX_PAYLOAD, "payload byte %u was never written", (unsigned int)unwritten);
}

ZTEST(rng, test_Get_secure_writes_every_requested_byte) {
    auto unwritten = FindUnwrittenByte(true);

    zassert_equal(unwritten, MAX_PAYLOAD, "payload byte %u was never written", (unsigned int)unwritten);
}

ZTEST(rng, test_Get_does_not_write_outside_the_requested_length) {
    constexpr size_t lengths[] = {1, 2, 3, 4, 5, 7, 8, 15, 16, 33, MAX_PAYLOAD};

    for(auto length : lengths) {
        GuardedBuffer buffer;

        Rng::Get(buffer.payload(), length);

        zassert_true(buffer.GuardsIntact(length), "guards clobbered for length %u", (unsigned int)length);
    }
}

ZTEST(rng, test_Get_with_zero_length_writes_nothing) {
    GuardedBuffer buffer;
    auto expected = buffer.storage;

    Rng::Get(buffer.payload(), 0);

    zassert_mem_equal(buffer.storage.data(), expected.data(), expected.size());
}

ZTEST(rng, test_Get_covers_every_bit_of_uint8) {
    zassert_true(SampleBits<uint8_t>().IsComplete());
}

ZTEST(rng, test_Get_covers_every_bit_of_uint16) {
    zassert_true(SampleBits<uint16_t>().IsComplete());
}

ZTEST(rng, test_Get_covers_every_bit_of_uint32) {
    zassert_true(SampleBits<uint32_t>().IsComplete());
}

ZTEST(rng, test_Get_covers_every_bit_of_uint64) {
    zassert_true(SampleBits<uint64_t>().IsComplete());
}

ZTEST(rng, test_Get_secure_covers_every_bit) {
    zassert_true(SampleBits<uint32_t>(true).IsComplete());
}

ZTEST(rng, test_Get_returns_varying_values) {
    auto first = Rng::Get<uint32_t>();

    bool differs = false;
    for(int i = 0; i < 32 && !differs; ++i)
        differs = Rng::Get<uint32_t>() != first;

    zassert_true(differs);
}

ZTEST(rng, test_Get_returns_the_requested_width) {
    zassert_equal(sizeof(Rng::Get<uint8_t>()), 1);
    zassert_equal(sizeof(Rng::Get<uint16_t>()), 2);
    zassert_equal(sizeof(Rng::Get<uint32_t>()), 4);
    zassert_equal(sizeof(Rng::Get<uint64_t>()), 8);
}
