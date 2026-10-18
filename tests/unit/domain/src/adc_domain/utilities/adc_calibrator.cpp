#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <zephyr/ztest.h>

#include "domain/adc_domain/models/adc_range.h"
#include "domain/adc_domain/utilities/adc_calibrator.h"
#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

using namespace eerie_leap::utilities::voltage_interpolator;
using eerie_leap::domain::adc_domain::models::AdcRange;
using eerie_leap::domain::adc_domain::utilities::AdcCalibrator;

namespace {

constexpr float kAdcMaxVolts = CONFIG_EERIE_LEAP_ADC_VOLTAGE_MAX_MV / 1000.0F;
constexpr float kSensorMaxVolts = CONFIG_EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV / 1000.0F;

// Readings are taken as sensor volts, so the calibration alone is under test.
constexpr AdcRange kIdentityRange{ .adc_max_v = 5.0F, .sensor_max_v = 5.0F };

std::shared_ptr<std::pmr::vector<CalibrationData>> MakeTable(std::initializer_list<CalibrationData> points) {
    return std::make_shared<std::pmr::vector<CalibrationData>>(points);
}

} // namespace

ZTEST_SUITE(adc_calibrator, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_calibrator, test_production_range_maps_5V_to_3V1) {
    zassert_equal(CONFIG_EERIE_LEAP_ADC_VOLTAGE_MAX_MV, 3100);
    zassert_equal(CONFIG_EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV, 5000);

    constexpr auto range = AdcRange::Default();

    zassert_within(range.ToAdcVolts(5.0F), 3.1F, 1e-3F);
    zassert_within(range.ToSensorVolts(3.1F), 5.0F, 1e-3F);
    zassert_within(range.ToAdcVolts(2.5F), 1.55F, 1e-3F);
    zassert_within(range.ToSensorVolts(1.55F), 2.5F, 1e-3F);

    zassert_equal(range.ToSensorVolts(-0.5F), 0.0F);
    zassert_equal(range.ToAdcVolts(-0.5F), 0.0F);
    zassert_within(range.ToSensorVolts(3.3F), 5.0F, 1e-3F);
    zassert_within(range.ToAdcVolts(12.0F), 3.1F, 1e-3F);

    // A calibrator without an explicit range uses the production one.
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }));

    zassert_equal(calibrator.GetRange().adc_max_v, range.adc_max_v);
    zassert_equal(calibrator.GetRange().sensor_max_v, range.sensor_max_v);
    zassert_within(calibrator.InterpolateToInputRange(3.1F), 5.0F, 1e-3F);
    zassert_within(calibrator.InterpolateToCalibratedRange(3.1F), 5.0F, 1e-3F);
    zassert_within(calibrator.InterpolateToCalibratedRange(1.55F), 2.5F, 1e-3F);
}

ZTEST(adc_calibrator, test_InterpolateToInputRange_maps_adc_range_to_sensor_range) {
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }));

    zassert_between_inclusive(calibrator.InterpolateToInputRange(0.0F), -0.001F, 0.001F);
    zassert_between_inclusive(
        calibrator.InterpolateToInputRange(kAdcMaxVolts), kSensorMaxVolts - 0.001F, kSensorMaxVolts + 0.001F
    );
    zassert_between_inclusive(
        calibrator.InterpolateToInputRange(kAdcMaxVolts / 2.0F), kSensorMaxVolts / 2.0F - 0.001F,
        kSensorMaxVolts / 2.0F + 0.001F
    );
}

ZTEST(adc_calibrator, test_InterpolateToInputRange_uses_the_instance_range) {
    AdcCalibrator calibrator(
        InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }),
        AdcRange{ .adc_max_v = 1.0F, .sensor_max_v = 10.0F }
    );

    zassert_within(calibrator.InterpolateToInputRange(0.25F), 2.5F, 1e-3F);
    zassert_within(calibrator.InterpolateToInputRange(1.0F), 10.0F, 1e-3F);
}

ZTEST(adc_calibrator, test_InterpolateToInputRange_is_monotonic) {
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }));
    float previous = calibrator.InterpolateToInputRange(0.0F);

    for(int step = 1; step <= 100; ++step) {
        float value = calibrator.InterpolateToInputRange(kAdcMaxVolts * step / 100.0F);

        zassert_true(value >= previous, "value dropped at step %d", step);
        previous = value;
    }
}

ZTEST(adc_calibrator, test_InterpolateToInputRange_clamps_out_of_range_input) {
    // The ADC can report slightly outside its span; the base range saturates instead of extrapolating.
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }));

    zassert_equal(calibrator.InterpolateToInputRange(-0.5F), 0.0F);
    zassert_within(calibrator.InterpolateToInputRange(kAdcMaxVolts * 2.0F), kSensorMaxVolts, 1e-3F);
}

ZTEST(adc_calibrator, test_constructor_rejects_null_calibration_data) {
    bool threw = false;
    try {
        AdcCalibrator calibrator(InterpolationMethod::LINEAR, nullptr);
        (void)calibrator;
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_calibrator, test_constructor_rejects_table_with_fewer_than_two_points) {
    bool threw = false;
    try {
        AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F } }));
        (void)calibrator;
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_calibrator, test_constructor_rejects_unsupported_interpolation_method) {
    bool threw = false;
    try {
        AdcCalibrator calibrator(InterpolationMethod::NONE, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }));
        (void)calibrator;
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_calibrator, test_constructor_rejects_a_range_that_is_not_positive) {
    const AdcRange ranges[] = {
        { .adc_max_v = 0.0F, .sensor_max_v = 5.0F },
        { .adc_max_v = 3.1F, .sensor_max_v = 0.0F },
        { .adc_max_v = -3.1F, .sensor_max_v = 5.0F },
    };

    for(const auto& range : ranges) {
        bool threw = false;
        try {
            AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }), range);
            (void)calibrator;
        } catch(const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "range %f/%f accepted", (double)range.adc_max_v, (double)range.sensor_max_v);
    }
}

ZTEST(adc_calibrator, test_GetInterpolationMethod) {
    AdcCalibrator linear(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }));
    AdcCalibrator spline(
        InterpolationMethod::CUBIC_SPLINE, MakeTable({ { 0.0F, 0.0F }, { 1.0F, 1.1F }, { 3.0F, 3.2F }, { 5.0F, 5.0F } })
    );

    zassert_equal(linear.GetInterpolationMethod(), InterpolationMethod::LINEAR);
    zassert_equal(spline.GetInterpolationMethod(), InterpolationMethod::CUBIC_SPLINE);
}

ZTEST(adc_calibrator, test_GetCalibrationTable_returns_the_original_table) {
    auto table = MakeTable({ { 0.0F, 0.0F }, { 3.0F, 3.3F } });
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, table);

    auto retrieved = calibrator.GetCalibrationTable();

    zassert_equal(retrieved.get(), table.get());
    zassert_equal(retrieved->size(), 2);
    zassert_equal(retrieved->at(1).voltage, 3.0F);
    zassert_equal(retrieved->at(1).value, 3.3F);
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_with_identity_table) {
    AdcCalibrator calibrator(
        InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }), kIdentityRange
    );

    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(2.5F), 2.49F, 2.51F);
    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(5.0F), 4.99F, 5.01F);
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_inverts_the_calibration) {
    // The calibrator maps a measured value back to the voltage that produced it.
    AdcCalibrator calibrator(
        InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 3.0F, 3.3F } }),
        AdcRange{ .adc_max_v = 3.3F, .sensor_max_v = 3.3F }
    );

    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(3.3F), 2.99F, 3.01F);
    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(1.65F), 1.49F, 1.51F);
}

ZTEST(adc_calibrator, test_normalized_table_is_sorted) {
    // Ordered by voltage, the measured column falls, so the interpolators' key does too until the
    // constructor sorts it; a binary search on the unsorted key would pick the wrong segment.
    auto table = MakeTable({ { 1.0F, 4.0F }, { 2.0F, 3.0F }, { 3.0F, 2.0F }, { 4.0F, 1.0F } });

    for(auto method : { InterpolationMethod::LINEAR, InterpolationMethod::CUBIC_SPLINE }) {
        AdcCalibrator calibrator(method, table, kIdentityRange);

        for(float measured : { 1.0F, 1.5F, 2.0F, 2.5F, 3.0F, 3.5F, 4.0F }) {
            float result = calibrator.InterpolateToCalibratedRange(measured);

            zassert_within(
                result, 5.0F - measured, 0.01F, "method %d: measured %f mapped to %f instead of %f",
                static_cast<int>(method), (double)measured, (double)result, (double)(5.0F - measured)
            );
        }
    }
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_passes_through_calibration_points) {
    auto table = MakeTable(
        { { 0.501F, 0.469F }, { 1.0F, 0.968F }, { 2.0F, 1.970F }, { 3.002F, 2.98F }, { 5.0F, 5.0F } }
    );

    for(auto method : { InterpolationMethod::LINEAR, InterpolationMethod::CUBIC_SPLINE }) {
        AdcCalibrator calibrator(method, table);

        for(const auto& point : *table) {
            // What the ADC pin sees when the unit measures point.value at the sensor input.
            float result = calibrator.InterpolateToCalibratedRange(calibrator.GetRange().ToAdcVolts(point.value));

            zassert_between_inclusive(
                result, point.voltage - 0.01F, point.voltage + 0.01F,
                "method %d: measured %f mapped to %f instead of %f", static_cast<int>(method), (double)point.value,
                (double)result, (double)point.voltage
            );
        }
    }
}

ZTEST(adc_calibrator, test_calibrators_are_independent) {
    AdcCalibrator identity(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }), kIdentityRange);
    AdcCalibrator halved(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 2.5F, 5.0F } }), kIdentityRange);

    zassert_between_inclusive(identity.InterpolateToCalibratedRange(5.0F), 4.99F, 5.01F);
    zassert_between_inclusive(halved.InterpolateToCalibratedRange(5.0F), 2.49F, 2.51F);
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_clamps_like_the_calibration_reading) {
    // The calibration reader saturates a reading above the ADC range at the sensor range, so the
    // runtime path must map the same reading to that point rather than extrapolating past it.
    constexpr auto range = AdcRange::Default();
    constexpr float kTopReading = 3.15F;

    zassert_true(kTopReading > range.adc_max_v);

    auto table = MakeTable(
        { { 0.0F, range.ToSensorVolts(-0.02F) }, { 2.5F, range.ToSensorVolts(1.6F) },
            { 5.0F, range.ToSensorVolts(kTopReading) } }
    );

    for(auto method : { InterpolationMethod::LINEAR, InterpolationMethod::CUBIC_SPLINE }) {
        AdcCalibrator calibrator(method, table, range);

        zassert_within(
            calibrator.InterpolateToCalibratedRange(kTopReading), 5.0F, 1e-3F, "method %d", static_cast<int>(method)
        );
        zassert_within(
            calibrator.InterpolateToCalibratedRange(range.adc_max_v * 2.0F), 5.0F, 1e-3F, "method %d",
            static_cast<int>(method)
        );
        zassert_within(
            calibrator.InterpolateToCalibratedRange(-0.02F), 0.0F, 1e-3F, "method %d", static_cast<int>(method)
        );
        zassert_within(
            calibrator.InterpolateToCalibratedRange(1.6F), 2.5F, 1e-3F, "method %d", static_cast<int>(method)
        );
    }
}

ZTEST(adc_calibrator, test_value_beyond_the_sensor_range_saturates_consistently) {
    // A table recorded before readings were clamped can hold a value above the sensor range. Its
    // point is keyed at the end of the ADC range, and the reading that produced it lands there too.
    constexpr auto range = AdcRange::Default();
    constexpr float kReading = 5.05F * 3.1F / 5.0F;

    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({ { 0.0F, 0.0F }, { 5.0F, 5.05F } }), range);

    zassert_within(calibrator.InterpolateToCalibratedRange(kReading), 5.0F, 1e-3F);
    zassert_within(calibrator.InterpolateToCalibratedRange(range.adc_max_v), 5.0F, 1e-3F);
}
