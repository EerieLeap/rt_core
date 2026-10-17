#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/system_domain/models/product_feature.h"

#include "canbus_com_test_support.h"

using namespace canbus_com_test;

using eerie_leap::domain::system_domain::models::BUILD_FEATURES;
using eerie_leap::domain::system_domain::models::HasFeature;
using eerie_leap::domain::system_domain::models::ProductFeature;

namespace {

Harness* harness_under_test = nullptr;

void* CanbusComFeaturesSetup() {
    return nullptr;
}

void CanbusComFeaturesBefore(void*) {
    harness_under_test = new Harness();
}

void CanbusComFeaturesAfter(void*) {
    delete harness_under_test;
    harness_under_test = nullptr;

    ResetLoopbackDevice();
}

} // namespace

ZTEST_SUITE(canbus_com_features, NULL, CanbusComFeaturesSetup, CanbusComFeaturesBefore, CanbusComFeaturesAfter, NULL);

ZTEST(canbus_com_features, test_features_start_with_the_build_but_without_logging) {
    const auto features = harness_under_test->com->GetFeatures();

    zassert_true(HasFeature(BUILD_FEATURES, ProductFeature::CANBUS), "The suite builds the CAN domain");
    zassert_true(HasFeature(features, ProductFeature::CANBUS));
    zassert_false(HasFeature(features, ProductFeature::LOGGING), "LOGGING is set only by a log writer");
    zassert_equal(features, BUILD_FEATURES & ~static_cast<uint16_t>(ProductFeature::LOGGING));
}

// This suite does not build the logging domain, so the build has no LOGGING to offer.
ZTEST(canbus_com_features, test_logging_cannot_be_set_when_the_build_lacks_it) {
    zassert_false(HasFeature(BUILD_FEATURES, ProductFeature::LOGGING));

    harness_under_test->com->SetLoggingAvailable(true);

    zassert_false(HasFeature(harness_under_test->com->GetFeatures(), ProductFeature::LOGGING));
}

ZTEST(canbus_com_features, test_device_info_carries_the_current_features) {
    const auto info = harness_under_test->com->GetNetworkInfo()->GetDeviceInfo();

    zassert_equal(info.features, harness_under_test->com->GetFeatures());
}
