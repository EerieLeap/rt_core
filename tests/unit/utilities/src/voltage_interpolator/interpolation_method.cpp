#include <stdexcept>
#include <string_view>
#include <zephyr/ztest.h>

#include "utilities/voltage_interpolator/interpolation_method.h"

using namespace eerie_leap::utilities::voltage_interpolator;

ZTEST_SUITE(interpolation_method, NULL, NULL, NULL, NULL, NULL);

ZTEST(interpolation_method, test_GetInterpolationMethodName) {
    zassert_str_equal(GetInterpolationMethodName(InterpolationMethod::NONE), "NONE");
    zassert_str_equal(GetInterpolationMethodName(InterpolationMethod::LINEAR), "LINEAR");
    zassert_str_equal(GetInterpolationMethodName(InterpolationMethod::CUBIC_SPLINE), "CUBIC_SPLINE");
}

ZTEST(interpolation_method, test_names_match_enum_order) {
    zassert_equal(InterpolationMethodNames.size(), 3);
    zassert_true(InterpolationMethodNames[static_cast<uint8_t>(InterpolationMethod::NONE)] == "NONE");
    zassert_true(InterpolationMethodNames[static_cast<uint8_t>(InterpolationMethod::LINEAR)] == "LINEAR");
    zassert_true(InterpolationMethodNames[static_cast<uint8_t>(InterpolationMethod::CUBIC_SPLINE)] == "CUBIC_SPLINE");
}

ZTEST(interpolation_method, test_GetInterpolationMethod_from_name) {
    zassert_equal(GetInterpolationMethod("NONE"), InterpolationMethod::NONE);
    zassert_equal(GetInterpolationMethod("LINEAR"), InterpolationMethod::LINEAR);
    zassert_equal(GetInterpolationMethod("CUBIC_SPLINE"), InterpolationMethod::CUBIC_SPLINE);
}

ZTEST(interpolation_method, test_name_round_trip) {
    for (auto method : { InterpolationMethod::NONE, InterpolationMethod::LINEAR, InterpolationMethod::CUBIC_SPLINE })
        zassert_equal(GetInterpolationMethod(GetInterpolationMethodName(method)), method);
}

ZTEST(interpolation_method, test_GetInterpolationMethod_rejects_unknown_name) {
    for (const char* name : { "QUADRATIC", "", "LINEAR " }) {
        bool threw = false;
        try {
            GetInterpolationMethod(name);
        } catch (const std::runtime_error&) {
            threw = true;
        }

        zassert_true(threw, "\"%s\" should be rejected", name);
    }
}

ZTEST(interpolation_method, test_GetInterpolationMethod_is_case_sensitive) {
    bool threw = false;
    try {
        GetInterpolationMethod("linear");
    } catch (const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}
