// A unit without an ADC (rt_gauge) runs every sensor but the analog ones, which get no reader.
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/reading_status.h"

#include "processing_test_support.h"

using namespace eerie_leap::domain::sensor_domain::models;

using namespace processing_test_support;

ZTEST_SUITE(no_adc, NULL, NULL, NULL, NULL, NULL);

ZTEST(no_adc, test_Start_runs_without_an_adc_and_skips_analog_sensors) {
    const auto fs_service = MakeFormattedFsService();

    // A configuration written for a unit with an ADC, so the analog sensor passes validation.
    const auto rig = MakeProcessingRig(
        fs_service, nullptr, nullptr,
        {
            MakeAnalogSensor("analog", 0, 20),
            MakeVirtualSensor("virtual", "40 + 2", 20),
        },
        4
    );

    zassert_true(rig.processing_service->Start());
    zassert_true(rig.processing_service->IsRunning());

    const auto generation = rig.processing_service->GetGeneration();
    zassert_not_null(generation.get());
    zassert_is_null(generation->adc_configuration.get(), "Nothing to snapshot without an ADC configuration manager");

    k_msleep(100);

    zassert_false(rig.frame->TryGetReading("analog").has_value(), "The analog sensor has no reader");

    const auto virtual_reading = rig.frame->TryGetReading("virtual");
    zassert_true(virtual_reading.has_value(), "The rate group runs without an ADC");
    zassert_equal(virtual_reading->status, ReadingStatus::PROCESSED);
    zassert_within(virtual_reading->value.value(), 42.0F, 1e-3F);

    zassert_true(rig.processing_service->Stop());
    zassert_is_null(rig.processing_service->GetGeneration().get());
}
