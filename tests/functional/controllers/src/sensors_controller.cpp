// SensorsController as rt_gauge builds it, without an ADC: a stored sensors configuration takes
// effect through a restart once processing has been started, also after a restart that failed.
#include <memory>
#include <memory_resource>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/time_service.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

#include "controllers/sensors_controller.h"

using eerie_leap::controllers::SensorsController;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

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

} // namespace

ZTEST(sensors_controller, test_an_update_restarts_processing_once_it_was_started) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    auto work_queue = std::make_shared<WorkQueueThread>("sensors_test_config_wq", 6144, 5);
    zassert_true(work_queue->Initialize());

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
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
