#include <cstdint>
#include <cstring>
#include <vector>
#include <zephyr/ztest.h>

#include <zcbor_encode.h>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/cbor/cbor_size_calculator.hpp"

using eerie_leap::utilities::cbor::CborHelpers;
using eerie_leap::utilities::cbor::CborSizeCalc;

namespace {

// Encodes with zcbor and returns the number of bytes actually written, so the
// calculator can be validated against the encoder it is meant to predict.
template<typename EncodeFunc>
size_t EncodedSize(EncodeFunc encode) {
    uint8_t buffer[128];
    ZCBOR_STATE_E(state, 2, buffer, sizeof(buffer), 1);

    zassert_true(encode(state), "zcbor encoding failed");

    return static_cast<size_t>(state->payload - buffer);
}

} // namespace

ZTEST_SUITE(cbor_size_calculator, NULL, NULL, NULL, NULL, NULL);

ZTEST(cbor_size_calculator, test_SizeOfUint_boundaries) {
    zassert_equal(CborSizeCalc::SizeOfUint(0), 1);
    zassert_equal(CborSizeCalc::SizeOfUint(23), 1);
    zassert_equal(CborSizeCalc::SizeOfUint(24), 2);
    zassert_equal(CborSizeCalc::SizeOfUint(0xFF), 2);
    zassert_equal(CborSizeCalc::SizeOfUint(0x100), 3);
    zassert_equal(CborSizeCalc::SizeOfUint(0xFFFF), 3);
    zassert_equal(CborSizeCalc::SizeOfUint(0x10000), 5);
    zassert_equal(CborSizeCalc::SizeOfUint(0xFFFFFFFFULL), 5);
    zassert_equal(CborSizeCalc::SizeOfUint(0x100000000ULL), 9);
    zassert_equal(CborSizeCalc::SizeOfUint(UINT64_MAX), 9);
}

ZTEST(cbor_size_calculator, test_SizeOfUint_matches_zcbor) {
    const uint64_t values[] = { 0, 23, 24, 0xFF, 0x100, 0xFFFF, 0x10000, 0xFFFFFFFFULL, 0x100000000ULL, UINT64_MAX };

    for(const uint64_t value : values) {
        const size_t encoded = EncodedSize([value](zcbor_state_t* state) {
            return zcbor_uint64_put(state, value);
        });

        zassert_equal(CborSizeCalc::SizeOfUint(value), encoded, "mismatch for value %llu", (unsigned long long)value);
    }
}

ZTEST(cbor_size_calculator, test_SizeOfInt_positive) {
    zassert_equal(CborSizeCalc::SizeOfInt(0), 1);
    zassert_equal(CborSizeCalc::SizeOfInt(23), 1);
    zassert_equal(CborSizeCalc::SizeOfInt(24), 2);
    zassert_equal(CborSizeCalc::SizeOfInt(INT64_MAX), 9);
}

ZTEST(cbor_size_calculator, test_SizeOfInt_negative) {
    zassert_equal(CborSizeCalc::SizeOfInt(-1), 1);
    zassert_equal(CborSizeCalc::SizeOfInt(-24), 1);
    zassert_equal(CborSizeCalc::SizeOfInt(-25), 2);
    zassert_equal(CborSizeCalc::SizeOfInt(-256), 2);
    zassert_equal(CborSizeCalc::SizeOfInt(-257), 3);
    zassert_equal(CborSizeCalc::SizeOfInt(-65536), 3);
    zassert_equal(CborSizeCalc::SizeOfInt(-65537), 5);
    zassert_equal(CborSizeCalc::SizeOfInt(INT64_MIN), 9);
}

ZTEST(cbor_size_calculator, test_SizeOfInt_matches_zcbor) {
    const int64_t values[] = { 0, 23, 24, -1, -24, -25, -256, -257, -65536, -65537, INT64_MAX, INT64_MIN };

    for(const int64_t value : values) {
        const size_t encoded = EncodedSize([value](zcbor_state_t* state) {
            return zcbor_int64_put(state, value);
        });

        zassert_equal(CborSizeCalc::SizeOfInt(value), encoded, "mismatch for value %lld", (long long)value);
    }
}

ZTEST(cbor_size_calculator, test_SizeOfUintFixed) {
    zassert_equal(CborSizeCalc::SizeOfUintFixed(1), 2);
    zassert_equal(CborSizeCalc::SizeOfUintFixed(2), 3);
    zassert_equal(CborSizeCalc::SizeOfUintFixed(4), 5);
    zassert_equal(CborSizeCalc::SizeOfUintFixed(8), 9);
}

ZTEST(cbor_size_calculator, test_SizeOfLength_boundaries) {
    zassert_equal(CborSizeCalc::SizeOfLength(0), 1);
    zassert_equal(CborSizeCalc::SizeOfLength(23), 1);
    zassert_equal(CborSizeCalc::SizeOfLength(24), 2);
    zassert_equal(CborSizeCalc::SizeOfLength(0xFF), 2);
    zassert_equal(CborSizeCalc::SizeOfLength(0x100), 3);
    zassert_equal(CborSizeCalc::SizeOfLength(0xFFFF), 3);
    zassert_equal(CborSizeCalc::SizeOfLength(0x10000), 5);
}

ZTEST(cbor_size_calculator, test_SizeOfTstr) {
    static constexpr char K_SHORT[] = "abc";
    static constexpr char BOUNDARY[] = "12345678901234567890123"; // 23 chars
    static constexpr char K_LONG[] = "123456789012345678901234";  // 24 chars

    zassert_equal(CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(K_SHORT)), 4);
    zassert_equal(CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(BOUNDARY)), 24);
    zassert_equal(CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(K_LONG)), 26);
}

ZTEST(cbor_size_calculator, test_SizeOfTstr_empty) {
    static constexpr char K_EMPTY[] = "";

    zassert_equal(CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(K_EMPTY)), 1);
}

ZTEST(cbor_size_calculator, test_SizeOfTstr_matches_zcbor) {
    static constexpr char K_SHORT[] = "abc";
    static constexpr char BOUNDARY[] = "12345678901234567890123";
    static constexpr char K_LONG[] = "123456789012345678901234";

    const char* values[] = { "", K_SHORT, BOUNDARY, K_LONG };

    for(const char* value : values) {
        auto zstr = CborHelpers::ToZcborString(value);
        const size_t encoded = EncodedSize([&zstr](zcbor_state_t* state) {
            return zcbor_tstr_encode(state, &zstr);
        });

        zassert_equal(CborSizeCalc::SizeOfTstr(zstr), encoded, "mismatch for \"%s\"", value);
    }
}

ZTEST(cbor_size_calculator, test_SizeOfBool) {
    zassert_equal(CborSizeCalc::SizeOfBool(true), 1);
    zassert_equal(CborSizeCalc::SizeOfBool(false), 1);

    zassert_equal(CborSizeCalc::SizeOfBool(true), EncodedSize([](zcbor_state_t* state) {
        return zcbor_bool_put(state, true);
    }));
}

ZTEST(cbor_size_calculator, test_SizeOfFloat) {
    zassert_equal(CborSizeCalc::SizeOfFloat(0.0F), 5);
    zassert_equal(CborSizeCalc::SizeOfFloat(-1234.5F), 5);

    zassert_equal(CborSizeCalc::SizeOfFloat(3.5F), EncodedSize([](zcbor_state_t* state) {
        return zcbor_float32_put(state, 3.5F);
    }));
}

ZTEST(cbor_size_calculator, test_SizeOfDouble) {
    zassert_equal(CborSizeCalc::SizeOfDouble(0.0), 9);
    zassert_equal(CborSizeCalc::SizeOfDouble(-1234.5), 9);

    zassert_equal(CborSizeCalc::SizeOfDouble(3.5), EncodedSize([](zcbor_state_t* state) {
        return zcbor_float64_put(state, 3.5);
    }));
}

ZTEST(cbor_size_calculator, test_SizeOfArrayOverhead) {
    zassert_equal(CborSizeCalc::SizeOfArrayOverhead(0), 1);
    zassert_equal(CborSizeCalc::SizeOfArrayOverhead(23), 1);
    zassert_equal(CborSizeCalc::SizeOfArrayOverhead(24), 2);
    zassert_equal(CborSizeCalc::SizeOfArrayStart(24), CborSizeCalc::SizeOfArrayOverhead(24));
}

ZTEST(cbor_size_calculator, test_SizeOfArrayUniform) {
    zassert_equal(CborSizeCalc::SizeOfArrayUniform(0, 5), 1);
    zassert_equal(CborSizeCalc::SizeOfArrayUniform(3, 5), 16);
    zassert_equal(CborSizeCalc::SizeOfArrayUniform(24, 2), 50);
}

ZTEST(cbor_size_calculator, test_SizeOfArray_with_size_func) {
    const std::vector<uint64_t> items{ 1, 24, 0x10000 };

    const size_t size = CborSizeCalc::SizeOfArray(items, [](uint64_t item) {
        return CborSizeCalc::SizeOfUint(item);
    });

    zassert_equal(size, 1 + 1 + 2 + 5);
}

ZTEST(cbor_size_calculator, test_SizeOfArray_empty) {
    const std::vector<uint64_t> items;

    const size_t size = CborSizeCalc::SizeOfArray(items, [](uint64_t item) {
        return CborSizeCalc::SizeOfUint(item);
    });

    zassert_equal(size, 1);
}

ZTEST(cbor_size_calculator, test_SizeOfIndefiniteArrayMarkers) {
    zassert_equal(CborSizeCalc::SizeOfIndefiniteArrayMarkers(), 2);
    zassert_equal(CborSizeCalc::SizeOfIndefiniteArrayStart(), 2);
}

ZTEST(cbor_size_calculator, test_SizeOfIndefiniteArray) {
    const size_t size = CborSizeCalc::SizeOfIndefiniteArray(3, [](size_t index) {
        return CborSizeCalc::SizeOfUint(index);
    });

    zassert_equal(size, 2 + 3);
}

ZTEST(cbor_size_calculator, test_SizeOfIndefiniteArray_matches_zcbor) {
    static constexpr char K_NAME[] = "sensor_1";

    const size_t expected = CborSizeCalc::SizeOfIndefiniteArrayStart() + CborSizeCalc::SizeOfUint(42)
        + CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(K_NAME)) + CborSizeCalc::SizeOfBool(true);

    const size_t encoded = EncodedSize([](zcbor_state_t* state) {
        const auto name = CborHelpers::ToZcborString(K_NAME);
        return zcbor_list_start_encode(state, 3)
            && zcbor_uint64_put(state, 42)
            && zcbor_tstr_encode(state, &name)
            && zcbor_bool_put(state, true)
            && zcbor_list_end_encode(state, 3);
    });

    zassert_equal(expected, encoded);
}

ZTEST(cbor_size_calculator, test_SizeOfMapOverhead) {
    zassert_equal(CborSizeCalc::SizeOfMapOverhead(0), 1);
    zassert_equal(CborSizeCalc::SizeOfMapOverhead(23), 1);
    zassert_equal(CborSizeCalc::SizeOfMapOverhead(24), 2);
    zassert_equal(CborSizeCalc::SizeOfMapStart(24), CborSizeCalc::SizeOfMapOverhead(24));
}

ZTEST(cbor_size_calculator, test_SizeOfIndefiniteMapMarkers) {
    zassert_equal(CborSizeCalc::SizeOfIndefiniteMapMarkers(), 2);
    zassert_equal(CborSizeCalc::SizeOfIndefiniteMapStart(), 2);
}

ZTEST(cbor_size_calculator, test_SizeOfIndefiniteMap_matches_zcbor) {
    static constexpr char K_KEY[] = "unit";
    static constexpr char K_VALUE[] = "celsius";

    const size_t expected = CborSizeCalc::SizeOfIndefiniteMapStart()
        + CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(K_KEY))
        + CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(K_VALUE));

    const size_t encoded = EncodedSize([](zcbor_state_t* state) {
        const auto key = CborHelpers::ToZcborString(K_KEY);
        const auto value = CborHelpers::ToZcborString(K_VALUE);
        return zcbor_map_start_encode(state, 1)
            && zcbor_tstr_encode(state, &key)
            && zcbor_tstr_encode(state, &value)
            && zcbor_map_end_encode(state, 1);
    });

    zassert_equal(expected, encoded);
}

ZTEST(cbor_size_calculator, test_SizeOfOptional_pointer) {
    const uint64_t value = 24;
    const uint64_t* present = &value;
    const uint64_t* absent = nullptr;

    const auto size_func = [](uint64_t item) {
        return CborSizeCalc::SizeOfUint(item);
    };

    zassert_equal(CborSizeCalc::SizeOfOptional(present, size_func), 2);
    zassert_equal(CborSizeCalc::SizeOfOptional(absent, size_func), 0);
}

ZTEST(cbor_size_calculator, test_SizeOfOptional_flag) {
    const uint64_t value = 24;
    const auto size_func = [](uint64_t item) {
        return CborSizeCalc::SizeOfUint(item);
    };

    zassert_equal(CborSizeCalc::SizeOfOptional(true, value, size_func), 2);
    zassert_equal(CborSizeCalc::SizeOfOptional(false, value, size_func), 0);
}
