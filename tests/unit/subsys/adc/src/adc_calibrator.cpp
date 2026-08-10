#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <zephyr/ztest.h>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"
#include "subsys/adc/utilities/adc_calibrator.h"

using namespace eerie_leap::utilities::voltage_interpolator;
using eerie_leap::subsys::adc::utilities::AdcCalibrator;

namespace {

constexpr float kAdcMaxVolts = CONFIG_EERIE_LEAP_ADC_VOLTAGE_MAX_MV / 1000.0F;
constexpr float kSensorMaxVolts = CONFIG_EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV / 1000.0F;

std::shared_ptr<std::pmr::vector<CalibrationData>> MakeTable(std::initializer_list<CalibrationData> points) {
    return std::make_shared<std::pmr::vector<CalibrationData>>(points);
}

} // namespace

ZTEST_SUITE(adc_calibrator, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_calibrator, test_InterpolateToInputRange_maps_adc_range_to_sensor_range) {
    zassert_between_inclusive(AdcCalibrator::InterpolateToInputRange(0.0F), -0.001, 0.001);
    zassert_between_inclusive(AdcCalibrator::InterpolateToInputRange(kAdcMaxVolts),
        kSensorMaxVolts - 0.001, kSensorMaxVolts + 0.001);
    zassert_between_inclusive(AdcCalibrator::InterpolateToInputRange(kAdcMaxVolts / 2.0F),
        kSensorMaxVolts / 2.0 - 0.001, kSensorMaxVolts / 2.0 + 0.001);
}

ZTEST(adc_calibrator, test_InterpolateToInputRange_is_monotonic) {
    float previous = AdcCalibrator::InterpolateToInputRange(0.0F);

    for(int step = 1; step <= 100; ++step) {
        float value = AdcCalibrator::InterpolateToInputRange(kAdcMaxVolts * step / 100.0F);

        zassert_true(value >= previous, "value dropped at step %d", step);
        previous = value;
    }
}

ZTEST(adc_calibrator, test_InterpolateToInputRange_handles_out_of_range_input) {
    // The ADC can report slightly outside the calibrated span; this must not read past the table.
    zassert_true(AdcCalibrator::InterpolateToInputRange(-0.5F) < 0.0F);
    zassert_true(AdcCalibrator::InterpolateToInputRange(kAdcMaxVolts * 2.0F) > kSensorMaxVolts);
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
        AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({{0.0F, 0.0F}}));
        (void)calibrator;
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_calibrator, test_constructor_rejects_unsupported_interpolation_method) {
    bool threw = false;
    try {
        AdcCalibrator calibrator(InterpolationMethod::NONE, MakeTable({{0.0F, 0.0F}, {5.0F, 5.0F}}));
        (void)calibrator;
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_calibrator, test_GetInterpolationMethod) {
    AdcCalibrator linear(InterpolationMethod::LINEAR, MakeTable({{0.0F, 0.0F}, {5.0F, 5.0F}}));
    AdcCalibrator spline(InterpolationMethod::CUBIC_SPLINE,
        MakeTable({{0.0F, 0.0F}, {1.0F, 1.1F}, {3.0F, 3.2F}, {5.0F, 5.0F}}));

    zassert_equal(linear.GetInterpolationMethod(), InterpolationMethod::LINEAR);
    zassert_equal(spline.GetInterpolationMethod(), InterpolationMethod::CUBIC_SPLINE);
}

ZTEST(adc_calibrator, test_GetCalibrationTable_returns_the_original_table) {
    auto table = MakeTable({{0.0F, 0.0F}, {3.0F, 3.3F}});
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, table);

    auto retrieved = calibrator.GetCalibrationTable();

    zassert_equal(retrieved.get(), table.get());
    zassert_equal(retrieved->size(), 2);
    zassert_equal(retrieved->at(1).voltage, 3.0F);
    zassert_equal(retrieved->at(1).value, 3.3F);
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_with_identity_table) {
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({{0.0F, 0.0F}, {5.0F, 5.0F}}));

    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(2.5F), 2.49, 2.51);
    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(5.0F), 4.99, 5.01);
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_inverts_the_calibration) {
    // The calibrator maps a measured value back to the voltage that produced it.
    AdcCalibrator calibrator(InterpolationMethod::LINEAR, MakeTable({{0.0F, 0.0F}, {3.0F, 3.3F}}));

    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(3.3F), 2.99, 3.01);
    zassert_between_inclusive(calibrator.InterpolateToCalibratedRange(1.65F), 1.49, 1.51);
}

ZTEST(adc_calibrator, test_InterpolateToCalibratedRange_passes_through_calibration_points) {
    auto table = MakeTable({{0.501F, 0.469F}, {1.0F, 0.968F}, {2.0F, 1.970F}, {3.002F, 2.98F}, {5.0F, 5.0F}});

    for(auto method : { InterpolationMethod::LINEAR, InterpolationMethod::CUBIC_SPLINE }) {
        AdcCalibrator calibrator(method, table);

        for(const auto& point : *table) {
            float result = calibrator.InterpolateToCalibratedRange(point.value);

            zassert_between_inclusive(result, point.voltage - 0.01, point.voltage + 0.01,
                "measured %f mapped to %f instead of %f",
                (double)point.value, (double)result, (double)point.voltage);
        }
    }
}

ZTEST(adc_calibrator, test_calibrators_are_independent) {
    AdcCalibrator identity(InterpolationMethod::LINEAR, MakeTable({{0.0F, 0.0F}, {5.0F, 5.0F}}));
    AdcCalibrator halved(InterpolationMethod::LINEAR, MakeTable({{0.0F, 0.0F}, {2.5F, 5.0F}}));

    zassert_between_inclusive(identity.InterpolateToCalibratedRange(5.0F), 4.99, 5.01);
    zassert_between_inclusive(halved.InterpolateToCalibratedRange(5.0F), 2.49, 2.51);
}
