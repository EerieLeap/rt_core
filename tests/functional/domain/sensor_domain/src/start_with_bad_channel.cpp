// A sensor whose ADC channel the generation cannot serve is left out at Start(); the others run.
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/reading_status.h"

#include "processing_test_support.h"

using namespace eerie_leap::domain::sensor_domain::models;

using namespace processing_test_support;

ZTEST_SUITE(start_with_bad_channel, NULL, NULL, NULL, NULL, NULL);

ZTEST(start_with_bad_channel, test_Start_skips_a_sensor_on_a_channel_without_calibration) {
    auto fs_service = MakeFormattedFsService();

    // The device tree has four channels, so the sensors configuration accepts channels 0..3.
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);
    zassert_equal(adc_manager->GetChannelCount(), 4);
    DriveConstant(*adc, 0, 1000);

    // The ADC configuration only knows two: what a configuration stored before the device tree
    // grew looks like before padding, forced here by a manager that sees a two-channel unit.
    auto adc_configuration_manager = MakeAdcConfigurationManager(
        fs_service, MakeAdcManager(std::make_shared<FakeAdc>(2)));
    zassert_true(adc_configuration_manager->Update(*MakeAdcConfiguration(2, 8, MakeCalibrator({ {0.0F, 0.0F}, {5.0F, 5.0F} }))));
    zassert_equal(adc_configuration_manager->Get()->channel_configurations->size(), 2);

    auto rig = MakeProcessingRig(fs_service, adc_manager, adc_configuration_manager, {
        MakeAnalogSensor("good", 0, 20),
        MakeAnalogSensor("bad", 3, 20),
        MakeVirtualSensor("derived", "good + 1", 20),
        // Alone in its rate group, which is dropped with it.
        MakeAnalogSensor("lonely_bad", 2, 50),
    }, adc_manager->GetChannelCount());

    zassert_true(rig.processing_service->Start(), "One bad sensor does not stop the others");
    zassert_true(rig.processing_service->IsRunning());

    k_msleep(150);

    const auto good = rig.frame->TryGetReading("good");
    zassert_true(good.has_value());
    zassert_equal(good->status, ReadingStatus::PROCESSED);
    zassert_true(good->value.has_value());

    const auto derived = rig.frame->TryGetReading("derived");
    zassert_true(derived.has_value());
    zassert_equal(derived->status, ReadingStatus::PROCESSED);
    zassert_within(derived->value.value(), good->value.value() + 1.0F, 1e-3F,
        "The group kept running past the sensor it left out");

    zassert_false(rig.frame->TryGetReading("bad").has_value(), "The sensor has no reader");
    zassert_false(rig.frame->TryGetReading("lonely_bad").has_value());

    zassert_true(rig.processing_service->Stop());
}
