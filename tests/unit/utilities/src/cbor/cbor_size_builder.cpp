#include <cstdint>
#include <zephyr/ztest.h>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/cbor/cbor_size_builder.hpp"

using eerie_leap::utilities::cbor::CborHelpers;
using eerie_leap::utilities::cbor::CborSizeBuilder;
using eerie_leap::utilities::cbor::CborSizeCalc;

ZTEST_SUITE(cbor_size_builder, NULL, NULL, NULL, NULL, NULL);

ZTEST(cbor_size_builder, test_default_is_empty) {
    CborSizeBuilder builder;

    zassert_equal(builder.Build(), 0);
    zassert_equal(builder.GetSize(), 0);
}

ZTEST(cbor_size_builder, test_initial_size) {
    CborSizeBuilder builder(17);

    zassert_equal(builder.Build(), 17);
}

ZTEST(cbor_size_builder, test_AddUint) {
    CborSizeBuilder builder;

    builder.AddUint(0).AddUint(24).AddUint(0x10000);

    zassert_equal(builder.Build(), 1 + 2 + 5);
}

ZTEST(cbor_size_builder, test_AddInt) {
    CborSizeBuilder builder;

    builder.AddInt(-1).AddInt(-25).AddInt(24);

    zassert_equal(builder.Build(), 1 + 2 + 2);
}

ZTEST(cbor_size_builder, test_AddUintFixed) {
    CborSizeBuilder builder;

    builder.AddUintFixed(8);

    zassert_equal(builder.Build(), 9);
}

ZTEST(cbor_size_builder, test_AddTstr) {
    static constexpr char kText[] = "sensor_1";
    CborSizeBuilder builder;

    builder.AddTstr(CborHelpers::ToZcborString(kText));

    zassert_equal(builder.Build(), 1 + sizeof(kText) - 1);
}

ZTEST(cbor_size_builder, test_AddBool_AddFloat_AddDouble) {
    CborSizeBuilder builder;

    builder.AddBool(true).AddFloat(1.5F).AddDouble(1.5);

    zassert_equal(builder.Build(), 1 + 5 + 9);
}

ZTEST(cbor_size_builder, test_AddArrayStart_AddMapStart) {
    CborSizeBuilder builder;

    builder.AddArrayStart(3).AddMapStart(24);

    zassert_equal(builder.Build(), 1 + 2);
}

ZTEST(cbor_size_builder, test_AddIndefiniteStarts) {
    CborSizeBuilder builder;

    builder.AddIndefiniteArrayStart().AddIndefiniteMapStart();

    zassert_equal(builder.Build(), 2 + 2);
}

ZTEST(cbor_size_builder, test_AddSize) {
    CborSizeBuilder builder;

    builder.AddSize(10).AddSize(0).AddSize(5);

    zassert_equal(builder.Build(), 15);
}

ZTEST(cbor_size_builder, test_AddOptional_pointer) {
    uint64_t value = 0x10000;
    auto size_func = [](uint64_t item) { return CborSizeCalc::SizeOfUint(item); };

    CborSizeBuilder builder;
    builder.AddOptional(&value, size_func).AddOptional(static_cast<const uint64_t*>(nullptr), size_func);

    zassert_equal(builder.Build(), 5);
}

ZTEST(cbor_size_builder, test_AddOptional_flag) {
    uint64_t value = 0x10000;
    auto size_func = [](uint64_t item) { return CborSizeCalc::SizeOfUint(item); };

    CborSizeBuilder builder;
    builder.AddOptional(true, value, size_func).AddOptional(false, value, size_func);

    zassert_equal(builder.Build(), 5);
}

ZTEST(cbor_size_builder, test_Reset) {
    CborSizeBuilder builder(42);

    builder.AddUint(1000);
    builder.Reset();

    zassert_equal(builder.Build(), 0);

    builder.AddBool(false);

    zassert_equal(builder.Build(), 1);
}

ZTEST(cbor_size_builder, test_implicit_size_t_conversion) {
    CborSizeBuilder builder;
    builder.AddUint(24).AddBool(true);

    size_t size = builder;

    zassert_equal(size, builder.Build());
    zassert_equal(size, 3);
}

ZTEST(cbor_size_builder, test_matches_calculator_for_composite_structure) {
    static constexpr char kId[] = "sensor_1";
    static constexpr char kUnit[] = "celsius";

    size_t expected = CborSizeCalc::SizeOfIndefiniteArrayStart()
        + CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(kId))
        + CborSizeCalc::SizeOfTstr(CborHelpers::ToZcborString(kUnit))
        + CborSizeCalc::SizeOfUint(1000)
        + CborSizeCalc::SizeOfFloat(0.5F)
        + CborSizeCalc::SizeOfBool(true);

    CborSizeBuilder builder;
    builder.AddIndefiniteArrayStart()
        .AddTstr(CborHelpers::ToZcborString(kId))
        .AddTstr(CborHelpers::ToZcborString(kUnit))
        .AddUint(1000)
        .AddFloat(0.5F)
        .AddBool(true);

    zassert_equal(builder.Build(), expected);
}
