// Calibration samples one channel on its own thread while the service that owns the ADC is paused.
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/adc/adc_emul.h>

#include "subsys/device_tree/dt_adc.h"
#include "subsys/adc/adc_emulator.h"
#include "subsys/adc/adc_manager.h"

#include "domain/adc_domain/models/adc_range.h"
#include "domain/adc_domain/services/calibration_service.h"

#include "calibration_test_support.h"

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::domain::adc_domain::models;
using namespace eerie_leap::domain::adc_domain::services;

using namespace calibration_test_support;

ZTEST_SUITE(calibration_service, NULL, NULL, NULL, NULL, NULL);

namespace {

// vref 3300 mV, gain 1, 12 bits on the overlay's first emulated ADC.
constexpr float kEmulLsbVolts = 3.3F / 4096.0F;
// adc_emul truncates mV × 4095 / 3300 to a whole count, so a constant input lands up to ~1.3 LSB low.
constexpr float kEmulTolerance = 2.0F * kEmulLsbVolts;

// The overlay's first emulated ADC, every channel back on a constant so a value function a
// previous test installed is never called.
std::shared_ptr<AdcEmulator> MakeEmulator() {
    DtAdc::Initialize();

    const auto& info = DtAdc::Get().value().at(0);
    for(const auto& channel_config : info.channel_configs)
        zassert_ok(adc_emul_const_value_set(info.adc_device, channel_config.channel_id, 0));

    return std::make_shared<AdcEmulator>(info);
}

std::shared_ptr<AdcManager> MakeAdcManager(std::shared_ptr<IAdc> adc) {
    auto adc_manager = std::make_shared<AdcManager>(std::vector<std::shared_ptr<IAdc>> { std::move(adc) });
    zassert_true(adc_manager->Initialize());

    return adc_manager;
}

// Pins an emulator channel to a constant input instead of the random stimulus.
void DriveConstant(AdcEmulator& adc, int channel, uint32_t mv) {
    const auto& info = DtAdc::Get().value().at(0);

    adc.SetExternallyDriven(channel);
    zassert_ok(adc_emul_const_value_set(info.adc_device, info.channel_configs.at(channel).channel_id, mv));
}

// Polls until a result newer than @p after_sequence exists.
std::optional<CalibrationService::Result> WaitForResultAfter(const CalibrationService& service, uint32_t after_sequence) {
    const int64_t end = k_uptime_get() + 10 * CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS;
    while(k_uptime_get() < end) {
        const auto latest = service.GetLatest();
        if(latest.has_value() && latest->sequence > after_sequence)
            return latest;

        k_msleep(5);
    }

    return std::nullopt;
}

} // namespace

ZTEST(calibration_service, test_Start_pauses_and_samples) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);
    DriveConstant(*adc, 1, 1500);

    auto paused_service = std::make_shared<RecordingService>();

    CalibrationService calibration_service(MakeTimeService(), adc_manager, paused_service);
    zassert_true(calibration_service.Initialize());
    zassert_false(calibration_service.GetLatest().has_value());
    zassert_false(calibration_service.IsRunning());
    zassert_false(calibration_service.GetChannel().has_value());

    const AdcStats stats_before = adc->GetStats();

    zassert_true(calibration_service.Start(1));
    zassert_true(calibration_service.IsRunning());
    zassert_equal(calibration_service.GetChannel(), 1);
    zassert_equal(paused_service->pauses, 1);
    zassert_true(paused_service->IsPaused());
    zassert_false(calibration_service.Start(1), "One job at a time");

    const auto first = WaitForResultAfter(calibration_service, 0);
    zassert_true(first.has_value(), "The job produced a result");
    const auto second = WaitForResultAfter(calibration_service, first->sequence);
    zassert_true(second.has_value(), "The job keeps sampling");

    zassert_true(second->sequence > first->sequence);
    zassert_true(second->timestamp > first->timestamp);
    zassert_equal(second->error, AdcError::NONE);
    zassert_within(second->adc_volts, 1.5F, kEmulTolerance);
    zassert_within(second->sensor_volts, AdcRange::Default().ToSensorVolts(second->adc_volts), 1e-6F);

    zassert_true(calibration_service.Stop());
    zassert_false(calibration_service.IsRunning());
    zassert_equal(calibration_service.GetChannel(), 1, "The latest result still belongs to channel 1");
    zassert_equal(paused_service->resumes, 1);
    zassert_true(paused_service->IsRunning());
    zassert_false(calibration_service.Stop(), "No job left to stop");
    zassert_equal(paused_service->resumes, 1, "Resumed once, by the Stop() that ended the job");

    // Every request was a calibration-sized one, and none follows Stop().
    const AdcStats stats_after = adc->GetStats();
    const uint32_t last_sequence = calibration_service.GetLatest()->sequence;
    const uint32_t requests = stats_after.requests - stats_before.requests;
    zassert_true(requests >= 2);
    zassert_equal(stats_after.conversions - stats_before.conversions,
        requests * CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLES_COUNT);

    k_msleep(2 * CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS);
    zassert_equal(adc->GetStats().requests, stats_after.requests);
    zassert_equal(calibration_service.GetLatest()->sequence, last_sequence, "The latest result stays readable");
}

ZTEST(calibration_service, test_Start_fails_when_pause_is_refused) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    // Another owner has it paused or stopped.
    auto paused_service = std::make_shared<RecordingService>();
    paused_service->is_pause_accepted = false;

    CalibrationService calibration_service(MakeTimeService(), adc_manager, paused_service);
    zassert_true(calibration_service.Initialize());

    const uint32_t requests_before = adc->GetStats().requests;

    zassert_false(calibration_service.Start(1));
    zassert_equal(paused_service->pauses, 1);

    k_msleep(2 * CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS);
    zassert_false(calibration_service.GetLatest().has_value(), "Nothing was sampled");
    zassert_equal(adc->GetStats().requests, requests_before);

    zassert_false(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 0, "Nothing it did not pause is resumed");
    zassert_true(paused_service->IsRunning());

    // A later start, once the service can be paused, works.
    paused_service->is_pause_accepted = true;
    zassert_true(calibration_service.Start(1));
    zassert_true(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 1);
}

ZTEST(calibration_service, test_Start_rejects_a_channel_the_unit_does_not_have) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    auto paused_service = std::make_shared<RecordingService>();

    CalibrationService calibration_service(MakeTimeService(), adc_manager, paused_service);
    zassert_true(calibration_service.Initialize());

    const uint32_t requests_before = adc->GetStats().requests;

    // The channel is resolved before anything is paused, so a bad one leaves the service running.
    zassert_false(calibration_service.Start(adc_manager->GetChannelCount()));
    zassert_false(calibration_service.Start(-1));
    zassert_equal(paused_service->pauses, 0);
    zassert_true(paused_service->IsRunning());

    k_msleep(2 * CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS);
    zassert_false(calibration_service.GetLatest().has_value(), "Nothing was sampled");
    zassert_equal(adc->GetStats().requests, requests_before);
    zassert_false(calibration_service.Stop(), "No job was started");
    zassert_equal(paused_service->resumes, 0);

    // The rejected start left the service usable.
    zassert_true(calibration_service.Start(1));
    zassert_equal(paused_service->pauses, 1);
    zassert_true(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 1);
    zassert_true(paused_service->IsRunning());
}

ZTEST(calibration_service, test_Stop_tolerates_a_resumed_service) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    auto paused_service = std::make_shared<RecordingService>();

    CalibrationService calibration_service(MakeTimeService(), adc_manager, paused_service);
    zassert_true(calibration_service.Initialize());

    zassert_true(calibration_service.Start(1));
    zassert_equal(paused_service->pauses, 1);

    // A configuration change restarts the service while the job runs, so it is no longer paused.
    zassert_true(paused_service->Start());
    paused_service->is_resume_accepted = false;

    zassert_true(calibration_service.Stop(), "The job ends although nothing was resumed");
    zassert_false(calibration_service.IsRunning());
    zassert_equal(paused_service->resumes, 1);
    zassert_true(paused_service->IsRunning(), "Left as its owner left it");
    zassert_false(calibration_service.Stop(), "No job left to stop");
    zassert_equal(paused_service->resumes, 1, "The failed resume is not retried");

    // The next job pauses and resumes as usual.
    paused_service->is_resume_accepted = true;
    zassert_true(calibration_service.Start(1));
    zassert_equal(paused_service->pauses, 2);
    zassert_true(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 2);
    zassert_true(paused_service->IsRunning());
}

ZTEST(calibration_service, test_SetServiceToPause_applies_to_the_next_job) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    auto first_service = std::make_shared<RecordingService>();
    auto second_service = std::make_shared<RecordingService>();

    // Constructed before the service it pauses exists, as AdcController may be.
    CalibrationService calibration_service(MakeTimeService(), adc_manager, nullptr);
    zassert_true(calibration_service.Initialize());

    calibration_service.SetServiceToPause(first_service);
    zassert_true(calibration_service.Start(1));
    zassert_true(first_service->IsPaused());

    // A running job still resumes what it paused.
    calibration_service.SetServiceToPause(second_service);
    zassert_true(calibration_service.Stop());
    zassert_equal(first_service->resumes, 1);
    zassert_true(first_service->IsRunning());
    zassert_equal(second_service->pauses, 0);
    zassert_equal(second_service->resumes, 0);

    zassert_true(calibration_service.Start(1));
    zassert_equal(first_service->pauses, 1);
    zassert_equal(second_service->pauses, 1);
    zassert_true(calibration_service.Stop());
    zassert_equal(second_service->resumes, 1);
}

// Start() runs on the MCUmgr work queue, which catches nothing: a pause that throws is a busy one.
ZTEST(calibration_service, test_Start_is_refused_when_pause_throws) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    auto paused_service = std::make_shared<RecordingService>();
    paused_service->is_pause_throwing = true;

    CalibrationService calibration_service(MakeTimeService(), adc_manager, paused_service);
    zassert_true(calibration_service.Initialize());

    zassert_false(calibration_service.Start(1));
    zassert_false(calibration_service.IsRunning());
    zassert_equal(paused_service->pauses, 1);
    zassert_false(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 0, "Nothing it did not pause is resumed");

    paused_service->is_pause_throwing = false;
    zassert_true(calibration_service.Start(1));
    zassert_true(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 1);
}

// So does Stop(), also from the destructor: a resume that throws still ends the job.
ZTEST(calibration_service, test_Stop_ends_the_job_when_resume_throws) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    auto paused_service = std::make_shared<RecordingService>();

    CalibrationService calibration_service(MakeTimeService(), adc_manager, paused_service);
    zassert_true(calibration_service.Initialize());

    zassert_true(calibration_service.Start(1));
    paused_service->is_resume_throwing = true;

    zassert_true(calibration_service.Stop(), "The job ends although resuming threw");
    zassert_false(calibration_service.IsRunning());
    zassert_equal(paused_service->resumes, 1);
    zassert_false(calibration_service.Stop(), "No job left to stop");
    zassert_equal(paused_service->resumes, 1, "The failed resume is not retried");

    // The next job is not refused for it.
    paused_service->is_resume_throwing = false;
    zassert_true(paused_service->Start());
    zassert_true(calibration_service.Start(1));
    zassert_true(calibration_service.Stop());
    zassert_equal(paused_service->resumes, 2);
    zassert_true(paused_service->IsRunning());
}

// The latest result always belongs to the channel GetChannel() reports.
ZTEST(calibration_service, test_Start_drops_the_previous_jobs_result) {
    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);

    CalibrationService calibration_service(MakeTimeService(), adc_manager, nullptr);
    zassert_true(calibration_service.Initialize());

    zassert_true(calibration_service.Start(1));
    zassert_true(WaitForResultAfter(calibration_service, 0).has_value());
    zassert_true(calibration_service.Stop());
    const uint32_t first_job_last = calibration_service.GetLatest()->sequence;

    zassert_true(calibration_service.Start(2));
    zassert_equal(calibration_service.GetChannel(), 2);
    const auto latest = calibration_service.GetLatest();
    zassert_true(!latest.has_value() || latest->sequence > first_job_last,
        "The first job's result %u is reported for channel 2", first_job_last);

    const auto second_job = WaitForResultAfter(calibration_service, first_job_last);
    zassert_true(second_job.has_value(), "The new job samples");
    zassert_true(calibration_service.Stop());
}
