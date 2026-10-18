// SensorsController as rt_gauge builds it, without an ADC: a stored sensors configuration takes
// effect through a restart once processing has been started, also after a restart that failed, and
// the restart stops and starts again the dependent services that were running.
#include <atomic>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "controllers/sensors_controller.h"

using eerie_leap::controllers::SensorsController;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::utilities::memory::Mrm;

ZTEST_SUITE(sensors_controller, NULL, NULL, NULL, NULL, NULL);

namespace {

std::shared_ptr<Sensor> MakeVirtualSensor(std::string_view id, const char* expression) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = 20;

    return sensor;
}

// Polls the frame, since the restart runs on the configuration work queue.
bool WaitForReading(const SensorReadingsFrame& frame, std::string_view id) {
    const int64_t end = k_uptime_get() + 1000;
    while(k_uptime_get() < end) {
        if(frame.TryGetReading(id).has_value())
            return true;

        k_msleep(5);
    }

    return false;
}

bool WaitFor(const std::atomic<int>& value, int expected) {
    const int64_t end = k_uptime_get() + 1000;
    while(value.load() != expected && k_uptime_get() < end)
        k_msleep(5);

    return value.load() == expected;
}

// A reader of the frame, like the log writer: records whether processing ran, on the generation it
// started with, whenever the restart stopped or started it.
class DependentService : public ServiceBase<> {
private:
    std::shared_ptr<SensorsProcessingService> processing_service_;

    [[nodiscard]] bool IsProcessingRunning() const {
        return processing_service_->IsRunning() && processing_service_->GetGeneration() != nullptr;
    }

protected:
    bool DoStart() override {
        if(!IsProcessingRunning())
            is_processing_running_at_every_start = false;
        starts++;

        return !refuse_start.load();
    }

    bool DoStop() override {
        if(!IsProcessingRunning())
            is_processing_running_at_every_stop = false;
        stops++;

        return true;
    }

public:
    std::atomic<int> starts = 0;
    std::atomic<int> stops = 0;
    std::atomic<bool> is_processing_running_at_every_start = true;
    std::atomic<bool> is_processing_running_at_every_stop = true;
    std::atomic<bool> refuse_start = false;

    explicit DependentService(std::shared_ptr<SensorsProcessingService> processing_service) :
        processing_service_(std::move(processing_service)) {}
};

} // namespace

ZTEST(sensors_controller, test_an_update_restarts_processing_once_it_was_started) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    auto work_queue = std::make_shared<WorkQueueThread>("sensors_test_config_wq", 6144, 5);
    zassert_true(work_queue->Initialize());

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );
    auto frame = std::make_shared<SensorReadingsFrame>();

    // No GPIO, no ADC and no ADC configuration manager: the defaults rt_gauge relies on.
    SensorsController controller(fs_service, work_queue, nullptr, time_service, frame, nullptr, nullptr);
    zassert_equal(controller.Initialize(), 0);

    const auto manager = controller.GetConfigurationManager();
    const auto processing_service = controller.GetProcessingService();

    // Before Start() an update is only stored.
    zassert_true(manager->Update({ MakeVirtualSensor("first", "1 + 1") }));
    k_msleep(100);
    zassert_true(processing_service->IsStopped(), "An update before Start() started processing");

    zassert_equal(controller.Start(), 0);
    zassert_true(processing_service->IsRunning());
    zassert_is_null(processing_service->GetGeneration()->adc_configuration.get());
    zassert_true(WaitForReading(*frame, "first"));

    // What a restart whose Start() failed leaves behind; the next update still restarts.
    zassert_true(processing_service->Stop());
    zassert_true(manager->Update({ MakeVirtualSensor("second", "2 + 2") }));
    zassert_true(WaitForReading(*frame, "second"), "The update after a failed restart was not put into effect");
    zassert_true(processing_service->IsRunning());
    zassert_false(frame->TryGetReading("first").has_value());

    zassert_true(processing_service->Stop());
}

ZTEST(sensors_controller, test_a_restart_stops_and_starts_again_the_dependents_that_were_running) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    auto work_queue = std::make_shared<WorkQueueThread>("sensors_test_dependents_wq", 6144, 5);
    zassert_true(work_queue->Initialize());

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );
    auto frame = std::make_shared<SensorReadingsFrame>();

    // rt_gauge's seven arguments.
    SensorsController controller(fs_service, work_queue, nullptr, time_service, frame, nullptr, nullptr);
    zassert_equal(
        controller.Initialize([](auto manager) {
            zassert_true(manager->Update({ MakeVirtualSensor("first", "1 + 1") }));
        }),
        0
    );

    const auto processing_service = controller.GetProcessingService();
    auto running = std::make_shared<DependentService>(processing_service);
    auto stopped = std::make_shared<DependentService>(processing_service);
    auto refusing = std::make_shared<DependentService>(processing_service);

    controller.RegisterDependentService(running);
    controller.RegisterDependentService(nullptr);
    controller.RegisterDependentService(stopped);
    controller.RegisterDependentService(refusing);

    zassert_equal(controller.Start(), 0);
    zassert_true(running->Start(), "the running dependent did not start");
    zassert_true(refusing->Start(), "the refusing dependent did not start the first time");
    refusing->refuse_start = true;

    zassert_true(controller.GetConfigurationManager()->Update({ MakeVirtualSensor("second", "2 + 2") }));
    zassert_true(WaitForReading(*frame, "second"), "the update was not put into effect");
    zassert_true(WaitFor(running->starts, 2), "a dependent that was running must be started again");
    zassert_true(
        WaitFor(refusing->starts, 2), "a dependent that was running must be started again, even one that refuses"
    );

    zassert_equal(running->stops.load(), 1, "a running dependent is stopped once per restart");
    zassert_true(running->IsRunning(), "the running dependent must run again");
    zassert_true(running->is_processing_running_at_every_stop.load(), "a dependent must stop before processing stops");
    zassert_true(
        running->is_processing_running_at_every_start.load(), "a dependent must start once processing runs again"
    );

    zassert_equal(stopped->starts.load(), 0, "a dependent that was not running must not be started");
    zassert_equal(stopped->stops.load(), 0, "a dependent that was not running must not be stopped");
    zassert_true(stopped->IsStopped());

    zassert_equal(refusing->stops.load(), 1, "a running dependent is stopped once per restart");
    zassert_true(refusing->IsStopped(), "a dependent that does not come back stays stopped");
    zassert_true(processing_service->IsRunning(), "a dependent that does not come back must not fail the restart");

    zassert_true(running->Stop());
    zassert_true(processing_service->Stop());
}

ZTEST(sensors_controller, test_a_disabled_sensor_gets_no_reading_from_processing_or_EmulateReadings) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    auto work_queue = std::make_shared<WorkQueueThread>("sensors_test_disabled_wq", 6144, 5);
    zassert_true(work_queue->Initialize());

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );
    auto frame = std::make_shared<SensorReadingsFrame>();

    SensorsController controller(fs_service, work_queue, nullptr, time_service, frame, nullptr, nullptr);
    zassert_equal(
        controller.Initialize([](auto manager) {
            auto disabled = MakeVirtualSensor("disabled", "2 + 2");
            disabled->is_enabled = false;

            zassert_true(manager->Update({ MakeVirtualSensor("enabled", "1 + 1"), disabled }));
        }),
        0
    );

    const auto processing_service = controller.GetProcessingService();

    zassert_equal(controller.Start(), 0);
    zassert_true(WaitForReading(*frame, "enabled"));
    k_msleep(100);

    zassert_true(frame->FindSlot(std::string_view("disabled")).has_value(), "A disabled sensor keeps its slot");
    zassert_false(frame->TryGetReading("disabled").has_value(), "Processing read a disabled sensor");

    // Stopping drops the readings and keeps the slots, so what follows comes from the emulation alone.
    zassert_true(processing_service->Stop());
    zassert_false(frame->TryGetReading("enabled").has_value());

    controller.EmulateReadings();
    zassert_true(frame->TryGetReading("enabled").has_value(), "The emulation skipped an enabled sensor");
    zassert_false(frame->TryGetReading("disabled").has_value(), "The emulation wrote a disabled sensor");
}
