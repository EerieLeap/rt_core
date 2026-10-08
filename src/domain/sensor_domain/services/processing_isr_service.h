#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/isr_sensor_readers/can_frame_router.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IIsrSensorReader;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::CanFrameRouter;
using eerie_leap::domain::sensor_domain::processors::ReadingPipeline;

// Runs the event-driven sensors of a generation: CAN sensors through one router, GPIO sensors
// through one reader each.
class ProcessingIsrService final : public ServiceBase<> {
private:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory_;
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<ReadingPipeline> pipeline_;

    std::shared_ptr<const SensorGeneration> generation_;
    std::unique_ptr<CanFrameRouter> can_frame_router_;
    std::vector<std::unique_ptr<IIsrSensorReader>> readers_;

    void ProcessSensor(const SensorRuntime& runtime, SensorReading& reading);
    void TearDown();

    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    ProcessingIsrService(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
        std::shared_ptr<CanbusService> canbus_service,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<ReadingPipeline> pipeline);

    // The generation to run; set while stopped, before Start().
    void SetGeneration(std::shared_ptr<const SensorGeneration> generation);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }
};

} // namespace eerie_leap::domain::sensor_domain::services
