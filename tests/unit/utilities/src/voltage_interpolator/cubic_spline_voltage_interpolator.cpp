#include <cmath>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <zephyr/ztest.h>

#include "utilities/voltage_interpolator/interpolation_method.h"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/i_voltage_interpolator.h"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"

using namespace eerie_leap::utilities::voltage_interpolator;

namespace {

std::shared_ptr<std::pmr::vector<CalibrationData>> MakeTable(std::initializer_list<CalibrationData> points) {
    return std::make_shared<std::pmr::vector<CalibrationData>>(points);
}

} // namespace

ZTEST_SUITE(cubic_spline_voltage_interpolator, NULL, NULL, NULL, NULL, NULL);

ZTEST(cubic_spline_voltage_interpolator, test_GetInterpolationMethod) {
    std::pmr::vector<CalibrationData> calibration_data_1 {
        {0.0, 0.0},
        {10, 100.0}
    };
    auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);
    CubicSplineVoltageInterpolator voltage_interpolator(calibration_data_1_ptr);

    zassert_equal(voltage_interpolator.GetInterpolationMethod(), InterpolationMethod::CUBIC_SPLINE);
}

ZTEST(cubic_spline_voltage_interpolator, test_GetCalibrationTable) {
    std::pmr::vector<CalibrationData> calibration_data_1 {
        {0.0, 0.0},
        {10, 100.0}
    };
    auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);
    CubicSplineVoltageInterpolator voltage_interpolator(calibration_data_1_ptr);

    zassert_equal(voltage_interpolator.GetCalibrationTable(), calibration_data_1_ptr);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate) {
    std::pmr::vector<CalibrationData> calibration_data_1 {
        {0.0, 0.0},
        {10, 100.0}
    };
    auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);
    CubicSplineVoltageInterpolator voltage_interpolator_1(calibration_data_1_ptr);

    zassert_equal(voltage_interpolator_1.Interpolate(-1.23, true), 0);
    float value = voltage_interpolator_1.Interpolate(4.26, true);
    zassert_between_inclusive(value, 42.59, 42.61);
    zassert_equal(voltage_interpolator_1.Interpolate(12, true), 100.0);

    std::pmr::vector<CalibrationData> calibration_data_2 {
        {0.0, 1.0},
        {1.0, 29.0},
        {2.0, 111.0},
        {2.5, 162.0},
        {3.3, 200.0}
    };
    auto calibration_data_2_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_2);
    CubicSplineVoltageInterpolator voltage_interpolator_2(calibration_data_2_ptr);

    zassert_equal(voltage_interpolator_2.Interpolate(-1.2, true), 1.0);
    value = voltage_interpolator_2.Interpolate(0.5, true);
    zassert_between_inclusive(value, 10.53, 10.54);
    value = voltage_interpolator_2.Interpolate(2.25, true);
    zassert_between_inclusive(value, 137.97, 137.99);
    zassert_equal(voltage_interpolator_2.Interpolate(2.5, true), 162.0);
    zassert_equal(voltage_interpolator_2.Interpolate(4, true), 200.0);
}

ZTEST(cubic_spline_voltage_interpolator, test_constructor_rejects_null_table) {
    bool threw = false;
    try {
        CubicSplineVoltageInterpolator interpolator{nullptr};
        (void)interpolator;
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(cubic_spline_voltage_interpolator, test_constructor_rejects_table_with_fewer_than_two_points) {
    for (auto table : { MakeTable({}), MakeTable({{0.0F, 0.0F}}) }) {
        bool threw = false;
        try {
            CubicSplineVoltageInterpolator interpolator(table);
            (void)interpolator;
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "table of size %zu should be rejected", table->size());
    }
}

ZTEST(cubic_spline_voltage_interpolator, test_GetCalibrationTable_shares_ownership) {
    auto table = MakeTable({{0.0F, 0.0F}, {10.0F, 100.0F}});
    const long before = table.use_count();

    CubicSplineVoltageInterpolator interpolator(table);

    zassert_equal(table.use_count(), before + 1);

    auto retrieved = interpolator.GetCalibrationTable();

    zassert_equal(retrieved.get(), table.get());
    zassert_equal(retrieved->size(), 2);
    zassert_equal(table.use_count(), before + 2);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_passes_through_calibration_points) {
    auto table = MakeTable({{0.0F, 1.0F}, {1.0F, 29.0F}, {2.0F, 111.0F}, {2.5F, 162.0F}, {3.3F, 200.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    for (const auto& point : *table) {
        float clamped = interpolator.Interpolate(point.voltage, true);
        float unclamped = interpolator.Interpolate(point.voltage);

        zassert_between_inclusive(clamped, point.value - 0.01, point.value + 0.01,
            "clamped node at %f returned %f", (double)point.voltage, (double)clamped);
        zassert_between_inclusive(unclamped, point.value - 0.01, point.value + 0.01,
            "unclamped node at %f returned %f", (double)point.voltage, (double)unclamped);
    }
}

ZTEST(cubic_spline_voltage_interpolator, test_two_point_table_degenerates_to_a_line) {
    auto table = MakeTable({{0.0F, 0.0F}, {10.0F, 100.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    zassert_between_inclusive(interpolator.Interpolate(2.5F), 24.99, 25.01);
    zassert_between_inclusive(interpolator.Interpolate(5.0F), 49.99, 50.01);
    zassert_between_inclusive(interpolator.Interpolate(7.5F), 74.99, 75.01);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_clamps_at_exact_bounds) {
    auto table = MakeTable({{1.0F, 10.0F}, {2.0F, 45.0F}, {4.0F, 40.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    zassert_equal(interpolator.Interpolate(1.0F, true), 10.0F);
    zassert_equal(interpolator.Interpolate(4.0F, true), 40.0F);
    zassert_equal(interpolator.Interpolate(-100.0F, true), 10.0F);
    zassert_equal(interpolator.Interpolate(100.0F, true), 40.0F);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_is_continuous_across_segments) {
    auto table = MakeTable({{0.0F, 1.0F}, {1.0F, 29.0F}, {2.0F, 111.0F}, {2.5F, 162.0F}, {3.3F, 200.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    float previous = interpolator.Interpolate(0.0F);
    for (int step = 1; step <= 330; ++step) {
        float value = interpolator.Interpolate(static_cast<float>(step) / 100.0F);

        zassert_true(fabsf(value - previous) < 10.0F, "discontinuity at step %d", step);
        previous = value;
    }
}

ZTEST(cubic_spline_voltage_interpolator, test_curve_differs_from_linear_between_nodes) {
    auto table = MakeTable({{0.0F, 1.0F}, {1.0F, 29.0F}, {2.0F, 111.0F}, {2.5F, 162.0F}, {3.3F, 200.0F}});
    CubicSplineVoltageInterpolator spline(table);
    LinearVoltageInterpolator linear(table);

    zassert_true(fabsf(spline.Interpolate(0.5F) - linear.Interpolate(0.5F)) > 1.0F);
    zassert_true(fabsf(spline.Interpolate(1.5F) - linear.Interpolate(1.5F)) > 1.0F);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_with_negative_voltages_and_values) {
    auto table = MakeTable({{-5.0F, -50.0F}, {0.0F, 0.0F}, {5.0F, 50.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    zassert_between_inclusive(interpolator.Interpolate(0.0F), -0.01, 0.01);
    zassert_between_inclusive(interpolator.Interpolate(-2.5F), -25.01, -24.99);
    zassert_equal(interpolator.Interpolate(-10.0F, true), -50.0F);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_unclamped_extrapolates_beyond_range) {
    auto table = MakeTable({{0.0F, 0.0F}, {10.0F, 100.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    // A two-point spline is a straight line, so extrapolation stays on that line.
    zassert_between_inclusive(interpolator.Interpolate(-2.0F), -20.01, -19.99);
    zassert_between_inclusive(interpolator.Interpolate(12.0F), 119.99, 120.01);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_unclamped_is_continuous_at_table_bounds) {
    auto table = MakeTable({{0.0F, 1.0F}, {1.0F, 29.0F}, {2.0F, 111.0F}, {2.5F, 162.0F}, {3.3F, 200.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    zassert_true(fabsf(interpolator.Interpolate(-0.001F) - 1.0F) < 0.5F);
    zassert_true(fabsf(interpolator.Interpolate(3.301F) - 200.0F) < 0.5F);
}

ZTEST(cubic_spline_voltage_interpolator, test_Interpolate_unclamped_over_full_sweep_stays_finite) {
    auto table = MakeTable({{0.0F, 1.0F}, {1.0F, 29.0F}, {2.0F, 111.0F}, {2.5F, 162.0F}, {3.3F, 200.0F}});
    CubicSplineVoltageInterpolator interpolator(table);

    for (int step = -500; step <= 800; ++step) {
        float value = interpolator.Interpolate(static_cast<float>(step) / 100.0F);

        zassert_true(std::isfinite(value), "non-finite result at step %d", step);
    }
}

ZTEST(cubic_spline_voltage_interpolator, test_usable_through_interface) {
    auto table = MakeTable({{0.0F, 0.0F}, {10.0F, 100.0F}});
    std::unique_ptr<IVoltageInterpolator> interpolator = std::make_unique<CubicSplineVoltageInterpolator>(table);

    zassert_equal(interpolator->GetInterpolationMethod(), InterpolationMethod::CUBIC_SPLINE);
    zassert_equal(interpolator->GetCalibrationTable().get(), table.get());
    zassert_between_inclusive(interpolator->Interpolate(5.0F, true), 49.99, 50.01);
}
