// An ADC configuration applied while sensors run takes effect through a processing restart: the
// running generation keeps the snapshot and calibrators it was built with until then, the next one
// gets the new ones, and the old calibrators go with the old generation.
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_serializer.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/models/reading_status.h"

#include "processing_test_support.h"

using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::domain::sensor_domain::models;

using namespace processing_test_support;

ZTEST_SUITE(adc_apply_restart, NULL, NULL, NULL, NULL, NULL);

namespace {

// What SensorsController does with the ADC configuration manager's handler, split in two so the
// test decides when the queued restart runs: the handler only reports the apply.
struct Restarter {
    std::shared_ptr<SensorsProcessingService> processing_service;
    k_sem applied;
    k_sem restarted;
    bool is_restarted = false;

    static WorkQueueTaskResult Restart(Restarter* restarter) {
        restarter->is_restarted = restarter->processing_service->Stop() && restarter->processing_service->Start();
        k_sem_give(&restarter->restarted);

        return {};
    }
};

struct ObservedReading {
    std::chrono::system_clock::time_point timestamp;
    ReadingStatus status;
    float voltage;
};

} // namespace

ZTEST(adc_apply_restart, test_ApplyCborConfiguration_restarts_processing_with_the_new_calibrator) {
    auto fs_service = MakeFormattedFsService();

    auto adc = MakeEmulator();
    auto adc_manager = MakeAdcManager(adc);
    DriveConstant(*adc, 0, 1000);

    // Calibrator A takes the reading as it is; B recorded 2.5 V for a true 5 V input, so it doubles.
    auto calibrator_a = MakeCalibrator({ {0.0F, 0.0F}, {5.0F, 5.0F} });
    auto calibrator_b = MakeCalibrator({ {0.0F, 0.0F}, {5.0F, 2.5F} });
    const float volts_a = calibrator_a->InterpolateToCalibratedRange(1.0F);
    const float volts_b = calibrator_b->InterpolateToCalibratedRange(1.0F);
    constexpr float tolerance = 0.02F;
    zassert_true(volts_b - volts_a > 0.5F, "A and B must be told apart");

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
    zassert_true(adc_configuration_manager->Update(*MakeAdcConfiguration(adc_manager->GetChannelCount(), 8, calibrator_a)));

    auto rig = MakeProcessingRig(fs_service, adc_manager, adc_configuration_manager, {
        MakeAnalogSensor("analog", 0, 20),
    }, adc_manager->GetChannelCount());

    auto config_thread = std::make_shared<WorkQueueThread>("adc_apply_config", 4096, 5);
    zassert_true(config_thread->Initialize());

    Restarter restarter { .processing_service = rig.processing_service };
    k_sem_init(&restarter.applied, 0, 1);
    k_sem_init(&restarter.restarted, 0, 1);
    auto restart_task = config_thread->CreateTask(Restarter::Restart, &restarter);

    adc_configuration_manager->RegisterConfigurationUpdatedHandler([&restarter] {
        k_sem_give(&restarter.applied);
    });

    zassert_true(rig.processing_service->Start());

    // The generation owns its snapshot of the configuration, and through it A's calibrators.
    auto generation_a = rig.processing_service->GetGeneration();
    zassert_not_null(generation_a.get());
    zassert_not_null(generation_a->adc_configuration.get());
    zassert_equal(generation_a->adc_configuration, adc_configuration_manager->Get());
    std::weak_ptr<const AdcCalibrator> calibrator_a_weak = generation_a->adc_configuration->channel_configurations->at(0)->calibrator;

    // Every committed reading, told apart by its tick's timestamp.
    std::array<ObservedReading, 256> observed;
    size_t observed_count = 0;
    auto observe_until = [&](size_t count, int timeout_ms) {
        const int64_t end = k_uptime_get() + timeout_ms;
        while(k_uptime_get() < end && observed_count < count) {
            const auto reading = rig.frame->TryGetReading("analog");
            if(reading.has_value() && reading->timestamp.has_value()
                && (observed_count == 0 || observed[observed_count - 1].timestamp != reading->timestamp.value())) {

                observed[observed_count++] = {
                    .timestamp = reading->timestamp.value(),
                    .status = reading->status,
                    .voltage = reading->voltage.value_or(0.0F)
                };
            }

            k_msleep(2);
        }
    };

    observe_until(3, 1000);
    const size_t observed_before_apply = observed_count;
    zassert_true(observed_before_apply >= 2, "Processing runs with A before the apply");

    // The client's apply: the stored configuration with channel 0 recalibrated to B.
    eerie_leap::configuration::cbor::CborSerializer<CborAdcConfig> serializer;
    auto cbor_config = serializer.Deserialize(adc_configuration_manager->GetCborConfiguration());
    zassert_not_null(cbor_config.get());
    cbor_config->CborAdcChannelConfig_m.at(0).calibration_table.float32float.back().float32float = 2.5F;
    zassert_true(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config)));
    zassert_ok(k_sem_take(&restarter.applied, K_NO_WAIT), "The apply reported itself");

    // Until the restart runs, the generation keeps reading with its own snapshot.
    zassert_not_equal(generation_a->adc_configuration, adc_configuration_manager->Get(),
        "The apply swapped the manager's configuration, not the generation's");
    observe_until(observed_before_apply + 2, 1000);
    const size_t observed_before_restart = observed_count;
    zassert_true(observed_before_restart - observed_before_apply >= 2, "Processing ran between the apply and the restart");
    zassert_equal(rig.processing_service->GetGeneration(), generation_a);

    for(size_t i = observed_before_apply; i < observed_before_restart; i++)
        zassert_within(observed[i].voltage, volts_a, tolerance, "reading %zu after the apply is not A", i);

    restart_task.Reschedule(K_NO_WAIT);
    zassert_ok(k_sem_take(&restarter.restarted, K_SECONDS(2)), "The restart ran");
    zassert_true(restarter.is_restarted);

    const auto generation_b = rig.processing_service->GetGeneration();
    zassert_not_null(generation_b.get());
    zassert_not_equal(generation_b, generation_a);
    zassert_equal(generation_b->adc_configuration, adc_configuration_manager->Get(), "The new generation snapshots B");

    // Nothing but the dropped generation kept A's calibrators: they go with it, while B runs.
    generation_a.reset();
    calibrator_a.reset();
    zassert_true(calibrator_a_weak.expired(), "A's calibrators lived as long as its generation");

    observe_until(observed_before_restart + 3, 1000);

    adc_configuration_manager->RegisterConfigurationUpdatedHandler(nullptr);
    zassert_true(rig.processing_service->Stop());
    restart_task.Cancel();

    // A … A B … B: never 0 V, never an error, never A again once B has been committed.
    size_t first_b = observed_count;
    for(size_t i = 0; i < observed_count; i++) {
        const auto& reading = observed[i];
        zassert_equal(reading.status, ReadingStatus::PROCESSED, "reading %zu", i);

        const bool is_a = std::abs(reading.voltage - volts_a) < tolerance;
        const bool is_b = std::abs(reading.voltage - volts_b) < tolerance;
        zassert_true(is_a || is_b, "reading %zu: %f V is neither A nor B", i, static_cast<double>(reading.voltage));

        if(is_b && first_b == observed_count)
            first_b = i;

        zassert_false(is_a && first_b < i, "reading %zu went back to A", i);
    }

    zassert_true(first_b >= observed_before_restart, "B only arrives with the restart");
    zassert_true(first_b < observed_count, "The restarted generation reads with B");
    zassert_true(observed_count - first_b >= 2);
}
