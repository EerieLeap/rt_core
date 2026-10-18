// Calibration pauses and resumes the processing service from the MCUmgr work queue while a
// configuration apply restarts it from the configuration work queue. The service serializes the
// transitions, so whatever the interleaving, it ends running a generation and sampling.
#include <chrono>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/adc_domain/services/calibration_service.h"

#include "processing_test_support.h"

using eerie_leap::domain::adc_domain::services::CalibrationService;

using namespace processing_test_support;

ZTEST_SUITE(calibration_restart_race, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr int kRounds = 40;
// As in the apps: the configuration queue above processing (6), MCUmgr below it.
constexpr int kConfigPriority = 5;
constexpr int kMcumgrPriority = 7;

K_THREAD_STACK_DEFINE(calibrating_stack, 8192);
K_THREAD_STACK_DEFINE(restarting_stack, 8192);

struct Race {
    std::shared_ptr<SensorsProcessingService> processing_service;
    std::shared_ptr<CalibrationService> calibration_service;
    int jobs = 0;
    int stopped_jobs = 0;
    int restarts = 0;
};

// What START and STOP do, with a job of varying length.
void Calibrate(void* context, void*, void*) {
    auto* race = static_cast<Race*>(context);

    for(int i = 0; i < kRounds; i++) {
        if(race->calibration_service->Start(1)) {
            race->jobs++;
            k_msleep(1 + i % 4);
            if(race->calibration_service->Stop())
                race->stopped_jobs++;
        }

        k_msleep(1 + i % 3);
    }
}

// What SensorsController::RestartProcessing() does for every apply. A START that waits for a
// restart gets the service between its Stop() and Start() and is refused, so every few rounds the
// queue idles long enough for a job to start, and the next restart lands inside it.
void Restart(void* context, void*, void*) {
    auto* race = static_cast<Race*>(context);

    for(int i = 0; i < kRounds; i++) {
        if(race->processing_service->Stop() && race->processing_service->Start())
            race->restarts++;

        k_msleep(i % 3 == 0 ? 25 : 1 + i % 5);
    }
}

} // namespace

ZTEST(calibration_restart_race, test_calibration_and_restarts_leave_processing_running) {
    auto fs_service = MakeFormattedFsService();

    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);
    DriveConstant(*adc, 0, 1000);
    DriveConstant(*adc, 1, 2000);

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
    zassert_true(adc_configuration_manager->Update(
        *MakeAdcConfiguration(adc_manager->GetChannelCount(), 8, MakeCalibrator({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }))
    ));

    auto rig = MakeProcessingRig(
        fs_service, adc_manager, adc_configuration_manager,
        {
            MakeAnalogSensor("fast", 0, 10),
            MakeAnalogSensor("slow", 0, 30),
        },
        adc_manager->GetChannelCount()
    );

    auto calibration_service = std::make_shared<CalibrationService>(
        rig.time_service, adc_manager, rig.processing_service
    );
    zassert_true(calibration_service->Initialize());

    zassert_true(rig.processing_service->Start());

    Race race{ .processing_service = rig.processing_service, .calibration_service = calibration_service };

    k_thread calibrating;
    k_thread restarting;
    k_thread_create(
        &calibrating, calibrating_stack, K_THREAD_STACK_SIZEOF(calibrating_stack), Calibrate, &race, nullptr, nullptr,
        kMcumgrPriority, 0, K_NO_WAIT
    );
    k_thread_create(
        &restarting, restarting_stack, K_THREAD_STACK_SIZEOF(restarting_stack), Restart, &race, nullptr, nullptr,
        kConfigPriority, 0, K_NO_WAIT
    );

    zassert_ok(k_thread_join(&calibrating, K_SECONDS(30)), "The calibration loop hung");
    zassert_ok(k_thread_join(&restarting, K_SECONDS(30)), "The restart loop hung");

    zassert_equal(race.restarts, kRounds, "Every restart completed");
    zassert_true(race.jobs > 0, "Calibration ran between the restarts");
    zassert_equal(race.stopped_jobs, race.jobs, "Every job STOP ended");
    zassert_false(calibration_service->IsRunning());

    // The last restart started it and the last STOP either resumed it or found it taken back.
    zassert_true(rig.processing_service->IsRunning());
    zassert_not_null(rig.processing_service->GetGeneration().get(), "Running with a generation");

    // And it samples: the reading keeps getting newer.
    k_msleep(50);
    const auto before = rig.frame->TryGetReading("fast");
    zassert_true(before.has_value() && before->timestamp.has_value());
    k_msleep(50);
    const auto after = rig.frame->TryGetReading("fast");
    zassert_true(after.has_value() && after->timestamp.has_value());
    zassert_true(after->timestamp.value() > before->timestamp.value(), "Processing samples after the race");

    calibration_service.reset();
    zassert_true(rig.processing_service->Stop());
}
